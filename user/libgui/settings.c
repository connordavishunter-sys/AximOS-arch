#include "backend.h"
#include "font.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// ~/.config/aximos/gui.conf has a setting on each line, its name then its value:
//
//     theme dark
//     shadows on
//     clock 24
//     resolution 1280x800
//     repeat 500 30
//     text smooth
//     mouse 5
//     app picture viewer
//     accent blue
//     taskbar bottom
//     autohide off
//     icons on
//     night off 50
//     airplane off
//     animations on
//     quiet off
//     sounds on
//     transparency on
//     blur on
//     corners 8
//     scale 2
//
// Anything missing or not understood is left as it is by default.

#define SETTINGS gui_own_path("gui.conf")
#define WALLPAPER gui_own_path("wallpaper")
#define OWN_PATHS 8 // which can be in use at once
#define DEFAULT_WALLPAPER GUI_WALLPAPERS "/default.jpg"

const char *gui_own_path(const char *name) {
    static char paths[OWN_PATHS][GUI_PATH_MAX];
    static int next;
    const char *home = getenv("HOME");
    char *path = paths[next++ % OWN_PATHS];

    snprintf(path, GUI_PATH_MAX, "%s/.config", home && home[0] ? home : "/tmp");
    mkdir(path, 0700);
    strcat(path, "/aximos");
    mkdir(path, 0700);
    snprintf(path + strlen(path), GUI_PATH_MAX - strlen(path), "/%s", name);
    return path;
}

const char *const gui_accent_names[GUI_ACCENTS] = {"Blue", "Red", "Green", "Purple", "Pink", "Orange", "Yellow",
                                                   "Light blue"};
const uint32_t gui_accent_colors[GUI_ACCENTS] = {0x3D63DD, 0xD83B3B, 0x2E9E4F, 0x8A4FD8,
                                                 0xD9488F, 0xE07B1F, 0xE5B700, 0x2FA4E7};
static const char *const accent_keys[GUI_ACCENTS] = {"blue", "red", "green", "purple", "pink", "orange", "yellow",
                                                     "lightblue"};

// White on all but yellow, which is too light for it.
uint32_t gui_on_accent(int accent) {
    return accent == 6 ? 0x1E2129 : 0xFFFFFF;
}

uint32_t gui_mix(uint32_t a, uint32_t b, int amount) {
    uint32_t keep = 256 - amount;

    return ((((a & 0xFF00FF) * keep + (b & 0xFF00FF) * amount) >> 8) & 0xFF00FF)
         | ((((a & 0x00FF00) * keep + (b & 0x00FF00) * amount) >> 8) & 0x00FF00);
}

void gui_settings_load(struct gui_settings *s) {
    FILE *f = fopen(SETTINGS, "r");
    char line[64], value[32], kind[16];
    int width, height;

    *s = (struct gui_settings){.shadows = true, .animations = true, .sounds = true, .transparency = true, .blur = true, .corners = GUI_CORNERS, .clock_12_hour = true, .repeat_delay = KEYBOARD_REPEAT_DELAY_DEFAULT,
                               .repeat_rate = KEYBOARD_REPEAT_RATE_DEFAULT, .mouse_speed = 5,
                               .apps = {"files", "viewer", "player", "netsurf", "editor"}, .night_strength = 50};
    while (f && fgets(line, sizeof(line), f)) {
        if (sscanf(line, "theme %31s", value) == 1)
            s->light = !strcmp(value, "light");
        else if (sscanf(line, "shadows %31s", value) == 1)
            s->shadows = strcmp(value, "off") != 0;
        else if (sscanf(line, "animations %31s", value) == 1)
            s->animations = strcmp(value, "off") != 0;
        else if (sscanf(line, "clock %31s", value) == 1)
            s->clock_12_hour = !strcmp(value, "12");
        else if (sscanf(line, "resolution %dx%d", &width, &height) == 2 && width > 0 && height > 0)
            s->width = width, s->height = height;
        else if (sscanf(line, "repeat %d %d", &width, &height) == 2 && width >= 0 && height >= 0)
            s->repeat_delay = width, s->repeat_rate = height;
        else if (sscanf(line, "text %31s", value) == 1)
            s->sharp_text = !strcmp(value, "sharp");
        else if (sscanf(line, "mouse %d", &width) == 1 && width >= 1 && width <= 10)
            s->mouse_speed = width;
        else if (sscanf(line, "accent %31s", value) == 1) {
            for (int i = 0; i < GUI_ACCENTS; i++)
                s->accent = !strcmp(value, accent_keys[i]) ? i : s->accent;
        } else if (sscanf(line, "taskbar %31s", value) == 1)
            s->taskbar_top = !strcmp(value, "top");
        else if (sscanf(line, "autohide %31s", value) == 1)
            s->autohide = !strcmp(value, "on");
        else if (sscanf(line, "icons %31s", value) == 1)
            s->hide_icons = !strcmp(value, "off");
        else if (sscanf(line, "night %31s %d", value, &width) == 2 && width >= 10 && width <= 100)
            s->night_light = !strcmp(value, "on"), s->night_strength = width;
        else if (sscanf(line, "airplane %31s", value) == 1)
            s->airplane = !strcmp(value, "on");
        else if (sscanf(line, "quiet %31s", value) == 1)
            s->quiet = !strcmp(value, "on");
        else if (sscanf(line, "sounds %31s", value) == 1)
            s->sounds = strcmp(value, "off") != 0;
        else if (sscanf(line, "transparency %31s", value) == 1)
            s->transparency = strcmp(value, "off") != 0;
        else if (sscanf(line, "blur %31s", value) == 1)
            s->blur = strcmp(value, "off") != 0;
        else if (sscanf(line, "scale %d", &width) == 1 && width >= 1 && width <= 2)
            s->scale = width;
        else if (sscanf(line, "lid %31s", value) == 1) {
            for (int i = 0; i < GUI_LIDS; i++)
                s->lid = !strcmp(value, gui_lid_names[i]) ? i : s->lid;
        } else if (sscanf(line, "power %31s", value) == 1) {
            for (int i = 0; i < GUI_POWERS; i++)
                s->power_button = !strcmp(value, gui_power_names[i]) ? i : s->power_button;
        }
        else if (sscanf(line, "corners %d", &s->corners) == 1)
            s->corners = s->corners < 0 ? 0 : s->corners > GUI_MAX_RADIUS ? GUI_MAX_RADIUS : s->corners;
        else if (sscanf(line, "app %15s %31s", kind, value) == 2) {
            for (int i = 0; i < GUI_KINDS; i++) {
                if (!strcmp(kind, gui_kinds[i]))
                    snprintf(s->apps[i], sizeof(s->apps[i]), "%s", value);
            }
        }
    }
    if (f)
        fclose(f);
    font_smooth = !s->sharp_text;
}

bool gui_settings_save(const struct gui_settings *s) {
    FILE *f = fopen(SETTINGS, "w");

    if (!f)
        return false;
    fprintf(f, "theme %s\nshadows %s\nclock %s\n", s->light ? "light" : "dark", s->shadows ? "on" : "off",
            s->clock_12_hour ? "12" : "24");
    if (s->width)
        fprintf(f, "resolution %dx%d\n", s->width, s->height);
    fprintf(f, "repeat %d %d\ntext %s\nmouse %d\n", s->repeat_delay, s->repeat_rate, s->sharp_text ? "sharp" : "smooth",
            s->mouse_speed);
    for (int i = 0; i < GUI_KINDS; i++)
        fprintf(f, "app %s %s\n", gui_kinds[i], s->apps[i]);
    fprintf(f, "accent %s\ntaskbar %s\nautohide %s\nicons %s\nnight %s %d\nairplane %s\n", accent_keys[s->accent],
            s->taskbar_top ? "top" : "bottom", s->autohide ? "on" : "off", s->hide_icons ? "off" : "on",
            s->night_light ? "on" : "off", s->night_strength, s->airplane ? "on" : "off");
    fprintf(f, "animations %s\nquiet %s\nsounds %s\ntransparency %s\nblur %s\ncorners %d\n",
            s->animations ? "on" : "off", s->quiet ? "on" : "off", s->sounds ? "on" : "off",
            s->transparency ? "on" : "off", s->blur ? "on" : "off", s->corners);
    if (s->scale)
        fprintf(f, "scale %d\n", s->scale);
    fprintf(f, "lid %s\npower %s\n", gui_lid_names[s->lid], gui_power_names[s->power_button]);
    return fclose(f) == 0;
}

const char *const gui_lid_names[GUI_LIDS] = {"suspend", "lock", "nothing"};
const char *const gui_power_names[GUI_POWERS] = {"ask", "suspend", "off", "nothing"};

const char *const gui_kinds[GUI_KINDS] = {"folder", "picture", "sound", "page", "text"};

int gui_kind_of(const char *path) {
    struct stat st;
    const struct gui_image *icon;

    const char *dot = strrchr(path, '.');

    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        return GUI_KIND_FOLDER;
    if (dot && (!strcasecmp(dot, ".html") || !strcasecmp(dot, ".htm")))
        return GUI_KIND_PAGE;
    icon = gui_file_icon(path, false);
    return icon == gui_icon("image") ? GUI_KIND_PICTURE : icon == gui_icon("audio") ? GUI_KIND_SOUND : GUI_KIND_TEXT;
}

const char *gui_app_for(const char *path) {
    static struct gui_settings settings;

    gui_settings_load(&settings);
    return settings.apps[gui_kind_of(path)];
}

void gui_settings_changed(void) {
    gui_backend_settings_changed();
}

void gui_wallpaper_get(char path[GUI_PATH_MAX]) {
    FILE *f = fopen(WALLPAPER, "r");

    snprintf(path, GUI_PATH_MAX, "%s", DEFAULT_WALLPAPER);
    if (f && fgets(path, GUI_PATH_MAX, f))
        path[strcspn(path, "\n")] = '\0';
    if (f)
        fclose(f);
}

bool gui_wallpaper_set(const char *path) {
    FILE *f = fopen(WALLPAPER, "w");

    if (!f)
        return false;
    fprintf(f, "%s\n", path);
    return fclose(f) == 0;
}

// In GUI_APPS, or one of its folders.
bool gui_app_path(const char *name, char path[GUI_PATH_MAX]) {
    DIR *dir;
    struct dirent *d;
    struct stat st;
    bool found = false;

    snprintf(path, GUI_PATH_MAX, GUI_APPS "/%s", name);
    if (!*name || strchr(name, '/') || (stat(path, &st) == 0 && !S_ISDIR(st.st_mode)))
        return *name && !strchr(name, '/');
    if (!(dir = opendir(GUI_APPS)))
        return false;
    while (!found && (d = readdir(dir))) {
        if (d->d_name[0] == '.')
            continue;
        snprintf(path, GUI_PATH_MAX, GUI_APPS "/%s/%s", d->d_name, name);
        found = stat(path, &st) == 0 && !S_ISDIR(st.st_mode);
    }
    closedir(dir);
    return found;
}

static int compare_names(const void *a, const void *b) {
    return strcmp(a, b);
}

static int add_names(const char *folder, char (*names)[32], int count, int max, bool deeper) {
    DIR *dir = opendir(folder);
    struct dirent *d;

    while (dir && count < max && (d = readdir(dir))) {
        char path[GUI_PATH_MAX];
        struct stat st;

        snprintf(path, sizeof(path), "%s/%s", folder, d->d_name);
        if (d->d_name[0] == '.' || stat(path, &st) < 0)
            continue;
        if (!S_ISDIR(st.st_mode))
            snprintf(names[count++], 32, "%s", d->d_name);
        else if (deeper)
            count = add_names(path, names, count, max, false);
    }
    if (dir)
        closedir(dir);
    return count;
}

int gui_app_names(char (*names)[32], int max) {
    int count = add_names(GUI_APPS, names, 0, max, true);

    qsort(names, count, 32, compare_names);
    return count;
}

bool gui_is_program(const char *path) {
    char magic[4];
    int fd = open(path, O_RDONLY);
    bool elf = fd >= 0 && read(fd, magic, 4) == 4 && !memcmp(magic, "\x7f" "ELF", 4);

    if (fd >= 0)
        close(fd);
    return elf;
}

// Time zones are systemd's: /etc/localtime links to the zone's rules, whose name is
// the rest of the path, and timedated sets it.
#define ZONES "/usr/share/zoneinfo"
#define LOCALTIME "/etc/localtime"

static void collect_zones(const char *relative, char ***names, int *count, int *room) {
    char path[GUI_PATH_MAX];
    DIR *dir;
    struct dirent *entry;

    snprintf(path, sizeof(path), "%s%s%s", ZONES, *relative ? "/" : "", relative);
    if (!(dir = opendir(path)))
        return;
    while ((entry = readdir(dir))) {
        char name[GUI_PATH_MAX];
        struct stat st;

        if (entry->d_name[0] == '.')
            continue;
        snprintf(name, sizeof(name), "%s%s%s", relative, *relative ? "/" : "", entry->d_name);
        snprintf(path, sizeof(path), "%s/%s", ZONES, name);
        if (stat(path, &st) < 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (strcmp(name, "posix") && strcmp(name, "right")) // copies of the rest, for other kinds of clock
                collect_zones(name, names, count, room);
            continue;
        }
        if (*count == *room) {
            char **bigger = realloc(*names, (*room = *room ? 2 * *room : 256) * sizeof(char *));

            if (!bigger)
                break;
            *names = bigger;
        }
        (*names)[(*count)++] = strdup(name);
    }
    closedir(dir);
}

static int compare_zones(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int gui_time_zones(char ***names) {
    int count = 0, room = 0;

    *names = NULL;
    collect_zones("", names, &count, &room);
    if (count)
        qsort(*names, count, sizeof(char *), compare_zones);
    return count;
}

void gui_time_zone_get(char name[GUI_PATH_MAX]) {
    char link[GUI_PATH_MAX];
    ssize_t n = readlink(LOCALTIME, link, sizeof(link) - 1);
    const char *zone = n > 0 ? (link[n] = '\0', strstr(link, "zoneinfo/")) : NULL;

    snprintf(name, GUI_PATH_MAX, "%s", zone ? zone + strlen("zoneinfo/") : "UTC");
}

bool gui_time_zone_set(const char *name) {
    int status;
    pid_t pid = fork();

    if (pid == 0) {
        int null = open("/dev/null", O_WRONLY);

        dup2(null, STDOUT_FILENO);
        execlp("timedatectl", "timedatectl", "set-timezone", name, (char *)NULL);
        _exit(127);
    }
    return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
