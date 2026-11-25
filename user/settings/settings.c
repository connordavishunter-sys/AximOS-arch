#include <aximos.h>
#include <audio.h>
#include <dirent.h>
#include <fcntl.h>
#include <gui.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_WALLPAPERS 12
#define THUMB_WIDTH 136 // 16:10, like the wallpapers
#define THUMB_HEIGHT 85
#define THUMBS_ACROSS 3
#define MAX_RESOLUTIONS 16
#define MAX_APPS 32
#define TEST_SOUND GUI_SHARE "/example_music/1.wav"

enum { APPEARANCE, DISPLAY, KEYBOARD, TIME, SOUND, APPS, TASKBAR, TABS };
static const char *const tabs[] = {"Appearance", "Display", "Keyboard", "Date & time", "Sound", "Apps", "Taskbar"};
static const char *const kind_names[GUI_KINDS] = {"Folders", "Pictures", "Sounds", "Web pages", "Other files"};

// The apps there are, to open each kind of file in.
static char app_names[MAX_APPS][32];
static const char *apps[MAX_APPS];
static int app_count;
static char try_typing[128];
static const char *const themes[] = {"Dark", "Light"}, *const clocks[] = {"24 hour", "12 hour"};
static const char *const positions[] = {"Bottom", "Top"}, *const scales[] = {"1x", "2x"};

// What the screen can be, as far as QEMU's graphics card goes. Their widths are
// multiples of 8, which it needs.
static const int sizes[][2] = {
    {800, 600},   {1024, 768},  {1152, 864},  {1280, 720},  {1280, 800}, {1280, 1024},
    {1440, 900},  {1600, 900},  {1680, 1050}, {1920, 1080}, {1920, 1200}, {2560, 1440}, {2560, 1600},
};

static struct gui_settings settings;
static int tab;
static char status[PATH_MAX + 64];

static char **zones;
static const char **shown; // the zones the search matches
static int zone_count, shown_count, chosen;
static char search[64], zone[GUI_PATH_MAX];

static struct {
    char path[GUI_PATH_MAX], name[32];
    struct gui_image thumb;
} wallpapers[MAX_WALLPAPERS];
static int wallpaper_count;
static char wallpaper[GUI_PATH_MAX];
static uint32_t none_pixels[THUMB_WIDTH * THUMB_HEIGHT]; // no wallpaper's thumbnail

static char resolution_names[MAX_RESOLUTIONS][16];
static const char *resolutions[MAX_RESOLUTIONS];
static int resolution_count, picked = -1, wanted_width, wanted_height; // picked, until it's applied

// After a new resolution, it goes back to the one before unless told to keep it,
// in case the screen can't show it.
#define KEEP_SECONDS 15
static int keep_left, keep_timer, old_width, old_height;

static bool contains(const char *text, const char *part) {
    size_t n = strlen(part);

    for (; *text; text++) {
        if (!strncasecmp(text, part, n))
            return true;
    }
    return !n;
}

// The zones the search matches, keeping the chosen one chosen if it still is.
static void filter(void) {
    const char *was = shown_count ? shown[chosen] : zone;

    shown_count = chosen = 0;
    for (int i = 0; i < zone_count; i++) {
        if (contains(zones[i], search)) {
            if (!strcmp(zones[i], was))
                chosen = shown_count;
            shown[shown_count++] = zones[i];
        }
    }
}

static void set_zone(const char *name) {
    if (!gui_time_zone_set(name)) {
        snprintf(status, sizeof(status), "Couldn't set the time zone to %s", name);
        return;
    }
    snprintf(zone, sizeof(zone), "%s", name);
    snprintf(status, sizeof(status), "The time zone is now %s", name);
    printf("settings: zone %s\n", name);
    gui_settings_changed(); // for the taskbar's clock
}

static void save(const char *what) {
    if (!gui_settings_save(&settings)) {
        snprintf(status, sizeof(status), "Couldn't save the settings");
        return;
    }
    printf("settings: %s\n", what);
    gui_settings_changed();
}

// The wallpapers that come with the system, the default first, each with a
// thumbnail and a name from its file's.
static int compare_wallpapers(const void *a, const void *b) {
    const char *x = a, *y = b;
    bool x_default = !strcmp(x + strlen(x) - 11, "default.jpg"), y_default = !strcmp(y + strlen(y) - 11, "default.jpg");

    return x_default != y_default ? y_default - x_default : strcmp(x, y);
}

static void load_wallpapers(void) {
    DIR *dir = opendir(GUI_WALLPAPERS);
    struct dirent *entry;

    while (dir && wallpaper_count < MAX_WALLPAPERS && (entry = readdir(dir))) {
        if (entry->d_name[0] != '.')
            snprintf(wallpapers[wallpaper_count++].path, GUI_PATH_MAX, GUI_WALLPAPERS "/%s", entry->d_name);
    }
    if (dir)
        closedir(dir);
    qsort(wallpapers, wallpaper_count, sizeof(*wallpapers), compare_wallpapers);
    for (int i = 0; i < wallpaper_count; i++) {
        struct gui_image full;
        char *name = wallpapers[i].name;

        snprintf(name, sizeof(wallpapers[i].name), "%s", strrchr(wallpapers[i].path, '/') + 1);
        name[strcspn(name, ".")] = '\0';
        name[0] = name[0] >= 'a' && name[0] <= 'z' ? name[0] - 32 : name[0];
        if (gui_image_load(wallpapers[i].path, &full)) {
            gui_image_scale(&full, THUMB_WIDTH, THUMB_HEIGHT, &wallpapers[i].thumb);
            gui_image_free(&full);
        }
    }
    gui_wallpaper_get(wallpaper);
}

// The sizes the screen can be, with the one it is now, if that's not among them.
static void list_resolutions(void) {
    const struct gui_screen *s = gui_screen();
    int width = s->width * s->scale, height = s->height * s->scale;
    bool listed = false;

    resolution_count = 0;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(*sizes); i++) {
        if (!listed && (sizes[i][0] > width || (sizes[i][0] == width && sizes[i][1] >= height))) {
            listed = true;
            if (sizes[i][0] != width || sizes[i][1] != height)
                snprintf(resolution_names[resolution_count++], 16, "%dx%d", width, height);
        }
        snprintf(resolution_names[resolution_count++], 16, "%dx%d", sizes[i][0], sizes[i][1]);
    }
    if (!listed)
        snprintf(resolution_names[resolution_count++], 16, "%dx%d", width, height);
    for (int i = 0; i < resolution_count; i++)
        resolutions[i] = resolution_names[i];
}

static void set_resolution(int width, int height) {
    char name[32];

    settings.width = width;
    settings.height = height;
    snprintf(name, sizeof(name), "%dx%d", width, height);
    save(name);
}

static void stop_countdown(void) {
    gui_timer_stop(keep_timer);
    keep_left = 0;
}

static void go_back(void) {
    printf("settings: going back to %dx%d\n", old_width, old_height);
    stop_countdown();
    set_resolution(old_width, old_height);
}

// What wm made of a resolution asked for.
static void screen_changed(void) {
    const struct gui_screen *s = gui_screen();
    int width = s->width * s->scale, height = s->height * s->scale;

    printf("settings: screen %dx%d%s%s\n", width, height, s->scale > 1 ? " at 2x" : "", s->can_resize ? "" : " fixed");
    if (wanted_width && (width != wanted_width || height != wanted_height))
        snprintf(status, sizeof(status), "The screen couldn't be %dx%d", wanted_width, wanted_height);
    else if (wanted_width) {
        keep_left = KEEP_SECONDS;
        keep_timer = gui_timer_start(1000, true);
        printf("settings: keep? %d\n", keep_left);
    }
    wanted_width = 0;
    picked = -1;
    list_resolutions();
}

// Says where a widget is when that changes, for the tests to find it.
static void report(struct gui_ui *ui, int index, const char *what) {
    static struct gui_rect places[41 + MAX_WALLPAPERS];
    struct gui_rect r = gui_last_rect(ui);

    if (memcmp(&places[index], &r, sizeof(r)))
        printf("settings: at %s %d %d %d %d\n", what, r.x, r.y, r.width, r.height);
    places[index] = r;
}

static void appearance(struct gui_ui *ui) {
    int theme = settings.light;

    if (gui_dropdown(ui, "Theme", themes, 2, &theme)) {
        settings.light = theme;
        save(settings.light ? "theme light" : "theme dark");
    }
    report(ui, 0, "theme");

    int accent = settings.accent;

    if (gui_dropdown(ui, "Accent colour", gui_accent_names, GUI_ACCENTS, &accent)) {
        char what[32];

        settings.accent = accent;
        snprintf(what, sizeof(what), "accent %s", gui_accent_names[accent]);
        save(what);
    }
    report(ui, 24, "accent");
    if (gui_checkbox(ui, "Window shadows", &settings.shadows))
        save(settings.shadows ? "shadows on" : "shadows off");
    report(ui, 1, "shadows");
    if (gui_checkbox(ui, "Animations", &settings.animations))
        save(settings.animations ? "animations on" : "animations off");
    report(ui, 30, "animations");
    if (gui_checkbox(ui, "Transparency", &settings.transparency))
        save(settings.transparency ? "transparency on" : "transparency off");
    report(ui, 33, "transparency");
    if (gui_checkbox(ui, "Blur behind see-through things", &settings.blur))
        save(settings.blur ? "blur on" : "blur off");
    report(ui, 34, "blur");
    gui_label(ui, "Corners, from square to round:");
    if (gui_slider(ui, "##corners", &settings.corners, 0, GUI_MAX_RADIUS)) {
        char what[32];

        snprintf(what, sizeof(what), "corners %d", settings.corners);
        save(what);
    }
    report(ui, 35, "corners");

    bool smooth = !settings.sharp_text;

    if (gui_checkbox(ui, "Smooth text", &smooth)) {
        settings.sharp_text = !smooth;
        save(smooth ? "text smooth" : "text sharp");
    }
    report(ui, 10, "smooth");
    gui_separator(ui);
    gui_label(ui, "Wallpaper");

    // None first, which leaves the desktop the theme's colour, as wm has it.
    struct gui_image none = {none_pixels, THUMB_WIDTH, THUMB_HEIGHT, 1};

    for (int i = 0; i < THUMB_WIDTH * THUMB_HEIGHT; i++)
        none_pixels[i] = settings.light ? 0xFFD5DBE6 : 0xFF1E2430;
    for (int i = -1; i < wallpaper_count; i++) {
        const char *path = i < 0 ? "none" : wallpapers[i].path, *name = i < 0 ? "None" : wallpapers[i].name;
        const struct gui_image *thumb = i < 0 ? &none : wallpapers[i].thumb.pixels ? &wallpapers[i].thumb : NULL;
        char label[64];

        if ((i + 1) % THUMBS_ACROSS)
            gui_same_line(ui);
        snprintf(label, sizeof(label), "%s##wallpaper", name);
        if (gui_image_button(ui, label, thumb, !strcmp(wallpaper, path)) && gui_wallpaper_set(path)) {
            snprintf(wallpaper, sizeof(wallpaper), "%s", path);
            printf("settings: wallpaper %s\n", wallpaper);
            gui_settings_changed();
        }
        snprintf(label, sizeof(label), "wallpaper %s", name);
        report(ui, 41 + i, label);
    }
}

// Less blue on the screen, which quick settings turns on and off as well.
static void night_light(struct gui_ui *ui) {
    char what[32];

    gui_separator(ui);
    gui_heading(ui, "Night light");
    if (gui_checkbox(ui, "Less blue light", &settings.night_light))
        save(settings.night_light ? "night light on" : "night light off");
    report(ui, 25, "night");
    gui_label(ui, "How much less:");
    if (gui_slider(ui, "##strength", &settings.night_strength, 10, 100)) {
        snprintf(what, sizeof(what), "night strength %d", settings.night_strength);
        save(what);
    }
    report(ui, 26, "strength");
}

// Where the taskbar goes, whether it hides, and whether the desktop has icons.
static void taskbar(struct gui_ui *ui) {
    int top = settings.taskbar_top;
    bool icons = !settings.hide_icons;

    if (gui_dropdown(ui, "Position", positions, 2, &top)) {
        settings.taskbar_top = top;
        save(top ? "taskbar top" : "taskbar bottom");
    }
    report(ui, 27, "position");
    if (gui_checkbox(ui, "Hide the taskbar until the mouse goes to it", &settings.autohide))
        save(settings.autohide ? "autohide on" : "autohide off");
    report(ui, 28, "autohide");
    if (gui_checkbox(ui, "Icons on the desktop", &icons)) {
        settings.hide_icons = !icons;
        save(icons ? "icons shown" : "icons hidden");
    }
    report(ui, 29, "icons");
}

// How big everything on the screen is: 2x draws each pixel as four.
static void scale(struct gui_ui *ui) {
    int at = gui_screen()->scale - 1;

    if (gui_dropdown(ui, "Scale", scales, 2, &at)) {
        settings.scale = at + 1;
        save(at ? "scale 2x" : "scale 1x");
    }
    report(ui, 36, "scale");
}

static void display(struct gui_ui *ui) {
    const struct gui_screen *s = gui_screen();
    char now[16];
    int at = 0;

    snprintf(now, sizeof(now), "%dx%d", s->width * s->scale, s->height * s->scale);
    if (!s->can_resize) {
        gui_label(ui, "The screen is %s, and its resolution can't be changed.", now);
        scale(ui);
        night_light(ui);
        return;
    }
    for (int i = 0; i < resolution_count; i++)
        at = strcmp(resolutions[i], now) ? at : i;

    // Choosing one only picks it, so going through them doesn't change the screen each time.
    int shown_at = picked >= 0 ? picked : at;

    if (gui_dropdown(ui, "Resolution", resolutions, resolution_count, &shown_at))
        picked = shown_at;
    report(ui, 2, "resolution");
    if (gui_button(ui, "Apply") && picked >= 0 && picked != at && !keep_left) {
        old_width = s->width * s->scale;
        old_height = s->height * s->scale;
        sscanf(resolutions[picked], "%dx%d", &wanted_width, &wanted_height);
        status[0] = '\0';
        set_resolution(wanted_width, wanted_height);
    }
    report(ui, 7, "apply");
    gui_same_line(ui);
    gui_label(ui, "The screen is %s.", now);
    gui_label(ui, "%s", status);
    scale(ui);
    night_light(ui);
}

static void date_and_time(struct gui_ui *ui) {
    int clock = settings.clock_12_hour;
    char now[32];
    time_t t = time(NULL);

    if (gui_dropdown(ui, "Clock", clocks, 2, &clock)) {
        settings.clock_12_hour = clock;
        save(settings.clock_12_hour ? "clock 12" : "clock 24");
    }
    report(ui, 3, "clock");
    gui_separator(ui);
    strftime(now, sizeof(now), settings.clock_12_hour ? "%I:%M %p" : "%H:%M", localtime(&t));
    gui_label(ui, "Time zone: %s, where it's %s", zone, now + (settings.clock_12_hour && now[0] == '0'));
    if (gui_button(ui, "Use this zone") && shown_count)
        set_zone(shown[chosen]);
    report(ui, 4, "use");
    gui_same_line(ui);
    if (gui_textbox(ui, "##search", search, sizeof(search)))
        filter();
    report(ui, 5, "search");
    gui_label(ui, "%s", status);
    if (gui_list(ui, "##zones", shown, shown_count, &chosen, 0) == GUI_LIST_ACTIVATED)
        set_zone(shown[chosen]);
    report(ui, 6, "zones");
}

static void keyboard(struct gui_ui *ui) {
    char what[32];

    gui_label(ui, "How long a key's held before it repeats, in milliseconds:");
    if (gui_slider(ui, "##delay", &settings.repeat_delay, 150, 1000)) {
        snprintf(what, sizeof(what), "repeat delay %d", settings.repeat_delay);
        save(what);
    }
    report(ui, 12, "delay");
    gui_label(ui, "How many times a second it repeats then, or 0 for never:");
    if (gui_slider(ui, "##rate", &settings.repeat_rate, 0, 60)) {
        snprintf(what, sizeof(what), "repeat rate %d", settings.repeat_rate);
        save(what);
    }
    report(ui, 13, "rate");
    gui_separator(ui);
    gui_label(ui, "Try it here:");
    gui_textbox(ui, "##try", try_typing, sizeof(try_typing));
    report(ui, 14, "try");
    gui_separator(ui);
    gui_heading(ui, "Mouse");
    gui_label(ui, "How fast the pointer goes, as the mouse moves it at 5:");
    if (gui_slider(ui, "##mouse", &settings.mouse_speed, 1, 10)) {
        snprintf(what, sizeof(what), "mouse speed %d", settings.mouse_speed);
        save(what);
    }
    report(ui, 15, "mouse");
}

// The volume of everything, which audiod keeps, and wm shows.
static void sound(struct gui_ui *ui) {
    struct audio_mixer m;

    if (!audio_mixer_get(&m) || !m.card) {
        gui_label(ui, "There's no sound card.");
        return;
    }
    struct audio_output outputs[8];
    const char *names[8];
    int count = audio_outputs(outputs, 8), which = 0;

    count = count > 8 ? 8 : count;
    for (int i = 0; i < count; i++) {
        names[i] = outputs[i].name;
        which = outputs[i].current ? i : which;
    }
    if (count > 1) { // each with its own volume, which the slider then shows
        if (gui_dropdown(ui, "Play through", names, count, &which) && audio_set_output(outputs[which].key))
            printf("settings: playing through %s\n", outputs[which].name);
        report(ui, 37, "output");
    }
    gui_label(ui, "How loud everything plays:");

    bool changed = gui_slider(ui, "##volume", &m.volume, 0, 100);

    report(ui, 16, "volume");
    changed |= gui_checkbox(ui, "Mute", &m.muted);
    report(ui, 17, "mute");
    if (changed && audio_set_volume(m.volume) && audio_set_muted(m.muted)) {
        printf("settings: volume %d%s\n", m.volume, m.muted ? ", muted" : "");
        gui_settings_changed();
    }
    if (gui_checkbox(ui, "Low latency", &m.low_latency) && audio_set_low_latency(m.low_latency))
        printf("settings: low latency %s\n", m.low_latency ? "on" : "off");
    report(ui, 32, "low latency");
    gui_label(ui, "Plays sound in about 5 ms rather than 20, but it can crackle while the computer's busy.");
    if (gui_checkbox(ui, "System sounds", &settings.sounds))
        save(settings.sounds ? "sounds on" : "sounds off");
    report(ui, 31, "sounds");
    gui_separator(ui);
    char player[GUI_PATH_MAX];

    if (gui_button(ui, "Play a test sound") && gui_app_path("player", player) && fork() == 0) {
        execl(player, "player", TEST_SOUND, (char *)NULL);
        _exit(127);
    }
    report(ui, 18, "test sound");
}

// Which app opens each kind of file, from Files, the desktop and notifications.
static void default_apps(struct gui_ui *ui) {
    gui_label(ui, "What opens each kind of file:");
    for (int kind = 0; kind < GUI_KINDS; kind++) {
        int at = 0;
        char label[48];

        for (int i = 0; i < app_count; i++)
            at = strcmp(apps[i], settings.apps[kind]) ? at : i;
        snprintf(label, sizeof(label), "%s##app%d", kind_names[kind], kind);
        if (gui_dropdown(ui, label, apps, app_count, &at)) {
            char what[64];

            snprintf(settings.apps[kind], sizeof(settings.apps[kind]), "%s", apps[at]);
            snprintf(what, sizeof(what), "app %s %s", gui_kinds[kind], apps[at]);
            save(what);
        }
        report(ui, 19 + kind, gui_kinds[kind]);
    }
}

static void load_apps(void) {
    app_count = gui_app_names(app_names, MAX_APPS);
    for (int i = 0; i < app_count; i++)
        apps[i] = app_names[i];
}

// Asks whether to keep the new resolution, above whichever tab is showing.
static void keep_strip(struct gui_ui *ui) {
    gui_label(ui, "Keep this resolution? Going back in %d second%s.", keep_left, keep_left == 1 ? "" : "s");
    if (gui_button(ui, "Keep")) {
        printf("settings: kept %dx%d\n", settings.width, settings.height);
        stop_countdown();
        ui->again = true; // without the strip
    }
    report(ui, 8, "keep");
    gui_same_line(ui);
    if (gui_button(ui, "Go back") || gui_key(ui, KEY_ESCAPE)) {
        go_back();
        ui->again = true;
    }
    report(ui, 9, "go back");
    gui_separator(ui);
}

static void frame(struct gui_ui *ui) {
    int was = tab;

    while (waitpid(-1, NULL, WNOHANG) > 0)
        ; // a test sound that's finished

    gui_ui_begin(ui);
    gui_tabs(ui, tabs, TABS, &tab, NULL);
    if (keep_left)
        keep_strip(ui);
    if (tab != was) {
        printf("settings: tab %s\n", tabs[tab]);
        status[0] = '\0';
    }
    // Pages scroll when the window's too small for them, but for the time zones',
    // whose list goes down to the bottom of the window and scrolls itself.
    bool scrolls = tab != TIME;
    char page[48];

    snprintf(page, sizeof(page), "##%s page", tabs[tab]);
    if (scrolls)
        gui_scroll_begin(ui, page, 0);
    if (tab == APPEARANCE)
        appearance(ui);
    else if (tab == DISPLAY)
        display(ui);
    else if (tab == KEYBOARD)
        keyboard(ui);
    else if (tab == SOUND)
        sound(ui);
    else if (tab == APPS)
        default_apps(ui);
    else if (tab == TASKBAR)
        taskbar(ui);
    else
        date_and_time(ui);
    if (scrolls)
        gui_scroll_end(ui);
    gui_ui_end(ui);
    fflush(stdout);
}

// Settings for the desktop: its theme, shadows, wallpaper, resolution, key repeat, mouse
// speed, clock, time zone, volume and the apps that open each kind of file.
int main(int argc, char **argv) {
    help_option(argc, argv, "usage: settings\n"
                            "Opens a window to change the theme, shadows, wallpaper, resolution, key repeat, mouse\n"
                            "speed, clock, time zone, volume and the apps that open each kind of file.\n");
    if (!gui_connect()) {
        dprintf(STDERR_FILENO, "settings: wm isn't running\n");
        return 1;
    }

    struct gui_window *window = gui_create_window("Settings", 680, 540);
    struct gui_ui ui;
    struct gui_message e;

    if (!window) {
        dprintf(STDERR_FILENO, "settings: couldn't make a window\n");
        return 1;
    }
    gui_time_zone_get(zone);
    gui_settings_load(&settings);
    zone_count = gui_time_zones(&zones);
    shown = calloc(zone_count + 1, sizeof(*shown));
    filter();
    load_wallpapers();
    load_apps();
    gui_ui_init(&ui, window);
    frame(&ui);
    while (gui_next_event(&e) && e.type != GUI_CLOSE) {
        if (e.type == GUI_SCREEN)
            screen_changed();
        if (e.type == GUI_SETTINGS) // changed elsewhere, as quick settings do
            gui_settings_load(&settings);
        if (e.type == GUI_TIMER && e.x == keep_timer && keep_left && --keep_left == 0)
            go_back();
        if (gui_ui_event(&ui, &e))
            frame(&ui);
    }
    if (keep_left) // closed without saying to keep it
        go_back();
    gui_destroy_window(window);
    return 0;
}
