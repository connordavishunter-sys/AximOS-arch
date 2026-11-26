#include <aximos.h>
#include "bluetooth.h"
#include "network.h"
#include "polkit.h"
#include "server.h"
#include <arpa/inet.h>
#include <audio.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gui.h>
#include <math.h>
#include <mouse_event.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#define MAX_WINDOWS 64
#define MAX_DAMAGE 32
#define MAX_SIZE 4096
#define CLIPBOARD_MAX (64 * 1024 * 1024)

#define TITLE_HEIGHT 24
#define BORDER 1
#define BUTTON_SIZE 16
#define CASCADE 32
#define RESIZE_MARGIN 6 // outside the frame, where dragging resizes
#define CORNER 16       // how far along an edge from a corner dragging moves both edges
#define MIN_WIDTH 120
#define MIN_HEIGHT 48

// Shadows reach SHADOW past the frame, moved SHADOW_DROP down, and are darkest,
// out of 255, at SHADOW_ALPHA. Their strength, out of 256, fades over FADE_MS
// between what's right for focused windows and the rest, and from nothing for
// new windows, quickly at first and slowing to a stop.
#define SHADOW 8
#define SHADOW_DROP 0
#define SHADOW_ALPHA 80
#define SHADOW_FOCUSED 256
#define SHADOW_UNFOCUSED 140
#define FADE_MS 240
#define FRAME_MS 16

#define TASKBAR_HEIGHT 32
#define BAR_ALPHA 200 // out of 256: the taskbar and title bars show a little of what's under them
#define RADIUS settings.corners              // of windows', menus' and panels' corners
#define SMALL_RADIUS (settings.corners / 2) // of buttons and highlights
#define TASKBAR_BUTTON_WIDTH 160
#define LAUNCHER_WIDTH 64

#define APPS_DIR GUI_APPS
#define ICONS GUI_SHARE "/icons"
#define MAX_ICONS 32
#define ICON_SIZE 16

#define MENU_WIDTH 200
#define ITEM_HEIGHT 24 // in menus and the Alt+Tab switcher
#define MENU_GAP 9     // a line between items in a menu
#define SWITCHER_WIDTH 360

// Colours, from the theme the settings choose. What's on the focused title bar,
// the red close button and the grey ones is white in both.
struct theme {
    uint32_t desktop, hint, border, title_focused, title_unfocused, title_text_unfocused;
    uint32_t taskbar, taskbar_line, taskbar_button, taskbar_minimized, text, dim_text, on_accent;
};

static const struct theme dark = {
    .desktop = 0x1E2430, .hint = 0x5C677D, .border = 0x0E1118, .title_focused = 0x3D63DD,
    .title_unfocused = 0x3A4150, .title_text_unfocused = 0xFFFFFF, .taskbar = 0x141821, .taskbar_line = 0x2A3140,
    .taskbar_button = 0x2E3646, .taskbar_minimized = 0x1D222C, .text = 0xFFFFFF, .dim_text = 0x8A93A6,
};
static const struct theme light = {
    .desktop = 0xD5DBE6, .hint = 0x6B7488, .border = 0x9AA2B2, .title_focused = 0x3D63DD,
    .title_unfocused = 0xD9DDE5, .title_text_unfocused = 0x2B2F38, .taskbar = 0xEDEFF3, .taskbar_line = 0xC9CED8,
    .taskbar_button = 0xDCE0E8, .taskbar_minimized = 0xE8EBF0, .text = 0x1E2129, .dim_text = 0x7A8396,
};
static struct theme theme_now; // the one chosen, with the accent colour
static const struct theme *theme = &dark;

#define DESKTOP_COLOR (theme->desktop)
#define HINT_COLOR (theme->hint)
#define BORDER_COLOR (theme->border)
#define TITLE_FOCUSED (theme->title_focused)
#define TITLE_UNFOCUSED (theme->title_unfocused)
#define TITLE_TEXT_UNFOCUSED (theme->title_text_unfocused)
#define TASKBAR_COLOR (theme->taskbar)
#define TASKBAR_LINE (theme->taskbar_line)
#define TASKBAR_BUTTON (theme->taskbar_button)
#define TASKBAR_MINIMIZED (theme->taskbar_minimized)
#define TASKBAR_TEXT (theme->text)
#define TASKBAR_DIM_TEXT (theme->dim_text)
#define TITLE_TEXT 0xFFFFFF
#define ON_ACCENT (theme->on_accent) // text on the accent colour
#define CLOSE_COLOR 0xE5484D
#define BUTTON_COLOR 0x56607A

// Super+Enter runs this.
#define LAUNCH_COMMAND "term"
#define RUN_COMMAND "run" // Super+R, and Run... in the menu
#define KEY_Q 0x10
#define KEY_R 0x13
#define KEY_L 0x26
#define KEY_S 0x1F
#define TASKS_COMMAND "tasks" // Ctrl+Shift+Escape
#define HINT "Super: apps   Super+R: run   Super+Enter: terminal   Alt+Tab: switch   Super+Q: close   Super+arrows: tile   " \
             "Super+1-4: workspaces   Super+Shift+Q: quit"

// Four workspaces, each with its own windows, which Super and a number goes to, and
// with Shift as well, sends the focused window to.
#define WORKSPACES 4
#define KEY_1 0x02
#define WORKSPACE_SIZE 16 // each one's box on the taskbar, after the launcher
#define WORKSPACE_GAP 4

// Dragging a window to the left or right edge of the screen tiles it on that half,
// and to the top maximizes it, as an outline shows while it's held there.
enum snap { SNAP_NONE, SNAP_LEFT, SNAP_RIGHT, SNAP_TOP, SNAP_TOP_LEFT, SNAP_TOP_RIGHT, SNAP_BOTTOM_LEFT, SNAP_BOTTOM_RIGHT };
static const char *const snap_names[] = {"", "left", "right", "maximized", "top left", "top right", "bottom left",
                                         "bottom right"};
#define SNAP_CORNER 64 // how far along a side edge from a corner a dragged window takes a quarter
#define SNAP_BORDER 3

struct rect {
    int x, y, width, height;
};

// x and y are where the contents start, inside the frame. The canvas is wm's copy of
// what the app last drew.
struct window {
    struct surface *surface;
    uint32_t id;
    int x, y;
    char title[GUI_TITLE_MAX];
    char app[GUI_TITLE_MAX]; // the program, which picks its icon
    struct gui_canvas canvas;
    bool minimized;
    bool put_away; // minimized by the show desktop corner, which brings it back

    // The size it's been asked to be. One request is out at a time, and the program
    // answers it with a new buffer of whatever size it chose.
    int want_width, want_height;
    int asked_width, asked_height;
    bool asking;

    // Where it was before being maximized or tiled, to go back to.
    bool tiled;
    enum snap snapped; // which, while tiled, or SNAP_NONE for a layout's part
    struct rect restore;

    // The edges being dragged by a resize. When the left or top one moves, the
    // window moves with each new size so the opposite edge stays still.
    int resize_edges;

    enum gui_cursor cursor; // what the program wants over its contents
    int shadow, fade_from, fade_to;
    long fade_start;
    bool bare;  // the log-in screen's, with no frame, in the middle of the screen, or a popup's
    bool login; // the log-in or lock screen's
    bool popup; // a menu or tooltip of an app's, with no frame, where the app put it
    bool translucent; // its pixels' top bytes say how see-through they are, 0 for not at all
    int workspace;

    // While it opens, is restored, maximized or tiled, it goes from anim_from to
    // its frame over anim_ms, fading in if anim_fade, drawn from layer, a copy of it.
    int anim_ms;
    bool anim_fade;
    int64_t anim_start;
    struct rect anim_from, anim_to; // where it was going as of the last frame
    struct gui_canvas layer;
    bool copy_stale; // changed since layer was drawn, for the overview or a preview
};

enum part { PART_NONE, PART_CONTENT, PART_TITLE, PART_CLOSE, PART_MAXIMIZE, PART_MINIMIZE, PART_RESIZE };
enum { EDGE_LEFT = 1, EDGE_RIGHT = 2, EDGE_TOP = 4, EDGE_BOTTOM = 8 };

// Composed here, at the display's own resolution: everything's laid out in the
// desktop's pixels, each scale of the display's wide and high, and drawn as sharply
// as the display is. Every canvas wm draws on is at this scale, apps' windows too.
static struct gui_canvas screen;
static const struct display *display;
static struct screen output;
static int scale = 1;

static struct rect scaled(struct rect r) {
    return (struct rect){r.x * scale, r.y * scale, r.width * scale, r.height * scale};
}

static struct window *windows[MAX_WINDOWS]; // bottom first
static int window_count;
static uint32_t next_id = 1;
static struct window *focused;

static uint8_t falloff[SHADOW + 1][SHADOW + 1];
// Out of 256, how much of each of the display's pixels in a corner, from the corner,
// is inside a window's rounded edge.
static uint16_t coverage[GUI_KEPT][GUI_KEPT];
static struct gui_settings settings;

// Whether a window shows what's under it, which the settings can turn off.
static bool see_through(const struct window *w) {
    return w->translucent && settings.transparency;
}
static struct gui_canvas wallpaper; // the screen's part of the scaled image, if there is one
static struct gui_image wallpaper_image; // scaled to cover the screen
static char wallpaper_path[GUI_PATH_MAX];
static struct {
    char name[GUI_TITLE_MAX];
    struct gui_image image; // no pixels if there's no such icon
} icons[MAX_ICONS];
static int icon_count;
static char *clipboard;
static size_t clipboard_length;
static int clipboard_width, clipboard_height; // a picture's, or 0 for text

// What's been copied as text, newest first, which the tray's clipboard panel puts back.
#define CLIPS_MAX 10
static char *clips[CLIPS_MAX];
static size_t clip_lengths[CLIPS_MAX];
static int clip_count;

static struct rect damage[MAX_DAMAGE];
static int damage_count;

static int cursor_x, cursor_y;
static uint32_t buttons;
static struct window *dragging; // by its title bar
static int drag_x, drag_y;
static struct window *grabbed; // pressed in its contents, so it gets the mouse until release
static char drag_file[GUI_PATH_MAX]; // dragged from grabbed, to drop on a window
static const struct gui_image *drag_icon;
static bool drag_given; // to the app it was taken over, as its own drag, so it's that app's to take
static uint16_t modifiers; // the keys held, which go with a drop: Ctrl copies, rather than moving
static struct window *resizing;
static int resize_x, resize_y, resize_width, resize_height;
static int resize_room; // above the frame, which it can't grow into
static bool quitting;
static int workspace; // the one shown
static enum snap snap; // where the window being dragged would go, if let go now

// With --login, the desktop is locked until the log-in screen, which wm starts
// with a connection of its own, says someone's logged in. While it's locked only
// that screen's window is shown, and it gets all the input.
#define LOGIN_PATH GUI_LIBEXEC "/login"
static bool login_mode, locked, logged_in; // login_mode is the greeter's, until someone logs in
static bool login_running; // the log-in or lock screen
static int64_t login_due = -1, login_started; // when to start it again, if it stops
static char **commands; // to start once someone first logs in
static int command_count;

// Menus: the start menu, and those from right-clicking the desktop, an icon on
// it, or the taskbar, with one item's submenu open at a time beside it.
enum action { ACT_NONE, ACT_LINE, ACT_APP, ACT_FOLDER, ACT_RUN, ACT_LOCK, ACT_LOG_OUT, ACT_SUSPEND, ACT_REBOOT, ACT_SHUT_DOWN,
              ACT_OPEN, ACT_OPEN_WITH, ACT_OPEN_IN, ACT_RUN_FILE, ACT_CUT, ACT_COPY, ACT_COPY_PATH, ACT_RENAME,
              ACT_DELETE, ACT_PROPERTIES, ACT_REFRESH, ACT_SHOW_ICONS, ACT_POSITION, ACT_TOP, ACT_BOTTOM, ACT_AUTOHIDE,
              ACT_NEW_FOLDER, ACT_NEW_FILE, ACT_SETTINGS, ACT_WIN_SHOW, ACT_WIN_MINIMIZE, ACT_WIN_MAXIMIZE, ACT_WIN_CLOSE };
#define MAX_ITEMS 40
struct item {
    char label[GUI_TITLE_MAX];
    char arg[GUI_PATH_MAX]; // the app or folder it's for
    const struct gui_image *icon;
    int action;
    bool check, checked; // a checkbox, and whether it's ticked
};
struct popup {
    struct item items[MAX_ITEMS];
    int count, choice;
    struct rect r;
};
static struct popup popups[2];       // the menu and its submenu
static int popup_depth, popup_focus; // how many are open, and which has the keyboard
static bool start_open;              // whether the menu's the start menu
static char popup_file[GUI_PATH_MAX]; // the desktop's icon a menu's about
static struct window *menu_window;     // or the window, for its taskbar button's
static bool super_down, super_alone; // Super pressed, and nothing else since

// The Alt+Tab switcher, while Alt is held: which window it's on, counting from the top.
static int switch_choice = -1;

// Cursors, in black X and white dots, each with the point that's on the mouse's
// position. The up and down arrow is the left and right one turned, and one
// diagonal is the other mirrored.
static const char *const arrow_rows[] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ",
    "X....X      ", "X.....X     ", "X......X    ", "X.......X   ", "X........X  ",
    "X.........X ", "X......XXXXX", "X...X..X    ", "X..XX..X    ", "X.X  X..X   ",
    "XX   X..X   ", "X     X..X  ", "      X..X  ", "       XX   ",
};
static const char *const text_rows[] = {
    "XXX XXX", "X..X..X", "XXX.XXX", "  X.X  ", "  X.X  ", "  X.X  ", "  X.X  ", "  X.X  ",
    "  X.X  ", "  X.X  ", "  X.X  ", "  X.X  ", "  X.X  ", "XXX.XXX", "X..X..X", "XXX XXX",
};
static const char *const across_rows[] = {
    "    X         X    ", "   XX         XX   ", "  X.XXXXXXXXXXX.X  ",
    " X...............X ", "X.................X", " X...............X ",
    "  X.XXXXXXXXXXX.X  ", "   XX         XX   ", "    X         X    ",
};
static const char *const diagonal_rows[] = {
    "XXXXXX       ", "X....X       ", "X...X        ", "X....X       ", "X.X...X      ",
    "XX X...X     ", "    X...X    ", "     X...X XX", "      X...X.X", "       X....X",
    "        X...X", "       X....X", "       XXXXXX",
};

struct cursor_shape {
    const char *const *rows;
    int width, height; // of rows, before turning
    int hot_x, hot_y;  // after
    bool turned, mirrored;
};

#define ROWS(r, width) r, width, (int)(sizeof(r) / sizeof(r[0]))
static const struct cursor_shape shapes[] = {
    [GUI_CURSOR_ARROW] = {ROWS(arrow_rows, 12), 0, 0, false, false},
    [GUI_CURSOR_TEXT] = {ROWS(text_rows, 7), 3, 8, false, false},
    [GUI_CURSOR_RESIZE_H] = {ROWS(across_rows, 19), 9, 4, false, false},
    [GUI_CURSOR_RESIZE_V] = {ROWS(across_rows, 19), 4, 9, true, false},
    [GUI_CURSOR_RESIZE_NWSE] = {ROWS(diagonal_rows, 13), 6, 6, false, false},
    [GUI_CURSOR_RESIZE_NESW] = {ROWS(diagonal_rows, 13), 6, 6, false, true},
};
static enum gui_cursor cursor_shown = GUI_CURSOR_ARROW;

static struct rect frame_of(const struct window *w) {
    if (w->bare)
        return (struct rect){w->x, w->y, w->canvas.width, w->canvas.height};
    return (struct rect){w->x - BORDER, w->y - TITLE_HEIGHT - BORDER, w->canvas.width + 2 * BORDER,
                         w->canvas.height + TITLE_HEIGHT + 2 * BORDER};
}

// The frame and its shadow.
static struct rect extent_of(const struct window *w) {
    struct rect f = frame_of(w);

    if (!settings.shadows)
        return f;
    return (struct rect){f.x - SHADOW, f.y - SHADOW + SHADOW_DROP, f.width + 2 * SHADOW, f.height + 2 * SHADOW};
}

// The title bar's buttons from the right: close, maximize, minimize.
static struct rect button_of(const struct window *w, int index) {
    int gap = (TITLE_HEIGHT - BUTTON_SIZE) / 2;

    return (struct rect){w->x + w->canvas.width - (index + 1) * (BUTTON_SIZE + gap),
                         w->y - TITLE_HEIGHT + gap, BUTTON_SIZE, BUTTON_SIZE};
}

// The taskbar goes along the top or the bottom of the screen. Hidden, it comes
// back while the mouse is at that edge, or on it.
static bool bar_shown = true;

static bool bar_visible(void) {
    return !settings.autohide || bar_shown;
}

// The taskbar and its flyouts float this far from the screen's edges, and from each other.
#define GAP 8

// Where windows go: everything but the taskbar and the gaps around it, unless it hides.
static struct rect desktop(void) {
    int used = TASKBAR_HEIGHT + 2 * GAP;

    if (settings.autohide)
        return (struct rect){0, 0, screen.width, screen.height};
    return (struct rect){0, settings.taskbar_top ? used : 0, screen.width, screen.height - used};
}

static struct rect taskbar(void) {
    return (struct rect){GAP, settings.taskbar_top ? GAP : screen.height - GAP - TASKBAR_HEIGHT, screen.width - 2 * GAP,
                         TASKBAR_HEIGHT};
}

// Where something height tall goes above or below the taskbar.
static int beside_bar(int height) {
    struct rect bar = taskbar();

    return settings.taskbar_top ? bar.y + bar.height + GAP : bar.y - GAP - height;
}

static struct rect launcher(void) {
    struct rect bar = taskbar();

    return (struct rect){bar.x + 4, bar.y + 4, LAUNCHER_WIDTH, TASKBAR_HEIGHT - 8};
}

static struct rect workspace_box(int index) {
    struct rect l = launcher();

    return (struct rect){l.x + l.width + 8 + index * (WORKSPACE_SIZE + WORKSPACE_GAP),
                         taskbar().y + (TASKBAR_HEIGHT - WORKSPACE_SIZE) / 2, WORKSPACE_SIZE, WORKSPACE_SIZE};
}

static struct rect snap_rect(enum snap to) {
    struct rect d = desktop();

    int half = d.width / 2, top = d.height / 2;

    return to == SNAP_LEFT           ? (struct rect){0, d.y, half, d.height}
         : to == SNAP_RIGHT        ? (struct rect){half, d.y, d.width - half, d.height}
         : to == SNAP_TOP_LEFT     ? (struct rect){0, d.y, half, top}
         : to == SNAP_TOP_RIGHT    ? (struct rect){half, d.y, d.width - half, top}
         : to == SNAP_BOTTOM_LEFT  ? (struct rect){0, d.y + top, half, d.height - top}
         : to == SNAP_BOTTOM_RIGHT ? (struct rect){half, d.y + top, d.width - half, d.height - top}
                            : d;
}

// What logging out, rebooting and shutting down wait for windows to close for.
enum { ITEM_LOG_OUT, ITEM_REBOOT, ITEM_SHUT_DOWN };

// In the middle of the desktop, with every window.
static struct rect switcher(void) {
    int height = window_count * ITEM_HEIGHT + 8;

    return (struct rect){(screen.width - SWITCHER_WIDTH) / 2, desktop().y + (desktop().height - height) / 2, SWITCHER_WIDTH,
                         height};
}

static struct window *switcher_window(int index) {
    return windows[window_count - 1 - index];
}

static bool contains(struct rect r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
}

// Windows on the workspace shown, and while it's locked, only the log-in screen's,
// which is on all of them.
static bool on_workspace(const struct window *w) {
    return w->bare || w->workspace == workspace;
}

static bool visible(const struct window *w) {
    return !w->minimized && on_workspace(w) && (!locked || w->login);
}

// Whether a window hides all of a rectangle: its frame does, but for the title
// bar, which shows a little of what's under it, and the rounded corners.
static bool covers(const struct window *w, struct rect r) {
    struct rect f = frame_of(w);
    int top = w->bare ? RADIUS : TITLE_HEIGHT + BORDER;

    return visible(w) && !see_through(w) && !w->anim_ms && r.x >= f.x && r.y >= f.y + top && r.x + r.width <= f.x + f.width
        && r.y + r.height <= f.y + f.height - RADIUS;
}

static bool overlaps(struct rect a, struct rect b) {
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

static long area(struct rect r) {
    return (long)r.width * r.height;
}

static struct rect bounds(struct rect a, struct rect b) {
    int x = a.x < b.x ? a.x : b.x, y = a.y < b.y ? a.y : b.y;
    int right = a.x + a.width > b.x + b.width ? a.x + a.width : b.x + b.width;
    int bottom = a.y + a.height > b.y + b.height ? a.y + a.height : b.y + b.height;

    return (struct rect){x, y, right - x, bottom - y};
}

// Clipped to the screen. When the list is full, everything merges into one.
static void add_damage(struct rect r) {
    if (r.x < 0) {
        r.width += r.x;
        r.x = 0;
    }
    if (r.y < 0) {
        r.height += r.y;
        r.y = 0;
    }
    if (r.width > screen.width - r.x)
        r.width = screen.width - r.x;
    if (r.height > screen.height - r.y)
        r.height = screen.height - r.y;
    if (r.width <= 0 || r.height <= 0)
        return;

    // A rectangle that overlaps another goes in with it when the two together are
    // no bigger than they are apart, as when a window moves a little, so that the
    // overlap isn't drawn twice. Merging can make more overlap, so it goes round.
    for (int i = 0; i < damage_count; i++) {
        struct rect both = bounds(damage[i], r);

        if (overlaps(damage[i], r) && area(both) <= area(damage[i]) + area(r)) {
            r = both;
            damage[i] = damage[--damage_count];
            i = -1;
        }
    }
    if (damage_count == MAX_DAMAGE) {
        for (int i = 1; i < damage_count; i++)
            damage[0] = bounds(damage[0], damage[i]);
        damage_count = 1;
        damage[0] = bounds(damage[0], r);
        return;
    }
    damage[damage_count++] = r;
}

static void damage_title(const struct window *w) {
    if (w)
        add_damage((struct rect){w->x, w->y - TITLE_HEIGHT, w->canvas.width, TITLE_HEIGHT});
}

static void damage_taskbar(void) {
    add_damage(taskbar());
}

static struct rect cursor_rect(void) {
    const struct cursor_shape *c = &shapes[cursor_shown];
    int width = c->turned ? c->height : c->width, height = c->turned ? c->width : c->height;

    return (struct rect){cursor_x - c->hot_x, cursor_y - c->hot_y, width, height};
}

// A pixel more all round, as a virtual machine's window that smooths the screen
// while scaling it blends each pixel into the next, and only redraws what changed.
// A dragged file's icon, by the cursor.
static struct rect drag_rect(void) {
    return (struct rect){cursor_x + 10, cursor_y + 14, ICON_SIZE, ICON_SIZE};
}

static void damage_cursor(void) {
    struct rect r = cursor_rect();

    add_damage((struct rect){r.x - 1, r.y - 1, r.width + 2, r.height + 2});
    if (drag_file[0])
        add_damage(drag_rect());
}

// How dark a shadow is by how far left or right, and up or down, a point is from
// the frame: (1 - d^2 / SHADOW^2)^2, which falls smoothly to nothing without
// needing a square root.
static void make_falloff(void) {
    for (int dx = 0; dx <= SHADOW; dx++) {
        for (int dy = 0; dy <= SHADOW; dy++) {
            long t = SHADOW * SHADOW - dx * dx - dy * dy;

            falloff[dx][dy] = t > 0 ? SHADOW_ALPHA * t * t / (SHADOW * SHADOW * SHADOW * SHADOW) : 0;
        }
    }
}

// Darkens what's under a window's shadow, outside its frame, by blending black over
// it, in the display's pixels.
static void draw_shadow(struct gui_canvas *c, const struct window *w, int ox, int oy) {
    struct rect f = scaled(frame_of(w)), e = scaled(extent_of(w)), s = {f.x, f.y + SHADOW_DROP * scale, f.width, f.height};
    struct rect area = scaled((struct rect){ox, oy, c->width, c->height});
    int x0 = e.x > area.x ? e.x : area.x, x1 = e.x + e.width < area.x + area.width ? e.x + e.width : area.x + area.width;
    int y0 = e.y > area.y ? e.y : area.y, y1 = e.y + e.height < area.y + area.height ? e.y + e.height : area.y + area.height;
    int radius = RADIUS * scale, reach = SHADOW * scale;

    for (int y = y0; y < y1; y++) {
        uint32_t *row = c->pixels + (long)(y - area.y) * c->stride - area.x;
        int dy = y < s.y ? s.y - y : y >= s.y + s.height ? y - (s.y + s.height - 1) : 0;
        bool beside_frame = y >= f.y && y < f.y + f.height;

        bool corner_row = y < f.y + radius || y >= f.y + f.height - radius;

        for (int x = x0; x < x1; x++) {
            // The window covers this part, but for its rounded corners.
            if (beside_frame && x >= f.x && x < f.x + f.width && (!corner_row || (x >= f.x + radius && x < f.x + f.width - radius))) {
                x = corner_row ? f.x + f.width - radius - 1 : f.x + f.width - 1;
                continue;
            }

            int dx = x < s.x ? s.x - x : x >= s.x + s.width ? x - (s.x + s.width - 1) : 0;
            uint32_t a = dx > reach || dy > reach ? 0 : falloff[dx / scale][dy / scale] * w->shadow >> 8;

            // In a rounded corner, only what's outside the curve has the shadow.
            if (beside_frame && x >= f.x && x < f.x + f.width) {
                int cx = x - f.x < f.x + f.width - 1 - x ? x - f.x : f.x + f.width - 1 - x;
                int cy = y - f.y < f.y + f.height - 1 - y ? y - f.y : f.y + f.height - 1 - y;

                a = a * (256 - coverage[cy][cx]) >> 8;
            }

            uint32_t keep = 256 - a;
            uint32_t p = row[x];

            // Red and blue together, then green, each kept by keep/256.
            if (a)
                row[x] = ((p & 0xFF00FF) * keep >> 8 & 0xFF00FF) | ((p & 0x00FF00) * keep >> 8 & 0x00FF00);
        }
    }
}

// An app's icon, loaded the first time it's wanted, or the plain program icon: at
// 2x, the one drawn for it, or else the 1x one, drawn twice the size.
static const struct gui_image *app_icon(const char *name) {
    char path[sizeof(ICONS) + 16 + GUI_TITLE_MAX];
    struct gui_image *image;

    for (int i = 0; i < icon_count; i++) {
        if (!strcmp(icons[i].name, name))
            return icons[i].image.pixels ? &icons[i].image : name[0] ? app_icon("") : NULL;
    }
    if (icon_count == MAX_ICONS)
        return NULL;
    snprintf(icons[icon_count].name, sizeof(icons[icon_count].name), "%s", name);
    image = &icons[icon_count++].image;
    for (int at = scale > 1 ? scale : 1; at >= 1 && !image->pixels; at--) {
        const char *suffix = at > 1 ? "@2x" : "";

        if (name[0])
            snprintf(path, sizeof(path), ICONS "/apps/%s%s.png", name, suffix);
        else
            snprintf(path, sizeof(path), ICONS "/program%s.png", suffix);
        if (gui_image_load(path, image))
            image->scale = at;
    }
    return app_icon(name);
}

// When the scale changes, they're loaded again for it.
static void forget_icons(void) {
    for (int i = 0; i < icon_count; i++)
        gui_image_free(&icons[i].image);
    icon_count = 0;
}

static void make_coverage(void) {
    for (int y = 0; y < RADIUS * scale; y++) {
        for (int x = 0; x < RADIUS * scale; x++)
            coverage[y][x] = gui_corner_coverage(x, y, RADIUS * scale);
    }
}

// A rectangle's corners, kept before it's drawn, or rounded after, on a canvas
// that's the part of the screen from ox, oy.
static void keep_corners(struct gui_canvas *c, struct rect r, int ox, int oy, uint32_t kept[4][GUI_KEPT][GUI_KEPT]) {
    gui_keep_corners(c, r.x - ox, r.y - oy, r.width, r.height, RADIUS, kept);
}

static void round_corners(struct gui_canvas *c, struct rect r, int ox, int oy, uint32_t kept[4][GUI_KEPT][GUI_KEPT],
                          uint32_t line) {
    gui_round_corners(c, r.x - ox, r.y - oy, r.width, r.height, RADIUS, kept, line);
}

// A colour over what's there, alpha of it out of 256.
static void blend_fill(struct gui_canvas *c, int x, int y, int width, int height, uint32_t color, int alpha) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + width > c->width ? c->width : x + width, y1 = y + height > c->height ? c->height : y + height;

    for (int row = y0 * scale; row < y1 * scale; row++) {
        uint32_t *p = c->pixels + (long)row * c->stride;

        for (int col = x0 * scale; col < x1 * scale; col++)
            p[col] = gui_mix(p[col], color, alpha);
    }
}

// See-through things blur what's under them by this much. Each blur reads that far
// around it, so the screen's drawn this far around what changed, for a few of them
// over each other: a submenu over a menu over a title bar.
#define BLUR 4
#define BLUR_MARGIN (4 * BLUR)

// How much of the taskbar and title bars' colour covers what's under them.
static int bar_alpha(void) {
    return settings.transparency ? BAR_ALPHA : 256;
}

// The average of taps pixels, from sums of red and blue together, and green, which
// stay apart as each is under 2^16 for up to 257 taps.
static uint32_t average(uint32_t rb, uint32_t g, int taps) {
    return ((rb >> 16) / taps) << 16 | ((rb & 0xFFFF) / taps) | ((g / taps) & 0x00FF00);
}

// Part of a canvas, each pixel the average of those up to BLUR of the desktop's
// pixels away, across then down, with sums that move along, so it costs as much
// however far it reaches. In the display's pixels.
static void blur(struct gui_canvas *c, int x, int y, int width, int height) {
    if (!settings.blur || !settings.transparency)
        return;
    static uint32_t *across;
    static size_t size;
    int reach = BLUR * scale, taps = 2 * reach + 1, cw = c->width * scale, ch = c->height * scale;
    int x0 = x < 0 ? 0 : x * scale, x1 = x + width > c->width ? cw : (x + width) * scale;
    int y0 = y < 0 ? 0 : y * scale, y1 = y + height > c->height ? ch : (y + height) * scale;
    int top = y0 - reach < 0 ? 0 : y0 - reach, bottom = y1 + reach > ch ? ch : y1 + reach;
    int w = x1 - x0;
    size_t needed = (size_t)w * (bottom - top);

    if (w <= 0 || y1 <= y0)
        return;
    if (needed > size) {
        uint32_t *bigger = realloc(across, needed * sizeof(*across));

        if (!bigger)
            return;
        across = bigger, size = needed;
    }
    for (int row = top; row < bottom; row++) {
        const uint32_t *p = c->pixels + (long)row * c->stride;
        uint32_t *out = across + (long)(row - top) * w - x0;
        uint32_t rb = 0, g = 0;

        for (int k = x0 - reach; k <= x0 + reach; k++) {
            uint32_t q = p[k < 0 ? 0 : k >= cw ? cw - 1 : k];

            rb += q & 0xFF00FF, g += q & 0x00FF00;
        }
        for (int col = x0; col < x1; col++) {
            uint32_t in = p[col + reach + 1 >= cw ? cw - 1 : col + reach + 1], gone = p[col - reach < 0 ? 0 : col - reach];

            out[col] = average(rb, g, taps);
            rb += (in & 0xFF00FF) - (gone & 0xFF00FF), g += (in & 0x00FF00) - (gone & 0x00FF00);
        }
    }
    for (int col = x0; col < x1; col++) {
        const uint32_t *q = across + col - x0;
        uint32_t rb = 0, g = 0;

        for (int k = y0 - reach; k <= y0 + reach; k++) {
            uint32_t v = q[(long)((k < top ? top : k >= bottom ? bottom - 1 : k) - top) * w];

            rb += v & 0xFF00FF, g += v & 0x00FF00;
        }
        for (int row = y0; row < y1; row++) {
            int next = row + reach + 1 >= bottom ? bottom - 1 : row + reach + 1, last = row - reach < top ? top : row - reach;
            uint32_t in = q[(long)(next - top) * w], gone = q[(long)(last - top) * w];

            c->pixels[(long)row * c->stride + col] = average(rb, g, taps);
            rb += (in & 0xFF00FF) - (gone & 0xFF00FF), g += (in & 0x00FF00) - (gone & 0x00FF00);
        }
    }
}

static void draw_icon(struct gui_canvas *c, const char *app, int x, int y, int ox, int oy) {
    const struct gui_image *icon = app_icon(app);

    if (icon)
        gui_draw_image(c, x - ox, y - oy, icon);
}

// While a window's copied to its layer for an animation, it's drawn plainly: its
// corners square and nothing blurred, as the layer has nothing under it.
static bool drawing_layer;

// A window's pixels. A see-through window's blur what's under them, and show it as
// much as their top bytes say.
static void draw_contents(struct gui_canvas *c, const struct window *w, int ox, int oy) {
    int x = w->x - ox, y = w->y - oy;
    int x0 = x < 0 ? 0 : x, x1 = x + w->canvas.width > c->width ? c->width : x + w->canvas.width;
    int y0 = y < 0 ? 0 : y, y1 = y + w->canvas.height > c->height ? c->height : y + w->canvas.height;

    if (!see_through(w) || drawing_layer) {
        gui_copy(c, x, y, &w->canvas, 0, 0, w->canvas.width, w->canvas.height);
        return;
    }
    blur(c, x, y, w->canvas.width, w->canvas.height);
    for (int row = y0 * scale; row < y1 * scale; row++) {
        const uint32_t *from = w->canvas.pixels + (long)(row - y * scale) * w->canvas.stride - x * scale;
        uint32_t *to = c->pixels + (long)row * c->stride;

        for (int col = x0 * scale; col < x1 * scale; col++) {
            uint32_t p = from[col];

            to[col] = p >> 24 ? gui_mix(p, to[col], p >> 24) : p;
        }
    }
}

// Windows have rounded corners, but for those maximized or tiled, which meet the
// edges of the screen or each other.
static bool rounded(const struct window *w) {
    return !w->tiled;
}

static void draw_window(struct gui_canvas *c, const struct window *w, int ox, int oy) {
    uint32_t kept[4][GUI_KEPT][GUI_KEPT];
    struct rect frame = frame_of(w);
    bool round = rounded(w) && !drawing_layer;

    if (round)
        keep_corners(c, frame, ox, oy, kept);
    if (w->bare) {
        draw_contents(c, w, ox, oy);
        round_corners(c, frame, ox, oy, kept, GUI_NONE);
        return;
    }

    struct rect close = button_of(w, 0), maximize = button_of(w, 1), minimize = button_of(w, 2);
    uint32_t title_color = w == focused ? TITLE_FOCUSED : TITLE_UNFOCUSED;
    int text_y = w->y - TITLE_HEIGHT + (TITLE_HEIGHT - GUI_CHAR_HEIGHT) / 2 - oy;

    // Only the border's edges, as the title bar and contents cover the rest.
    gui_fill(c, frame.x - ox, frame.y - oy, frame.width, BORDER, BORDER_COLOR);
    gui_fill(c, frame.x - ox, frame.y + frame.height - BORDER - oy, frame.width, BORDER, BORDER_COLOR);
    gui_fill(c, frame.x - ox, frame.y - oy, BORDER, frame.height, BORDER_COLOR);
    gui_fill(c, frame.x + frame.width - BORDER - ox, frame.y - oy, BORDER, frame.height, BORDER_COLOR);
    if (!drawing_layer)
        blur(c, w->x - ox, w->y - TITLE_HEIGHT - oy, w->canvas.width, TITLE_HEIGHT);
    blend_fill(c, w->x - ox, w->y - TITLE_HEIGHT - oy, w->canvas.width, TITLE_HEIGHT, title_color,
               drawing_layer ? 256 : bar_alpha());
    draw_icon(c, w->app, w->x + 5, w->y - TITLE_HEIGHT + (TITLE_HEIGHT - ICON_SIZE) / 2, ox, oy);

    // A title too long for the bar stops short of the buttons.
    struct gui_canvas title = *c;
    int right = minimize.x - 4 - ox;

    title.width = right < 0 ? 0 : right < c->width ? right : c->width;
    gui_text(&title, w->x + 10 + ICON_SIZE - ox, text_y, w->title, w == focused ? ON_ACCENT : TITLE_TEXT_UNFOCUSED, 1);
    gui_fill_rounded(c, close.x - ox, close.y - oy, BUTTON_SIZE, BUTTON_SIZE, SMALL_RADIUS, CLOSE_COLOR);
    gui_text(c, close.x + (BUTTON_SIZE - gui_text_width("x", 1)) / 2 - ox, text_y, "x", TITLE_TEXT, 1);
    gui_fill_rounded(c, maximize.x - ox, maximize.y - oy, BUTTON_SIZE, BUTTON_SIZE, SMALL_RADIUS, BUTTON_COLOR);
    gui_fill(c, maximize.x + 4 - ox, maximize.y + 4 - oy, 8, 8, TITLE_TEXT);
    gui_fill(c, maximize.x + 5 - ox, maximize.y + 6 - oy, 6, 5, BUTTON_COLOR);
    gui_fill_rounded(c, minimize.x - ox, minimize.y - oy, BUTTON_SIZE, BUTTON_SIZE, SMALL_RADIUS, BUTTON_COLOR);
    gui_fill(c, minimize.x + 4 - ox, minimize.y + 11 - oy, 8, 2, TITLE_TEXT);
    draw_contents(c, w, ox, oy);
    if (round)
        round_corners(c, frame, ox, oy, kept, BORDER_COLOR);
}

// The workspace's windows in the order they were made, which is how the taskbar
// shows them.
static int windows_by_age(struct window *out[MAX_WINDOWS]) {
    int count = 0;

    for (int i = 0; i < window_count; i++) {
        if (!on_workspace(windows[i]))
            continue;

        int j = count++;

        while (j > 0 && out[j - 1]->id > windows[i]->id) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = windows[i];
    }
    return count;
}

static struct rect tray(void);

static struct rect taskbar_button(int index, int count) {
    int start = workspace_box(WORKSPACES).x + 8, room = tray().x - 8 - start;
    int width = count && room / count < TASKBAR_BUTTON_WIDTH ? room / count : TASKBAR_BUTTON_WIDTH;

    return (struct rect){start + index * width, taskbar().y + 4, width - 4, TASKBAR_HEIGHT - 8};
}

// Where the taskbar's buttons are, when that changes, for the tests to find them.
static void report_taskbar(void) {
    static char said[4096];
    char now[4096] = "";
    struct window *list[MAX_WINDOWS];
    int count = windows_by_age(list), used = 0;

    for (int i = 0; i < count && used < (int)sizeof(now) - 160; i++) {
        struct rect b = taskbar_button(i, count);

        used += snprintf(now + used, sizeof(now) - used, "wm: taskbar button %d at %d %d %d %d, %.60s\n", i, b.x, b.y,
                         b.width, b.height, list[i]->title);
    }
    if (strcmp(now, said)) {
        fputs(now, stdout);
        memcpy(said, now, sizeof(said));
    }
}

// Text cut to fit a width, which is how titles go on the taskbar.
static void fitted_text(struct gui_canvas *c, int x, int y, int width, const char *text, uint32_t color) {
    char shown[GUI_TITLE_MAX];

    snprintf(shown, sizeof(shown), "%.*s", width > 0 ? (int)gui_text_fit(text, strlen(text), width) : 0, text);
    gui_text(c, x, y, shown, color, 1);
}

// Menus, the tray's panels, notifications and the switcher, with rounded corners
// like windows'.
// The taskbar, its flyouts, notifications and the switcher: rounded, outlined and see-through, blurring what's under them.
static void draw_glass(struct gui_canvas *c, struct rect r, int ox, int oy) {
    uint32_t kept[4][GUI_KEPT][GUI_KEPT];
    int x = r.x - ox, y = r.y - oy;

    keep_corners(c, r, ox, oy, kept);
    blur(c, x, y, r.width, r.height);
    blend_fill(c, x, y, r.width, r.height, TASKBAR_COLOR, bar_alpha());
    gui_fill(c, x, y, r.width, 1, TASKBAR_LINE);
    gui_fill(c, x, y + r.height - 1, r.width, 1, TASKBAR_LINE);
    gui_fill(c, x, y, 1, r.height, TASKBAR_LINE);
    gui_fill(c, x + r.width - 1, y, 1, r.height, TASKBAR_LINE);
    round_corners(c, r, ox, oy, kept, TASKBAR_LINE);
}

// 12 hour time goes without a leading zero, as people write it.
static const char *clock_text(char out[16]) {
    time_t now = time(NULL);

    strftime(out, 16, settings.clock_12_hour ? "%I:%M %p" : "%H:%M", localtime(&now));
    return out + (settings.clock_12_hour && out[0] == '0');
}

// Notifications stack up from the bottom right corner, the newest lowest, until
// they've been up NOTICE_MS or they're clicked. A new one from an app that has one
// up takes its place, counting them. The last HISTORY are kept for the tray, newest
// first, with how many came since it was last looked at. With do not disturb on,
// they only go there.
#define MAX_NOTICES 4
#define NOTICE_MS 5000
#define NOTICE_WIDTH 300
#define NOTICE_HEIGHT 52
#define HISTORY 6
struct notice {
    char title[GUI_TITLE_MAX], text[GUI_TITLE_MAX], app[GUI_TITLE_MAX];
    char file[GUI_PATH_MAX]; // that clicking it opens, if there is one
    char label[16], command[GUI_PATH_MAX + 32]; // its button, if it has one, and what that runs
    char time[8];
    int count; // how many it stands for, from the same app
    int64_t until;
};
static struct notice notices[MAX_NOTICES], history[HISTORY];
static int notice_count, history_count, unseen;

static int64_t now_ms(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// Windows grow in from a little smaller as they fade in, and shrink as they fade
// out, or fly to and from their taskbar buttons when minimized and restored.
#define OPEN_MS 180
#define CLOSE_MS 120
#define MINIMIZE_MS 200
#define MAXIMIZE_MS 200
#define ANIM_SCALE 240 // out of 256, the size windows start opening and finish closing at

// A window going away, closed or minimized, drawn from a copy of how it looked.
struct ghost {
    struct gui_canvas layer;
    bool rounded, translucent;
    int shadow, ms;
    int64_t start;
    struct rect from, to;
};
static struct ghost ghosts[MAX_WINDOWS];
static int ghost_count;

// How far through an animation it is, out of 256, and that eased: quickly then
// slowing for what arrives, and slowly then quickly for what goes.
static int progress(int64_t start, int ms) {
    int64_t t = (now_ms() - start) * 256 / ms;

    return t < 0 ? 0 : t > 256 ? 256 : (int)t;
}

static int ease_out(int t) {
    return 256 - (256 - t) * (256 - t) * (256 - t) / 65536;
}

static int ease_in(int t) {
    return t * t * t / 65536;
}

static struct rect between(struct rect a, struct rect b, int t) {
    return (struct rect){a.x + (b.x - a.x) * t / 256, a.y + (b.y - a.y) * t / 256, a.width + (b.width - a.width) * t / 256,
                         a.height + (b.height - a.height) * t / 256};
}

// A frame at ANIM_SCALE of its size, about its middle.
static struct rect shrunk(struct rect r) {
    int width = r.width * ANIM_SCALE / 256, height = r.height * ANIM_SCALE / 256;

    return (struct rect){r.x + (r.width - width) / 2, r.y + (r.height - height) / 2, width, height};
}

static struct rect with_shadow(struct rect r) {
    return (struct rect){r.x - SHADOW, r.y - SHADOW, r.width + 2 * SHADOW, r.height + 2 * SHADOW};
}

// Where a window's button is on the taskbar.
static struct rect button_for(const struct window *w) {
    struct window *list[MAX_WINDOWS];
    int count = windows_by_age(list);

    for (int i = 0; i < count; i++) {
        if (list[i] == w)
            return taskbar_button(i, count);
    }
    return taskbar();
}

// Draws a window, at full size, into a layer the size of its frame.
static bool fill_layer(struct gui_canvas *layer, const struct window *w) {
    struct rect f = frame_of(w);

    if (layer->width != f.width || layer->height != f.height || layer->scale != scale) {
        uint32_t *pixels = realloc(layer->pixels, (size_t)f.width * f.height * scale * scale * sizeof(uint32_t));

        if (!pixels)
            return false;
        *layer = (struct gui_canvas){pixels, f.width, f.height, f.width * scale, scale};
    }
    drawing_layer = true;
    draw_window(layer, w, f.x, f.y);
    drawing_layer = false;
    return true;
}

// A layer drawn into r, alpha of it out of 256, with its shadow, its corners
// rounded, and see-through where its pixels say if it's translucent.
static void draw_layer(struct gui_canvas *c, const struct gui_canvas *layer, struct rect r, int alpha, int shadow,
                       bool round, bool translucent, int ox, int oy) {
    int x0 = r.x - ox < 0 ? 0 : r.x - ox, x1 = r.x + r.width - ox > c->width ? c->width : r.x + r.width - ox;
    int y0 = r.y - oy < 0 ? 0 : r.y - oy, y1 = r.y + r.height - oy > c->height ? c->height : r.y + r.height - oy;
    int lw = layer->width * scale, lh = layer->height * scale, radius = RADIUS * scale;

    if (r.width <= 0 || r.height <= 0)
        return;
    if (settings.shadows && r.width > 4 * RADIUS && r.height > 4 * RADIUS) {
        struct window outline = {.x = r.x + BORDER, .y = r.y + TITLE_HEIGHT + BORDER, .shadow = shadow * alpha / 256};

        outline.canvas.width = r.width - 2 * BORDER;
        outline.canvas.height = r.height - TITLE_HEIGHT - 2 * BORDER;
        draw_shadow(c, &outline, ox, oy);
    }
    // In the display's pixels, each from the nearest of the layer's.
    for (int y = y0 * scale; y < y1 * scale; y++) {
        int ly = (y + (oy - r.y) * scale) * layer->height / r.height, cy = ly < lh - 1 - ly ? ly : lh - 1 - ly;
        const uint32_t *from = layer->pixels + (long)ly * layer->stride;
        uint32_t *to = c->pixels + (long)y * c->stride;

        for (int x = x0 * scale; x < x1 * scale; x++) {
            int lx = (x + (ox - r.x) * scale) * layer->width / r.width, cx = lx < lw - 1 - lx ? lx : lw - 1 - lx;
            uint32_t p = from[lx];
            int a = translucent ? alpha * (256 - (int)(p >> 24)) / 256 : alpha;

            if (round && cx < radius && cy < radius)
                a = a * coverage[cy][cx] / 256;
            to[x] = gui_mix(to[x], p, a);
        }
    }
}

// Where an opening or restored window is now, and how much of it shows.
// The frame a window's going to have, at the size it's been asked to be, which
// its program may not have answered yet.
static struct rect wanted_frame(const struct window *w) {
    struct rect f = frame_of(w);

    return (struct rect){f.x, f.y, f.width + w->want_width - w->canvas.width, f.height + w->want_height - w->canvas.height};
}

static struct rect arriving(const struct window *w, int *alpha) {
    int t = ease_out(progress(w->anim_start, w->anim_ms));

    *alpha = w->anim_fade ? t : 256;
    return between(w->anim_from, wanted_frame(w), t);
}

// All that a window covers, as it's drawn now.
static struct rect drawn_extent(const struct window *w) {
    if (!w->anim_ms)
        return extent_of(w);
    return bounds(with_shadow(w->anim_from), bounds(extent_of(w), with_shadow(wanted_frame(w))));
}

static void arrive(struct window *w, struct rect from, int ms) {
    if (!settings.animations || w->bare)
        return;
    // What the animation it replaces drew, between where that went from and to,
    // is drawn over.
    if (w->anim_ms)
        add_damage(bounds(with_shadow(w->anim_from), with_shadow(w->anim_to)));
    w->anim_from = from;
    w->anim_to = wanted_frame(w);
    w->anim_ms = ms;
    w->anim_fade = true;
    w->anim_start = now_ms();
}

// Moves a window to a new frame from where it was, as maximizing and tiling do.
static void reframe(struct window *w, struct rect from) {
    int alpha;

    // From where it's got to, if it was still on the way somewhere.
    if (w->anim_ms && settings.animations)
        from = arriving(w, &alpha);
    arrive(w, from, MAXIMIZE_MS);
    w->anim_fade = false;
}

// Leaves a copy of a window behind, going from its frame to a rectangle.
static void depart(const struct window *w, struct rect to, int ms) {
    struct ghost *g = &ghosts[ghost_count];

    if (!settings.animations || w->bare || !visible(w) || ghost_count == MAX_WINDOWS)
        return;
    *g = (struct ghost){.rounded = rounded(w), .translucent = see_through(w), .shadow = w->shadow, .ms = ms,
                        .start = now_ms(), .from = frame_of(w), .to = to};
    if (fill_layer(&g->layer, w))
        ghost_count++;
}

static void draw_ghosts(struct gui_canvas *c, int ox, int oy) {
    for (int i = 0; i < ghost_count; i++) {
        const struct ghost *g = &ghosts[i];
        int t = ease_in(progress(g->start, g->ms));

        draw_layer(c, &g->layer, between(g->from, g->to, t), 256 - t, g->shadow, g->rounded, g->translucent, ox, oy);
    }
}

// Moves each animation on a frame, returning whether any has further to go.
static bool animate(void) {
    bool going = false;

    for (int i = 0; i < window_count; i++) {
        struct window *w = windows[i];

        if (!w->anim_ms)
            continue;
        w->anim_to = wanted_frame(w);
        add_damage(drawn_extent(w));
        if (progress(w->anim_start, w->anim_ms) < 256 && fill_layer(&w->layer, w))
            going = true;
        else
            w->anim_ms = 0;
    }
    for (int i = 0; i < ghost_count; i++) {
        struct ghost *g = &ghosts[i];

        add_damage(bounds(with_shadow(g->from), with_shadow(g->to)));
        if (progress(g->start, g->ms) < 256) {
            going = true;
            continue;
        }
        free(g->layer.pixels);
        *g-- = ghosts[--ghost_count];
    }
    return going;
}

static struct rect notice_rect(int index) {
    int from_bottom = notice_count - index;

    int bottom = settings.taskbar_top ? screen.height : taskbar().y;

    return (struct rect){screen.width - NOTICE_WIDTH - GAP, bottom - from_bottom * (NOTICE_HEIGHT + GAP), NOTICE_WIDTH,
                         NOTICE_HEIGHT};
}

static void damage_notices(void) {
    for (int i = 0; i < notice_count; i++)
        add_damage(notice_rect(i));
}

static void remove_notice(int index) {
    damage_notices();
    memmove(notices + index, notices + index + 1, (notice_count - index - 1) * sizeof(*notices));
    notice_count--;
}

// Takes away those whose time is up, returning how long until the next one's is.
static int expire_notices(void) {
    int64_t now = now_ms(), next = -1;

    for (int i = notice_count - 1; i >= 0; i--) {
        if (notices[i].until <= now)
            remove_notice(i);
    }
    for (int i = 0; i < notice_count; i++)
        next = next < 0 || notices[i].until - now < next ? notices[i].until - now : next;
    return (int)next;
}

// A notification's button, at its right.
static struct rect notice_button(int index) {
    struct rect r = notice_rect(index);
    int width = gui_text_width(notices[index].label, 1) + 16;

    return (struct rect){r.x + r.width - 12 - width, r.y + (r.height - 22) / 2, width, 22};
}

static int notice_at(int x, int y) {
    for (int i = 0; i < notice_count; i++) {
        if (contains(notice_rect(i), x, y))
            return i;
    }
    return -1;
}

static void draw_notice(struct gui_canvas *c, struct rect r, const struct notice *n, int ox, int oy) {
    int text_x = r.x + 12 + (n->app[0] ? ICON_SIZE + 10 : 0), width = r.x + r.width - 12 - text_x;

    if (n->app[0])
        draw_icon(c, n->app, r.x + 12, r.y + 10, ox, oy);
    fitted_text(c, text_x - ox, r.y + 8 - oy, width, n->title, TASKBAR_TEXT);
    fitted_text(c, text_x - ox, r.y + 8 + GUI_CHAR_HEIGHT + 4 - oy, width, n->text, TASKBAR_DIM_TEXT);
}

static void draw_notices(struct gui_canvas *c, int ox, int oy) {
    for (int i = 0; i < notice_count; i++) {
        struct rect r = notice_rect(i), text = r, b = notice_button(i);
        struct notice shown = notices[i];

        draw_glass(c, r, ox, oy);
        gui_fill_rounded(c, r.x + 3 - ox, r.y + RADIUS - oy, 3, r.height - 2 * RADIUS, GUI_MAX_RADIUS, TITLE_FOCUSED);
        if (shown.count > 1)
            snprintf(shown.title, sizeof(shown.title), "%.50s (%d)", notices[i].title, shown.count);
        if (shown.label[0]) {
            text.width = b.x - r.x;
            gui_fill_rounded(c, b.x - ox, b.y - oy, b.width, b.height, SMALL_RADIUS,
                             contains(b, cursor_x, cursor_y) ? TITLE_FOCUSED : TASKBAR_BUTTON);
            gui_text(c, b.x + 8 - ox, b.y + (b.height - GUI_CHAR_HEIGHT) / 2 - oy, shown.label,
                     contains(b, cursor_x, cursor_y) ? ON_ACCENT : TASKBAR_TEXT, 1);
        }
        draw_notice(c, text, &shown, ox, oy);
    }
}

// The tray, left of the clock: the clipboard's history, Bluetooth, with an adapter,
// the network, quick settings, shown by the battery's and the speaker's icons, and
// notifications, each opening a panel by it. The clock opens a calendar.
enum { TRAY_QUICK, TRAY_NOTICES, TRAY_CLIPS, TRAY_NETWORK, TRAY_BLUETOOTH, TRAY_ITEMS, TRAY_CALENDAR = TRAY_ITEMS };
enum { TILE_AIRPLANE, TILE_NIGHT, TILE_DARK, TILE_QUIET, TILES };
#define DAY_WIDTH 32
#define DAY_HEIGHT 24
#define TRAY_ICON 24
#define PANEL_LINE 20
#define CLIP_ROW 24
#define QUICK_WIDTH 400
#define TILE_HEIGHT 52
#define VOLUME_STEP 5
#define NET_CHECK_MS 5000 // how often the battery and the volume are looked at
static int panel = -1; // the one open
static int64_t panel_opened, menu_opened; // when they did, to fade them in
static void autohide(void);

// What the player's playing, which it keeps in NOW_PLAYING, shown under the volume.
#define NOW_PLAYING "/tmp/now-playing"
static char now_playing[GUI_TITLE_MAX];
static int mixer = -1, volume = 100; // mixer's 0 with a sound card
static bool muted, setting_volume;
static const struct network *net; // as NetworkManager has it
static const struct bluetooth *bt; // as BlueZ has it
static int calendar_month; // shown, from this one

// The laptop's battery and backlight, from /sys/class, and the popup their keys
// and the volume's show.
#define POWER_DIR "/sys/class/power_supply"
#define BACKLIGHT_DIR "/sys/class/backlight"
#define BRIGHTNESS_STEP 5
#define OSD_MS 1200
#define SUSPEND_WAIT_MS 500 // for the lock screen to be drawn first
#define BATTERY_WIDTH 52    // in the tray: its icon, and how much is left
static char backlight[GUI_PATH_MAX]; // its folder, or "" without one
static int brightness, brightness_max = 1; // as the backlight has them
static int battery = -1, warned = 100;    // percent left, or -1 without one, and the last warning's
static bool charging, setting_brightness;
static int64_t osd_until, suspend_due = -1; // when the popup goes, and when the computer's to suspend
static bool osd_volume;                    // whether the popup's the volume's, or the brightness's
static void lock(void);
static bool can_lock(void);

static bool connected(void) {
    return net->address != 0;
}

// Quick settings has the battery, with one, and the speaker, with a sound card, or
// the theme's icon with neither.
static struct rect tray_item(int item) {
    char clock[16];
    int width = gui_text_width(clock_text(clock), 1), right = taskbar().x + taskbar().width - width - 12 - 8;
    int y = taskbar().y + 4, quick = (mixer >= 0 ? TRAY_ICON : 0) + (battery >= 0 ? BATTERY_WIDTH : 0);
    int quick_x = right - TRAY_ICON - (quick ? quick : TRAY_ICON) - 2;
    int bt_width = bt->adapter ? TRAY_ICON + 2 : 0;

    if (item == TRAY_CALENDAR)
        return (struct rect){right + 2, y, width + 16, TASKBAR_HEIGHT - 8};
    if (item == TRAY_NOTICES)
        return (struct rect){right - TRAY_ICON, y, TRAY_ICON, TASKBAR_HEIGHT - 8};
    if (item == TRAY_NETWORK)
        return (struct rect){quick_x - 2 - TRAY_ICON, y, TRAY_ICON, TASKBAR_HEIGHT - 8};
    if (item == TRAY_BLUETOOTH)
        return (struct rect){quick_x - 2 - TRAY_ICON - bt_width, y, bt_width ? TRAY_ICON : 0, TASKBAR_HEIGHT - 8};
    if (item == TRAY_CLIPS)
        return (struct rect){quick_x - 2 - TRAY_ICON - bt_width - 2 - TRAY_ICON, y, TRAY_ICON, TASKBAR_HEIGHT - 8};
    return (struct rect){quick_x, y, quick ? quick : TRAY_ICON, TASKBAR_HEIGHT - 8};
}

// Says where the network's and quick settings' icons are, for the tests.
static void report_tray(void) {
    struct rect q = tray_item(TRAY_QUICK), n = tray_item(TRAY_NETWORK);

    printf("wm: quick settings' icons at %d %d %d %d\n", q.x, q.y, q.width, q.height);
    printf("wm: network icon at %d %d %d %d\n", n.x, n.y, n.width, n.height);
    if (bt->adapter) {
        struct rect b = tray_item(TRAY_BLUETOOTH);

        printf("wm: bluetooth icon at %d %d %d %d\n", b.x, b.y, b.width, b.height);
    }
}

static struct rect tray(void) {
    struct rect first = tray_item(TRAY_CLIPS), last = tray_item(TRAY_NOTICES);

    return (struct rect){first.x, first.y, last.x + last.width - first.x, first.height};
}

static int tray_item_at(int x, int y) {
    for (int i = 0; i <= TRAY_CALENDAR; i++) {
        if (contains(tray_item(i), x, y))
            return i;
    }
    return -1;
}

// What can be played through, listed under the volume when there's a choice.
#define MAX_OUTPUTS 8
static struct audio_output outputs[MAX_OUTPUTS];
static int output_count;

static int outputs_height(void) {
    return output_count > 1 ? output_count * PANEL_LINE + 6 : 0;
}

static int sound_height(void) {
    return mixer < 0 ? 0 : 32 + (now_playing[0] ? 22 : 0) + outputs_height();
}

// The brightness, under the volume, then the battery.
static int laptop_height(void) {
    return (backlight[0] ? 32 : 0) + (battery >= 0 ? PANEL_LINE + 4 : 0);
}

// The Wi-Fi panel, by the network's icon: Wi-Fi's switch, the cable if there's one,
// the networks nearby, and a row for joining a hidden one. Clicking an open network
// joins it, and any other opens under its row, for its password, or to leave or
// forget it. A hidden network's row opens for its name and password.
#define NETWORK_WIDTH 320
#define WIFI_ROW 36
#define WIFI_LINE 34  // each line of what an opened row has under it
#define WIFI_SHOWN 8  // networks listed at most
#define SWITCH_WIDTH 36
static char wifi_opened[SSID_MAX];                 // the network whose row is open, or ""
static bool hidden_opened;                         // the hidden network's row is open
static char wifi_name[SSID_MAX], wifi_password[64]; // what's been typed there
static int wifi_typing;                            // into 0 nothing, 1 the hidden network's name, 2 the password
static char wifi_message[128];
enum { FIELD_NONE, FIELD_NAME, FIELD_PASSWORD };

static bool wifi_listed(void) {
    return net->running && net->wifi_card && net->wifi_on;
}

static int wifi_shown(void) {
    return !wifi_listed() ? 0 : net->wifi_count < WIFI_SHOWN ? net->wifi_count : WIFI_SHOWN;
}

// The open row's place in the list, which is wifi_shown() for the hidden network's, or -1.
static int wifi_open_index(void) {
    if (hidden_opened)
        return wifi_shown();
    for (int i = 0; wifi_opened[0] && i < wifi_shown(); i++) {
        if (!strcmp(net->wifi[i].ssid, wifi_opened))
            return i;
    }
    return -1;
}

static int wifi_open_height(void) {
    return wifi_open_index() < 0 ? 0 : (hidden_opened ? 2 : 1) * WIFI_LINE + 6;
}

static int wired_height(void) {
    return net->cable ? PANEL_LINE + 8 : 0;
}

// Why there's no list, when there isn't.
static const char *wifi_note(void) {
    return !net->running ? "NetworkManager isn't running" : !net->wifi_card ? "No Wi-Fi card" : !net->wifi_on ? "Wi-Fi is off"
                                                                                                             : NULL;
}

// The Bluetooth panel: a switch, then the devices, connected and paired ones first,
// then those found nearby while it's open. A device's row opens to what can be done
// with it, and pairing that needs a yes, or a code typed, says so under the list.
#define BT_SHOWN 8
static char bt_opened[64]; // the device whose row is open, by its path, or ""
static char bt_message[128];

static bool bt_listed(void) {
    return bt->running && bt->adapter && bt->powered;
}

static int bt_shown(void) {
    return !bt_listed() ? 0 : bt->count < BT_SHOWN ? bt->count : BT_SHOWN;
}

static int bt_open_index(void) {
    for (int i = 0; bt_opened[0] && i < bt_shown(); i++) {
        if (!strcmp(bt->devices[i].path, bt_opened))
            return i;
    }
    return -1;
}

static const char *bt_note(void) {
    return !bt->running ? "Bluetooth isn't running" : !bt->adapter ? "No Bluetooth adapter" : !bt->powered ? "Bluetooth is off"
         : !bt->count ? (bt->discovering ? "Looking for devices..." : "No devices found") : NULL;
}

// Pairing asking for a yes has buttons; a code to type on the device doesn't.
static bool bt_asks_yes(void) {
    return bluetooth_request()->asking == BT_CONFIRM || bluetooth_request()->asking == BT_AUTHORIZE;
}

static int bt_asking_height(void) {
    return !bluetooth_request()->asking ? 0 : 2 * PANEL_LINE + (bt_asks_yes() ? WIFI_LINE : 0) + 12;
}

static int bluetooth_height(void) {
    return 40 + (bt_note() ? PANEL_LINE + 8 : 0) + bt_shown() * WIFI_ROW + (bt_open_index() >= 0 ? WIFI_LINE + 6 : 0)
         + bt_asking_height() + (bt_message[0] ? PANEL_LINE + 4 : 0) + 6;
}

static int network_height(void) {
    return 40 + wired_height() + (wifi_note() ? PANEL_LINE + 8 : 0)
         + (wifi_listed() ? (wifi_shown() + 1) * WIFI_ROW + wifi_open_height() : 0) + (wifi_message[0] ? PANEL_LINE + 4 : 0)
         + 6;
}

static struct rect panel_rect(int item) {
    static const int widths[] = {QUICK_WIDTH, NOTICE_WIDTH, NOTICE_WIDTH, NETWORK_WIDTH, NETWORK_WIDTH, 7 * DAY_WIDTH + 24};
    int heights[] = {12 + TILE_HEIGHT + 12 + sound_height() + laptop_height() + 9 + 6 * PANEL_LINE + 8,
                     40 + (history_count ? history_count : 1) * (NOTICE_HEIGHT - 8),
                     40 + (clip_count ? clip_count : 1) * CLIP_ROW, network_height(), bluetooth_height(),
                     40 + 7 * DAY_HEIGHT};
    struct rect t = tray_item(item);
    int x = t.x + t.width - widths[item], most = screen.width - GAP - widths[item];

    x = x < most ? x : most;
    return (struct rect){x > GAP ? x : GAP, beside_bar(heights[item]), widths[item], heights[item]};
}

// The buttons across the top of quick settings.
static struct rect quick_tile(int tile) {
    struct rect p = panel_rect(TRAY_QUICK);
    int width = (p.width - 24 - (TILES - 1) * 8) / TILES;

    return (struct rect){p.x + 12 + tile * (width + 8), p.y + 12, width, TILE_HEIGHT};
}

// The speaker, which mutes, and the slider beside it, where it can be clicked,
// with the line it's drawn on through its middle.
static struct rect mute_button(void) {
    struct rect p = panel_rect(TRAY_QUICK);

    return (struct rect){p.x + 10, p.y + 12 + TILE_HEIGHT + 12, 24, 20};
}

static struct rect volume_slider(void) {
    struct rect p = panel_rect(TRAY_QUICK), m = mute_button();

    return (struct rect){m.x + m.width + 10, m.y, p.x + p.width - 56 - (m.x + m.width + 10), 20};
}

static struct rect output_row(int i) {
    struct rect p = panel_rect(TRAY_QUICK), m = mute_button();

    return (struct rect){p.x + 10, m.y + 32 + (now_playing[0] ? 22 : 0) + i * PANEL_LINE, p.width - 20, PANEL_LINE};
}

// The brightness's sun and slider, laid out as the volume's.
static struct rect sun_icon(void) {
    struct rect m = mute_button();

    return (struct rect){m.x, m.y + sound_height(), m.width, m.height};
}

static struct rect brightness_slider(void) {
    struct rect v = volume_slider();

    return (struct rect){v.x, sun_icon().y, v.width, v.height};
}

// The popup the keys show, over the middle of the bottom of the screen.
static struct rect osd_rect(void) {
    return (struct rect){(screen.width - 240) / 2, screen.height - TASKBAR_HEIGHT - 2 * GAP - 48, 240, 48};
}

// The calendar's arrows to the month before and after, either side of its name.
static struct rect month_arrow(bool next) {
    struct rect p = panel_rect(TRAY_CALENDAR);

    return (struct rect){next ? p.x + p.width - 8 - 24 : p.x + 8, p.y + 6, 24, 24};
}

static struct rect clear_button(int item) {
    struct rect p = panel_rect(item);

    return (struct rect){p.x + p.width - 64, p.y + 6, 56, 22};
}

static struct rect history_row(int index) {
    struct rect p = panel_rect(TRAY_NOTICES);

    return (struct rect){p.x + 4, p.y + 34 + index * (NOTICE_HEIGHT - 8), p.width - 8, NOTICE_HEIGHT - 8};
}

static struct rect clip_row(int index) {
    struct rect p = panel_rect(TRAY_CLIPS);

    return (struct rect){p.x + 4, p.y + 34 + index * CLIP_ROW, p.width - 8, CLIP_ROW};
}

static void damage_tray(void) {
    add_damage(tray());
    add_damage(tray_item(TRAY_CALENDAR));
    if (panel >= 0)
        add_damage(panel_rect(panel));
}

static void flyout_leaves(struct rect r);

static void close_panel(void) {
    if (panel >= 0)
        flyout_leaves(panel_rect(panel));
    if (panel == TRAY_BLUETOOTH)
        bluetooth_look(false);
    damage_tray();
    panel = -1;
    setting_volume = setting_brightness = false;
    autohide();
}

// Short sounds of wm's own, made as they're played by a child, so wm never waits
// for the sound card: notes that each start at once and fade away, a sine with a
// little of its octave so they're softer than beeps, or for the shutter, a hiss.
enum sound { SOUND_NOTICE, SOUND_LOG_IN, SOUND_LOG_OUT, SOUND_VOLUME, SOUND_SHUTTER, SOUND_SNAP };
#define SOUND_RATE 48000
#define SOUND_NOTES 3

static void play_sound(enum sound which) {
    static const struct { float hz; int start, length; } notes[][SOUND_NOTES] = { // the times in ms
        [SOUND_NOTICE] = {{880, 0, 300}, {1318.5f, 90, 450}},
        [SOUND_LOG_IN] = {{523.3f, 0, 500}, {659.3f, 110, 500}, {784, 220, 800}},
        [SOUND_LOG_OUT] = {{784, 0, 500}, {659.3f, 110, 500}, {523.3f, 220, 800}},
        [SOUND_VOLUME] = {{1046.5f, 0, 60}},
        [SOUND_SHUTTER] = {{0, 0, 120}},
        [SOUND_SNAP] = {{784, 0, 70}, {1046.5f, 35, 110}},
    };
    long frames = 0;
    struct audio_stream *stream;
    float *mixed;
    int16_t *out;

    if (!settings.sounds || mixer < 0 || fork() != 0)
        return;
    for (int i = 0; i < SOUND_NOTES; i++) {
        long end = (long)(notes[which][i].start + notes[which][i].length) * SOUND_RATE / 1000;

        frames = end > frames ? end : frames;
    }
    mixed = calloc(frames, sizeof(*mixed));
    out = malloc(frames * 2 * sizeof(*out));
    if (!mixed || !out || !(stream = audio_play(SOUND_RATE, 2)))
        _exit(1);
    for (int i = 0; i < SOUND_NOTES && notes[which][i].length; i++) {
        float hz = notes[which][i].hz, step = 2 * (float)M_PI * hz / SOUND_RATE;
        long start = (long)notes[which][i].start * SOUND_RATE / 1000, length = (long)notes[which][i].length * SOUND_RATE / 1000;

        for (long f = 0; f < length; f++) {
            float fade = expf(-6.0f * f / length) * (f < 96 ? f / 96.0f : 1); // 2 ms in, so it doesn't click
            float wave = hz ? sinf(step * f) + 0.3f * sinf(2 * step * f) : (float)rand() / RAND_MAX * 2 - 1;

            mixed[start + f] += 0.2f * fade * wave;
        }
    }
    for (long f = 0; f < frames; f++) {
        float v = mixed[f] > 1 ? 1 : mixed[f] < -1 ? -1 : mixed[f];

        out[2 * f] = out[2 * f + 1] = (int16_t)(v * 32767);
    }
    audio_stream_write(stream, out, frames);
    audio_stream_close(stream); // and it plays to its end
    _exit(0);
}

// Adds a notification, returning it: the one up, or with do not disturb, the one
// in the history.
static struct notice *add_notice(const char *title, const char *text, const char *app) {
    time_t now = time(NULL);
    struct notice *n = &history[0];
    int count = 1;

    damage_notices();
    damage_tray(); // before the history panel grows
    memmove(history + 1, history, (HISTORY - 1) * sizeof(*history));
    history_count += history_count < HISTORY;
    *n = (struct notice){.count = 1, .until = now_ms() + NOTICE_MS};
    snprintf(n->title, sizeof(n->title), "%.*s", GUI_TITLE_MAX - 1, title);
    snprintf(n->text, sizeof(n->text), "%.*s", GUI_TITLE_MAX - 1, text);
    snprintf(n->app, sizeof(n->app), "%s", app ? app : "");
    strftime(n->time, sizeof(n->time), "%H:%M", localtime(&now));
    unseen += panel != TRAY_NOTICES;
    damage_tray();
    printf("wm: notice %s: %s", n->title, n->text);
    if (settings.quiet) {
        printf(" (quiet)\n");
        return n;
    }
    for (int i = 0; i < notice_count && app && app[0]; i++) {
        if (!strcmp(notices[i].app, app)) {
            count += notices[i].count;
            remove_notice(i);
            break;
        }
    }
    if (notice_count == MAX_NOTICES) // the oldest makes way
        memmove(notices, notices + 1, --notice_count * sizeof(*notices));
    n = &notices[notice_count++];
    *n = history[0];
    n->count = count;
    printf(count > 1 ? " (%d)\n" : "\n", count);
    damage_notices();
    return n;
}

// A file to open for the notice just added, kept in its history as well.
static void notice_file(struct notice *n, const char *path) {
    snprintf(n->file, sizeof(n->file), "%s", path);
    snprintf(history[0].file, sizeof(history[0].file), "%s", path);
}

// A button for the notice just added, which runs a command.
static void notice_action(struct notice *n, const char *label, const char *command) {
    snprintf(n->label, sizeof(n->label), "%s", label);
    snprintf(n->command, sizeof(n->command), "%s", command);
    snprintf(history[0].label, sizeof(history[0].label), "%s", n->label);
    snprintf(history[0].command, sizeof(history[0].command), "%s", command);
    damage_notices();
}

static void clear_history(void) {
    damage_tray();
    history_count = unseen = 0;
}

static void set_volume(int to, bool mute) {
    to = to < 0 ? 0 : to > 100 ? 100 : to;
    if (to == volume && mute == muted)
        return;
    volume = to;
    muted = mute;
    audio_set_volume(volume);
    audio_set_muted(muted);
    printf("wm: volume %d%s\n", volume, muted ? ", muted" : "");
    damage_tray();
}

// What audiod has, which it keeps across restarts, and tells wm of each change to,
// through audio_watch.
static int audio_watch = -1;

static void load_outputs(void) {
    struct audio_output found[MAX_OUTPUTS];
    int n = mixer < 0 ? 0 : audio_outputs(found, MAX_OUTPUTS);

    n = n < 0 ? 0 : n > MAX_OUTPUTS ? MAX_OUTPUTS : n;
    if (n != output_count || memcmp(found, outputs, n * sizeof(*found))) {
        memcpy(outputs, found, n * sizeof(*found));
        output_count = n;
        damage_tray();
        if (panel == TRAY_QUICK) // its size changes with them
            add_damage((struct rect){0, 0, screen.width, screen.height});
    }
}

static void show_mixer(const struct audio_mixer *m) {
    int had = mixer;

    mixer = m->card ? 0 : -1;
    if (had != mixer || m->volume != volume || m->muted != muted) {
        volume = m->volume;
        muted = m->muted;
        printf("wm: the volume's %d%s\n", volume, muted ? ", muted" : "");
        damage_tray();
    }
    if (had != mixer) // the speaker comes or goes beside the battery
        report_tray();
    load_outputs();
}

static void load_volume(void) {
    struct audio_mixer m;

    if (audio_watch < 0 && (audio_watch = audio_mixer_watch(&m)) >= 0)
        show_mixer(&m);
}

static void mixer_changed(void) {
    struct audio_mixer m;

    if (audio_mixer_changed(audio_watch, &m)) {
        show_mixer(&m);
        return;
    }
    close(audio_watch); // audiod's stopped, and is looked for again later
    audio_watch = -1;
    show_mixer(&(struct audio_mixer){0});
}

static void set_volume_at(int x) {
    struct rect r = volume_slider();

    set_volume(((x - r.x) * 100 + r.width / 2) / r.width, muted);
}

// A number or a word from a file in one of /sys/class's folders.
static int read_number(const char *dir, const char *name) {
    char path[GUI_PATH_MAX + 32];
    FILE *f;
    int n = -1;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if ((f = fopen(path, "r"))) {
        if (fscanf(f, "%d", &n) != 1)
            n = -1;
        fclose(f);
    }
    return n;
}

static void read_word(const char *dir, const char *name, char *out, size_t size) {
    char path[GUI_PATH_MAX + 32];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    out[0] = '\0';
    if ((f = fopen(path, "r"))) {
        if (fgets(out, size, f))
            out[strcspn(out, "\n")] = '\0';
        fclose(f);
    }
}

// The backlight to set: the firmware's or the platform's before a graphics card's own.
static void find_backlight(void) {
    DIR *dir = opendir(BACKLIGHT_DIR);
    struct dirent *e;
    int best = 0;

    while (dir && (e = readdir(dir))) {
        char path[GUI_PATH_MAX], type[32];

        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), BACKLIGHT_DIR "/%s", e->d_name);
        read_word(path, "type", type, sizeof(type));

        int rank = !strcmp(type, "firmware") ? 3 : !strcmp(type, "platform") ? 2 : 1;

        if (rank > best) {
            best = rank;
            snprintf(backlight, sizeof(backlight), "%s", path);
        }
    }
    if (dir)
        closedir(dir);
    brightness_max = backlight[0] ? read_number(backlight, "max_brightness") : 1;
    brightness = backlight[0] ? read_number(backlight, "brightness") : 0;
    if (brightness_max <= 0)
        backlight[0] = '\0', brightness_max = 1;
}

static int brightness_percent(void) {
    return (brightness * 100 + brightness_max / 2) / brightness_max;
}

static void show_osd(bool volume_shown) {
    osd_volume = volume_shown;
    osd_until = now_ms() + OSD_MS;
    add_damage(osd_rect());
}

// The backlight's file is root's, so the logged-in session asks logind, which lets
// whoever's at the seat set it.
static bool logind_brightness(int level) {
    static sd_bus *bus;
    sd_bus_error e = SD_BUS_ERROR_NULL;

    if (!bus && sd_bus_open_system(&bus) < 0)
        return false;
    int r = sd_bus_call_method(bus, "org.freedesktop.login1", "/org/freedesktop/login1/session/auto",
                               "org.freedesktop.login1.Session", "SetBrightness", &e, NULL, "ssu", "backlight",
                               strrchr(backlight, '/') + 1, (uint32_t)level);
    if (r < 0)
        printf("wm: couldn't set the brightness: %s\n", e.message ? e.message : strerror(-r));
    sd_bus_error_free(&e);
    return r >= 0;
}

// Never quite dark, which would look like the screen had gone off.
static void set_brightness(int level) {
    char path[GUI_PATH_MAX + 16];
    FILE *f;

    level = level < 1 ? 1 : level > brightness_max ? brightness_max : level;
    if (!backlight[0] || level == brightness)
        return;
    snprintf(path, sizeof(path), "%s/brightness", backlight);
    if ((f = fopen(path, "w"))) {
        fprintf(f, "%d\n", level);
        if (fclose(f) != 0)
            return;
    } else if (!logind_brightness(level))
        return;
    brightness = level;
    printf("wm: brightness %d\n", brightness_percent());
    damage_tray();
}

// A step of the keys, at least one of the backlight's own.
static void step_brightness(int direction) {
    int step = brightness_max * BRIGHTNESS_STEP / 100;

    set_brightness(brightness + direction * (step > 0 ? step : 1));
    show_osd(false);
}

static void set_brightness_at(int x) {
    struct rect r = brightness_slider();
    int percent = ((x - r.x) * 100 + r.width / 2) / r.width;

    set_brightness((percent * brightness_max + 50) / 100);
}

// Locks first, so it's locked when it wakes, then suspends once that's drawn.
static void suspend_soon(void) {
    if (suspend_due >= 0)
        return;
    if (can_lock() && !locked)
        lock();
    suspend_due = now_ms() + SUSPEND_WAIT_MS;
    printf("wm: suspending\n");
}

// The battery, looked at with the network, warning as it runs low, and suspending
// before it's empty.
static void check_battery(void) {
    DIR *dir = opendir(POWER_DIR);
    struct dirent *e;
    int was = battery;
    bool was_charging = charging;

    battery = -1;
    while (dir && battery < 0 && (e = readdir(dir))) {
        char path[GUI_PATH_MAX], type[32], status[32];

        snprintf(path, sizeof(path), POWER_DIR "/%s", e->d_name);
        read_word(path, "type", type, sizeof(type));
        if (e->d_name[0] == '.' || strcmp(type, "Battery") || read_number(path, "present") == 0)
            continue;
        battery = read_number(path, "capacity");
        read_word(path, "status", status, sizeof(status));
        charging = strcmp(status, "Discharging") != 0; // or full, or held below full
    }
    if (dir)
        closedir(dir);
    if (battery != was || charging != was_charging) {
        damage_taskbar();
        damage_tray();
    }
    if (battery < 0 || charging) {
        warned = 100;
    } else if (battery <= 2 && warned > 2) {
        add_notice("The battery's empty", "Suspending, so nothing's lost", NULL);
        suspend_soon();
        warned = 2;
    } else if (battery <= 5 && warned > 5) {
        add_notice("The battery's very low", "5% left. Plug the charger in.", NULL);
        warned = 5;
    } else if (battery <= 10 && warned > 10) {
        add_notice("The battery's low", "10% left", NULL);
        warned = 10;
    }
}

// How long until the popup goes or the computer suspends, or -1 if neither's due.
static int check_laptop(void) {
    int64_t now = now_ms(), wait = -1;

    if (suspend_due >= 0 && now >= suspend_due) {
        suspend_due = -1;
        if (fork() == 0) {
            execlp("systemctl", "systemctl", "suspend", (char *)NULL);
            _exit(127);
        }
    }
    if (osd_until && now >= osd_until) {
        osd_until = 0;
        add_damage(osd_rect());
    }
    if (suspend_due >= 0)
        wait = suspend_due - now;
    if (osd_until && (wait < 0 || osd_until - now < wait))
        wait = osd_until - now;
    return (int)wait;
}

// Looks at the network again, redrawing the tray if it's come or gone.
// The network's icon and quick settings show what NetworkManager's said.
static void check_network(void) {
    static bool was;

    damage_tray();
    if (panel == TRAY_QUICK || panel == TRAY_NETWORK)
        add_damage((struct rect){panel_rect(panel).x, 0, panel_rect(panel).width, screen.height});
    if (connected() != was)
        printf("wm: network %s\n", connected() ? "connected" : "disconnected");
    was = connected();
}

static bool leap(int year) {
    return (year % 4 == 0 && year % 100) || year % 400 == 0;
}

// Monday as 0, by Sakamoto's method.
static int weekday(int year, int month, int day) {
    static const int offsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};

    year -= month < 2;
    return (year + year / 4 - year / 100 + year / 400 + offsets[month] + day + 6) % 7;
}

// The month shown, from this one, as a year and a month from 0.
static void shown_month(int *year, int *month) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    int months = t->tm_year * 12 + t->tm_mon + calendar_month;

    *year = 1900 + (months >= 0 ? months / 12 : (months - 11) / 12);
    *month = months - (*year - 1900) * 12;
}

static const char *const month_names[] = {"January", "February", "March", "April", "May", "June", "July",
                                          "August", "September", "October", "November", "December"};

// Says where a panel's parts are, for the tests to find them.
static void report_panel(int item) {
    struct rect p = panel_rect(item);

    printf("wm: panel %d at %d %d %d %d\n", item, p.x, p.y, p.width, p.height);
    if (item != TRAY_QUICK)
        return;
    for (int i = 0; i < TILES; i++) {
        struct rect t = quick_tile(i);

        printf("wm: tile %d at %d %d %d %d\n", i, t.x, t.y, t.width, t.height);
    }
    if (mixer >= 0) {
        struct rect m = mute_button(), v = volume_slider();

        printf("wm: mute at %d %d %d %d\nwm: slider at %d %d %d %d\n", m.x, m.y, m.width, m.height, v.x, v.y, v.width,
               v.height);
        for (int i = 0; output_count > 1 && i < output_count; i++) {
            struct rect r = output_row(i);

            printf("wm: output %d at %d %d %d %d, %s%s\n", i, r.x, r.y, r.width, r.height, outputs[i].name,
                   outputs[i].current ? ", playing" : "");
        }
    }
    if (backlight[0]) {
        struct rect b = brightness_slider();

        printf("wm: brightness slider at %d %d %d %d\n", b.x, b.y, b.width, b.height);
    }
}

static void close_wifi_row(void);
static void report_wifi(void);

static void open_panel(int item) {
    close_panel();
    calendar_month = 0;
    now_playing[0] = '\0';
    if (item == TRAY_QUICK) {
        FILE *f = fopen(NOW_PLAYING, "r");

        if (f && fgets(now_playing, sizeof(now_playing), f))
            now_playing[strcspn(now_playing, "\n")] = '\0';
        if (f)
            fclose(f);
        printf("wm: quick settings%s%s\n", now_playing[0] ? ", " : "", now_playing);
        check_network();
        check_battery();
    }
    if (item == TRAY_NETWORK) {
        close_wifi_row();
        wifi_message[0] = '\0';
        network_scan();
        printf("wm: wifi panel\n");
    }
    if (item == TRAY_BLUETOOTH) {
        bt_opened[0] = bt_message[0] = '\0';
        bluetooth_look(true);
        printf("wm: bluetooth panel\n");
    }
    panel = item;
    panel_opened = now_ms();
    report_panel(item);
    if (item == TRAY_NETWORK)
        report_wifi();
    autohide();
    if (item == TRAY_CALENDAR) {
        int year, month;

        shown_month(&year, &month);
        printf("wm: calendar %s %d\n", month_names[month], year);
    }
    if (item == TRAY_CLIPS)
        printf("wm: clipboard history %d\n", clip_count);
    if (item == TRAY_NOTICES) {
        unseen = 0;
        while (notice_count)
            remove_notice(0); // they're all in the panel
    }
    damage_tray();
}

static void draw_line(struct gui_canvas *c, struct rect p, int y, const char *label, const char *value, int ox,
                      int oy) {
    gui_text(c, p.x + 12 - ox, y - oy, label, TASKBAR_DIM_TEXT, 1);
    gui_text(c, p.x + 96 - ox, y - oy, value, TASKBAR_TEXT, 1);
}

static void draw_address(struct gui_canvas *c, struct rect p, int y, const char *label, uint32_t address, int ox,
                         int oy) {
    struct in_addr in = {htonl(address)};

    draw_line(c, p, y, label, address ? inet_ntoa(in) : "none", ox, oy);
}

static void draw_amount(struct gui_canvas *c, struct rect p, int y, const char *label, uint64_t bytes, int ox,
                        int oy) {
    char shown[32];

    snprintf(shown, sizeof(shown), bytes >= 10 << 20 ? "%llu MB" : bytes >= 10 << 10 ? "%llu KB" : "%llu bytes",
             (unsigned long long)(bytes >= 10 << 20 ? bytes >> 20 : bytes >= 10 << 10 ? bytes >> 10 : bytes));
    draw_line(c, p, y, label, shown, ox, oy);
}

static void draw_network(struct gui_canvas *c, struct rect p, int y, int ox, int oy) {
    const char *state = !net->running                  ? "No NetworkManager"
                      : connected()                    ? "Connected"
                      : !net->wifi_on && !net->cable   ? "Airplane mode"
                      : !net->wifi_card && !net->cable ? "No network card"
                                                       : "Not connected";

    gui_text(c, p.x + 12 - ox, y - oy, "Network", TASKBAR_TEXT, 1);
    gui_text(c, p.x + p.width - 12 - gui_text_width(state, 1) - ox, y - oy, state,
             connected() ? TASKBAR_TEXT : TASKBAR_DIM_TEXT, 1);
    draw_address(c, p, y + PANEL_LINE, "Address", net->address, ox, oy);
    draw_address(c, p, y + 2 * PANEL_LINE, "Gateway", net->gateway, ox, oy);
    draw_address(c, p, y + 3 * PANEL_LINE, "DNS", net->dns, ox, oy);
    draw_amount(c, p, y + 4 * PANEL_LINE, "Received", net->rx_bytes, ox, oy);
    draw_amount(c, p, y + 5 * PANEL_LINE, "Sent", net->tx_bytes, ox, oy);
}

// Where the Wi-Fi panel's parts are.
static struct rect wifi_switch(void) {
    struct rect p = panel_rect(TRAY_NETWORK);

    return (struct rect){p.x + p.width - 12 - SWITCH_WIDTH, p.y + 11, SWITCH_WIDTH, 18};
}

static struct rect wifi_row(int i) {
    struct rect p = panel_rect(TRAY_NETWORK);
    int open = wifi_open_index(), y = p.y + 40 + wired_height() + (wifi_note() ? PANEL_LINE + 8 : 0);

    return (struct rect){p.x + 4, y + i * WIFI_ROW + (open >= 0 && i > open ? wifi_open_height() : 0), p.width - 8, WIFI_ROW};
}

// A text box under the open row: the hidden network's name above its password.
static struct rect wifi_field(int field) {
    struct rect r = wifi_row(wifi_open_index());
    int line = hidden_opened && field == FIELD_PASSWORD;

    return (struct rect){r.x + 8, r.y + r.height + 2 + line * WIFI_LINE, r.width - 16 - 84, 26};
}

// The buttons under the open row, on its last line, 0 the rightmost.
static struct rect wifi_button(int which) {
    struct rect r = wifi_row(wifi_open_index());
    int y = r.y + r.height + 2 + (hidden_opened ? WIFI_LINE : 0);

    return (struct rect){r.x + r.width - 8 - 76 - which * 84, y, 76, 26};
}

static const struct wifi *opened_wifi(void) {
    int i = wifi_open_index();

    return i >= 0 && !hidden_opened ? &net->wifi[i] : NULL;
}

// What the open row asks for: a password, for a secured network that isn't saved.
static bool wifi_needs_password(void) {
    const struct wifi *w = opened_wifi();

    return hidden_opened || (w && w->secured && !w->saved && !w->active);
}

// The open row's buttons, rightmost first, or NULL where there's none.
static void wifi_buttons(const char *labels[2]) {
    const struct wifi *w = opened_wifi();

    labels[0] = labels[1] = NULL;
    if (hidden_opened || wifi_needs_password())
        labels[0] = "Connect";
    else if (w && w->active)
        labels[0] = w->saved ? "Forget" : NULL, labels[1] = "Disconnect";
    else if (w)
        labels[0] = "Connect", labels[1] = w->saved ? "Forget" : NULL;
    if (!labels[0])
        labels[0] = labels[1], labels[1] = NULL;
}

// Says where the panel's parts are, when that changes, for the tests to find them.
static void report_wifi(void) {
    static char last[4096];
    char now[4096];
    int n = 0;
    struct rect s = wifi_switch();
    const char *labels[2];

    n += snprintf(now + n, sizeof(now) - n, "wm: wifi %s at %d %d %d %d\n", net->wifi_on ? "on" : "off", s.x, s.y,
                  s.width, s.height);
    if (net->cable)
        n += snprintf(now + n, sizeof(now) - n, "wm: wired %s %s\n", net->wired_name,
                      net->wired ? "connected" : "not connected");
    for (int i = 0; i < wifi_shown() && n < (int)sizeof(now) - 200; i++) {
        struct rect r = wifi_row(i);
        const struct wifi *w = &net->wifi[i];

        n += snprintf(now + n, sizeof(now) - n, "wm: wifi row %d at %d %d %d %d, %s%s%s%s\n", i, r.x, r.y, r.width,
                      r.height, w->ssid, w->active ? ", connected" : "", w->saved ? ", saved" : "",
                      w->secured ? ", secured" : "");
    }
    if (wifi_listed()) {
        struct rect r = wifi_row(wifi_shown());

        n += snprintf(now + n, sizeof(now) - n, "wm: wifi row hidden at %d %d %d %d\n", r.x, r.y, r.width, r.height);
    }
    if (wifi_open_index() >= 0) {
        wifi_buttons(labels);
        for (int field = FIELD_NAME; field <= FIELD_PASSWORD; field++) {
            struct rect f = wifi_field(field);

            if ((field == FIELD_NAME && hidden_opened) || (field == FIELD_PASSWORD && wifi_needs_password()))
                n += snprintf(now + n, sizeof(now) - n, "wm: wifi field %s at %d %d %d %d\n",
                              field == FIELD_NAME ? "name" : "password", f.x, f.y, f.width, f.height);
        }
        for (int i = 0; i < 2 && labels[i]; i++) {
            struct rect b = wifi_button(i);

            n += snprintf(now + n, sizeof(now) - n, "wm: wifi button %s at %d %d %d %d\n", labels[i], b.x, b.y, b.width,
                          b.height);
        }
    }
    if (strcmp(now, last))
        fputs(now, stdout);
    memcpy(last, now, sizeof(last));
}

// The panel's whole column, as its height changes with what's in it.
static void damage_network_panel(void) {
    struct rect p = panel_rect(TRAY_NETWORK);

    add_damage((struct rect){p.x, 0, p.width, screen.height});
}

static void close_wifi_row(void) {
    wifi_opened[0] = '\0';
    hidden_opened = false;
    wifi_typing = FIELD_NONE;
    memset(wifi_password, 0, sizeof(wifi_password));
    wifi_name[0] = '\0';
}

static void wifi_join(void) {
    const char *ssid = hidden_opened ? wifi_name : wifi_opened;
    char name[SSID_MAX];

    if (!ssid[0])
        return;
    snprintf(name, sizeof(name), "%s", ssid);
    if (network_join(name, wifi_needs_password() ? wifi_password : NULL, hidden_opened)) {
        snprintf(wifi_message, sizeof(wifi_message), "Joining %s...", name);
        printf("wm: joining %s\n", name);
        close_wifi_row();
    } else {
        snprintf(wifi_message, sizeof(wifi_message), "%s", network_error());
        printf("wm: can't join %s: %s\n", name, network_error());
    }
}

static void wifi_action(const char *label) {
    char ssid[SSID_MAX];

    snprintf(ssid, sizeof(ssid), "%s", wifi_opened);
    if (!strcmp(label, "Connect")) {
        wifi_join();
    } else if (!strcmp(label, "Disconnect")) {
        if (network_leave())
            printf("wm: left %s\n", ssid);
        close_wifi_row();
    } else if (!strcmp(label, "Forget")) {
        bool forgot = network_forget(ssid);

        if (forgot)
            printf("wm: forgot %s\n", ssid);
        snprintf(wifi_message, sizeof(wifi_message), forgot ? "Forgot %s" : "%s", forgot ? ssid : network_error());
        close_wifi_row();
    }
}

static void network_click(void) {
    const char *labels[2];
    int open = wifi_open_index();

    damage_network_panel();
    if (contains(wifi_switch(), cursor_x, cursor_y)) {
        if (network_set_wifi(!net->wifi_on))
            printf("wm: wifi %s\n", net->wifi_on ? "off" : "on");
        close_wifi_row();
        return;
    }
    if (open >= 0) {
        wifi_buttons(labels);
        for (int i = 0; i < 2 && labels[i]; i++) {
            if (contains(wifi_button(i), cursor_x, cursor_y)) {
                wifi_action(labels[i]);
                return;
            }
        }
        for (int field = FIELD_NAME; field <= FIELD_PASSWORD; field++) {
            if (contains(wifi_field(field), cursor_x, cursor_y)
                && (field == FIELD_PASSWORD ? wifi_needs_password() : hidden_opened)) {
                wifi_typing = field;
                return;
            }
        }
    }
    for (int i = 0; i <= wifi_shown() && wifi_listed(); i++) {
        if (!contains(wifi_row(i), cursor_x, cursor_y))
            continue;
        if (i == open) {
            close_wifi_row();
            return;
        }
        close_wifi_row();
        wifi_message[0] = '\0';
        if (i == wifi_shown()) {
            hidden_opened = true;
            wifi_typing = FIELD_NAME;
            return;
        }

        const struct wifi *w = &net->wifi[i];

        snprintf(wifi_opened, sizeof(wifi_opened), "%s", w->ssid);
        if (!w->secured && !w->saved && !w->active) {
            wifi_join(); // an open network, which needs nothing more
            return;
        }
        wifi_typing = wifi_needs_password() ? FIELD_PASSWORD : FIELD_NONE;
        return;
    }
}

// Typing into the open row's boxes: Tab goes between them, Enter joins, and Escape
// closes the row.
static bool wifi_key(const struct key_event *e) {
    char *text = wifi_typing == FIELD_NAME ? wifi_name : wifi_password;
    size_t size = wifi_typing == FIELD_NAME ? sizeof(wifi_name) : sizeof(wifi_password), length = strlen(text);

    if (panel != TRAY_NETWORK || !wifi_typing)
        return false;
    if (!e->pressed)
        return true;
    damage_network_panel();
    if (e->code == KEY_ESCAPE)
        close_wifi_row();
    else if (e->code == KEY_ENTER || e->code == (KEY_EXTENDED | KEY_ENTER))
        wifi_join();
    else if (e->code == KEY_TAB && hidden_opened)
        wifi_typing = wifi_typing == FIELD_NAME ? FIELD_PASSWORD : FIELD_NAME;
    else if (e->code == KEY_BACKSPACE && length)
        text[length - 1] = '\0';
    else if (e->text >= ' ' && e->text < 0x7F && length + 1 < size)
        text[length] = (char)e->text, text[length + 1] = '\0';
    return true;
}

// Signal strength as four bars, as many lit as it's strong.
static void draw_bars(struct gui_canvas *c, int x, int y, int strength, int ox, int oy) {
    int lit = strength > 75 ? 4 : strength > 50 ? 3 : strength > 25 ? 2 : 1;

    for (int i = 0; i < 4; i++) {
        int height = 4 + i * 3;

        gui_fill(c, x + i * 4 - ox, y + 13 - height - oy, 3, height, i < lit ? TASKBAR_TEXT : TASKBAR_LINE);
    }
}

static void draw_field(struct gui_canvas *c, int field, const char *placeholder, int ox, int oy) {
    struct rect f = wifi_field(field);
    const char *text = field == FIELD_NAME ? wifi_name : wifi_password;
    char shown[SSID_MAX + 64];
    size_t length = strlen(text);

    if (field == FIELD_PASSWORD) { // as dots
        length = length < sizeof(shown) - 1 ? length : sizeof(shown) - 1;
        memset(shown, '*', length);
        shown[length] = '\0';
    } else {
        snprintf(shown, sizeof(shown), "%s", text);
    }
    gui_fill_rounded(c, f.x - ox, f.y - oy, f.width, f.height, SMALL_RADIUS, wifi_typing == field ? TITLE_FOCUSED : TASKBAR_BUTTON);
    gui_fill_rounded(c, f.x + 1 - ox, f.y + 1 - oy, f.width - 2, f.height - 2, SMALL_RADIUS, TASKBAR_COLOR);

    int fits = (int)gui_text_fit(shown, strlen(shown), f.width - 16), from = (int)strlen(shown) - fits;
    int text_y = f.y + (f.height - GUI_CHAR_HEIGHT) / 2 - oy;

    if (shown[0])
        gui_text(c, f.x + 8 - ox, text_y, shown + from, TASKBAR_TEXT, 1);
    else
        gui_text(c, f.x + 8 - ox, text_y, placeholder, TASKBAR_DIM_TEXT, 1);
    if (wifi_typing == field) // where what's typed goes
        gui_fill(c, f.x + 8 + (shown[0] ? gui_text_width(shown + from, 1) : 0) - ox, text_y + 1, 1, GUI_CHAR_HEIGHT - 2,
                 TASKBAR_TEXT);
}

static void draw_button(struct gui_canvas *c, struct rect b, const char *label, bool main, int ox, int oy) {
    bool hovered = contains(b, cursor_x, cursor_y);

    gui_fill_rounded(c, b.x - ox, b.y - oy, b.width, b.height, SMALL_RADIUS,
                     main ? TITLE_FOCUSED : hovered ? TASKBAR_LINE : TASKBAR_BUTTON);
    gui_text(c, b.x + (b.width - gui_text_width(label, 1)) / 2 - ox, b.y + (b.height - GUI_CHAR_HEIGHT) / 2 - oy, label,
             main ? ON_ACCENT : TASKBAR_TEXT, 1);
}

static void draw_wifi_row(struct gui_canvas *c, int i, int ox, int oy) {
    struct rect r = wifi_row(i);
    const struct wifi *w = &net->wifi[i];
    const struct gui_image *lock = w->secured ? gui_icon("lock") : NULL;
    const char *state = !strcmp(net->joining, w->ssid) ? "Joining..." : w->active ? "Connected" : w->saved ? "Saved"
                      : w->secured ? "Secured" : "Open";
    char name[SSID_MAX];

    if (wifi_open_index() == i || contains(r, cursor_x, cursor_y))
        gui_fill_rounded(c, r.x - ox, r.y - oy, r.width, r.height, SMALL_RADIUS, TASKBAR_BUTTON);
    draw_bars(c, r.x + 10, r.y + 11, w->strength, ox, oy);
    snprintf(name, sizeof(name), "%.*s", (int)gui_text_fit(w->ssid, strlen(w->ssid), r.width - 80), w->ssid);
    gui_text(c, r.x + 34 - ox, r.y + 2 - oy, name, TASKBAR_TEXT, 1);
    gui_text(c, r.x + 34 - ox, r.y + 2 + GUI_CHAR_HEIGHT - oy, state, w->active ? TITLE_FOCUSED : TASKBAR_DIM_TEXT, 1);
    if (lock)
        gui_draw_image(c, r.x + r.width - 8 - ICON_SIZE - ox, r.y + (r.height - ICON_SIZE) / 2 - oy, lock);
}

static void draw_network_panel(struct gui_canvas *c, struct rect p, int ox, int oy) {
    struct rect s = wifi_switch();
    const char *note = wifi_note(), *labels[2];
    int y = p.y + 40, open = wifi_open_index();

    report_wifi();
    gui_text(c, p.x + 12 - ox, p.y + 12 - oy, "Wi-Fi", TASKBAR_TEXT, 1);
    gui_fill_rounded(c, s.x - ox, s.y - oy, s.width, s.height, s.height / 2, net->wifi_on ? TITLE_FOCUSED : TASKBAR_BUTTON);
    gui_fill_rounded(c, s.x + (net->wifi_on ? s.width - s.height : 0) + 2 - ox, s.y + 2 - oy, s.height - 4, s.height - 4,
                     (s.height - 4) / 2, 0xFFFFFF);
    if (net->cable) {
        const char *state = net->wired ? "Connected" : "Not connected";

        gui_text(c, p.x + 12 - ox, y - oy, "Wired", TASKBAR_TEXT, 1);
        gui_text(c, p.x + 12 + gui_text_width("Wired ", 1) - ox, y - oy, net->wired_name, TASKBAR_DIM_TEXT, 1);
        gui_text(c, p.x + p.width - 12 - gui_text_width(state, 1) - ox, y - oy, state,
                 net->wired ? TASKBAR_TEXT : TASKBAR_DIM_TEXT, 1);
        y += wired_height();
    }
    if (note)
        gui_text(c, p.x + 12 - ox, y - oy, note, TASKBAR_DIM_TEXT, 1);
    for (int i = 0; i < wifi_shown(); i++)
        draw_wifi_row(c, i, ox, oy);
    if (wifi_listed()) {
        struct rect h = wifi_row(wifi_shown());

        if (hidden_opened || contains(h, cursor_x, cursor_y))
            gui_fill_rounded(c, h.x - ox, h.y - oy, h.width, h.height, SMALL_RADIUS, TASKBAR_BUTTON);
        gui_text(c, h.x + 34 - ox, h.y + (h.height - GUI_CHAR_HEIGHT) / 2 - oy, "Hidden network...", TASKBAR_TEXT, 1);
    }
    if (open >= 0) {
        if (hidden_opened)
            draw_field(c, FIELD_NAME, "Network name", ox, oy);
        if (wifi_needs_password())
            draw_field(c, FIELD_PASSWORD, hidden_opened ? "Password, if it has one" : "Password", ox, oy);
        wifi_buttons(labels);
        for (int i = 0; i < 2 && labels[i]; i++)
            draw_button(c, wifi_button(i), labels[i], !strcmp(labels[i], "Connect"), ox, oy);
    }
    if (wifi_message[0]) {
        char shown[sizeof(wifi_message)];

        snprintf(shown, sizeof(shown), "%.*s", (int)gui_text_fit(wifi_message, strlen(wifi_message), p.width - 24),
                 wifi_message);
        gui_text(c, p.x + 12 - ox, p.y + p.height - 6 - PANEL_LINE - oy, shown, TASKBAR_DIM_TEXT, 1);
    }
}

// What NetworkManager's said, after poll: the tray, and what joining came to.
static void network_changed(const char *was_joining, int joined) {
    check_network();
    if (joined > 0) {
        snprintf(wifi_message, sizeof(wifi_message), "Joined %s", was_joining);
        printf("wm: joined %s\n", was_joining);
    } else if (joined < 0) {
        snprintf(wifi_message, sizeof(wifi_message), "%s", network_error());
        printf("wm: couldn't join %s: %s\n", was_joining, network_error());
    }
    if (panel == TRAY_NETWORK) {
        damage_network_panel();
        report_wifi();
    }
}

// Where the Bluetooth panel's parts are.
static struct rect bt_switch(void) {
    struct rect p = panel_rect(TRAY_BLUETOOTH);

    return (struct rect){p.x + p.width - 12 - SWITCH_WIDTH, p.y + 11, SWITCH_WIDTH, 18};
}

static struct rect bt_row(int i) {
    struct rect p = panel_rect(TRAY_BLUETOOTH);
    int open = bt_open_index(), y = p.y + 40 + (bt_note() ? PANEL_LINE + 8 : 0);

    return (struct rect){p.x + 4, y + i * WIFI_ROW + (open >= 0 && i > open ? WIFI_LINE + 6 : 0), p.width - 8, WIFI_ROW};
}

static const struct bt_device *bt_opened_device(void) {
    int i = bt_open_index();

    return i >= 0 ? &bt->devices[i] : NULL;
}

// The open row's buttons, rightmost first, or NULL, and none while it's busy.
static void bt_buttons(const char *labels[2]) {
    const struct bt_device *d = bt_opened_device();

    labels[0] = labels[1] = NULL;
    if (!d || !strcmp(bt->busy, d->path))
        return;
    if (d->connected)
        labels[0] = "Forget", labels[1] = "Disconnect";
    else if (d->paired)
        labels[0] = "Connect", labels[1] = "Forget";
    else
        labels[0] = "Pair";
}

static struct rect bt_button(int which) {
    struct rect r = bt_row(bt_open_index());

    return (struct rect){r.x + r.width - 8 - 90 - which * 98, r.y + r.height + 2, 90, 26};
}

// What pairing asks, under the list, and its buttons: 0 yes, 1 no.
static struct rect bt_asking(void) {
    struct rect r = bt_row(bt_shown());

    return (struct rect){r.x, r.y + 4, r.width, bt_asking_height() - 8};
}

static struct rect bt_answer(int which) {
    struct rect a = bt_asking();

    return (struct rect){a.x + a.width - 8 - 90 - which * 98, a.y + 4 + 2 * PANEL_LINE, 90, 26};
}

static bool bt_on(void) {
    return bt->powered && !bt->blocked;
}

// Says where the panel's parts are, when that changes, for the tests to find them.
static void report_bluetooth(void) {
    static char last[4096];
    char now[4096];
    int n = 0;
    struct rect s = bt_switch();
    const char *labels[2];

    n += snprintf(now + n, sizeof(now) - n, "wm: bluetooth %s at %d %d %d %d\n", bt_on() ? "on" : "off", s.x, s.y,
                  s.width, s.height);
    for (int i = 0; i < bt_shown() && n < (int)sizeof(now) - 200; i++) {
        struct rect r = bt_row(i);
        const struct bt_device *d = &bt->devices[i];

        n += snprintf(now + n, sizeof(now) - n, "wm: bluetooth row %d at %d %d %d %d, %s%s%s\n", i, r.x, r.y, r.width,
                      r.height, d->name, d->connected ? ", connected" : "", d->paired ? ", paired" : "");
    }
    bt_buttons(labels);
    for (int i = 0; i < 2 && labels[i]; i++) {
        struct rect b = bt_button(i);

        n += snprintf(now + n, sizeof(now) - n, "wm: bluetooth button %s at %d %d %d %d\n", labels[i], b.x, b.y,
                      b.width, b.height);
    }
    for (int i = 0; bt_asks_yes() && i < 2; i++) {
        struct rect b = bt_answer(i);

        n += snprintf(now + n, sizeof(now) - n, "wm: bluetooth answer %s at %d %d %d %d\n", i ? "no" : "yes", b.x, b.y,
                      b.width, b.height);
    }
    if (strcmp(now, last))
        fputs(now, stdout);
    memcpy(last, now, sizeof(last));
}

static void damage_bluetooth_panel(void) {
    struct rect p = panel_rect(TRAY_BLUETOOTH);

    add_damage((struct rect){p.x, 0, p.width, screen.height});
}

static void draw_bt_row(struct gui_canvas *c, int i, int ox, int oy) {
    struct rect r = bt_row(i);
    const struct bt_device *d = &bt->devices[i];
    const struct gui_image *icon = gui_icon(!strncmp(d->icon, "audio", 5) ? "speaker" : "bluetooth");
    char state[48], name[64];

    if (!strcmp(bt->busy, d->path))
        snprintf(state, sizeof(state), "%s...", bt->doing);
    else if (d->connected && d->battery >= 0)
        snprintf(state, sizeof(state), "Connected, %d%%", d->battery);
    else
        snprintf(state, sizeof(state), "%s", d->connected ? "Connected" : d->paired ? "Paired" : "Not paired");
    if (bt_open_index() == i || contains(r, cursor_x, cursor_y))
        gui_fill_rounded(c, r.x - ox, r.y - oy, r.width, r.height, SMALL_RADIUS, TASKBAR_BUTTON);
    if (icon)
        gui_draw_image(c, r.x + 10 - ox, r.y + (r.height - ICON_SIZE) / 2 - oy, icon);
    snprintf(name, sizeof(name), "%.*s", (int)gui_text_fit(d->name, strlen(d->name), r.width - 50), d->name);
    gui_text(c, r.x + 34 - ox, r.y + 2 - oy, name, TASKBAR_TEXT, 1);
    gui_text(c, r.x + 34 - ox, r.y + 2 + GUI_CHAR_HEIGHT - oy, state, d->connected ? TITLE_FOCUSED : TASKBAR_DIM_TEXT, 1);
}

static void draw_bluetooth_panel(struct gui_canvas *c, struct rect p, int ox, int oy) {
    const struct bt_request *asked = bluetooth_request();
    struct rect s = bt_switch();
    const char *note = bt_note(), *labels[2];
    int y = p.y + 40;

    report_bluetooth();
    gui_text(c, p.x + 12 - ox, p.y + 12 - oy, "Bluetooth", TASKBAR_TEXT, 1);
    gui_fill_rounded(c, s.x - ox, s.y - oy, s.width, s.height, s.height / 2, bt_on() ? TITLE_FOCUSED : TASKBAR_BUTTON);
    gui_fill_rounded(c, s.x + (bt_on() ? s.width - s.height : 0) + 2 - ox, s.y + 2 - oy, s.height - 4, s.height - 4,
                     (s.height - 4) / 2, 0xFFFFFF);
    if (note)
        gui_text(c, p.x + 12 - ox, y - oy, note, TASKBAR_DIM_TEXT, 1);
    for (int i = 0; i < bt_shown(); i++)
        draw_bt_row(c, i, ox, oy);
    bt_buttons(labels);
    for (int i = 0; i < 2 && labels[i]; i++)
        draw_button(c, bt_button(i), labels[i], !strcmp(labels[i], "Connect") || !strcmp(labels[i], "Pair"), ox, oy);
    if (asked->asking) {
        struct rect a = bt_asking();
        char line[2][128];

        if (asked->asking == BT_CONFIRM) {
            snprintf(line[0], sizeof(line[0]), "Pair with %s?", asked->device);
            snprintf(line[1], sizeof(line[1]), "Check it shows %s.", asked->code);
        } else if (asked->asking == BT_AUTHORIZE) {
            snprintf(line[0], sizeof(line[0]), "%s wants to pair.", asked->device);
            snprintf(line[1], sizeof(line[1]), "Allow it?");
        } else {
            snprintf(line[0], sizeof(line[0]), "Type %s on %s,", asked->code, asked->device);
            snprintf(line[1], sizeof(line[1]), "then press Enter on it.");
        }
        gui_fill_rounded(c, a.x - ox, a.y - oy, a.width, a.height, SMALL_RADIUS, TASKBAR_BUTTON);
        for (int i = 0; i < 2; i++)
            fitted_text(c, a.x + 10 - ox, a.y + 6 + i * PANEL_LINE - oy, a.width - 20, line[i], TASKBAR_TEXT);
        if (bt_asks_yes()) {
            draw_button(c, bt_answer(0), asked->asking == BT_CONFIRM ? "Pair" : "Allow", true, ox, oy);
            draw_button(c, bt_answer(1), "Cancel", false, ox, oy);
        }
    }
    if (bt_message[0])
        fitted_text(c, p.x + 12 - ox, p.y + p.height - 6 - PANEL_LINE - oy, p.width - 24, bt_message, TASKBAR_DIM_TEXT);
}

static void bt_action(const char *label) {
    const struct bt_device *d = bt_opened_device();
    char path[64], name[64];
    bool ok;

    if (!d)
        return;
    snprintf(path, sizeof(path), "%s", d->path);
    snprintf(name, sizeof(name), "%s", d->name);
    bt_message[0] = '\0';
    if (!strcmp(label, "Forget")) {
        ok = bluetooth_forget(path);
        bt_opened[0] = '\0';
    } else {
        ok = !strcmp(label, "Disconnect") ? bluetooth_disconnect(path) : bluetooth_connect(path);
    }
    if (ok)
        printf("wm: bluetooth %s %s\n", label, name);
    else
        snprintf(bt_message, sizeof(bt_message), "%s", bluetooth_error());
}

static void bluetooth_click(void) {
    const char *labels[2];

    damage_bluetooth_panel();
    if (contains(bt_switch(), cursor_x, cursor_y)) {
        bool on = !bt_on();

        if (bluetooth_set_on(on))
            printf("wm: bluetooth turned %s\n", on ? "on" : "off");
        else
            snprintf(bt_message, sizeof(bt_message), "Couldn't turn Bluetooth %s.", on ? "on" : "off");
        bt_opened[0] = '\0';
        return;
    }
    for (int i = 0; bt_asks_yes() && i < 2; i++) {
        if (contains(bt_answer(i), cursor_x, cursor_y)) {
            bluetooth_answer(i == 0);
            return;
        }
    }
    bt_buttons(labels);
    for (int i = 0; i < 2 && labels[i]; i++) {
        if (contains(bt_button(i), cursor_x, cursor_y)) {
            bt_action(labels[i]);
            return;
        }
    }
    for (int i = 0; i < bt_shown(); i++) {
        if (contains(bt_row(i), cursor_x, cursor_y)) {
            if (i == bt_open_index())
                bt_opened[0] = '\0';
            else
                snprintf(bt_opened, sizeof(bt_opened), "%s", bt->devices[i].path);
            bt_message[0] = '\0';
            return;
        }
    }
}

// What BlueZ's said, after poll: the tray, which has Bluetooth's icon while there's an
// adapter, and the panel, which opens by itself when pairing asks something.
static void bluetooth_changed(int done) {
    static bool had_adapter;

    if (bt->adapter != had_adapter) {
        had_adapter = bt->adapter;
        damage_taskbar();
        report_tray();
    }
    damage_tray();
    if (done < 0)
        snprintf(bt_message, sizeof(bt_message), "%s", bluetooth_error());
    else if (done > 0)
        bt_message[0] = '\0';
    if (bluetooth_request()->asking && panel != TRAY_BLUETOOTH && !locked)
        open_panel(TRAY_BLUETOOTH);
    if (panel == TRAY_BLUETOOTH) {
        damage_bluetooth_panel();
        report_bluetooth();
    }
}

static void close_menu(void);

// The password polkit asks for, when an app wants something only an admin can do,
// in a dialog over everything, which has the keyboard until it's answered.
#define AUTH_WIDTH 440
#define AUTH_HEIGHT 200
static char auth_typed[256];
static bool auth_shown;

static struct rect auth_rect(void) {
    return (struct rect){(screen.width - AUTH_WIDTH) / 2, (screen.height - AUTH_HEIGHT) / 2, AUTH_WIDTH, AUTH_HEIGHT};
}

static struct rect auth_field(void) {
    struct rect r = auth_rect();

    return (struct rect){r.x + 16, r.y + 102, r.width - 32, 28};
}

static struct rect auth_button(int i) { // 0 Cancel, 1 Authenticate
    struct rect r = auth_rect();

    return (struct rect){r.x + r.width - 16 - 120 - (1 - i) * 128, r.y + r.height - 16 - 28, 120, 28};
}

static void draw_auth(struct gui_canvas *c, int ox, int oy) {
    const struct polkit_request *p = polkit_now();
    struct rect r = auth_rect(), f = auth_field();
    const char *m = p->message;
    char line[256], dots[64];
    size_t typed = strlen(auth_typed) < sizeof(dots) - 1 ? strlen(auth_typed) : sizeof(dots) - 1;

    draw_glass(c, r, ox, oy);
    gui_text(c, r.x + 16 - ox, r.y + 14 - oy, "Authentication needed", TASKBAR_TEXT, 1);
    for (int i = 0, y = r.y + 40; i < 2 && *m; i++, y += 20) { // what it's for, in up to two lines
        size_t n = gui_text_fit(m, strlen(m), r.width - 32);

        while (m[n] && n > 0 && m[n] != ' ')
            n--;
        n = n ? n : gui_text_fit(m, strlen(m), r.width - 32);
        snprintf(line, sizeof(line), "%.*s", (int)n, m);
        gui_text(c, r.x + 16 - ox, y - oy, line, TASKBAR_DIM_TEXT, 1);
        for (m += n; *m == ' '; m++)
            ;
    }
    snprintf(line, sizeof(line), "Password for %s", p->user);
    gui_text(c, f.x - ox, f.y - 22 - oy, line, TASKBAR_TEXT, 1);
    memset(dots, '*', typed);
    dots[typed] = '\0';
    gui_fill_rounded(c, f.x - ox, f.y - oy, f.width, f.height, SMALL_RADIUS, TITLE_FOCUSED);
    gui_fill_rounded(c, f.x + 1 - ox, f.y + 1 - oy, f.width - 2, f.height - 2, SMALL_RADIUS, TASKBAR_COLOR);
    gui_text(c, f.x + 8 - ox, f.y + (f.height - GUI_CHAR_HEIGHT) / 2 - oy, dots, TASKBAR_TEXT, 1);
    if (!p->checking)
        gui_fill(c, f.x + 8 + gui_text_width(dots, 1) - ox, f.y + 7 - oy, 1, f.height - 14, TASKBAR_TEXT);
    gui_text(c, r.x + 16 - ox, auth_button(0).y + 6 - oy, p->checking ? "Checking..." : p->wrong ? "That password isn't right."
                                                                                                  : "",
             TASKBAR_DIM_TEXT, 1);
    draw_button(c, auth_button(0), "Cancel", false, ox, oy);
    draw_button(c, auth_button(1), "Authenticate", true, ox, oy);
}

// Shown as polkit asks, and gone as it's answered.
static void auth_changed(void) {
    bool asking = polkit_now()->asking;

    if (asking != auth_shown) {
        auth_shown = asking;
        explicit_bzero(auth_typed, sizeof(auth_typed));
        add_damage(auth_rect());
        if (asking) {
            struct rect r = auth_rect(), a = auth_button(1), b = auth_button(0);

            close_menu();
            close_panel();
            printf("wm: password dialog at %d %d %d %d\n", r.x, r.y, r.width, r.height);
            printf("wm: auth button Authenticate at %d %d %d %d\n", a.x, a.y, a.width, a.height);
            printf("wm: auth button Cancel at %d %d %d %d\n", b.x, b.y, b.width, b.height);
        }
        return;
    }
    add_damage(auth_rect());
}

static void auth_submit(void) {
    if (auth_typed[0] && !polkit_now()->checking) {
        polkit_answer(auth_typed);
        explicit_bzero(auth_typed, sizeof(auth_typed));
    }
    add_damage(auth_rect());
}

static void auth_key(const struct key_event *e) {
    size_t length = strlen(auth_typed);

    if (!e->pressed)
        return;
    if (e->code == KEY_ESCAPE)
        polkit_cancel();
    else if (e->code == KEY_ENTER || e->code == (KEY_EXTENDED | KEY_ENTER))
        auth_submit();
    else if (e->code == KEY_BACKSPACE && length)
        auth_typed[length - 1] = '\0';
    else if (e->text >= ' ' && e->text < 0x7F && length + 1 < sizeof(auth_typed))
        auth_typed[length] = (char)e->text, auth_typed[length + 1] = '\0';
    auth_changed();
}

static void auth_mouse(uint32_t pressed, bool moved) {
    if (pressed & MOUSE_BUTTON_LEFT) {
        if (contains(auth_button(0), cursor_x, cursor_y))
            polkit_cancel();
        else if (contains(auth_button(1), cursor_x, cursor_y))
            auth_submit();
    }
    if (pressed || moved)
        auth_changed();
}

static void draw_history(struct gui_canvas *c, struct rect p, int ox, int oy) {
    struct rect clear = clear_button(TRAY_NOTICES);

    gui_text(c, p.x + 12 - ox, p.y + 10 - oy, "Notifications", TASKBAR_TEXT, 1);
    if (history_count) {
        gui_fill_rounded(c, clear.x - ox, clear.y - oy, clear.width, clear.height, SMALL_RADIUS, TASKBAR_BUTTON);
        gui_text(c, clear.x + (clear.width - gui_text_width("Clear", 1)) / 2 - ox,
                 clear.y + (clear.height - GUI_CHAR_HEIGHT) / 2 - oy, "Clear", TASKBAR_TEXT, 1);
    } else {
        gui_text(c, p.x + 12 - ox, p.y + 44 - oy, "Nothing new", TASKBAR_DIM_TEXT, 1);
    }
    for (int i = 0; i < history_count; i++) {
        struct rect r = history_row(i);
        const struct notice *n = &history[i];

        if (contains(r, cursor_x, cursor_y) && (n->file[0] || n->command[0]))
            gui_fill_rounded(c, r.x - ox, r.y - oy, r.width, r.height, SMALL_RADIUS, TASKBAR_BUTTON);
        gui_text(c, r.x + r.width - 8 - gui_text_width(n->time, 1) - ox, r.y + 4 - oy, n->time, TASKBAR_DIM_TEXT, 1);
        draw_notice(c, (struct rect){r.x - 4, r.y - 4, r.width - 6 * GUI_CHAR_WIDTH, r.height}, n, ox, oy);
    }
}

static bool tile_on(int tile) {
    return tile == TILE_AIRPLANE ? net->running && !net->wifi_on : tile == TILE_NIGHT ? settings.night_light
         : tile == TILE_QUIET ? settings.quiet : !settings.light;
}

// A slider's line, filled to its knob, which is dim when what it sets is off.
static void draw_slider(struct gui_canvas *c, struct rect r, int percent, bool dim, int ox, int oy) {
    int knob = r.x + r.width * percent / 100, line = r.y + r.height / 2;
    uint32_t colour = dim ? TASKBAR_DIM_TEXT : TITLE_FOCUSED;

    gui_fill_rounded(c, r.x - ox, line - 2 - oy, r.width, 4, GUI_MAX_RADIUS, TASKBAR_LINE);
    gui_fill_rounded(c, r.x - ox, line - 2 - oy, knob - r.x, 4, GUI_MAX_RADIUS, colour);
    gui_fill_rounded(c, knob - 5 - ox, line - 8 - oy, 10, 16, GUI_MAX_RADIUS, colour);
}

// The battery, as a cell filled as far as it's charged, red when it's low, with a
// bolt while it charges.
static void draw_battery(struct gui_canvas *c, int x, int y, int ox, int oy) {
    int fill = 12 * (battery > 100 ? 100 : battery) / 100;
    uint32_t colour = battery <= 10 && !charging ? CLOSE_COLOR : charging ? 0x46BE6E : TASKBAR_TEXT;

    gui_frame_rounded(c, x - ox, y + 3 - oy, 16, 10, 2, TASKBAR_COLOR, TASKBAR_TEXT);
    gui_fill(c, x + 16 - ox, y + 6 - oy, 2, 4, TASKBAR_TEXT);
    gui_fill(c, x + 2 - ox, y + 5 - oy, fill > 0 ? fill : 1, 6, colour);
}

// The popup the volume and brightness keys show: what they set, and how far.
static void draw_osd(struct gui_canvas *c, int ox, int oy) {
    struct rect r = osd_rect();
    int level = osd_volume ? volume : brightness_percent();
    const struct gui_image *icon = gui_icon(!osd_volume ? "brightness" : muted || !volume ? "muted" : "speaker");

    draw_glass(c, r, ox, oy);
    if (icon)
        gui_draw_image(c, r.x + 16 - ox, r.y + (r.height - ICON_SIZE) / 2 - oy, icon);
    draw_slider(c, (struct rect){r.x + 44, r.y + 14, r.width - 64, 20}, level, osd_volume && muted, ox, oy);
}

// The tiles, the volume and what's playing, the brightness and battery, then the network.
static void draw_quick(struct gui_canvas *c, struct rect p, int ox, int oy) {
    static const char *const labels[] = {"Airplane", "Night light", "Dark mode", "Quiet"};
    static const char *const tile_icons[] = {"airplane", "night", "theme", "quiet"};
    int network_y = p.y + 12 + TILE_HEIGHT + 12 + sound_height() + laptop_height();

    for (int i = 0; i < TILES; i++) {
        struct rect t = quick_tile(i);
        const struct gui_image *icon = gui_icon(tile_icons[i]);
        bool on = tile_on(i);

        gui_fill_rounded(c, t.x - ox, t.y - oy, t.width, t.height, RADIUS, on ? TITLE_FOCUSED : TASKBAR_BUTTON);
        if (icon)
            gui_draw_image(c, t.x + (t.width - ICON_SIZE) / 2 - ox, t.y + 8 - oy, icon);
        gui_text(c, t.x + (t.width - gui_text_width(labels[i], 1)) / 2 - ox, t.y + t.height - GUI_CHAR_HEIGHT - 6 - oy,
                 labels[i], on ? ON_ACCENT : TASKBAR_TEXT, 1);
    }
    if (mixer >= 0) {
        struct rect r = volume_slider(), m = mute_button();
        const struct gui_image *speaker = gui_icon(muted || !volume ? "muted" : "speaker");
        char percent[8];

        snprintf(percent, sizeof(percent), "%d%%", volume);
        if (contains(m, cursor_x, cursor_y))
            gui_fill_rounded(c, m.x - ox, m.y - oy, m.width, m.height, SMALL_RADIUS, TASKBAR_BUTTON);
        if (speaker)
            gui_draw_image(c, m.x + (m.width - ICON_SIZE) / 2 - ox, m.y + (m.height - ICON_SIZE) / 2 - oy, speaker);
        draw_slider(c, r, volume, muted, ox, oy);
        gui_text(c, p.x + p.width - 12 - gui_text_width(percent, 1) - ox, r.y + (r.height - GUI_CHAR_HEIGHT) / 2 - oy,
                 percent, muted ? TASKBAR_DIM_TEXT : TASKBAR_TEXT, 1);
        if (now_playing[0]) {
            char shown[GUI_TITLE_MAX + 16];

            snprintf(shown, sizeof(shown), "%.*s", (int)gui_text_fit(now_playing, strlen(now_playing), p.width - 24), now_playing);
            gui_text(c, p.x + 12 - ox, m.y + 28 - oy, shown, TASKBAR_DIM_TEXT, 1);
        }
        for (int i = 0; output_count > 1 && i < output_count; i++) {
            struct rect o = output_row(i);
            char name[sizeof(outputs[i].name)];

            snprintf(name, sizeof(name), "%.*s", (int)gui_text_fit(outputs[i].name, strlen(outputs[i].name), o.width - 16),
                     outputs[i].name);
            if (outputs[i].current || contains(o, cursor_x, cursor_y))
                gui_fill_rounded(c, o.x - ox, o.y - oy, o.width, o.height, SMALL_RADIUS,
                                 outputs[i].current ? TITLE_FOCUSED : TASKBAR_BUTTON);
            gui_text(c, o.x + 8 - ox, o.y + (o.height - GUI_CHAR_HEIGHT) / 2 - oy, name,
                     outputs[i].current ? ON_ACCENT : TASKBAR_TEXT, 1);
        }
    }
    if (backlight[0]) {
        struct rect r = brightness_slider(), i = sun_icon();
        const struct gui_image *sun = gui_icon("brightness");
        char percent[8];

        snprintf(percent, sizeof(percent), "%d%%", brightness_percent());
        if (sun)
            gui_draw_image(c, i.x + (i.width - ICON_SIZE) / 2 - ox, i.y + (i.height - ICON_SIZE) / 2 - oy, sun);
        draw_slider(c, r, brightness_percent(), false, ox, oy);
        gui_text(c, p.x + p.width - 12 - gui_text_width(percent, 1) - ox, r.y + (r.height - GUI_CHAR_HEIGHT) / 2 - oy,
                 percent, TASKBAR_TEXT, 1);
    }
    if (battery >= 0) {
        int y = sun_icon().y + (backlight[0] ? 32 : 0);
        char text[48];

        snprintf(text, sizeof(text), "%d%%%s", battery, !charging ? "" : battery >= 100 ? ", full" : ", plugged in");
        draw_battery(c, p.x + 13, y + 2, ox, oy);
        gui_text(c, p.x + 44 - ox, y + (20 - GUI_CHAR_HEIGHT) / 2 + 2 - oy, text, TASKBAR_TEXT, 1);
    }
    gui_fill(c, p.x + 12 - ox, network_y - oy, p.width - 24, 1, TASKBAR_LINE);
    draw_network(c, p, network_y + 9, ox, oy);
}

static void draw_calendar(struct gui_canvas *c, struct rect p, int ox, int oy) {
    static const char *const days[] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
    static const int lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    time_t now = time(NULL);
    struct tm today = *localtime(&now);
    int year, month;
    char title[32];

    shown_month(&year, &month);
    snprintf(title, sizeof(title), "%s %d", month_names[month], year);
    gui_text(c, p.x + (p.width - gui_text_width(title, 1)) / 2 - ox, p.y + 10 - oy, title, TASKBAR_TEXT, 1);
    for (int next = 0; next < 2; next++) {
        struct rect a = month_arrow(next);

        gui_fill_rounded(c, a.x - ox, a.y - oy, a.width, a.height, SMALL_RADIUS, TASKBAR_BUTTON);
        gui_text(c, a.x + 8 - ox, a.y + 4 - oy, next ? ">" : "<", TASKBAR_TEXT, 1);
    }

    int left = p.x + 12, top = p.y + 40, first = weekday(year, month, 1);
    int length = lengths[month] + (month == 1 && leap(year));

    for (int i = 0; i < 7; i++)
        gui_text(c, left + i * DAY_WIDTH + 8 - ox, top + 4 - oy, days[i], TASKBAR_DIM_TEXT, 1);
    for (int day = 1; day <= length; day++) {
        int cell = first + day - 1, x = left + cell % 7 * DAY_WIDTH, y = top + (cell / 7 + 1) * DAY_HEIGHT;
        bool is_today = day == today.tm_mday && month == today.tm_mon && year == today.tm_year + 1900;
        char number[4];

        snprintf(number, sizeof(number), "%d", day);
        if (is_today)
            gui_fill_rounded(c, x + 2 - ox, y - oy, DAY_WIDTH - 4, DAY_HEIGHT - 2, SMALL_RADIUS, TITLE_FOCUSED);
        gui_text(c, x + DAY_WIDTH - 8 - gui_text_width(number, 1) - ox, y + 3 - oy, number,
                 is_today ? ON_ACCENT : cell % 7 >= 5 ? TASKBAR_DIM_TEXT : TASKBAR_TEXT, 1);
    }
}

// The first line of what was copied, as far as it fits, with anything that isn't
// printable as a space.
static void draw_clips(struct gui_canvas *c, struct rect p, int ox, int oy) {
    struct rect clear = clear_button(TRAY_CLIPS);

    gui_text(c, p.x + 12 - ox, p.y + 10 - oy, "Clipboard", TASKBAR_TEXT, 1);
    if (!clip_count)
        gui_text(c, p.x + 12 - ox, p.y + 44 - oy, "Nothing copied yet", TASKBAR_DIM_TEXT, 1);
    else {
        gui_fill_rounded(c, clear.x - ox, clear.y - oy, clear.width, clear.height, SMALL_RADIUS, TASKBAR_BUTTON);
        gui_text(c, clear.x + (clear.width - gui_text_width("Clear", 1)) / 2 - ox,
                 clear.y + (clear.height - GUI_CHAR_HEIGHT) / 2 - oy, "Clear", TASKBAR_TEXT, 1);
    }
    for (int i = 0; i < clip_count; i++) {
        struct rect r = clip_row(i);
        char line[GUI_TITLE_MAX];
        size_t n = 0;

        while (n < clip_lengths[i] && n < sizeof(line) - 1 && clips[i][n] != '\n') {
            line[n] = clips[i][n] >= ' ' && clips[i][n] < 127 ? clips[i][n] : ' ';
            n++;
        }
        line[n] = '\0';
        if (contains(r, cursor_x, cursor_y))
            gui_fill_rounded(c, r.x - ox, r.y - oy, r.width, r.height, SMALL_RADIUS, TASKBAR_BUTTON);
        fitted_text(c, r.x + 8 - ox, r.y + (r.height - GUI_CHAR_HEIGHT) / 2 - oy, r.width - 16, line,
                    i ? TASKBAR_TEXT : TITLE_FOCUSED);
    }
}

static void draw_open_panel(struct gui_canvas *c, int unused, int ox, int oy) {
    struct rect p = panel_rect(panel);

    (void)unused;

    draw_glass(c, p, ox, oy);
    if (panel == TRAY_QUICK)
        draw_quick(c, p, ox, oy);
    else if (panel == TRAY_NOTICES)
        draw_history(c, p, ox, oy);
    else if (panel == TRAY_CLIPS)
        draw_clips(c, p, ox, oy);
    else if (panel == TRAY_NETWORK)
        draw_network_panel(c, p, ox, oy);
    else if (panel == TRAY_BLUETOOTH)
        draw_bluetooth_panel(c, p, ox, oy);
    else
        draw_calendar(c, p, ox, oy);
}

static void draw_tray(struct gui_canvas *c, int ox, int oy) {
    struct rect q = tray_item(TRAY_QUICK), b = tray_item(TRAY_NOTICES), k = tray_item(TRAY_CLIPS);
    struct rect n = tray_item(TRAY_NETWORK), t = tray_item(TRAY_BLUETOOTH);
    const struct gui_image *clip = gui_icon("clipboard"), *theme_icon = gui_icon("theme");
    const struct gui_image *bluetooth = bt->adapter ? gui_icon(bt_on() ? "bluetooth" : "bluetooth-off") : NULL;
    const struct gui_image *network = gui_icon(connected() ? "network" : net->running && !net->wifi_on ? "airplane" : "offline");
    const struct gui_image *speaker = gui_icon(muted || !volume ? "muted" : "speaker"), *bell = gui_icon("bell");
    int icon_y = q.y + (q.height - ICON_SIZE) / 2;

    for (int i = 0; i < TRAY_ITEMS; i++) {
        struct rect t = tray_item(i);

        if (t.width && (panel == i || contains(t, cursor_x, cursor_y)))
            gui_fill_rounded(c, t.x - ox, t.y - oy, t.width, t.height, SMALL_RADIUS, TASKBAR_BUTTON);
    }
    if (bluetooth)
        gui_draw_image(c, t.x + (t.width - ICON_SIZE) / 2 - ox, icon_y - oy, bluetooth);
    if (battery >= 0) {
        char percent[8];

        snprintf(percent, sizeof(percent), "%d%%", battery);
        draw_battery(c, q.x + 4, icon_y, ox, oy);
        gui_text(c, q.x + 24 - ox, q.y + (q.height - GUI_CHAR_HEIGHT) / 2 - oy, percent, TASKBAR_TEXT, 1);
    }
    if (network)
        gui_draw_image(c, n.x + (n.width - ICON_SIZE) / 2 - ox, icon_y - oy, network);
    if (theme_icon && battery < 0 && mixer < 0)
        gui_draw_image(c, q.x + (q.width - ICON_SIZE) / 2 - ox, icon_y - oy, theme_icon);
    if (speaker && mixer >= 0)
        gui_draw_image(c, q.x + q.width - 4 - ICON_SIZE - ox, icon_y - oy, speaker);
    if (bell)
        gui_draw_image(c, b.x + (b.width - ICON_SIZE) / 2 - ox, icon_y - oy, bell);
    if (clip)
        gui_draw_image(c, k.x + (k.width - ICON_SIZE) / 2 - ox, icon_y - oy, clip);
    if (unseen)
        gui_fill_rounded(c, b.x + b.width - 8 - ox, b.y + 1 - oy, 6, 6, GUI_MAX_RADIUS, CLOSE_COLOR);
}

static void open_file(const char *path);
static void launch(const char *command);
static void save_settings(const char *what);

static void clear_clips(void) {
    for (int i = 0; i < clip_count; i++)
        free(clips[i]);
    clip_count = 0;
    if (panel == TRAY_CLIPS)
        add_damage(panel_rect(TRAY_CLIPS));
}

// Text put on the clipboard goes first in its history, coming up from further
// down if it was there already.
static void remember_clipboard(void) {
    char *copy;
    int found = clip_count == CLIPS_MAX ? CLIPS_MAX - 1 : -1;

    if (!clipboard || clipboard_width || !clipboard_length || !(copy = malloc(clipboard_length)))
        return;
    memcpy(copy, clipboard, clipboard_length);
    if (panel == TRAY_CLIPS)
        add_damage(panel_rect(TRAY_CLIPS));
    for (int i = 0; i < clip_count; i++) {
        if (clip_lengths[i] == clipboard_length && !memcmp(clips[i], clipboard, clipboard_length))
            found = i;
    }
    if (found >= 0) {
        free(clips[found]);
        memmove(&clips[found], &clips[found + 1], (clip_count - found - 1) * sizeof(*clips));
        memmove(&clip_lengths[found], &clip_lengths[found + 1], (clip_count - found - 1) * sizeof(*clip_lengths));
        clip_count--;
    }
    memmove(&clips[1], &clips[0], clip_count * sizeof(*clips));
    memmove(&clip_lengths[1], &clip_lengths[0], clip_count * sizeof(*clip_lengths));
    clips[0] = copy;
    clip_lengths[0] = clipboard_length;
    clip_count++;
    if (panel == TRAY_CLIPS)
        add_damage(panel_rect(TRAY_CLIPS));
}

// Puts what was copied before back on the clipboard.
static void use_clip(int index) {
    char *copy = malloc(clip_lengths[index]);

    if (!copy)
        return;
    memcpy(copy, clips[index], clip_lengths[index]);
    free(clipboard);
    clipboard = copy;
    clipboard_length = clip_lengths[index];
    clipboard_width = clipboard_height = 0;
    printf("wm: clipboard from history %d\n", index);
    server_set_clipboard(clipboard, clipboard_length, 0, 0);
    remember_clipboard();
}

// A tile in quick settings turns what it's for on or off.
static void toggle_tile(int tile) {
    if (tile == TILE_AIRPLANE) { // NetworkManager's, turning every radio off
        if (network_set_wifi(!net->wifi_on))
            printf("wm: airplane %s\n", net->wifi_on ? "on" : "off");
        check_network();
    } else if (tile == TILE_NIGHT) {
        settings.night_light = !settings.night_light;
        save_settings(settings.night_light ? "night light on" : "night light off");
    } else if (tile == TILE_QUIET) {
        settings.quiet = !settings.quiet;
        save_settings(settings.quiet ? "quiet on" : "quiet off");
    } else {
        settings.light = !settings.light;
        save_settings(settings.light ? "theme light" : "theme dark");
    }
}

// A click in an open panel does what it's on there, returning false if it's
// outside the panel.
static bool panel_click(void) {
    if (!contains(panel_rect(panel), cursor_x, cursor_y))
        return false;
    if (panel == TRAY_NETWORK) {
        network_click();
        return true;
    }
    if (panel == TRAY_BLUETOOTH) {
        bluetooth_click();
        return true;
    }
    for (int i = 0; panel == TRAY_QUICK && i < TILES; i++) {
        if (contains(quick_tile(i), cursor_x, cursor_y))
            toggle_tile(i);
    }
    if (panel == TRAY_QUICK && mixer >= 0 && contains(volume_slider(), cursor_x, cursor_y)) {
        setting_volume = true;
        set_volume_at(cursor_x);
    } else if (panel == TRAY_QUICK && backlight[0] && contains(brightness_slider(), cursor_x, cursor_y)) {
        setting_brightness = true;
        set_brightness_at(cursor_x);
    } else if (panel == TRAY_QUICK && mixer >= 0 && contains(mute_button(), cursor_x, cursor_y)) {
        set_volume(volume, !muted);
        play_sound(SOUND_VOLUME);
    } else if (panel == TRAY_QUICK && mixer >= 0 && output_count > 1) {
        for (int i = 0; i < output_count; i++) {
            if (contains(output_row(i), cursor_x, cursor_y) && !outputs[i].current && audio_set_output(outputs[i].key))
                printf("wm: playing through %s\n", outputs[i].name);
        }
    } else if (panel == TRAY_NOTICES && history_count && contains(clear_button(TRAY_NOTICES), cursor_x, cursor_y)) {
        clear_history();
        printf("wm: notifications cleared\n");
    } else if (panel == TRAY_CALENDAR && (contains(month_arrow(false), cursor_x, cursor_y)
                                          || contains(month_arrow(true), cursor_x, cursor_y))) {
        int year, month;

        calendar_month += contains(month_arrow(true), cursor_x, cursor_y) ? 1 : -1;
        shown_month(&year, &month);
        printf("wm: calendar %s %d\n", month_names[month], year);
        add_damage(panel_rect(TRAY_CALENDAR));
    }
    if (panel == TRAY_CLIPS && clip_count && contains(clear_button(TRAY_CLIPS), cursor_x, cursor_y)) {
        clear_clips();
        printf("wm: clipboard history cleared\n");
        return true;
    }
    for (int i = 0; panel == TRAY_CLIPS && i < clip_count; i++) {
        if (contains(clip_row(i), cursor_x, cursor_y)) {
            use_clip(i);
            close_panel();
            return true;
        }
    }
    for (int i = 0; panel == TRAY_NOTICES && i < history_count; i++) {
        if (contains(history_row(i), cursor_x, cursor_y) && (history[i].file[0] || history[i].command[0])) {
            if (history[i].file[0])
                open_file(history[i].file);
            else
                launch(history[i].command);
            close_panel();
        }
    }
    return true;
}

static void draw_taskbar(struct gui_canvas *c, int ox, int oy) {
    struct rect bar = taskbar(), l = launcher();
    struct window *list[MAX_WINDOWS];
    int count = windows_by_age(list);
    int text_y = bar.y + (TASKBAR_HEIGHT - GUI_CHAR_HEIGHT) / 2 - oy;
    char clock[16];
    const char *shown = clock_text(clock);

    draw_glass(c, bar, ox, oy);
    gui_fill_rounded(c, l.x - ox, l.y - oy, l.width, l.height, SMALL_RADIUS, TASKBAR_BUTTON);
    gui_text(c, l.x + (l.width - gui_text_width("Apps", 1)) / 2 - ox, text_y, "Apps", TASKBAR_TEXT, 1);

    // A box for each workspace, the one shown in the accent colour, with a dot in
    // those that have windows.
    for (int i = 0; i < WORKSPACES; i++) {
        struct rect b = workspace_box(i);
        bool used = false;

        for (int j = 0; j < window_count && !used; j++)
            used = !windows[j]->bare && windows[j]->workspace == i;
        gui_fill_rounded(c, b.x - ox, b.y - oy, b.width, b.height, SMALL_RADIUS, i == workspace ? TITLE_FOCUSED : TASKBAR_BUTTON);
        if (used)
            gui_fill_rounded(c, b.x + b.width / 2 - 2 - ox, b.y + b.height / 2 - 2 - oy, 4, 4, GUI_MAX_RADIUS, i == workspace ? ON_ACCENT : TASKBAR_TEXT);
    }

    for (int i = 0; i < count; i++) {
        struct rect b = taskbar_button(i, count);
        struct window *w = list[i];
        uint32_t color = w == focused ? TITLE_FOCUSED : w->minimized ? TASKBAR_MINIMIZED : TASKBAR_BUTTON;

        gui_fill_rounded(c, b.x - ox, b.y - oy, b.width, b.height, SMALL_RADIUS, color);
        draw_icon(c, w->app, b.x + 4, b.y + (b.height - ICON_SIZE) / 2, ox, oy);
        fitted_text(c, b.x + 8 + ICON_SIZE - ox, text_y, b.width - 12 - ICON_SIZE, w->title,
                    w == focused ? ON_ACCENT : w->minimized ? TASKBAR_DIM_TEXT : TASKBAR_TEXT);
    }

    if (panel == TRAY_CALENDAR) {
        struct rect t = tray_item(TRAY_CALENDAR);

        gui_fill_rounded(c, t.x - ox, t.y - oy, t.width, t.height, SMALL_RADIUS, TASKBAR_BUTTON);
    }
    gui_text(c, bar.x + bar.width - gui_text_width(shown, 1) - 12 - ox, text_y, shown, TASKBAR_TEXT, 1);
    draw_tray(c, ox, oy);
}

// A row in the menu or the switcher, with an icon, if it's not NULL.
static void draw_item(struct gui_canvas *c, struct rect r, const char *text, const struct gui_image *icon, bool chosen,
                      uint32_t color, int ox, int oy) {
    int text_x = 12 + ICON_SIZE;

    if (chosen)
        gui_fill_rounded(c, r.x - ox, r.y - oy, r.width, r.height, SMALL_RADIUS, TITLE_FOCUSED);
    if (icon)
        gui_draw_image(c, r.x + 6 - ox, r.y + (r.height - ICON_SIZE) / 2 - oy, icon);
    fitted_text(c, r.x + text_x - ox, r.y + (r.height - GUI_CHAR_HEIGHT) / 2 - oy, r.width - text_x - 8, text,
                chosen ? ON_ACCENT : color);
}

static int item_height(const struct item *item) {
    return item->action == ACT_LINE ? MENU_GAP : ITEM_HEIGHT;
}

static bool has_submenu(const struct item *item) {
    return item->action == ACT_FOLDER || item->action == ACT_OPEN_WITH || item->action == ACT_POSITION;
}

static bool choosable(const struct item *item) {
    return item->action != ACT_NONE && item->action != ACT_LINE;
}

static struct rect item_rect(const struct popup *p, int index) {
    int y = p->r.y + 4;

    for (int i = 0; i < index; i++)
        y += item_height(&p->items[i]);
    return (struct rect){p->r.x + 4, y, p->r.width - 8, item_height(&p->items[index])};
}

static int item_at(const struct popup *p, int x, int y) {
    for (int i = 0; i < p->count; i++) {
        if (choosable(&p->items[i]) && contains(item_rect(p, i), x, y))
            return i;
    }
    return -1;
}

static void damage_popups(void) {
    for (int i = 0; i < popup_depth; i++)
        add_damage(popups[i].r);
}

// A menu width wide at x, y, or left of x and above y if it doesn't fit there,
// kept on the screen.
static void place_popup(struct popup *p, int x, int y, int width) {
    int height = 8;

    for (int i = 0; i < p->count; i++)
        height += item_height(&p->items[i]);
    x = x + width > screen.width ? x - width : x;
    y = y + height > screen.height ? screen.height - height : y;
    p->r = (struct rect){x < 0 ? 0 : x, y < 0 ? 0 : y, width, height};
}

// A tick in a box.
static void draw_check(struct gui_canvas *c, int x, int y, bool on, int ox, int oy) {
    gui_frame_rounded(c, x - ox, y - oy, 14, 14, SMALL_RADIUS, on ? TITLE_FOCUSED : TASKBAR_COLOR, TASKBAR_TEXT);
}

static void draw_popup(struct gui_canvas *c, int level, int ox, int oy) {
    const struct popup *p = &popups[level];

    draw_glass(c, p->r, ox, oy);
    for (int i = 0; i < p->count; i++) {
        const struct item *item = &p->items[i];
        struct rect r = item_rect(p, i);
        bool chosen = i == p->choice;

        if (item->action == ACT_LINE) {
            gui_fill(c, r.x + 4 - ox, r.y + MENU_GAP / 2 - oy, r.width - 8, 1, TASKBAR_LINE);
            continue;
        }
        draw_item(c, r, item->label, item->icon, chosen && choosable(item),
                  item->action == ACT_NONE ? TASKBAR_DIM_TEXT : TASKBAR_TEXT, ox, oy);
        if (item->check)
            draw_check(c, r.x + 7, r.y + (r.height - 14) / 2, item->checked, ox, oy);
        if (has_submenu(item))
            gui_text(c, r.x + r.width - 14 - ox, r.y + (r.height - GUI_CHAR_HEIGHT) / 2 - oy, ">",
                     chosen ? ON_ACCENT : TASKBAR_DIM_TEXT, 1);
    }
}

static void draw_switcher(struct gui_canvas *c, int ox, int oy) {
    struct rect s = switcher();

    draw_glass(c, s, ox, oy);
    for (int i = 0; i < window_count; i++) {
        struct window *w = switcher_window(i);

        draw_item(c, (struct rect){s.x + 4, s.y + 4 + i * ITEM_HEIGHT, s.width - 8, ITEM_HEIGHT}, w->title, app_icon(w->app),
                  i == switch_choice, w->minimized ? TASKBAR_DIM_TEXT : TASKBAR_TEXT, ox, oy);
    }
}

// The shape's character at x, y, as it's shown, turned or mirrored, and ' ' outside it.
static char cursor_at(const struct cursor_shape *shape, struct rect r, int x, int y) {
    if (x < 0 || y < 0 || x >= r.width || y >= r.height)
        return ' ';

    int col = shape->turned ? y : x, row = shape->turned ? x : y;

    return shape->rows[row][shape->mirrored ? shape->width - 1 - col : col];
}

// At 2x by Scale2x, which makes each pixel four, following its neighbours' edges, so
// the diagonals stay smooth rather than going to steps.
static void draw_cursor(struct gui_canvas *c, int ox, int oy) {
    const struct cursor_shape *shape = &shapes[cursor_shown];
    struct rect r = cursor_rect();
    int k = scale == 2 ? 2 : 1;

    for (int y = 0; y < r.height; y++) {
        for (int x = 0; x < r.width; x++) {
            char p = cursor_at(shape, r, x, y), up = cursor_at(shape, r, x, y - 1), down = cursor_at(shape, r, x, y + 1);
            char left = cursor_at(shape, r, x - 1, y), right = cursor_at(shape, r, x + 1, y);
            char quarter[4] = {p, p, p, p};

            if (k == 2 && up != down && left != right) {
                quarter[0] = left == up ? up : p, quarter[1] = up == right ? right : p;
                quarter[2] = left == down ? left : p, quarter[3] = down == right ? right : p;
            }
            for (int q = 0; q < k * k; q++) {
                int px = (r.x + x - ox) * scale + q % k * (scale / k), py = (r.y + y - oy) * scale + q / k * (scale / k);

                if (quarter[q] == ' ' || px < 0 || py < 0 || px >= c->width * scale || py >= c->height * scale)
                    continue;
                for (int i = 0; i < scale / k; i++) // past 2x, each quarter's square filled
                    for (int j = 0; j < scale / k; j++)
                        c->pixels[(long)(py + i) * c->stride + px + j] = quarter[q] == 'X' ? 0x000000 : 0xFFFFFF;
            }
        }
    }
}

static bool no_cursor; // while composing a screenshot

// Where a dragged window would snap to: an outline, over a quarter of the accent colour.
static void draw_snap(struct gui_canvas *c, int ox, int oy) {
    struct rect r = snap_rect(snap), inner = {r.x + SNAP_BORDER, r.y + SNAP_BORDER, r.width - 2 * SNAP_BORDER,
                                             r.height - 2 * SNAP_BORDER};
    int x0 = r.x > ox ? r.x : ox, x1 = r.x + r.width < ox + c->width ? r.x + r.width : ox + c->width;
    int y0 = r.y > oy ? r.y : oy, y1 = r.y + r.height < oy + c->height ? r.y + r.height : oy + c->height;
    uint32_t a = TITLE_FOCUSED;

    inner = scaled(inner);
    for (int y = y0 * scale; y < y1 * scale; y++) {
        uint32_t *row = c->pixels + (long)(y - oy * scale) * c->stride - ox * scale;

        for (int x = x0 * scale; x < x1 * scale; x++) {
            uint32_t p = row[x];

            row[x] = !contains(inner, x, y) ? a
                   : ((((p & 0xFF00FF) * 3 + (a & 0xFF00FF)) >> 2) & 0xFF00FF)
                         | ((((p & 0x00FF00) * 3 + (a & 0x00FF00)) >> 2) & 0x00FF00);
        }
    }
}

// Icons on the desktop for what's in Desktop in the home folder, on a grid, with
// pictures as small copies of themselves. Clicking one selects it, double-clicking
// opens it, and dragging it moves it to another place on the grid, or into a
// folder there, or takes it to a window, as from Files. A file from Files dropped
// on the desktop is copied there. Where each goes is kept in LAYOUT, a line each:
// its column, its row and its name.
#define DESKTOP_ICON 32
#define DESKTOP_CELL_WIDTH 88
#define DESKTOP_CELL_HEIGHT 72
#define MAX_DESKTOP 64
#define DOUBLE_CLICK_MS 400
#define DRAG_DISTANCE 6
#define LAYOUT gui_own_path("desktop")
struct desktop_item {
    char name[256];
    bool folder;
    time_t changed;
    off_t size;
    int column, row;
    struct gui_image picture;
};
static struct desktop_item desktop_items[MAX_DESKTOP];
static int desktop_count, desktop_selected = -1, desktop_pressed = -1, pressed_x, pressed_y;
static int desktop_dragged = -1; // the one being dragged
static int64_t desktop_clicked; // when the one selected was clicked
static char desktop_dir[GUI_PATH_MAX];

// The grid, from the desktop's top left, leaving room for the hint at the bottom.
static int desktop_rows(void) {
    int rows = (desktop().height - 3 * GUI_CHAR_HEIGHT - 8) / DESKTOP_CELL_HEIGHT;

    return rows > 0 ? rows : 1;
}

static int desktop_columns(void) {
    int columns = (desktop().width - 8) / DESKTOP_CELL_WIDTH;

    return columns > 0 ? columns : 1;
}

static struct rect grid_cell(int column, int row) {
    struct rect d = desktop();

    return (struct rect){d.x + 8 + column * DESKTOP_CELL_WIDTH, d.y + 8 + row * DESKTOP_CELL_HEIGHT,
                         DESKTOP_CELL_WIDTH - 4, DESKTOP_CELL_HEIGHT - 4};
}

static struct rect desktop_cell(int index) {
    return grid_cell(desktop_items[index].column, desktop_items[index].row);
}

// The cell a point is in, kept on the grid.
static void cell_at(int x, int y, int *column, int *row) {
    struct rect d = desktop();

    *column = (x - d.x - 8) / DESKTOP_CELL_WIDTH;
    *row = (y - d.y - 8) / DESKTOP_CELL_HEIGHT;
    *column = *column < 0 ? 0 : *column >= desktop_columns() ? desktop_columns() - 1 : *column;
    *row = *row < 0 ? 0 : *row >= desktop_rows() ? desktop_rows() - 1 : *row;
}

static void desktop_path(int index, char out[GUI_PATH_MAX]) {
    snprintf(out, GUI_PATH_MAX, "%s/%s", desktop_dir, desktop_items[index].name);
}

static int desktop_item_at(int x, int y) {
    for (int i = 0; i < desktop_count && !settings.hide_icons; i++) {
        if (contains(desktop_cell(i), x, y))
            return i;
    }
    return -1;
}

static int item_in_cell(int column, int row) {
    for (int i = 0; i < desktop_count; i++) {
        if (desktop_items[i].column == column && desktop_items[i].row == row)
            return i;
    }
    return -1;
}

static int compare_desktop(const void *a, const void *b) {
    const struct desktop_item *x = a, *y = b;

    return x->folder != y->folder ? y->folder - x->folder : strcmp(x->name, y->name);
}

// A picture shrunk to fit, or an icon made bigger.
static void make_picture(struct desktop_item *item, const char *path) {
    const struct gui_image *icon = gui_file_icon(item->name, item->folder);
    struct gui_image full;

    // At the display's own resolution.
    if (icon == gui_icon("image") && gui_image_load(path, &full)) {
        int big = full.width > full.height ? full.width : full.height, w = full.width * DESKTOP_ICON * scale / big,
            h = full.height * DESKTOP_ICON * scale / big;

        if (!gui_image_scale(&full, w > 0 ? w : 1, h > 0 ? h : 1, &item->picture))
            item->picture = (struct gui_image){0};
        gui_image_free(&full);
    } else if (!icon || !gui_image_scale(icon, DESKTOP_ICON * scale, DESKTOP_ICON * scale, &item->picture)) {
        item->picture = (struct gui_image){0};
    }
    item->picture.scale = scale;
}

static void save_layout(void) {
    FILE *f = fopen(LAYOUT, "w");

    for (int i = 0; f && i < desktop_count; i++)
        fprintf(f, "%d %d %s\n", desktop_items[i].column, desktop_items[i].row, desktop_items[i].name);
    if (f)
        fclose(f);
}

// Each item goes where LAYOUT says, if it's on the grid and free, and the rest in
// the first free cells, down each column from the left.
static void lay_out(void) {
    FILE *f = fopen(LAYOUT, "r");
    char line[300], name[256];
    int column, row;

    for (int i = 0; i < desktop_count; i++)
        desktop_items[i].column = desktop_items[i].row = -1;
    while (f && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        if (sscanf(line, "%d %d %255[^\n]", &column, &row, name) != 3 || column < 0 || row < 0
            || column >= desktop_columns() || row >= desktop_rows() || item_in_cell(column, row) >= 0)
            continue;
        for (int i = 0; i < desktop_count; i++) {
            if (!strcmp(desktop_items[i].name, name) && desktop_items[i].column < 0) {
                desktop_items[i].column = column;
                desktop_items[i].row = row;
            }
        }
    }
    if (f)
        fclose(f);
    for (int i = 0, cell = 0; i < desktop_count; i++) {
        while (desktop_items[i].column < 0 && cell < desktop_columns() * desktop_rows()) {
            if (item_in_cell(cell / desktop_rows(), cell % desktop_rows()) < 0) {
                desktop_items[i].column = cell / desktop_rows();
                desktop_items[i].row = cell % desktop_rows();
            }
            cell++;
        }
    }
    save_layout();
}

// Lists Desktop again, redrawing the desktop only if something's changed, or always.
static void load_desktop(bool always) {
    static struct desktop_item found[MAX_DESKTOP];
    DIR *dir = opendir(desktop_dir);
    struct dirent *d;
    int count = 0;

    while (dir && count < MAX_DESKTOP && (d = readdir(dir))) {
        char path[GUI_PATH_MAX];
        struct stat st;

        if (d->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "%s/%s", desktop_dir, d->d_name);
        if (stat(path, &st) < 0)
            continue;
        found[count] = (struct desktop_item){.folder = S_ISDIR(st.st_mode), .changed = st.st_mtime, .size = st.st_size};
        snprintf(found[count++].name, sizeof(found[0].name), "%s", d->d_name);
    }
    if (dir)
        closedir(dir);
    qsort(found, count, sizeof(*found), compare_desktop);

    bool same = count == desktop_count && !always;

    for (int i = 0; same && i < count; i++)
        same = !strcmp(found[i].name, desktop_items[i].name) && found[i].changed == desktop_items[i].changed
            && found[i].size == desktop_items[i].size && found[i].folder == desktop_items[i].folder;
    if (same)
        return;
    for (int i = 0; i < desktop_count; i++) {
        add_damage(desktop_cell(i));
        gui_image_free(&desktop_items[i].picture);
    }
    memcpy(desktop_items, found, count * sizeof(*found));
    desktop_count = count;
    desktop_selected = desktop_pressed = desktop_dragged = -1;
    lay_out();
    printf("wm: desktop:");
    for (int i = 0; i < count; i++) {
        char path[GUI_PATH_MAX];

        desktop_path(i, path);
        make_picture(&desktop_items[i], path);
        add_damage(desktop_cell(i));
        printf(" %s", desktop_items[i].name);
    }
    printf("\n");
    for (int i = 0; i < count; i++) {
        struct rect r = desktop_cell(i);

        printf("wm: desktop icon at %d %d %d %d, %s\n", r.x, r.y, r.width, r.height, desktop_items[i].name);
    }
}

// Half the colour over what's there.
static void tint(struct gui_canvas *c, struct rect r, int ox, int oy, uint32_t colour) {
    int w = c->width * scale, h = c->height * scale;

    r = scaled((struct rect){r.x - ox, r.y - oy, r.width, r.height});
    for (int y = r.y < 0 ? 0 : r.y; y < r.y + r.height && y < h; y++) {
        uint32_t *row = c->pixels + (long)y * c->stride;

        for (int x = r.x < 0 ? 0 : r.x; x < r.x + r.width && x < w; x++)
            row[x] = ((row[x] >> 1) & 0x7F7F7F) + ((colour >> 1) & 0x7F7F7F);
    }
}

static char desktop_renaming[256]; // what's being renamed, below
static void draw_rename_field(struct gui_canvas *c, int i, int ox, int oy);

static void draw_desktop(struct gui_canvas *c, int ox, int oy) {
    for (int i = 0; i < desktop_count && !settings.hide_icons; i++) {
        struct rect r = desktop_cell(i);
        const struct desktop_item *item = &desktop_items[i];
        char label[12];
        int width;

        if (i == desktop_selected)
            tint(c, r, ox, oy, TITLE_FOCUSED);
        if (item->picture.pixels)
            gui_draw_image(c, r.x + (r.width - item->picture.width / scale) / 2 - ox,
                           r.y + 6 + (DESKTOP_ICON - item->picture.height / scale) / 2 - oy, &item->picture);
        if (!strcmp(item->name, desktop_renaming)) {
            draw_rename_field(c, i, ox, oy);
            continue;
        }
        snprintf(label, sizeof(label), "%.10s", item->name);
        width = gui_text_width(label, 1);
        gui_text(c, r.x + (r.width - width) / 2 + 1 - ox, r.y + DESKTOP_ICON + 13 - oy, label, 0x000000, 1);
        gui_text(c, r.x + (r.width - width) / 2 - ox, r.y + DESKTOP_ICON + 12 - oy, label, 0xFFFFFF, 1);
    }
}

static void select_desktop_item(int index) {
    if (desktop_selected >= 0)
        add_damage(desktop_cell(desktop_selected));
    desktop_selected = index;
    if (index >= 0)
        add_damage(desktop_cell(index));
}

// Renaming what's on the desktop where its name's shown, as Windows does: the name
// but for its extension is selected, typing changes it, Enter or a click elsewhere
// keeps it, and Escape doesn't.
static char desktop_new_name[256];
static size_t rename_at, rename_anchor; // the cursor, and the other end of what's selected

static int renamed_index(void) {
    for (int i = 0; desktop_renaming[0] && i < desktop_count; i++) {
        if (!strcmp(desktop_items[i].name, desktop_renaming))
            return i;
    }
    return -1;
}

// The box, under its icon, as wide as the name, from the cell's width to three cells'.
static struct rect rename_field(int i) {
    struct rect r = desktop_cell(i);
    int width = gui_text_width(desktop_new_name, 1) + 16;

    width = width < r.width ? r.width : width > 3 * r.width ? 3 * r.width : width;
    return (struct rect){r.x + (r.width - width) / 2, r.y + DESKTOP_ICON + 9, width, GUI_CHAR_HEIGHT + 6};
}

static void damage_rename(void) {
    int i = renamed_index();

    if (i >= 0) {
        add_damage(rename_field(i));
        add_damage(desktop_cell(i));
    }
}

static void start_desktop_rename(const char *name) {
    const char *dot;

    damage_rename();
    snprintf(desktop_renaming, sizeof(desktop_renaming), "%s", name);
    snprintf(desktop_new_name, sizeof(desktop_new_name), "%s", name);

    int i = renamed_index();

    if (i < 0) {
        desktop_renaming[0] = '\0';
        return;
    }
    dot = desktop_items[i].folder ? NULL : strrchr(desktop_new_name, '.');
    rename_anchor = 0;
    rename_at = dot && dot > desktop_new_name ? (size_t)(dot - desktop_new_name) : strlen(desktop_new_name);
    select_desktop_item(i);
    damage_rename();

    struct rect f = rename_field(i);

    printf("wm: renaming %s on the desktop, in a box at %d %d %d %d\n", name, f.x, f.y, f.width, f.height);
}

// Done, keeping the new name or not. The layout has it under its new name, so it
// stays where it was on the grid.
static void desktop_rename_done(bool keep) {
    int i = renamed_index();
    char from[GUI_PATH_MAX], to[GUI_PATH_MAX];

    damage_rename();
    if (keep && i >= 0 && desktop_new_name[0] && strcmp(desktop_new_name, desktop_renaming) && !strchr(desktop_new_name, '/')) {
        snprintf(from, sizeof(from), "%s/%s", desktop_dir, desktop_renaming);
        snprintf(to, sizeof(to), "%s/%s", desktop_dir, desktop_new_name);
        if (access(to, F_OK) == 0 || rename(from, to) < 0) {
            printf("wm: couldn't rename %s to %s\n", desktop_renaming, desktop_new_name);
        } else {
            printf("wm: renamed %s to %s on the desktop\n", desktop_renaming, desktop_new_name);
            snprintf(desktop_items[i].name, sizeof(desktop_items[i].name), "%s", desktop_new_name);
            save_layout();
            desktop_renaming[0] = '\0';
            load_desktop(true);
            for (int j = 0; j < desktop_count; j++) {
                if (!strcmp(desktop_items[j].name, desktop_new_name))
                    select_desktop_item(j);
            }
            return;
        }
    }
    desktop_renaming[0] = '\0';
}

static void desktop_rename_key(const struct key_event *e) {
    size_t length = strlen(desktop_new_name), low = rename_at < rename_anchor ? rename_at : rename_anchor;
    size_t high = rename_at < rename_anchor ? rename_anchor : rename_at;
    bool shift = e->modifiers & KEY_MOD_SHIFT;

    if (!e->pressed)
        return;
    damage_rename();
    if (e->code == KEY_ESCAPE)
        return desktop_rename_done(false);
    if (e->code == KEY_ENTER || e->code == (KEY_EXTENDED | KEY_ENTER))
        return desktop_rename_done(true);
    if ((e->modifiers & KEY_MOD_CTRL) && (e->text == 'a' || e->text == 1))
        rename_anchor = 0, rename_at = length;
    else if (e->code == KEY_LEFT || e->code == KEY_RIGHT || e->code == KEY_HOME || e->code == KEY_END) {
        rename_at = e->code == KEY_HOME ? 0 : e->code == KEY_END ? length
                  : !shift && low != high ? (e->code == KEY_LEFT ? low : high)
                  : e->code == KEY_LEFT ? (rename_at ? rename_at - 1 : 0) : (rename_at < length ? rename_at + 1 : length);
        rename_anchor = shift ? rename_anchor : rename_at;
    } else if ((e->code == KEY_BACKSPACE || e->code == KEY_DELETE) && low != high) {
        memmove(desktop_new_name + low, desktop_new_name + high, length - high + 1);
        rename_at = rename_anchor = low;
    } else if (e->code == KEY_BACKSPACE && rename_at > 0) {
        memmove(desktop_new_name + rename_at - 1, desktop_new_name + rename_at, length - rename_at + 1);
        rename_at = rename_anchor = rename_at - 1;
    } else if (e->code == KEY_DELETE && rename_at < length) {
        memmove(desktop_new_name + rename_at, desktop_new_name + rename_at + 1, length - rename_at);
    } else if (e->text >= ' ' && e->text < 0x7F && e->text != '/' && !(e->modifiers & (KEY_MOD_CTRL | KEY_MOD_ALT))
               && length - (high - low) + 1 < sizeof(desktop_new_name)) {
        memmove(desktop_new_name + low + 1, desktop_new_name + high, length - high + 1);
        desktop_new_name[low] = (char)e->text;
        rename_at = rename_anchor = low + 1;
    }
    damage_rename();
}

static void draw_rename_field(struct gui_canvas *c, int i, int ox, int oy) {
    struct rect f = rename_field(i);
    size_t low = rename_at < rename_anchor ? rename_at : rename_anchor, high = rename_at < rename_anchor ? rename_anchor : rename_at;
    char part[256];
    int x = f.x + 8, y = f.y + 3;
    int fits = (int)gui_text_fit(desktop_new_name, strlen(desktop_new_name), f.width - 16);
    size_t first = rename_at > (size_t)fits ? rename_at - fits : 0; // long names scroll to the cursor

    gui_fill_rounded(c, f.x - ox, f.y - oy, f.width, f.height, SMALL_RADIUS, TITLE_FOCUSED);
    gui_fill_rounded(c, f.x + 1 - ox, f.y + 1 - oy, f.width - 2, f.height - 2, SMALL_RADIUS, TASKBAR_COLOR);
    if (low < high && high > first) {
        size_t from = low > first ? low : first;

        snprintf(part, sizeof(part), "%.*s", (int)(from - first), desktop_new_name + first);
        int start = gui_text_width(part, 1);

        snprintf(part, sizeof(part), "%.*s", (int)(high - from), desktop_new_name + from);
        gui_fill(c, x + start - ox, y - oy, gui_text_width(part, 1), GUI_CHAR_HEIGHT, TITLE_FOCUSED);
    }
    snprintf(part, sizeof(part), "%.*s", fits + 1, desktop_new_name + first);
    struct gui_canvas inside = gui_part(c, 0, 0, f.x + f.width - 6 - ox > 0 ? f.x + f.width - 6 - ox : 0, c->height);

    inside.width = inside.width < c->width ? inside.width : c->width;
    gui_text(&inside, x - ox, y - oy, part, TASKBAR_TEXT, 1);
    snprintf(part, sizeof(part), "%.*s", (int)(rename_at - first), desktop_new_name + first);
    gui_fill(c, x + gui_text_width(part, 1) - ox, y - oy, 1, GUI_CHAR_HEIGHT, TASKBAR_TEXT);
}

// A new folder or empty file on the desktop, called what's free of "New folder",
// "New folder 2" and on, then renamed in place.
static void desktop_new(bool folder) {
    char name[64], path[GUI_PATH_MAX];
    int fd;

    for (int n = 1; n < 1000; n++) {
        if (folder)
            snprintf(name, sizeof(name), n > 1 ? "New folder %d" : "New folder", n);
        else
            snprintf(name, sizeof(name), n > 1 ? "New file %d.txt" : "New file.txt", n);
        snprintf(path, sizeof(path), "%s/%s", desktop_dir, name);
        if (folder ? mkdir(path, 0755) == 0 : (fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644)) >= 0 && close(fd) == 0) {
            printf("wm: made %s\n", path);
            load_desktop(true);
            start_desktop_rename(name);
            return;
        }
        if (errno != EEXIST)
            break;
    }
    printf("wm: couldn't make a new %s: %s\n", folder ? "folder" : "file", strerror(errno));
}

// A click on the desktop selects what's there, or nothing, and a second soon after opens it.
static void desktop_press(void) {
    int index = desktop_item_at(cursor_x, cursor_y);
    int64_t now = now_ms();
    char path[GUI_PATH_MAX];

    if (index >= 0 && index == desktop_selected && now - desktop_clicked < DOUBLE_CLICK_MS) {
        desktop_path(index, path);
        open_file(path);
        now = 0; // a third click starts again
    }
    select_desktop_item(index);
    desktop_clicked = now;
    desktop_pressed = index;
    pressed_x = cursor_x;
    pressed_y = cursor_y;
}

// Holding the mouse on an icon and moving it drags the file.
static void desktop_drag(void) {
    if (desktop_pressed < 0 || drag_file[0] || abs(cursor_x - pressed_x) + abs(cursor_y - pressed_y) < DRAG_DISTANCE)
        return;
    desktop_path(desktop_pressed, drag_file);
    drag_icon = gui_file_icon(drag_file, desktop_items[desktop_pressed].folder);
    desktop_dragged = desktop_pressed;
    desktop_pressed = -1;
    add_damage(drag_rect());
    printf("wm: dragging %s\n", drag_file);
}

// An icon let go on the desktop goes to the cell it's over, or into the folder
// there. Anything else there, it stays where it was.
static void desktop_drop(void) {
    struct desktop_item *item = &desktop_items[desktop_dragged];
    int column, row, there;

    cell_at(cursor_x, cursor_y, &column, &row);
    there = item_in_cell(column, row);
    if (there >= 0 && there != desktop_dragged && desktop_items[there].folder) {
        char from[GUI_PATH_MAX], to[GUI_PATH_MAX + 256];

        desktop_path(desktop_dragged, from);
        snprintf(to, sizeof(to), "%s/%s/%s", desktop_dir, desktop_items[there].name, item->name);
        if (rename(from, to) == 0)
            printf("wm: moved %s into %s\n", item->name, desktop_items[there].name);
        load_desktop(false);
        return;
    }
    if (there >= 0)
        return;
    add_damage(desktop_cell(desktop_dragged));
    item->column = column;
    item->row = row;
    add_damage(desktop_cell(desktop_dragged));
    save_layout();
    printf("wm: moved %s to %d %d\n", item->name, column, row);
}

// A copy of a file let go on the desktop goes in Desktop, in the cell it was let
// go over, unless it's there already.
static void copy_to_desktop(const char *path) {
    const char *slash = strrchr(path, '/');
    pid_t child;
    int column, row;
    FILE *f;

    if (!slash || ((size_t)(slash - path) == strlen(desktop_dir) && !strncmp(path, desktop_dir, slash - path)))
        return;
    if ((child = fork()) == 0) {
        execlp("cp", "cp", "-r", path, desktop_dir, (char *)NULL);
        _exit(127);
    }
    if (child > 0)
        waitpid(child, NULL, 0);
    printf("wm: copied %s to the desktop\n", path);
    cell_at(cursor_x, cursor_y, &column, &row);
    if (item_in_cell(column, row) < 0 && (f = fopen(LAYOUT, "a"))) {
        fprintf(f, "%d %d %s\n", column, row, slash + 1);
        fclose(f);
    }
    load_desktop(false);
}

// Deleting from the desktop puts it in the trash.
static void trash_file(const char *path) {
    if (gui_trash(path))
        printf("wm: trashed %s\n", path);
    else
        add_notice("Couldn't move it to the trash", strerror(errno), NULL);
    load_desktop(false);
}

// Locking, logging out and logging in fade from what the screen showed to what
// it shows next. Locking and logging out hold what was there until the lock or
// log-in screen's window comes, or HOLD_MS goes by without it.
#define SCREEN_FADE_MS 250
#define HOLD_MS 1500

static struct {
    uint32_t *was; // the screen as it was, or NULL
    int width, height; // of the screen then
    bool holding;
    int64_t start; // of holding, then of fading
} screen_fade;

static void fade_screen(bool hold) {
    size_t size = (size_t)screen.width * screen.height * scale * scale * sizeof(uint32_t);

    if (screen_fade.was && (screen_fade.width != screen.width || screen_fade.height != screen.height)) {
        free(screen_fade.was);
        screen_fade.was = NULL;
    }
    if (!settings.animations || (!screen_fade.was && !(screen_fade.was = malloc(size))))
        return;
    screen_fade.width = screen.width;
    screen_fade.height = screen.height;
    for (int y = 0; y < screen.height * scale; y++)
        memcpy(screen_fade.was + (long)y * screen.width * scale, screen.pixels + (long)y * screen.stride,
               screen.width * scale * sizeof(uint32_t));
    screen_fade.holding = hold;
    screen_fade.start = now_ms();
    add_damage((struct rect){0, 0, screen.width, screen.height});
}

// What the screen is now, after holding.
static void stop_holding(void) {
    if (screen_fade.was && screen_fade.holding) {
        screen_fade.holding = false;
        screen_fade.start = now_ms();
    }
}

// How much of the new screen shows, out of 256.
static int screen_shown(void) {
    if (!screen_fade.was || screen_fade.width != screen.width || screen_fade.height != screen.height)
        return 256;
    return screen_fade.holding ? 0 : ease_out(progress(screen_fade.start, SCREEN_FADE_MS));
}

static bool animate_screen(void) {
    if (!screen_fade.was)
        return false;
    if (screen_fade.holding && now_ms() - screen_fade.start >= HOLD_MS)
        stop_holding();
    add_damage((struct rect){0, 0, screen.width, screen.height});
    if (screen_shown() < 256)
        return true;
    free(screen_fade.was);
    screen_fade.was = NULL;
    return false;
}

// Super+Tab shows every window on the workspace at once, side by side and smaller,
// over the darkened desktop. Clicking one, or Enter on the one chosen with the
// arrows, goes to it, and Escape, Super+Tab or a click elsewhere goes back. Copies
// of windows, here and in the taskbar's previews, are drawn from their layers.
#define OVERVIEW_GAP 24
static bool overview;
static int overview_choice;

static void close_menu(void);
static void show(struct window *w);

// The layer of a window, drawn again if it's changed since.
static void ready_copy(struct window *w) {
    struct rect f = frame_of(w);

    if (w->copy_stale || w->layer.width != f.width || w->layer.height != f.height)
        w->copy_stale = !fill_layer(&w->layer, w);
}

static int overview_columns(int count) {
    int columns = 1;

    while (columns * columns < count)
        columns++;
    return columns;
}

// Where a window's copy goes: in a grid of cells, the last row in the middle, as
// big as fits in its cell above its title, but no bigger than it is.
static struct rect overview_thumb(int index, int count, const struct window *w) {
    struct rect d = desktop(), f = frame_of(w);
    int columns = overview_columns(count), rows = (count + columns - 1) / columns, row = index / columns;
    int in_row = row == rows - 1 ? count - row * columns : columns;
    int cell_width = (d.width - OVERVIEW_GAP) / columns, cell_height = (d.height - OVERVIEW_GAP) / rows;
    int room_width = cell_width - OVERVIEW_GAP, room_height = cell_height - OVERVIEW_GAP - GUI_CHAR_HEIGHT - 8;
    int width = f.width, height = f.height;
    int x = d.x + OVERVIEW_GAP + (columns - in_row) * cell_width / 2 + index % columns * cell_width;
    int y = d.y + OVERVIEW_GAP + row * cell_height;

    if (width > room_width)
        height = height * room_width / width, width = room_width;
    if (height > room_height)
        width = width * room_height / height, height = room_height;
    return (struct rect){x + (room_width - width) / 2, y + (room_height - height) / 2, width, height};
}

// All a copy covers, with its highlight, shadow and title.
static struct rect overview_area(struct rect t) {
    return with_shadow((struct rect){t.x - 4, t.y - 4, t.width + 8, t.height + 14 + GUI_CHAR_HEIGHT});
}

static int overview_at(int x, int y) {
    struct window *list[MAX_WINDOWS];
    int count = windows_by_age(list);

    for (int i = 0; i < count; i++) {
        if (contains(overview_thumb(i, count, list[i]), x, y))
            return i;
    }
    return -1;
}

static void draw_overview(struct gui_canvas *c, int ox, int oy) {
    struct window *list[MAX_WINDOWS];
    struct rect area = {ox, oy, c->width, c->height};
    int count = windows_by_age(list);

    blend_fill(c, 0, 0, c->width, c->height, 0x000000, 112);
    for (int i = 0; i < count; i++) {
        struct window *w = list[i];
        struct rect t = overview_thumb(i, count, w);
        int text_width = gui_text_width(w->title, 1) < t.width ? gui_text_width(w->title, 1) : t.width;

        if (!overlaps(overview_area(t), area))
            continue;
        ready_copy(w);
        if (i == overview_choice)
            gui_fill_rounded(c, t.x - 4 - ox, t.y - 4 - oy, t.width + 8, t.height + 8, RADIUS, TITLE_FOCUSED);
        draw_layer(c, &w->layer, t, 256, w->shadow, rounded(w), see_through(w), ox, oy);
        fitted_text(c, t.x + (t.width - text_width) / 2 - ox, t.y + t.height + 8 - oy, t.width, w->title, TITLE_TEXT);
    }
}

static void choose_in_overview(int index) {
    struct window *list[MAX_WINDOWS];
    int count = windows_by_age(list);

    if (index == overview_choice || index < 0 || index >= count)
        return;
    add_damage(overview_area(overview_thumb(overview_choice, count, list[overview_choice])));
    add_damage(overview_area(overview_thumb(index, count, list[index])));
    overview_choice = index;
}

static void set_overview(bool on) {
    struct window *list[MAX_WINDOWS];
    int count = windows_by_age(list);

    if (on == overview || (on && !count))
        return;
    close_menu();
    close_panel();
    fade_screen(false);
    overview = on;
    dragging = grabbed = resizing = NULL;
    add_damage((struct rect){0, 0, screen.width, screen.height});
    printf("wm: overview %s\n", on ? "shown" : "closed");
    overview_choice = 0;
    for (int i = 0; on && i < count; i++) {
        struct rect t = overview_thumb(i, count, list[i]);

        list[i]->copy_stale = true;
        overview_choice = list[i] == focused ? i : overview_choice;
        printf("wm: overview %s at %d %d %d %d\n", list[i]->title, t.x, t.y, t.width, t.height);
    }
}

static void pick_in_overview(int index) {
    struct window *list[MAX_WINDOWS];
    int count = windows_by_age(list);

    set_overview(false);
    if (index >= 0 && index < count) {
        printf("wm: overview picked %s\n", list[index]->title);
        show(list[index]);
    }
}

// Arrows move through the copies, Enter goes to the one chosen, and Escape or Super+Tab
// goes back. Other keys do nothing meanwhile.
static void overview_key(const struct key_event *e) {
    struct window *list[MAX_WINDOWS];
    int count = windows_by_age(list), columns = overview_columns(count);

    if (!e->pressed)
        return;
    if (e->code == KEY_ESCAPE || (e->code == KEY_TAB && (e->modifiers & KEY_MOD_SUPER)))
        set_overview(false);
    else if (e->code == KEY_ENTER)
        pick_in_overview(overview_choice);
    else if (e->code == KEY_LEFT || e->code == KEY_RIGHT || e->code == KEY_TAB)
        choose_in_overview((overview_choice + (e->code == KEY_LEFT ? count - 1 : 1)) % count);
    else if (e->code == KEY_UP || e->code == KEY_DOWN)
        choose_in_overview(overview_choice + (e->code == KEY_UP ? -columns : columns));
}

// Resting the mouse on a window's taskbar button shows a copy of it there, which
// comes at once moving from one button to the next.
#define PREVIEW_MS 400
#define PREVIEW_WIDTH 220
#define PREVIEW_HEIGHT 140
static struct window *hovered; // whose button the mouse is on
static int64_t hovered_since, preview_opened;
static bool previewing; // hovered's preview is up

static struct rect preview_rect(void) {
    struct rect b = button_for(hovered), f = frame_of(hovered);
    int width = f.width, height = f.height, x, most;

    if (width > PREVIEW_WIDTH)
        height = height * PREVIEW_WIDTH / width, width = PREVIEW_WIDTH;
    if (height > PREVIEW_HEIGHT)
        width = width * PREVIEW_HEIGHT / height, height = PREVIEW_HEIGHT;
    width = width + 16 > 120 ? width + 16 : 120;
    height += 24 + GUI_CHAR_HEIGHT;
    x = b.x + (b.width - width) / 2;
    most = screen.width - GAP - width;
    x = x < most ? x : most;
    return (struct rect){x > GAP ? x : GAP, beside_bar(height), width, height};
}

static void draw_preview(struct gui_canvas *c, int unused, int ox, int oy) {
    struct rect p = preview_rect(), f = frame_of(hovered);
    int height = p.height - 24 - GUI_CHAR_HEIGHT, width = f.width * height / f.height;

    (void)unused;
    width = width < p.width - 16 ? width : p.width - 16;
    draw_glass(c, p, ox, oy);
    fitted_text(c, p.x + 8 - ox, p.y + 8 - oy, p.width - 16, hovered->title, TASKBAR_TEXT);
    ready_copy(hovered);
    draw_layer(c, &hovered->layer, (struct rect){p.x + (p.width - width) / 2, p.y + 16 + GUI_CHAR_HEIGHT, width, height}, 256, 0,
               rounded(hovered), see_through(hovered), ox, oy);
}

static void hide_preview(void) {
    if (previewing)
        flyout_leaves(preview_rect());
    previewing = false;
    hovered = NULL;
}

static void hover_taskbar(void) {
    struct window *list[MAX_WINDOWS], *now = NULL;
    int count = windows_by_age(list);
    bool was = previewing;

    for (int i = 0; i < count && bar_visible() && !popup_depth && panel < 0 && !buttons && !overview; i++) {
        if (contains(taskbar_button(i, count), cursor_x, cursor_y))
            now = list[i];
    }
    if (now == hovered)
        return;
    hide_preview();
    hovered = now;
    hovered_since = was ? 0 : now_ms();
}

// Shows the preview once the mouse has rested long enough, returning how long until
// then, or -1.
static int check_preview(void) {
    int64_t wait = hovered && !previewing ? hovered_since + PREVIEW_MS - now_ms() : -1;

    if (!hovered || previewing || wait > 0)
        return (int)wait;
    previewing = true;
    preview_opened = now_ms();
    hovered->copy_stale = true;
    add_damage(with_shadow(preview_rect()));
    printf("wm: preview %s\n", hovered->title);
    return -1;
}

// A window's copy is drawn again when it changes.
static void copy_changed(struct window *w) {
    struct window *list[MAX_WINDOWS];
    int count = windows_by_age(list);

    w->copy_stale = true;
    for (int i = 0; overview && i < count; i++) {
        if (list[i] == w)
            add_damage(overview_area(overview_thumb(i, count, w)));
    }
    if (previewing && hovered == w)
        add_damage(preview_rect());
}

// Resting the mouse on a window's maximize button offers layouts under it, each a
// small picture of the desktop split into parts. Clicking a part puts the window
// there. They're in sixths across and halves down.
#define LAYOUT_MS 400
#define LAYOUT_WIDTH 64
#define LAYOUT_HEIGHT 40
struct zone {
    int x, y, width, height;
};
static const struct {
    int count;
    struct zone zones[4];
} layouts[] = {
    {2, {{0, 0, 3, 2}, {3, 0, 3, 2}}},
    {4, {{0, 0, 3, 1}, {3, 0, 3, 1}, {0, 1, 3, 1}, {3, 1, 3, 1}}},
    {3, {{0, 0, 2, 2}, {2, 0, 2, 2}, {4, 0, 2, 2}}},
    {2, {{0, 0, 4, 2}, {4, 0, 2, 2}}},
};
#define LAYOUTS (int)(sizeof(layouts) / sizeof(layouts[0]))
static struct window *layout_for; // whose maximize button the mouse is on, or was
static int64_t layout_since, layout_opened;
static bool layout_shown;

static void place(struct window *w, struct rect frame);
static struct window *window_at(int x, int y, enum part *part, int *edges);
static void set_focus(struct window *w);
static void raise_window(struct window *w);

static struct rect layout_rect(void) {
    struct rect b = button_of(layout_for, 1);
    int width = LAYOUTS * (LAYOUT_WIDTH + 8) + 8, height = LAYOUT_HEIGHT + 16;
    int x = b.x + BUTTON_SIZE / 2 - width / 2, y = b.y + BUTTON_SIZE + 6;

    x = x + width > screen.width - GAP ? screen.width - GAP - width : x;
    return (struct rect){x > GAP ? x : GAP, y + height > screen.height ? b.y - 6 - height : y, width, height};
}

// A part of a rectangle, as a zone of it, less gap all round.
static struct rect zone_in(struct rect r, struct zone z, int gap) {
    int x = r.x + r.width * z.x / 6, y = r.y + r.height * z.y / 2;

    return (struct rect){x + gap, y + gap, r.x + r.width * (z.x + z.width) / 6 - x - 2 * gap,
                         r.y + r.height * (z.y + z.height) / 2 - y - 2 * gap};
}

static struct rect layout_picture(int layout) {
    struct rect p = layout_rect();

    return (struct rect){p.x + 8 + layout * (LAYOUT_WIDTH + 8), p.y + 8, LAYOUT_WIDTH, LAYOUT_HEIGHT};
}

// The layout and part under the mouse, if there is one.
static bool layout_part_at(int *layout, int *part) {
    for (int i = 0; i < LAYOUTS; i++) {
        for (int j = 0; j < layouts[i].count; j++) {
            if (contains(zone_in(layout_picture(i), layouts[i].zones[j], 1), cursor_x, cursor_y)) {
                *layout = i, *part = j;
                return true;
            }
        }
    }
    return false;
}

static void draw_layouts(struct gui_canvas *c, int unused, int ox, int oy) {
    int over_layout = -1, over_part = -1;

    (void)unused;
    layout_part_at(&over_layout, &over_part);
    draw_glass(c, layout_rect(), ox, oy);
    for (int i = 0; i < LAYOUTS; i++) {
        for (int j = 0; j < layouts[i].count; j++) {
            struct rect z = zone_in(layout_picture(i), layouts[i].zones[j], 1);

            gui_fill_rounded(c, z.x - ox, z.y - oy, z.width, z.height, SMALL_RADIUS,
                             i == over_layout && j == over_part ? TITLE_FOCUSED : TASKBAR_BUTTON);
        }
    }
}

static void hide_layouts(void) {
    if (layout_shown)
        flyout_leaves(layout_rect());
    layout_shown = false;
    layout_for = NULL;
}

// Follows the mouse onto a maximize button, and off it and the layouts.
static void hover_layouts(void) {
    enum part part;
    int edges;
    struct window *w = buttons || overview ? NULL : window_at(cursor_x, cursor_y, &part, &edges);

    if (layout_shown && contains(layout_rect(), cursor_x, cursor_y) && !buttons) {
        add_damage(layout_rect());
        return;
    }
    if (w && part == PART_MAXIMIZE && !w->bare) {
        if (w != layout_for) {
            hide_layouts();
            layout_for = w;
            layout_since = now_ms();
        }
        return;
    }
    hide_layouts();
}

// Shows the layouts once the mouse has rested long enough, returning how long
// until then, or -1.
static int check_layouts(void) {
    int64_t wait = layout_for && !layout_shown ? layout_since + LAYOUT_MS - now_ms() : -1;

    if (!layout_for || layout_shown || wait > 0)
        return (int)wait;
    layout_shown = true;
    layout_opened = now_ms();
    add_damage(with_shadow(layout_rect()));
    printf("wm: layouts for %s at %d %d\n", layout_for->title, layout_rect().x, layout_rect().y);
    return -1;
}

// A click on the layouts puts the window in the part it's on, returning whether it was on them.
static bool layouts_click(void) {
    int layout, part;

    if (!layout_shown || !contains(layout_rect(), cursor_x, cursor_y))
        return false;
    if (layout_part_at(&layout, &part)) {
        struct window *w = layout_for;
        struct rect was = frame_of(w), r = zone_in(desktop(), layouts[layout].zones[part], 0);

        hide_layouts();
        place(w, r);
        w->snapped = SNAP_NONE;
        reframe(w, was);
        set_focus(w);
        raise_window(w);
        printf("wm: layout %s at %d %d %d %d\n", w->title, r.x, r.y, r.width, r.height);
    }
    return true;
}

// Shift+Print Screen and Super+Shift+S choose what to save. The screen stays as it
// was, darkened but for the area dragged out, which is saved when the mouse is let
// go. Clicking takes the window under the mouse, or the screen, and Escape or the
// right button stops.
static struct {
    uint32_t *pixels; // the screen as it was, while choosing
    bool dragging;
    int x, y; // where the drag started
    struct rect shown; // the area as last drawn
} choosing;

static struct rect chosen_area(void) {
    int x = choosing.x < cursor_x ? choosing.x : cursor_x, y = choosing.y < cursor_y ? choosing.y : cursor_y;

    return (struct rect){x, y, abs(cursor_x - choosing.x) + 1, abs(cursor_y - choosing.y) + 1};
}

#define CHOOSING_HINT "Drag across an area, or click a window. Escape stops."

static struct rect choosing_hint(void) {
    int width = gui_text_width(CHOOSING_HINT, 1) + 24;

    return (struct rect){(screen.width - width) / 2, 16, width, 28};
}

static void draw_choosing(struct gui_canvas *c, int ox, int oy) {
    struct rect a = choosing.dragging ? chosen_area() : (struct rect){0}, h = choosing_hint();
    char size[32];

    a = scaled(a);
    for (int y = 0; y < c->height * scale; y++) {
        const uint32_t *from = choosing.pixels + (long)(y + oy * scale) * screen.width * scale + ox * scale;
        uint32_t *to = c->pixels + (long)y * c->stride;

        for (int x = 0; x < c->width * scale; x++)
            to[x] = contains(a, x + ox * scale, y + oy * scale) ? from[x] : from[x] >> 1 & 0x7F7F7F;
    }
    gui_fill_rounded(c, h.x - ox, h.y - oy, h.width, h.height, RADIUS, TASKBAR_COLOR);
    gui_text(c, h.x + 12 - ox, h.y + (h.height - GUI_CHAR_HEIGHT) / 2 - oy, CHOOSING_HINT, TASKBAR_TEXT, 1);
    if (!choosing.dragging)
        return;
    gui_fill(c, a.x - ox, a.y - oy, a.width, 1, TITLE_FOCUSED);
    gui_fill(c, a.x - ox, a.y + a.height - 1 - oy, a.width, 1, TITLE_FOCUSED);
    gui_fill(c, a.x - ox, a.y - oy, 1, a.height, TITLE_FOCUSED);
    gui_fill(c, a.x + a.width - 1 - ox, a.y - oy, 1, a.height, TITLE_FOCUSED);
    snprintf(size, sizeof(size), "%d x %d", a.width, a.height);
    gui_text(c, a.x + a.width - gui_text_width(size, 1) - ox, a.y + a.height + 6 - oy, size, 0xFFFFFF, 1);
}

static void choosing_moved(void) {
    struct rect a = chosen_area(), grown = {a.x - 120, a.y - 4, a.width + 124, a.height + 8 + 2 * GUI_CHAR_HEIGHT};

    add_damage(choosing.shown);
    add_damage(grown);
    choosing.shown = grown;
}

// Menus and the taskbar's panels fade in as they slide FLYOUT_SLIDE towards the
// taskbar, and fade out sliding the other way, from a copy of how they looked.
#define FLYOUT_MS 150
#define FLYOUT_SLIDE 8

static struct {
    struct gui_canvas copy; // of the screen around it, from at
    struct rect at, r;
    int64_t start;
} flyout_ghost;

// Which way away from the taskbar is, down or up, as 1 or -1.
static int from_bar(void) {
    return settings.taskbar_top ? -1 : 1;
}

static int flyout_progress(int64_t start) {
    return settings.animations ? progress(start, FLYOUT_MS) : 256;
}

// Draws a flyout that opened at start, shifted and mixed with what's under it.
static void draw_flyout(struct gui_canvas *c, struct rect r, int64_t start,
                        void (*draw)(struct gui_canvas *, int, int, int), int arg, int ox, int oy) {
    static uint32_t *under;
    static size_t size;
    int t = ease_out(flyout_progress(start)), dy = (256 - t) * FLYOUT_SLIDE / 256 * from_bar();
    int x0 = r.x - ox < 0 ? 0 : r.x - ox, x1 = r.x + r.width - ox > c->width ? c->width : r.x + r.width - ox;
    int y0 = r.y + dy - oy < 0 ? 0 : r.y + dy - oy, y1 = r.y + dy + r.height - oy > c->height ? c->height : r.y + dy + r.height - oy;
    size_t needed = x1 > x0 && y1 > y0 ? (size_t)(x1 - x0) * (y1 - y0) * scale * scale : 0;

    if (t == 256 || !needed) {
        draw(c, arg, ox, oy - dy);
        return;
    }
    if (needed > size) {
        uint32_t *bigger = realloc(under, needed * sizeof(*under));

        if (!bigger) {
            draw(c, arg, ox, oy - dy);
            return;
        }
        under = bigger, size = needed;
    }
    x0 *= scale, x1 *= scale, y0 *= scale, y1 *= scale; // in the display's pixels
    for (int y = y0; y < y1; y++)
        memcpy(under + (size_t)(y - y0) * (x1 - x0), c->pixels + (long)y * c->stride + x0, (x1 - x0) * sizeof(*under));
    draw(c, arg, ox, oy - dy);
    for (int y = y0; y < y1; y++) {
        uint32_t *row = c->pixels + (long)y * c->stride, *was = under + (size_t)(y - y0) * (x1 - x0);

        for (int x = x0; x < x1; x++)
            row[x] = gui_mix(was[x - x0], row[x], t);
    }
}

static void draw_area(struct rect r, struct gui_canvas c);

// Keeps a copy of a flyout as it closes, to fade out.
static void flyout_leaves(struct rect r) {
    int x0 = r.x - BLUR_MARGIN < 0 ? 0 : r.x - BLUR_MARGIN, y0 = r.y - BLUR_MARGIN < 0 ? 0 : r.y - BLUR_MARGIN;
    int x1 = r.x + r.width + BLUR_MARGIN, y1 = r.y + r.height + BLUR_MARGIN;
    struct rect at = {x0, y0, (x1 < screen.width ? x1 : screen.width) - x0, (y1 < screen.height ? y1 : screen.height) - y0};
    uint32_t *pixels;

    if (!settings.animations
        || !(pixels = realloc(flyout_ghost.copy.pixels, (size_t)at.width * at.height * scale * scale * sizeof(uint32_t))))
        return;
    flyout_ghost.copy = (struct gui_canvas){pixels, at.width, at.height, at.width * scale, scale};
    flyout_ghost.at = at;
    flyout_ghost.r = r;
    flyout_ghost.start = now_ms();
    no_cursor = true;
    draw_area(at, flyout_ghost.copy);
    no_cursor = false;
    add_damage(at);
}

static void draw_flyout_ghost(struct gui_canvas *c, int ox, int oy) {
    struct rect r = flyout_ghost.r;
    int t = ease_in(flyout_progress(flyout_ghost.start)), dy = t * FLYOUT_SLIDE / 256 * from_bar();

    if (t == 256 || !flyout_ghost.copy.pixels)
        return;
    // In the display's pixels.
    r = scaled(r), dy *= scale, ox *= scale, oy *= scale;
    for (int y = r.y + dy; y < r.y + dy + r.height; y++) {
        const uint32_t *from = flyout_ghost.copy.pixels + (long)(y - dy - flyout_ghost.at.y * scale) * flyout_ghost.copy.stride
                             - flyout_ghost.at.x * scale;

        if (y < oy || y >= oy + c->height * scale)
            continue;
        for (int x = r.x < ox ? ox : r.x; x < r.x + r.width && x < ox + c->width * scale; x++) {
            uint32_t *to = c->pixels + (long)(y - oy) * c->stride + x - ox;

            *to = gui_mix(*to, from[x], 256 - t);
        }
    }
}

// Damages the flyouts while they move, returning whether any has further to go.
static bool animate_flyouts(void) {
    bool going = false;

    // Each goes on a frame past its end, to be drawn as it ends.
    if (popup_depth && settings.animations && now_ms() - menu_opened < FLYOUT_MS + FRAME_MS) {
        add_damage(with_shadow(popups[0].r));
        going = true;
    }
    if (panel >= 0 && settings.animations && now_ms() - panel_opened < FLYOUT_MS + FRAME_MS) {
        add_damage(with_shadow(panel_rect(panel)));
        going = true;
    }
    if (previewing && settings.animations && now_ms() - preview_opened < FLYOUT_MS + FRAME_MS) {
        add_damage(with_shadow(preview_rect()));
        going = true;
    }
    if (layout_shown && settings.animations && now_ms() - layout_opened < FLYOUT_MS + FRAME_MS) {
        add_damage(with_shadow(layout_rect()));
        going = true;
    }
    if (flyout_ghost.copy.pixels) {
        add_damage(with_shadow(flyout_ghost.r));
        if (flyout_progress(flyout_ghost.start) < 256)
            going = true;
        else {
            free(flyout_ghost.copy.pixels);
            flyout_ghost.copy.pixels = NULL;
        }
    }
    return going;
}

// Draws everything that overlaps a rectangle, on a canvas that is only that part
// of the screen.
static void draw_area(struct rect r, struct gui_canvas c) {
    int hint_y = desktop().y + desktop().height - 2 * GUI_CHAR_HEIGHT, first = -1;
    struct rect bar = taskbar();

    if (choosing.pixels) {
        draw_choosing(&c, r.x, r.y);
        if (!no_cursor)
            draw_cursor(&c, r.x, r.y);
        return;
    }
    // Nothing under a window that covers all of it can be seen.
    for (int i = window_count - 1; i >= 0 && first < 0 && !overview; i--) {
        if (covers(windows[i], r))
            first = i;
    }
    if (first < 0) {
        if (wallpaper.pixels)
            gui_copy(&c, 0, 0, &wallpaper, r.x, r.y, r.width, r.height);
        else
            gui_fill(&c, 0, 0, r.width, r.height, DESKTOP_COLOR);
        if (!locked) {
            gui_text(&c, GUI_CHAR_WIDTH - r.x, hint_y - r.y, HINT, HINT_COLOR, 1);
            draw_desktop(&c, r.x, r.y);
        }
        first = 0;
    }
    for (int i = first; i < window_count && !overview; i++) {
        struct window *w = windows[i];
        int alpha;

        if (!visible(w) || !overlaps(drawn_extent(w), r))
            continue;
        if (w->anim_ms) {
            struct rect now = arriving(w, &alpha);

            draw_layer(&c, &w->layer, now, alpha, w->shadow, rounded(w), see_through(w), r.x, r.y);
            continue;
        }
        if (settings.shadows)
            draw_shadow(&c, w, r.x, r.y);
        draw_window(&c, w, r.x, r.y);
    }
    if (overview)
        draw_overview(&c, r.x, r.y);
    else
        draw_ghosts(&c, r.x, r.y);
    if (locked) {
        if (!no_cursor)
            draw_cursor(&c, r.x, r.y);
        return;
    }
    if (snap && overlaps(snap_rect(snap), r))
        draw_snap(&c, r.x, r.y);
    if (bar_visible() && overlaps(r, bar))
        draw_taskbar(&c, r.x, r.y);
    draw_notices(&c, r.x, r.y);
    if (osd_until && overlaps(osd_rect(), r))
        draw_osd(&c, r.x, r.y);
    if (popup_depth)
        draw_flyout(&c, popups[0].r, menu_opened, draw_popup, 0, r.x, r.y);
    if (popup_depth > 1)
        draw_popup(&c, 1, r.x, r.y);
    if (panel >= 0)
        draw_flyout(&c, panel_rect(panel), panel_opened, draw_open_panel, 0, r.x, r.y);
    if (previewing)
        draw_flyout(&c, preview_rect(), preview_opened, draw_preview, 0, r.x, r.y);
    if (layout_shown)
        draw_flyout(&c, layout_rect(), layout_opened, draw_layouts, 0, r.x, r.y);
    draw_flyout_ghost(&c, r.x, r.y);
    if (switch_choice >= 0)
        draw_switcher(&c, r.x, r.y);
    if (drag_file[0] && drag_icon && !no_cursor) {
        struct rect d = drag_rect();

        gui_draw_image(&c, d.x - r.x, d.y - r.y, drag_icon);
    }
    if (auth_shown)
        draw_auth(&c, r.x, r.y);
    if (!no_cursor)
        draw_cursor(&c, r.x, r.y);
}

// Composes all that changed before showing any of it, so a window being moved is
// never seen gone from both places.
// The night light takes away some of the blue, and less of the green, as the
// screen's shown, which is how these keep each.
static uint8_t night_green[256], night_blue[256];

static void make_night(void) {
    int k = settings.night_strength;

    for (int i = 0; i < 256; i++) {
        night_green[i] = i * (256 - k * 72 / 100) >> 8;
        night_blue[i] = i * (256 - k * 184 / 100) >> 8;
    }
}

// A row of the screen on the display's, with the night light's colours.
static void show_row(uint8_t *to, const uint32_t *from, int width) {
    uint32_t *out = (uint32_t *)to;

    if (!settings.night_light) {
        memcpy(to, from, width * sizeof(uint32_t));
        return;
    }
    for (int x = 0; x < width; x++)
        out[x] = (from[x] & 0xFF0000) | night_green[from[x] >> 8 & 0xFF] << 8 | night_blue[from[x] & 0xFF];
}

// Draws a rectangle of the screen. As what's see-through blurs what's under it,
// a little more is drawn around it, on the side, so the blur reads what's really there.
static void compose(struct rect r) {
    static uint32_t *pixels;
    static size_t size;
    int x0 = r.x - BLUR_MARGIN < 0 ? 0 : r.x - BLUR_MARGIN, y0 = r.y - BLUR_MARGIN < 0 ? 0 : r.y - BLUR_MARGIN;
    int x1 = r.x + r.width + BLUR_MARGIN, y1 = r.y + r.height + BLUR_MARGIN;
    struct rect e = {x0, y0, (x1 < screen.width ? x1 : screen.width) - x0, (y1 < screen.height ? y1 : screen.height) - y0};
    size_t needed = (size_t)e.width * e.height * scale * scale;
    struct gui_canvas area = {pixels, e.width, e.height, e.width * scale, scale};

    if (needed > size) {
        uint32_t *bigger = realloc(pixels, needed * sizeof(*pixels));

        if (!bigger)
            return;
        area.pixels = pixels = bigger, size = needed;
    }
    draw_area(e, area);
    gui_copy(&screen, r.x, r.y, &area, r.x - e.x, r.y - e.y, r.width, r.height);
}

static void redraw(void) {
    static uint32_t mixed[MAX_SIZE * GUI_MAX_SCALE];
    int shown = screen_shown();

    for (int i = 0; i < damage_count; i++)
        compose(damage[i]);
    for (int i = 0; i < damage_count; i++) {
        struct rect r = scaled(damage[i]); // in the display's pixels, from here

        for (int row = r.y; row < r.y + r.height; row++) {
            uint32_t *now = screen.pixels + (long)row * screen.stride + r.x;

            // Part way through fading, the row's what was there mixed with what's there now.
            if (shown < 256) {
                const uint32_t *was = screen_fade.was + (long)row * screen.width * scale + r.x;

                for (int x = 0; x < r.width; x++)
                    mixed[x] = gui_mix(was[x], now[x], shown);
                now = mixed;
            }
            show_row(output.pixels + (long)row * output.pitch + r.x * sizeof(uint32_t), now, r.width);
        }
        display->changed(r.x, r.y, r.width, r.height);
    }
    if (damage_count)
        display->show();
    damage_count = 0;
}

static void send_to(struct window *w, struct gui_message m) {
    m.window = w->id;
    surface_send(w->surface, &m);
}

static void fade(struct window *w, int to) {
    w->fade_from = w->shadow;
    w->fade_to = to;
    w->fade_start = now_ms();
}

// Sets each fading shadow to how far it is through its fade, eased out as
// 1 - (1 - t)^3, returning whether any has further to go.
static bool fade_shadows(void) {
    const long whole = (long)FADE_MS * FADE_MS * FADE_MS;
    long now = now_ms();
    bool fading = false;

    for (int i = 0; i < window_count; i++) {
        struct window *w = windows[i];
        long left = FADE_MS - (now - w->fade_start);

        if (w->shadow == w->fade_to)
            continue;
        left = left < 0 ? 0 : left;
        w->shadow = w->fade_from + (w->fade_to - w->fade_from) * (whole - left * left * left) / whole;
        fading |= w->shadow != w->fade_to;
        if (!w->minimized)
            add_damage(extent_of(w));
    }
    return fading;
}

static void set_focus(struct window *w) {
    if (w == focused)
        return;
    damage_taskbar();
    if (focused) {
        send_to(focused, (struct gui_message){.type = GUI_FOCUS, .focused = 0});
        damage_title(focused);
        fade(focused, SHADOW_UNFOCUSED);
    }
    focused = w;
    if (w) {
        send_to(w, (struct gui_message){.type = GUI_FOCUS, .focused = 1});
        damage_title(w);
        fade(w, SHADOW_FOCUSED);
    }
}

static int index_of(const struct window *w) {
    for (int i = 0; i < window_count; i++) {
        if (windows[i] == w)
            return i;
    }
    return -1;
}

static void raise_window(struct window *w) {
    int i = index_of(w);

    memmove(&windows[i], &windows[i + 1], (window_count - i - 1) * sizeof(*windows));
    windows[window_count - 1] = w;
    add_damage(extent_of(w));
}

static void load_settings(void);
static void settings_changed(void);

// The top window that isn't minimized gets the focus.
static void focus_top(void) {
    for (int i = window_count - 1; i >= 0; i--) {
        if (visible(windows[i])) {
            set_focus(windows[i]);
            return;
        }
    }
    set_focus(NULL);
}

static void minimize(struct window *w) {
    depart(w, button_for(w), MINIMIZE_MS);
    w->minimized = true;
    add_damage(extent_of(w));
    damage_taskbar();
    if (focused == w)
        focus_top();
}

// Takes away the outline of where a dragged window would snap to.
static void stop_snap(void) {
    if (snap)
        add_damage(snap_rect(snap));
    snap = SNAP_NONE;
}

static void go_to_workspace(int to);

static void show(struct window *w) {
    if (!on_workspace(w))
        go_to_workspace(w->workspace);
    if (w->minimized)
        arrive(w, button_for(w), MINIMIZE_MS);
    w->minimized = false;
    w->put_away = false;
    raise_window(w);
    set_focus(w);
    damage_taskbar();
}

static void go_to_workspace(int to) {
    if (to == workspace)
        return;
    workspace = to;
    add_damage((struct rect){0, 0, screen.width, screen.height});
    focus_top();
    printf("wm: workspace %d\n", to + 1);
}

// The focused window goes to another workspace, and the next one here gets the focus.
static void send_to_workspace(struct window *w, int to) {
    if (!w || w->bare || w->workspace == to)
        return;
    add_damage(extent_of(w));
    damage_taskbar();
    w->workspace = to;
    focus_top();
    printf("wm: %s to workspace %d\n", w->title, to + 1);
}

static void remove_window(struct window *w) {
    int i = index_of(w);

    depart(w, shrunk(frame_of(w)), CLOSE_MS);
    add_damage(drawn_extent(w));
    if (hovered == w)
        hide_preview();
    if (layout_for == w)
        hide_layouts();
    if (overview) {
        add_damage((struct rect){0, 0, screen.width, screen.height});
        overview_choice = 0;
    }
    if (switch_choice >= 0) {
        add_damage(switcher());
        switch_choice = window_count > 1 ? switch_choice % (window_count - 1) : -1;
    }
    damage_taskbar();
    memmove(&windows[i], &windows[i + 1], (window_count - i - 1) * sizeof(*windows));
    window_count--;
    if (dragging == w) {
        dragging = NULL;
        stop_snap();
    }
    if (grabbed == w)
        grabbed = NULL;
    if (resizing == w)
        resizing = NULL;
    if (focused == w) {
        focused = NULL;
        focus_top();
    }
    if (menu_window == w)
        menu_window = NULL;
    free(w->canvas.pixels);
    free(w->layer.pixels);
    free(w);
}

static void update_cursor(void);
static void unlock(void);

static void center(struct window *w) {
    w->x = (screen.width - w->canvas.width) / 2;
    w->y = (screen.height - w->canvas.height) / 2;
}

void wm_room(int *width, int *height) {
    struct rect d = desktop();

    *width = d.width - 2 * GAP - 2 * BORDER;
    *height = d.height - 2 * GAP - TITLE_HEIGHT - 2 * BORDER;
}

static void move_window(struct window *w, int x, int y);
static void ask_size(struct window *w, int width, int height);

// A new window that wouldn't all be on the desktop where it opens goes where it is,
// and one bigger than it is made to fit.
static void fit_new(struct window *w) {
    struct rect d = desktop(), f = frame_of(w);
    int most_width, most_height;

    wm_room(&most_width, &most_height);
    if (f.width > d.width - 2 * GAP || f.height > d.height - 2 * GAP) {
        move_window(w, d.x + GAP + BORDER, d.y + GAP + TITLE_HEIGHT + BORDER);
        ask_size(w, w->canvas.width < most_width ? w->canvas.width : most_width,
                 w->canvas.height < most_height ? w->canvas.height : most_height);
        printf("wm: fitted a window %dx%d to the desktop\n", w->canvas.width, w->canvas.height);
        return;
    }
    if (f.x + f.width > d.x + d.width - GAP || f.y + f.height > d.y + d.height - GAP)
        move_window(w, d.x + d.width - GAP - f.width + BORDER, d.y + d.height - GAP - f.height + TITLE_HEIGHT + BORDER);
}

struct window *wm_window_new(struct surface *s, int width, int height, bool popup, int x, int y) {
    struct window *w = window_count < MAX_WINDOWS && width > 0 && height > 0 && width <= MAX_SIZE && height <= MAX_SIZE
                         ? calloc(1, sizeof(*w))
                         : NULL;

    if (!w || !(w->canvas.pixels = calloc((size_t)width * height * scale * scale, sizeof(uint32_t)))) {
        free(w);
        return NULL;
    }
    w->canvas = (struct gui_canvas){w->canvas.pixels, width, height, width * scale, scale};

    // Each new window goes a little further down and right than the last.
    int step = (next_id - 1) % 8;

    w->surface = s;
    w->id = next_id++;
    w->x = 80 + step * CASCADE;
    w->y = 60 + TITLE_HEIGHT + step * CASCADE;
    w->want_width = w->asked_width = width;
    w->want_height = w->asked_height = height;
    w->login = surface_is_login(s);
    w->popup = popup;
    w->bare = w->login || popup;
    w->workspace = workspace;
    if (w->login)
        center(w);
    if (popup)
        w->x = x, w->y = y;
    windows[window_count++] = w;
    if (!popup && !w->login)
        fit_new(w);
    if (!popup) {
        arrive(w, shrunk(frame_of(w)), OPEN_MS);
        damage_taskbar();
    }
    if (switch_choice >= 0)
        add_damage(switcher());
    add_damage(extent_of(w));
    if (visible(w) && !popup) // so nothing can take what's typed on the log-in screen
        set_focus(w);
    if (w->login)
        stop_holding();
    return w;
}

void wm_window_gone(struct window *w) {
    remove_window(w);
}

void wm_window_position(const struct window *w, int *x, int *y) {
    *x = w->x, *y = w->y;
}

void wm_window_move(struct window *w, int x, int y) {
    add_damage(extent_of(w));
    w->x = x, w->y = y;
    add_damage(extent_of(w));
}

void wm_window_raise(struct window *w) {
    if (!w->popup && !locked)
        show(w);
}

// Asks for a new size, unless an answer is still awaited, in which case this is
// asked for once it comes.
static void ask_size(struct window *w, int width, int height) {
    struct rect d = desktop();

    w->want_width = width < MIN_WIDTH ? MIN_WIDTH : width > d.width ? d.width : width;
    w->want_height = height < MIN_HEIGHT ? MIN_HEIGHT : height > d.height ? d.height : height;
    if (w->asking || (w->want_width == w->canvas.width && w->want_height == w->canvas.height))
        return;
    w->asking = true;
    w->asked_width = w->want_width;
    w->asked_height = w->want_height;
    send_to(w, (struct gui_message){.type = GUI_RESIZE, .width = w->want_width, .height = w->want_height});
}

// A new size, as the app chose after being asked, or for itself.
static bool take_size(struct window *w, int width, int height) {
    uint32_t *pixels = width <= MAX_SIZE && height <= MAX_SIZE ? calloc((size_t)width * height * scale * scale, sizeof(uint32_t))
                                                               : NULL;

    if (!pixels)
        return false;
    add_damage(extent_of(w));
    if (w->resize_edges & EDGE_LEFT)
        w->x += w->canvas.width - width;
    if (w->resize_edges & EDGE_TOP)
        w->y += w->canvas.height - height;
    free(w->canvas.pixels);
    w->canvas = (struct gui_canvas){pixels, width, height, width * scale, scale};
    if (w->login) // which sizes itself, rather than being asked to
        center(w);
    if (w->bare) {
        w->want_width = w->asked_width = width;
        w->want_height = w->asked_height = height;
    }
    add_damage(extent_of(w));
    w->asking = false;
    if (w->want_width != w->asked_width || w->want_height != w->asked_height)
        ask_size(w, w->want_width, w->want_height);
    return true;
}

void wm_window_pixels(struct window *w, const uint32_t *pixels, int width, int height, int stride, int x, int y,
                      int changed_width, int changed_height) {
    struct gui_canvas from = {(uint32_t *)pixels, width, height, stride, scale};

    if (width != w->canvas.width || height != w->canvas.height || w->canvas.scale != scale) {
        if (!take_size(w, width, height))
            return;
        x = y = 0, changed_width = width, changed_height = height;
    }
    gui_copy(&w->canvas, x, y, &from, x, y, changed_width, changed_height);
    add_damage((struct rect){w->x + x, w->y + y, changed_width, changed_height});
    copy_changed(w);
}

void wm_window_title(struct window *w, const char *title) {
    snprintf(w->title, sizeof(w->title), "%s", title);
    damage_title(w);
    damage_taskbar();
    copy_changed(w);
}

// Apps are known by their app id, or X class, which picks their icon.
void wm_window_app(struct window *w, const char *app) {
    const char *dot = strrchr(app, '.'); // as in org.codeberg.dnkl.foot

    snprintf(w->app, sizeof(w->app), "%s", dot && dot[1] ? dot + 1 : app);
    for (char *c = w->app; *c; c++)
        *c = tolower((unsigned char)*c);
    add_damage(extent_of(w));
    damage_taskbar();
}

void wm_window_translucent(struct window *w, bool on) {
    w->translucent = on;
    add_damage(extent_of(w));
}

void wm_window_cursor(struct window *w, enum gui_cursor cursor) {
    w->cursor = cursor;
    update_cursor();
}

void wm_notify(struct window *from, const char *title, const char *body, const char *label, const char *command) {
    struct notice *n = add_notice(title, body, from ? from->app : NULL);

    if (!settings.quiet)
        play_sound(SOUND_NOTICE);
    if (label && command)
        notice_action(n, label, command);
}

void wm_settings_changed(void) {
    settings_changed();
}

void wm_unlock(void) {
    if (locked)
        unlock();
}

void wm_desktop_drop(const char *path) {
    if (locked || (bar_visible() && contains(taskbar(), cursor_x, cursor_y)))
        return;
    printf("wm: %s dropped on the desktop\n", path);
    copy_to_desktop(path);
}

void wm_clipboard_changed(char *data, size_t length, int width, int height) {
    free(clipboard);
    clipboard = data;
    clipboard_length = data ? length : 0;
    clipboard_width = width;
    clipboard_height = height;
    remember_clipboard();
}

// The log-in screen stopping without anyone logging in is started again, after a
// second if it only just started, so one that can't run doesn't keep wm busy.
void wm_login_gone(void) {
    login_running = false;
    if (locked)
        login_due = login_started + 1000;
}

// Whoever's logged in can lock their desktop, but not the greeter's, and not on
// another Linux, which has its own.
static bool can_lock(void) {
    return !login_mode && !getenv("WM_NESTED");
}

// Starts the log-in screen, or the lock screen, with a connection of its own, so wm
// knows its windows from any other's.
static void start_login(void) {
    login_due = -1;
    login_started = now_ms();
    login_running = server_start_login(LOGIN_PATH, logged_in ? "--lock" : NULL);
    if (!login_running) {
        printf("wm: can't start the log-in screen: %s\n", strerror(errno));
        login_due = login_started + 1000;
    }
}

static void close_menu(void);

// Hides the desktop behind the lock screen, or the log-in screen once logged out.
static void lock(void) {
    close_menu();
    close_panel();
    switch_choice = -1;
    super_down = super_alone = false; // let go while locked
    dragging = grabbed = resizing = NULL;
    drag_file[0] = '\0';
    drag_given = false;
    stop_snap();
    set_focus(NULL);
    fade_screen(true);
    locked = true;
    add_damage((struct rect){0, 0, screen.width, screen.height});
    printf("wm: locked\n");
    if (!login_running)
        start_login();
}

// Logging out closes every program's connection, and so the programs, and
// forgets what they left: the clipboard and notifications.
static void log_out(void) {
    server_close_apps();
    free(clipboard);
    clipboard = NULL;
    clipboard_length = clipboard_width = clipboard_height = 0;
    clear_clips();
    notice_count = 0;
    clear_history();
    logged_in = false;
    workspace = 0;
    printf("wm: logged out\n");
    play_sound(SOUND_LOG_OUT);
    quitting = true; // and the session shows the log-in screen again
}

static void unlock(void) {
    if (login_mode && login_running) { // the greeter's done, and greetd starts the desktop as them
        printf("wm: logged in\n");
        quitting = true;
        return;
    }
    fade_screen(false);
    locked = false;
    add_damage((struct rect){0, 0, screen.width, screen.height});
    focus_top();
    if (logged_in)
        printf("wm: unlocked\n");
    if (!logged_in) {
        logged_in = true;
        play_sound(SOUND_LOG_IN);
        for (int i = 0; i < command_count; i++)
            launch(commands[i]);
        command_count = 0; // only the first time
    }
}

// Which edges of a frame a resize from a point would move: those it's on, or near
// the end of.
static int edges_at(struct rect f, int x, int y) {
    bool left = x < f.x + BORDER, right = x >= f.x + f.width - BORDER;
    bool top = y < f.y + BORDER, bottom = y >= f.y + f.height - BORDER;

    if (left || right) {
        top |= y < f.y + CORNER;
        bottom |= y >= f.y + f.height - CORNER;
    } else if (top || bottom) {
        left = x < f.x + CORNER;
        right = x >= f.x + f.width - CORNER;
    }
    return (left ? EDGE_LEFT : 0) | (right ? EDGE_RIGHT : 0) | (top ? EDGE_TOP : 0) | (bottom ? EDGE_BOTTOM : 0);
}

// Which window is at a point, and which part of it. edges says which edges a
// resize there would move.
static struct window *window_at(int x, int y, enum part *part, int *edges) {
    for (int i = window_count - 1; i >= 0 && !overview; i--) {
        struct window *w = windows[i];
        struct rect f = frame_of(w);
        int margin = w->bare ? 0 : RESIZE_MARGIN;
        struct rect grab = {f.x - margin, f.y - margin, f.width + 2 * margin, f.height + 2 * margin};

        if (!visible(w) || !contains(grab, x, y))
            continue;
        *edges = w->bare ? 0 : edges_at(f, x, y);
        *part = *edges ? PART_RESIZE
              : contains(button_of(w, 0), x, y) ? PART_CLOSE
              : contains(button_of(w, 1), x, y) ? PART_MAXIMIZE
              : contains(button_of(w, 2), x, y) ? PART_MINIMIZE
              : contains((struct rect){w->x, w->y, w->canvas.width, w->canvas.height}, x, y) ? PART_CONTENT
                                                                                            : PART_TITLE;
        return w;
    }
    *part = PART_NONE;
    return NULL;
}

struct surface *wm_surface_at(int x, int y, int *inside_x, int *inside_y) {
    enum part part;
    int edges;
    struct window *w = window_at(x, y, &part, &edges);

    if (!w || part != PART_CONTENT)
        return NULL;
    *inside_x = x - w->x, *inside_y = y - w->y;
    return w->surface;
}

static void send_mouse(struct window *w, uint32_t type, uint32_t changed, int wheel) {
    send_to(w, (struct gui_message){
                   .type = type, .x = cursor_x - w->x, .y = cursor_y - w->y,
                   .buttons = buttons, .changed = changed, .wheel = wheel, .key.modifiers = modifiers,
               });
}

static void launch(const char *command) {
    if (fork() == 0) {
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
}

// Opens a file in the app the settings choose for its kind, or runs it if it's a program.
static void open_file(const char *path) {
    const char *app = settings.apps[gui_kind_of(path)];
    char program[GUI_PATH_MAX];
    bool runs = gui_kind_of(path) != GUI_KIND_FOLDER && gui_is_program(path);

    if (!runs && !gui_app_path(app, program))
        return;
    printf(runs ? "wm: running %s\n" : "wm: opening %s in %s\n", path, app);
    if (gui_kind_of(path) != GUI_KIND_FOLDER)
        gui_recent_add(path);
    if (fork() == 0) {
        if (runs)
            execl(path, strrchr(path, '/') ? strrchr(path, '/') + 1 : path, (char *)NULL);
        else
            execl(program, app, path, (char *)NULL);
        _exit(127);
    }
}

// Puts pixels on the clipboard, as a picture, every one of the canvas's own.
static bool copy_picture(const struct gui_canvas *from) {
    int width = from->width * gui_scale_of(from), height = from->height * gui_scale_of(from);
    size_t row = (size_t)width * sizeof(uint32_t);
    char *copy = malloc(row * height);

    if (!copy)
        return false;
    for (int y = 0; y < height; y++)
        memcpy(copy + y * row, from->pixels + (long)y * from->stride, row);
    free(clipboard);
    clipboard = copy;
    clipboard_length = row * height;
    clipboard_width = width;
    clipboard_height = height;
    server_set_clipboard(clipboard, clipboard_length, clipboard_width, clipboard_height);
    return true;
}

// The part of a rectangle on the screen.
static struct rect on_screen(struct rect r) {
    int right = r.x + r.width < screen.width ? r.x + r.width : screen.width;
    int bottom = r.y + r.height < screen.height ? r.y + r.height : screen.height;

    r.x = r.x > 0 ? r.x : 0;
    r.y = r.y > 0 ? r.y : 0;
    return (struct rect){r.x, r.y, right - r.x, bottom - r.y};
}

// Saves a picture of the screen in a new file in Pictures, which the notice saying
// so opens, with a button to show it in Files. It's put on the clipboard as well.
static void save_shot(const struct gui_canvas *shot) {
    char dir[GUI_PATH_MAX], path[GUI_PATH_MAX], command[GUI_PATH_MAX + 32], stamp[32];
    const char *home = getenv("HOME");
    time_t now = time(NULL);

    snprintf(dir, sizeof(dir), "%s/Pictures", home && strcmp(home, "/") ? home : "");
    mkdir(dir, 0755);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d-%H%M%S", localtime(&now));
    snprintf(path, sizeof(path), "%s/screenshot-%s.png", dir, stamp);
    for (int i = 2; access(path, F_OK) == 0; i++)
        snprintf(path, sizeof(path), "%s/screenshot-%s-%d.png", dir, stamp, i);
    if (!gui_image_save(path, shot)) {
        printf("wm: screenshot %s: %s\n", path, gui_image_error());
        add_notice("Couldn't save the screenshot", gui_image_error(), NULL);
        return;
    }
    printf("wm: screenshot %s %dx%d\n", path, shot->width, shot->height);
    play_sound(SOUND_SHUTTER);

    struct notice *n = add_notice(copy_picture(shot) ? "Screenshot saved and copied" : "Screenshot saved",
                                  strrchr(path, '/') + 1, "viewer");

    snprintf(command, sizeof(command), "files --select '%s'", path);
    notice_file(n, path);
    notice_action(n, "Show", command);
}

// Print Screen saves the screen, and with Alt the focused window, as it's shown but
// without the cursor.
static void screenshot(bool window) {
    struct rect r = on_screen(window && focused ? frame_of(focused) : (struct rect){0, 0, screen.width, screen.height});

    if (r.width <= 0 || r.height <= 0)
        return;
    no_cursor = true;
    compose(r);
    no_cursor = false;
    add_damage(r); // to put the cursor back
    struct gui_canvas shot = gui_part(&screen, r.x, r.y, r.width, r.height);

    save_shot(&shot);
}

static void start_choosing(void) {
    struct rect all = {0, 0, screen.width, screen.height};
    uint32_t *pixels;

    if (choosing.pixels || !(pixels = malloc((size_t)screen.width * screen.height * scale * scale * sizeof(uint32_t))))
        return;
    close_menu();
    close_panel();
    hide_preview();
    set_overview(false);
    no_cursor = true;
    compose(all);
    no_cursor = false;
    for (int y = 0; y < screen.height * scale; y++)
        memcpy(pixels + (long)y * screen.width * scale, screen.pixels + (long)y * screen.stride,
               screen.width * scale * sizeof(uint32_t));
    choosing.pixels = pixels;
    choosing.dragging = false;
    add_damage(all);
    printf("wm: choosing an area\n");
}

static void stop_choosing(void) {
    free(choosing.pixels);
    choosing.pixels = NULL;
    add_damage((struct rect){0, 0, screen.width, screen.height});
}

// The mouse while choosing, which only this gets.
static void choose_with_mouse(uint32_t pressed, uint32_t released, bool moved) {
    if (pressed & MOUSE_BUTTON_RIGHT) {
        printf("wm: screenshot stopped\n");
        stop_choosing();
        return;
    }
    if (pressed & MOUSE_BUTTON_LEFT) {
        choosing.dragging = true;
        choosing.x = cursor_x;
        choosing.y = cursor_y;
        choosing.shown = (struct rect){0};
    }
    if (choosing.dragging && moved)
        choosing_moved();
    if (!(released & MOUSE_BUTTON_LEFT) || !choosing.dragging)
        return;

    // A click, rather than a drag, takes what it's on.
    struct rect r = chosen_area();
    enum part part;
    int edges;

    if (r.width < 4 && r.height < 4) {
        struct window *w = window_at(cursor_x, cursor_y, &part, &edges);

        r = on_screen(w ? frame_of(w) : (struct rect){0, 0, screen.width, screen.height});
    }
    struct gui_canvas was = {choosing.pixels, screen.width, screen.height, screen.width * scale, scale};

    struct gui_canvas shot = gui_part(&was, r.x, r.y, r.width, r.height);

    save_shot(&shot);
    stop_choosing();
}

static void close_menu(void) {
    if (popup_depth)
        flyout_leaves(popups[0].r);
    damage_popups();
    popup_depth = popup_focus = 0;
    start_open = false;
    autohide();
}

static struct item *add_item(struct popup *p, int action, const char *label, const char *arg,
                             const struct gui_image *icon) {
    static struct item spare; // what goes past the end of a full menu
    struct item *item = p->count < MAX_ITEMS ? &p->items[p->count++] : &spare;

    *item = (struct item){.action = action, .icon = icon};
    snprintf(item->label, sizeof(item->label), "%s", label);
    snprintf(item->arg, sizeof(item->arg), "%s", arg ? arg : "");
    return item;
}

static void add_check(struct popup *p, int action, const char *label, bool on) {
    struct item *item = add_item(p, action, label, NULL, NULL);

    item->check = true;
    item->checked = on;
}

struct named {
    char name[GUI_TITLE_MAX];
    bool folder;
};

static int compare_named(const void *a, const void *b) {
    const struct named *x = a, *y = b;

    return x->folder != y->folder ? y->folder - x->folder : strcmp(x->name, y->name);
}

// The apps in a folder, and with folders, the folders in it first, as they are.
static void add_apps(struct popup *p, const char *folder, bool folders) {
    struct named found[MAX_ITEMS];
    int count = 0;
    DIR *dir = opendir(folder);
    struct dirent *d;

    while (dir && count < MAX_ITEMS && (d = readdir(dir))) {
        char path[GUI_PATH_MAX];
        struct stat st;

        snprintf(path, sizeof(path), "%s/%s", folder, d->d_name);
        if (d->d_name[0] == '.' || stat(path, &st) < 0 || (S_ISDIR(st.st_mode) && !folders))
            continue;
        found[count].folder = S_ISDIR(st.st_mode);
        snprintf(found[count++].name, GUI_TITLE_MAX, "%s", d->d_name);
    }
    if (dir)
        closedir(dir);
    qsort(found, count, sizeof(*found), compare_named);
    for (int i = 0; i < count; i++) {
        char path[GUI_PATH_MAX];

        snprintf(path, sizeof(path), "%s/%s", folder, found[i].name);
        add_item(p, found[i].folder ? ACT_FOLDER : ACT_APP, found[i].name, path,
                 found[i].folder ? gui_icon("folder") : app_icon(found[i].name));
    }
}

static int first_choosable(const struct popup *p) {
    for (int i = 0; i < p->count; i++) {
        if (choosable(&p->items[i]))
            return i;
    }
    return -1;
}

// Says where a menu is, for the tests to find its items.
static void show_popup(int level) {
    struct popup *p = &popups[level];

    popup_depth = level + 1;
    if (level == 0)
        menu_opened = now_ms();
    add_damage(p->r);
    autohide();
    printf("wm: menu %d at %d %d %d %d\n", level, p->r.x, p->r.y, p->r.width, p->r.height);
}

// Typing with the start menu open searches the apps' names, in every folder.
static char search[32];

static bool has_text(const char *name, const char *text) {
    for (; *name; name++) {
        size_t i = 0;

        while (text[i] && tolower((unsigned char)name[i]) == tolower((unsigned char)text[i]))
            i++;
        if (!text[i])
            return true;
    }
    return !*text;
}

static void add_found(struct popup *p) {
    char names[MAX_ITEMS][32], path[GUI_PATH_MAX], heading[GUI_TITLE_MAX];
    int count = gui_app_names(names, MAX_ITEMS), found = 0;

    snprintf(heading, sizeof(heading), "Search: %s", search);
    add_item(p, ACT_NONE, heading, NULL, NULL);
    for (int i = 0; i < count; i++) {
        if (has_text(names[i], search) && gui_app_path(names[i], path)) {
            add_item(p, ACT_APP, names[i], path, app_icon(names[i]));
            found++;
        }
    }
    if (!found)
        add_item(p, ACT_NONE, "No apps match", NULL, NULL);
    printf("wm: search %s: %d apps\n", search, found);
}

// The apps, by the folders in APPS_DIR, listed afresh each time so new ones show
// up, or those a search finds.
static void fill_start_menu(void) {
    struct popup *p = &popups[0];

    add_damage(p->r);
    p->count = 0;
    if (search[0]) {
        add_found(p);
        place_popup(p, GAP, 0, MENU_WIDTH);
        p->r.y = beside_bar(p->r.height);
        p->choice = first_choosable(p);
        add_damage(p->r);
        return;
    }
    add_apps(p, APPS_DIR, true);
    if (!p->count)
        add_item(p, ACT_NONE, "No apps in " APPS_DIR, NULL, NULL);
    add_item(p, ACT_LINE, "", NULL, NULL);
    add_item(p, ACT_RUN, "Run...", NULL, app_icon(RUN_COMMAND));
    if (can_lock()) { // locking and logging out are for a desktop someone logged in to
        add_item(p, ACT_LOCK, "Lock", NULL, gui_icon("lock"));
        add_item(p, ACT_LOG_OUT, "Log out", NULL, gui_icon("logout"));
    }
    add_item(p, ACT_SUSPEND, "Suspend", NULL, gui_icon("suspend"));
    add_item(p, ACT_REBOOT, "Reboot", NULL, gui_icon("reboot"));
    add_item(p, ACT_SHUT_DOWN, "Shut down", NULL, gui_icon("shutdown"));
    place_popup(p, GAP, 0, MENU_WIDTH);
    p->r.y = beside_bar(p->r.height);
    p->choice = first_choosable(p);
    add_damage(p->r);
}

static void open_start_menu(void) {
    set_overview(false);
    close_panel();
    close_menu();
    search[0] = '\0';
    fill_start_menu();
    start_open = true;
    show_popup(0);
}

// What the power button asks: whether to suspend, restart or shut down, in the
// middle of the screen.
static void open_power_menu(void) {
    struct popup *p = &popups[0];

    set_overview(false);
    close_panel();
    close_menu();
    p->count = 0;
    if (can_lock())
        add_item(p, ACT_LOCK, "Lock", NULL, gui_icon("lock"));
    add_item(p, ACT_SUSPEND, "Suspend", NULL, gui_icon("suspend"));
    add_item(p, ACT_REBOOT, "Reboot", NULL, gui_icon("reboot"));
    add_item(p, ACT_SHUT_DOWN, "Shut down", NULL, gui_icon("shutdown"));
    place_popup(p, (screen.width - MENU_WIDTH) / 2, 0, MENU_WIDTH);
    p->r.y = (screen.height - p->r.height) / 2;
    p->choice = first_choosable(p);
    show_popup(0);
    printf("wm: power menu\n");
}

static void close_submenu(void);

// Letters typed and Backspace change the search, which the menu then shows.
static bool search_key(const struct key_event *e) {
    size_t len = strlen(search);
    bool typed = e->text >= ' ' && e->text < 127 && !(e->modifiers & (KEY_MOD_CTRL | KEY_MOD_ALT | KEY_MOD_SUPER));

    if (!start_open || !popup_depth || popup_focus || !(typed || (e->code == KEY_BACKSPACE && len)))
        return false;
    if (!e->pressed)
        return true;
    if (typed && len < sizeof(search) - 1) {
        search[len] = (char)e->text;
        search[len + 1] = '\0';
    } else if (!typed) {
        search[len - 1] = '\0';
    }
    close_submenu();
    fill_start_menu();
    printf("wm: menu 0 at %d %d %d %d\n", popups[0].r.x, popups[0].r.y, popups[0].r.width, popups[0].r.height);
    return true;
}

// What can be done with an icon on the desktop, as in Files, or on the desktop itself.
static void open_desktop_menu(int index) {
    struct popup *p = &popups[0];

    close_panel();
    close_menu();
    p->count = 0;
    if (index >= 0) {
        bool folder = desktop_items[index].folder;
        char editor[GUI_PATH_MAX];

        desktop_path(index, popup_file);
        add_item(p, ACT_OPEN, "Open", NULL, NULL);
        if (!folder && gui_app_path("editor", editor))
            add_item(p, ACT_OPEN_IN, "Open in Editor", editor, NULL);
        if (!folder)
            add_item(p, ACT_OPEN_WITH, "Open with", NULL, NULL);
        if (!folder && gui_is_program(popup_file))
            add_item(p, ACT_RUN_FILE, "Run", NULL, NULL);
        add_item(p, ACT_LINE, "", NULL, NULL);
        add_item(p, ACT_CUT, "Cut", NULL, NULL);
        add_item(p, ACT_COPY, "Copy", NULL, NULL);
        add_item(p, ACT_COPY_PATH, "Copy path", NULL, NULL);
        add_item(p, ACT_RENAME, "Rename", NULL, NULL);
        add_item(p, ACT_DELETE, "Delete", NULL, NULL);
        add_item(p, ACT_LINE, "", NULL, NULL);
        add_item(p, ACT_PROPERTIES, "Properties", NULL, NULL);
    } else {
        add_item(p, ACT_NEW_FOLDER, "New folder", NULL, NULL);
        add_item(p, ACT_NEW_FILE, "New file", NULL, NULL);
        add_item(p, ACT_LINE, "", NULL, NULL);
        add_item(p, ACT_REFRESH, "Refresh", NULL, NULL);
        add_check(p, ACT_SHOW_ICONS, "Show icons", !settings.hide_icons);
        add_item(p, ACT_LINE, "", NULL, NULL);
        add_item(p, ACT_SETTINGS, "Settings", NULL, NULL);
    }
    place_popup(p, cursor_x, cursor_y, MENU_WIDTH);
    p->choice = -1;
    show_popup(0);
}

// A window's taskbar button's: what can be done with the window from there.
static void toggle_maximized(struct window *w);

static void open_window_menu(struct window *w) {
    struct popup *p = &popups[0];

    close_panel();
    close_menu();
    menu_window = w;
    p->count = 0;
    add_item(p, w->minimized ? ACT_WIN_SHOW : ACT_WIN_MINIMIZE, w->minimized ? "Restore" : "Minimize", NULL, NULL);
    if (!w->bare)
        add_item(p, ACT_WIN_MAXIMIZE, w->tiled ? "Restore size" : "Maximize", NULL, NULL);
    add_item(p, ACT_LINE, "", NULL, NULL);
    add_item(p, ACT_WIN_CLOSE, "Close", NULL, NULL);
    place_popup(p, cursor_x, 0, MENU_WIDTH);
    p->r.y = beside_bar(p->r.height);
    p->choice = -1;
    show_popup(0);
}

static void open_taskbar_menu(void) {
    struct popup *p = &popups[0];

    close_panel();
    close_menu();
    p->count = 0;
    add_item(p, ACT_POSITION, "Position", NULL, NULL);
    add_check(p, ACT_AUTOHIDE, "Autohide taskbar", settings.autohide);
    place_popup(p, cursor_x, 0, MENU_WIDTH);
    p->r.y = beside_bar(p->r.height);
    p->choice = -1;
    show_popup(0);
}

static void close_submenu(void) {
    if (popup_depth > 1)
        add_damage(popups[1].r);
    popup_depth = 1;
    popup_focus = 0;
}

// Beside the item it's for, or on the other side if there isn't room.
static void open_submenu(int index) {
    const struct item *parent = &popups[0].items[index];
    struct popup *p = &popups[1], *menu = &popups[0];
    struct rect r = item_rect(menu, index);
    int width = 180, x = menu->r.x + menu->r.width - 2;

    close_submenu();
    p->count = 0;
    if (parent->action == ACT_FOLDER) {
        add_apps(p, parent->arg, false);
    } else if (parent->action == ACT_OPEN_WITH) {
        char names[MAX_ITEMS][32], path[GUI_PATH_MAX];
        int count = gui_app_names(names, MAX_ITEMS);

        for (int i = 0; i < count; i++) {
            if (gui_app_path(names[i], path))
                add_item(p, ACT_OPEN_IN, names[i], path, app_icon(names[i]));
        }
    } else {
        add_check(p, ACT_TOP, "Top", settings.taskbar_top);
        add_check(p, ACT_BOTTOM, "Bottom", !settings.taskbar_top);
    }
    if (!p->count)
        add_item(p, ACT_NONE, "Empty", NULL, NULL);
    place_popup(p, x + width > screen.width ? menu->r.x - width + 2 : x, r.y - 4, width);
    p->choice = -1;
    show_popup(1);
}

// Logging out, rebooting and shutting down ask every window to close first, so
// programs can save what they have, and go ahead once all have closed. Any still
// open after LEAVE_MS, waiting for an answer about changes, stop it.
#define LEAVE_MS 1500
static int leaving = -1; // the item waiting for windows to close
static int64_t leave_due;

static void leave(int item) {
    if (item == ITEM_LOG_OUT) {
        log_out();
        return;
    }
    if (getenv("WM_NESTED")) { // in a window on another desktop, the computer isn't wm's to turn off
        quitting = true;
        return;
    }
    if (fork() == 0) { // systemd stops everything, wm too
        execlp("systemctl", "systemctl", item == ITEM_REBOOT ? "reboot" : "poweroff", (char *)NULL);
        _exit(127);
    }
}

static void start_leaving(int item) {
    if (!window_count) {
        leave(item);
        return;
    }
    leaving = item;
    leave_due = now_ms() + LEAVE_MS;
    for (int i = 0; i < window_count; i++)
        send_to(windows[i], (struct gui_message){.type = GUI_CLOSE});
    printf("wm: asking %d windows to close\n", window_count);
}

// How long until it's time to give up waiting, or -1 if nothing is.
static int check_leaving(void) {
    if (leaving < 0)
        return -1;
    if (!window_count) {
        int item = leaving;

        leaving = -1;
        leave(item);
        return -1;
    }
    if (now_ms() < leave_due)
        return (int)(leave_due - now_ms());
    add_notice(leaving == ITEM_LOG_OUT ? "Not logged out" : leaving == ITEM_REBOOT ? "Not restarted" : "Not shut down",
               "A program is still open", NULL);
    leaving = -1;
    return -1;
}

static void load_desktop(bool always);
static void trash_file(const char *path);

// Saves a setting changed here, and tells everyone, as Settings does.
static void save_settings(const char *what) {
    if (gui_settings_save(&settings))
        printf("wm: %s\n", what);
    settings_changed();
}

// What the desktop's file is for the clipboard: its path, and when it's cut, where
// Files looks to move it, rather than copy it, when it's pasted.
#define CUT "/tmp/files-cut"
static void put_path(const char *path, bool cut) {
    FILE *f;

    free(clipboard);
    clipboard = strdup(path);
    clipboard_length = clipboard ? strlen(path) : 0;
    clipboard_width = clipboard_height = 0;
    remember_clipboard();
    unlink(CUT);
    if (cut && (f = fopen(CUT, "w"))) {
        fputs(path, f);
        fclose(f);
    }
    printf("wm: %s %s\n", cut ? "cut" : "copied", path);
}

// Runs a program, with an option and a file, either of which may be NULL.
static void start(const char *program, const char *option, const char *file) {
    const char *name = strrchr(program, '/') ? strrchr(program, '/') + 1 : program;

    if (fork() == 0) {
        execl(program, name, option ? option : file, option ? file : (char *)NULL, (char *)NULL);
        _exit(127);
    }
}

static void files_with(const char *option, const char *file) {
    char files[GUI_PATH_MAX];

    if (gui_app_path("files", files))
        start(files, option, file);
}

static void act(const struct item *item) {
    switch (item->action) {
    case ACT_APP:
        start(item->arg, NULL, NULL);
        break;
    case ACT_RUN:
        launch(RUN_COMMAND);
        break;
    case ACT_LOCK:
        lock();
        break;
    case ACT_SUSPEND:
        suspend_soon();
        break;
    case ACT_LOG_OUT:
    case ACT_REBOOT:
    case ACT_SHUT_DOWN:
        start_leaving(item->action == ACT_LOG_OUT ? ITEM_LOG_OUT : item->action == ACT_REBOOT ? ITEM_REBOOT : ITEM_SHUT_DOWN);
        break;
    case ACT_OPEN:
        open_file(popup_file);
        break;
    case ACT_OPEN_IN:
        printf("wm: opening %s in %s\n", popup_file, item->label);
        start(item->arg, NULL, popup_file);
        break;
    case ACT_RUN_FILE:
        printf("wm: running %s\n", popup_file);
        start(popup_file, NULL, NULL);
        break;
    case ACT_CUT:
    case ACT_COPY:
    case ACT_COPY_PATH:
        put_path(popup_file, item->action == ACT_CUT);
        break;
    case ACT_RENAME:
        start_desktop_rename(strrchr(popup_file, '/') + 1);
        break;
    case ACT_PROPERTIES:
        files_with("--properties", popup_file);
        break;
    case ACT_DELETE:
        trash_file(popup_file);
        break;
    case ACT_NEW_FOLDER:
    case ACT_NEW_FILE:
        desktop_new(item->action == ACT_NEW_FOLDER);
        break;
    case ACT_SETTINGS: {
        char settings_app[GUI_PATH_MAX];

        if (gui_app_path("settings", settings_app))
            start(settings_app, NULL, NULL);
        break;
    }
    case ACT_WIN_SHOW:
    case ACT_WIN_MINIMIZE:
    case ACT_WIN_MAXIMIZE:
    case ACT_WIN_CLOSE:
        if (!menu_window)
            break;
        printf("wm: %s from its taskbar button\n", item->label);
        if (item->action == ACT_WIN_SHOW)
            show(menu_window);
        else if (item->action == ACT_WIN_MINIMIZE)
            minimize(menu_window);
        else if (item->action == ACT_WIN_MAXIMIZE) {
            show(menu_window);
            toggle_maximized(menu_window);
        } else
            send_to(menu_window, (struct gui_message){.type = GUI_CLOSE});
        break;
    case ACT_REFRESH:
        load_desktop(true);
        break;
    case ACT_SHOW_ICONS:
        settings.hide_icons = !settings.hide_icons;
        save_settings(settings.hide_icons ? "icons hidden" : "icons shown");
        break;
    case ACT_TOP:
    case ACT_BOTTOM:
        settings.taskbar_top = item->action == ACT_TOP;
        save_settings(settings.taskbar_top ? "taskbar top" : "taskbar bottom");
        break;
    case ACT_AUTOHIDE:
        settings.autohide = !settings.autohide;
        save_settings(settings.autohide ? "autohide on" : "autohide off");
        break;
    }
}

// An item with a submenu opens it, with the keyboard, and any other does what
// it's for, closing the menus.
static void choose(int level, int index) {
    struct item item = popups[level].items[index];

    if (has_submenu(&item) && level == 0) {
        open_submenu(index);
        popup_focus = 1;
        popups[1].choice = first_choosable(&popups[1]);
    } else if (choosable(&item) && !has_submenu(&item)) {
        close_menu();
        act(&item);
    }
}

// The mouse over a menu highlights what it's on, opening its submenu if it has one.
static void hover_menu(void) {
    for (int level = popup_depth - 1; level >= 0; level--) {
        struct popup *p = &popups[level];
        int i = item_at(p, cursor_x, cursor_y);
        bool open = popup_depth > 1 && p->choice == i;

        if (!contains(p->r, cursor_x, cursor_y))
            continue;
        popup_focus = level;
        if (i < 0 || i == p->choice)
            return;
        p->choice = i;
        add_damage(p->r);
        if (level == 0 && has_submenu(&p->items[i]) && !open)
            open_submenu(i);
        else if (level == 0 && !has_submenu(&p->items[i]))
            close_submenu();
        return;
    }
}

// A press in a menu chooses what it's on, and anywhere else closes the menus,
// returning whether it should go no further: when it was in one, or on the
// launcher, which would open the start menu again.
static bool menu_press(uint32_t changed) {
    for (int level = popup_depth - 1; level >= 0; level--) {
        struct popup *p = &popups[level];
        int i = item_at(p, cursor_x, cursor_y);

        if (!contains(p->r, cursor_x, cursor_y))
            continue;
        if (i >= 0 && (changed & MOUSE_BUTTON_LEFT))
            choose(level, i);
        return true;
    }

    bool on_launcher = start_open && bar_visible() && contains(launcher(), cursor_x, cursor_y);

    close_menu();
    return on_launcher;
}

static void move_window(struct window *w, int x, int y) {
    add_damage(extent_of(w));
    w->x = x;
    w->y = y;
    add_damage(extent_of(w));
}

// Puts a window's frame on part of the desktop, keeping where it was to go back to.
static void place(struct window *w, struct rect frame) {
    if (!w->tiled)
        w->restore = frame_of(w);
    w->tiled = true;
    w->resize_edges = 0;
    move_window(w, frame.x + BORDER, frame.y + TITLE_HEIGHT + BORDER);
    ask_size(w, frame.width - 2 * BORDER, frame.height - TITLE_HEIGHT - 2 * BORDER);
}

static void untile(struct window *w) {
    struct rect r = w->restore;

    w->tiled = false;
    w->resize_edges = 0;
    move_window(w, r.x + BORDER, r.y + TITLE_HEIGHT + BORDER);
    ask_size(w, r.width - 2 * BORDER, r.height - TITLE_HEIGHT - 2 * BORDER);
}

// Moves a window, smoothly, to a part of the desktop, or back where it was for SNAP_NONE.
static void snap_to(struct window *w, enum snap to) {
    struct rect was = frame_of(w);

    if (to == SNAP_NONE)
        untile(w);
    else
        place(w, snap_rect(to));
    if (to != SNAP_NONE)
        play_sound(SOUND_SNAP);
    w->snapped = to;
    reframe(w, was);
}

static void toggle_maximized(struct window *w) {
    snap_to(w, w->tiled ? SNAP_NONE : SNAP_TOP);
}

// Super+Up and Down take a window on one side to its top or bottom quarter, and
// from a quarter back to the side.
static enum snap snap_up(enum snap from) {
    return from == SNAP_LEFT ? SNAP_TOP_LEFT : from == SNAP_RIGHT ? SNAP_TOP_RIGHT
         : from == SNAP_BOTTOM_LEFT ? SNAP_LEFT : from == SNAP_BOTTOM_RIGHT ? SNAP_RIGHT : SNAP_NONE;
}

static enum snap snap_down(enum snap from) {
    return from == SNAP_LEFT ? SNAP_BOTTOM_LEFT : from == SNAP_RIGHT ? SNAP_BOTTOM_RIGHT
         : from == SNAP_TOP_LEFT ? SNAP_LEFT : from == SNAP_TOP_RIGHT ? SNAP_RIGHT : SNAP_NONE;
}

static void taskbar_click(void) {
    struct window *list[MAX_WINDOWS];
    int count = windows_by_age(list);

    if (contains(launcher(), cursor_x, cursor_y))
        open_start_menu();
    for (int i = 0; i < WORKSPACES; i++) {
        if (contains(workspace_box(i), cursor_x, cursor_y))
            go_to_workspace(i);
    }
    for (int i = 0; i < count; i++) {
        if (!contains(taskbar_button(i, count), cursor_x, cursor_y))
            continue;
        if (list[i] == focused)
            minimize(list[i]);
        else
            show(list[i]);
    }
}

static void press_here(uint32_t changed) {
    enum part part;
    int edges;
    struct window *w = window_at(cursor_x, cursor_y, &part, &edges);

    super_alone = false;

    // While it's locked, a press only goes to the log-in screen.
    if (locked) {
        if (w) {
            set_focus(w);
            grabbed = w;
            send_mouse(w, GUI_MOUSE_BUTTON, changed, 0);
        }
        return;
    }

    if (layouts_click())
        return;
    hide_layouts();

    // Clicking a notification puts it away.
    int notice = popup_depth || panel >= 0 ? -1 : notice_at(cursor_x, cursor_y);

    if (notice >= 0) {
        if (notices[notice].label[0] && contains(notice_button(notice), cursor_x, cursor_y)) {
            printf("wm: notice action %s: %s\n", notices[notice].label, notices[notice].command);
            launch(notices[notice].command);
        } else if (notices[notice].file[0]) {
            open_file(notices[notice].file);
        }
        remove_notice(notice);
        return;
    }

    // A click in an open panel does what it's on, and anywhere else closes it. Its
    // icon in the tray closes it too, and another's opens that one.
    int item = bar_visible() ? tray_item_at(cursor_x, cursor_y) : -1;

    if (panel >= 0) {
        int was = panel;

        if (panel_click())
            return;
        close_panel();
        if (item == was)
            return;
    }
    if (item >= 0 && (changed & MOUSE_BUTTON_LEFT)) {
        close_menu();
        open_panel(item);
        return;
    }

    // A click anywhere closes a menu, and does what it would have done unless it
    // was in the menu or on the launcher.
    if (popup_depth && menu_press(changed))
        return;
    if (bar_visible() && contains(taskbar(), cursor_x, cursor_y)) {
        if (changed & MOUSE_BUTTON_LEFT)
            taskbar_click();
        else if (changed & MOUSE_BUTTON_RIGHT) {
            struct window *list[MAX_WINDOWS], *on = NULL;
            int count = windows_by_age(list);

            for (int i = 0; i < count; i++)
                on = contains(taskbar_button(i, count), cursor_x, cursor_y) ? list[i] : on;
            if (on)
                open_window_menu(on);
            else
                open_taskbar_menu();
        }
        return;
    }
    if (!w) {
        if (changed & MOUSE_BUTTON_LEFT)
            desktop_press();
        else if (changed & MOUSE_BUTTON_RIGHT) {
            int index = desktop_item_at(cursor_x, cursor_y);

            select_desktop_item(index);
            open_desktop_menu(index);
        }
        return;
    }
    set_focus(w);
    if (w != windows[window_count - 1])
        raise_window(w);

    if (part == PART_CONTENT) {
        grabbed = w;
        send_mouse(w, GUI_MOUSE_BUTTON, changed, 0);
    } else if (part == PART_CLOSE) {
        send_to(w, (struct gui_message){.type = GUI_CLOSE});
    } else if (part == PART_MAXIMIZE) {
        toggle_maximized(w);
    } else if (part == PART_MINIMIZE) {
        minimize(w);
    } else if (part == PART_RESIZE && (changed & MOUSE_BUTTON_LEFT)) {
        resizing = w;
        w->resize_edges = edges;
        resize_x = cursor_x;
        resize_y = cursor_y;
        resize_width = w->canvas.width;
        resize_height = w->canvas.height;
        resize_room = frame_of(w).y;
        w->tiled = false;
    } else if (changed & MOUSE_BUTTON_LEFT) {
        dragging = w;
        drag_x = cursor_x - w->x;
        drag_y = cursor_y - w->y;
    }
}

// The show desktop corner puts away the workspace's windows, or brings back those it
// put away if there are none left out.
static void show_desktop(void) {
    struct window *list[MAX_WINDOWS];
    int count = windows_by_age(list), shown = 0;

    for (int i = 0; i < count; i++)
        shown += !list[i]->bare && !list[i]->minimized;
    for (int i = 0; i < count; i++) {
        if (list[i]->bare)
            continue;
        if (shown && !list[i]->minimized) {
            minimize(list[i]);
            list[i]->put_away = true;
        } else if (!shown && list[i]->put_away) {
            show(list[i]);
        }
    }
    printf("wm: desktop %s\n", shown ? "shown" : "covered again");
}

// The gap between the taskbar and the screen's edges counts as the taskbar, so the
// mouse can be thrown at the edge: a press there is taken as one on the bar, in line
// with it, or at its end beside the side gaps. The gap at the right end of it is the
// show desktop corner.
static void press(uint32_t changed) {
    struct rect bar = taskbar();
    enum part part;
    int x = cursor_x, y = cursor_y, edges;
    bool beyond = settings.taskbar_top ? y < bar.y + bar.height : y >= bar.y;
    bool outside = !locked && bar_visible() && beyond && !contains(bar, x, y) && !window_at(x, y, &part, &edges);

    if (outside && x >= bar.x + bar.width && (changed & MOUSE_BUTTON_LEFT)) {
        show_desktop();
        return;
    }
    if (outside) {
        cursor_x = x < bar.x + 4 ? bar.x + 4 : x > bar.x + bar.width - 5 ? bar.x + bar.width - 5 : x;
        cursor_y = bar.y + bar.height / 2;
    }
    press_here(changed);
    cursor_x = x;
    cursor_y = y;
}

// A file let go over a window's contents goes to it, through the drag the server
// was given, and the window comes to the top.
static void drop_file(void) {
    enum part part;
    int edges;
    struct window *w = window_at(cursor_x, cursor_y, &part, &edges);

    add_damage(drag_rect());
    if (!w && !locked && !(bar_visible() && contains(taskbar(), cursor_x, cursor_y)))
    {
        if (desktop_dragged >= 0)
            desktop_drop();
        else
            copy_to_desktop(drag_file);
    }
    desktop_dragged = -1;
    if (w && part == PART_CONTENT && !locked) {
        set_focus(w);
        raise_window(w);
        printf("wm: dropped %s on %s\n", drag_file, w->title);
    }
    drag_file[0] = '\0';
    drag_given = false;
}

// A mouse that says how far it moved goes the speed the settings choose times
// that, out of 5, keeping what's left of a pixel for next time.
static int faster(int d, int axis) {
    static int left[2];
    int scaled = d * settings.mouse_speed + left[axis];

    left[axis] = scaled % 5;
    return scaled / 5;
}

// A hidden taskbar shows while the mouse is at its edge of the screen, or on it,
// or a menu or panel is open.
static void autohide(void) {
    // The bar, and the gap between it and the edge.
    struct rect bar = taskbar(), near = {0, settings.taskbar_top ? 0 : bar.y, screen.width, bar.height + GAP};
    bool at_edge = settings.taskbar_top ? cursor_y == 0 : cursor_y == screen.height - 1;
    bool show = !settings.autohide || at_edge || popup_depth || panel >= 0 || (bar_shown && contains(near, cursor_x, cursor_y));

    if (show == bar_shown)
        return;
    bar_shown = show;
    add_damage(bar);
    printf("wm: taskbar %s\n", show ? "shown" : "hidden");
}

static void handle_mouse(const struct mouse_event *e) {
    enum part part;
    int edges;
    uint32_t pressed = e->buttons & ~buttons, released = buttons & ~e->buttons;
    // To the nearest screen pixel, as the host gives the last one as a little under its share of
    // the range, then the desktop's pixel that's in. Moved, it goes as far in the desktop's pixels.
    int64_t range = MOUSE_ABSOLUTE_MAX + 1;
    int x = e->absolute ? (int)(((int64_t)e->x * output.width + range / 2) / range) / scale : cursor_x + faster(e->dx, 0);
    int y = e->absolute ? (int)(((int64_t)e->y * output.height + range / 2) / range) / scale : cursor_y + faster(e->dy, 1);

    x = x < 0 ? 0 : x >= screen.width ? screen.width - 1 : x;
    y = y < 0 ? 0 : y >= screen.height ? screen.height - 1 : y;

    bool moved = x != cursor_x || y != cursor_y;

    if (moved) {
        damage_cursor();
        cursor_x = x;
        cursor_y = y;
        damage_cursor();
    }
    buttons = e->buttons;
    if (auth_shown) {
        auth_mouse(pressed, moved);
        return;
    }
    if (desktop_renaming[0] && pressed && (renamed_index() < 0 || !contains(rename_field(renamed_index()), cursor_x, cursor_y)))
        desktop_rename_done(true); // a click elsewhere keeps it, then does what it would

    if (choosing.pixels) {
        choose_with_mouse(pressed, released, moved);
        return;
    }
    if (moved)
        autohide();
    if (overview) {
        int at = overview_at(cursor_x, cursor_y);

        if (moved)
            choose_in_overview(at);
        if (pressed & MOUSE_BUTTON_LEFT && at >= 0)
            pick_in_overview(at);
        else if (pressed)
            set_overview(false);
        return;
    }
    if (moved || pressed) {
        hover_taskbar();
        if (!(pressed && layout_shown)) // which the press may be on
            hover_layouts();
    }

    // Windows can be dragged partly off the screen, but the title bar stays on it.
    // A tiled one goes back to its own size, as far across under the mouse as it was.
    if (dragging && moved) {
        int top = cursor_y - drag_y, bottom = desktop().y + desktop().height - BORDER, least = desktop().y + TITLE_HEIGHT;
        struct rect d = desktop();
        bool high = cursor_y < d.y + SNAP_CORNER, low = cursor_y >= d.y + d.height - SNAP_CORNER;
        enum snap to = cursor_x <= 0             ? (high ? SNAP_TOP_LEFT : low ? SNAP_BOTTOM_LEFT : SNAP_LEFT)
                     : cursor_x >= screen.width - 1 ? (high ? SNAP_TOP_RIGHT : low ? SNAP_BOTTOM_RIGHT : SNAP_RIGHT)
                     : cursor_y <= 0               ? SNAP_TOP
                                                   : SNAP_NONE;

        if (dragging->tiled) {
            struct rect r = dragging->restore;

            drag_x = drag_x * (r.width - 2 * BORDER) / dragging->canvas.width;
            untile(dragging);
        }
        move_window(dragging, cursor_x - drag_x, top < least ? least : top > bottom ? bottom : top);
        if (to != snap) {
            add_damage(snap_rect(snap ? snap : to));
            add_damage(snap_rect(to));
            snap = to;
        }
    }
    if (resizing && moved) {
        int edges = resizing->resize_edges, dx = cursor_x - resize_x, dy = cursor_y - resize_y;

        if (edges & EDGE_TOP && dy < -resize_room)
            dy = -resize_room;
        ask_size(resizing, resize_width + (edges & EDGE_RIGHT ? dx : edges & EDGE_LEFT ? -dx : 0),
                 resize_height + (edges & EDGE_BOTTOM ? dy : edges & EDGE_TOP ? -dy : 0));
    }

    // Menus highlight what's under the mouse, and so do the tray and its panels.
    if (popup_depth && moved)
        hover_menu();
    if (panel >= 0 && moved)
        add_damage(panel_rect(panel));
    if (moved && bar_visible())
        add_damage(tray());

    int wheel = e->wheel;

    // The volume follows the mouse while it's held on the slider, and the wheel
    // changes it over the speaker or the sound menu.
    if (setting_volume && moved)
        set_volume_at(cursor_x);
    if (setting_brightness && moved)
        set_brightness_at(cursor_x);
    if (released & MOUSE_BUTTON_LEFT)
        setting_brightness = false;
    if ((released & MOUSE_BUTTON_LEFT) && setting_volume) {
        setting_volume = false;
        play_sound(SOUND_VOLUME); // to hear how loud it is now
    }
    if (e->wheel && mixer >= 0 && !locked
        && ((bar_visible() && contains(tray_item(TRAY_QUICK), cursor_x, cursor_y))
            || (panel == TRAY_QUICK && contains(panel_rect(TRAY_QUICK), cursor_x, cursor_y)))) {
        set_volume(volume - e->wheel * VOLUME_STEP, muted);
        play_sound(SOUND_VOLUME);
        wheel = 0; // so no window gets it
    }

    struct window *target = grabbed ? grabbed : dragging || resizing ? NULL : window_at(cursor_x, cursor_y, &part, &edges);

    if (target && !grabbed && part != PART_CONTENT)
        target = NULL;
    if (target && moved)
        send_mouse(target, GUI_MOUSE_MOVE, 0, 0);
    if (target && wheel)
        send_mouse(target, GUI_MOUSE_WHEEL, 0, wheel);

    if (moved && (buttons & MOUSE_BUTTON_LEFT))
        desktop_drag();
    if (moved && drag_file[0] && !drag_given && !locked && (drag_given = server_drag_file(drag_file)))
        printf("wm: gave %s to the app it's over\n", drag_file);
    if (pressed)
        press(pressed);
    if (released && grabbed)
        send_mouse(grabbed, GUI_MOUSE_BUTTON, released, 0);
    if (!buttons && drag_file[0])
        drop_file();
    if (!buttons && dragging && snap) {
        snap_to(dragging, snap);
        printf("wm: snapped %s %s\n", dragging->title, snap_names[snap]);
        stop_snap();
    }
    if (!buttons) {
        dragging = NULL;
        grabbed = NULL;
        resizing = NULL;
        desktop_pressed = -1;
    }
    update_cursor();
}

static void finish_switch(bool chosen) {
    struct window *w = switcher_window(switch_choice);

    add_damage(switcher());
    switch_choice = -1;
    if (chosen)
        show(w);
}

// Alt+Tab goes down the windows, from the one under the focused one, and Shift goes
// back up. Letting go of Alt picks the one it's on, and Escape stays where it was.
static bool switch_key(const struct key_event *e) {
    if (e->code == KEY_TAB && (e->modifiers & KEY_MOD_ALT)) {
        if (e->pressed && window_count) {
            int step = e->modifiers & KEY_MOD_SHIFT ? -1 : 1;

            switch_choice = ((switch_choice < 0 ? 0 : switch_choice) + step + window_count) % window_count;
            add_damage(switcher());
        }
        return true;
    }
    if (switch_choice < 0)
        return false;
    if (e->code == KEY_ESCAPE && e->pressed) {
        finish_switch(false);
        return true;
    }
    if (!(e->modifiers & KEY_MOD_ALT))
        finish_switch(true);
    return false;
}

// Up and Down move through a menu, Right and Enter open a submenu, Left and Escape
// close it, Enter does what's highlighted, and Escape closes the menu.
static bool menu_key(const struct key_event *e) {
    uint16_t code = e->code;

    if (!popup_depth || (e->modifiers & KEY_MOD_SUPER)
        || !(code == KEY_UP || code == KEY_DOWN || code == KEY_LEFT || code == KEY_RIGHT || code == KEY_ENTER
             || code == KEY_ESCAPE))
        return false;
    if (!e->pressed)
        return true;

    struct popup *p = &popups[popup_focus];

    if (code == KEY_ESCAPE || code == KEY_LEFT) {
        if (popup_focus)
            close_submenu();
        else if (code == KEY_ESCAPE)
            close_menu();
    } else if (code == KEY_ENTER || code == KEY_RIGHT) {
        if (p->choice >= 0 && (code == KEY_ENTER || has_submenu(&p->items[p->choice])))
            choose(popup_focus, p->choice);
    } else if (p->count) {
        int step = code == KEY_DOWN ? 1 : p->count - 1, i = p->choice < 0 ? (code == KEY_DOWN ? p->count - 1 : 0) : p->choice;

        for (int tries = 0; tries < p->count; tries++) {
            i = (i + step) % p->count;
            if (choosable(&p->items[i])) {
                p->choice = i;
                break;
            }
        }
        add_damage(p->r);
        if (popup_focus == 0)
            close_submenu();
    }
    return true;
}

// Super pressed and let go on its own opens or closes the menu.
static void super_key(const struct key_event *e) {
    bool is_super = e->code == KEY_LEFT_SUPER || e->code == KEY_RIGHT_SUPER;

    if (is_super && e->pressed && !super_down)
        super_alone = true;
    else if (!is_super && e->pressed)
        super_alone = false;
    if (is_super)
        super_down = e->pressed;
    if (is_super && !e->pressed && super_alone) {
        if (popup_depth)
            close_menu();
        else
            open_start_menu();
    }
}

static enum gui_cursor resize_cursor(int edges) {
    bool across = edges & (EDGE_LEFT | EDGE_RIGHT), down = edges & (EDGE_TOP | EDGE_BOTTOM);

    if (across && down)
        return !(edges & EDGE_LEFT) == !(edges & EDGE_TOP) ? GUI_CURSOR_RESIZE_NWSE : GUI_CURSOR_RESIZE_NESW;
    return across ? GUI_CURSOR_RESIZE_H : GUI_CURSOR_RESIZE_V;
}

// The cursor shows what dragging would do: resize on an edge, and over a window's
// contents, whatever its program wants.
static void update_cursor(void) {
    enum part part = PART_NONE;
    int edges = 0;
    struct window *w = resizing ? resizing : grabbed ? grabbed : choosing.pixels ? NULL : window_at(cursor_x, cursor_y, &part, &edges);
    enum gui_cursor shape = resizing                                         ? resize_cursor(resizing->resize_edges)
                          : grabbed || (w && part == PART_CONTENT && !popup_depth) ? w->cursor
                          : part == PART_RESIZE && !dragging                 ? resize_cursor(edges)
                                                                             : GUI_CURSOR_ARROW;

    if (shape == cursor_shown)
        return;
    damage_cursor();
    cursor_shown = shape;
    damage_cursor();
}

// Super with something else is for wm, and every other key goes to the focused window.
// The laptop's keys, which work while it's locked too: closing the lid suspends,
// and the power button asks what to do, or suspends from the lock screen. Swipes
// of three fingers go to the workspace either side, or show the overview.
static bool laptop_key(const struct key_event *e) {
    if (e->code == KEY_LID || e->code == KEY_POWER) { // as the settings say
        int does = e->code == KEY_LID ? settings.lid : settings.power_button;

        if (!e->pressed)
            return true;
        if (e->code == KEY_LID ? does == GUI_LID_SUSPEND : does == GUI_POWER_SUSPEND || (does == GUI_POWER_ASK && locked))
            suspend_soon();
        else if (e->code == KEY_LID && does == GUI_LID_LOCK && !locked)
            lock();
        else if (e->code == KEY_POWER && does == GUI_POWER_ASK)
            open_power_menu();
        else if (e->code == KEY_POWER && does == GUI_POWER_OFF && fork() == 0) {
            execlp("systemctl", "systemctl", "poweroff", (char *)NULL);
            _exit(127);
        }
    } else if (e->code == KEY_BRIGHTNESS_UP || e->code == KEY_BRIGHTNESS_DOWN) {
        if (e->pressed && backlight[0])
            step_brightness(e->code == KEY_BRIGHTNESS_UP ? 1 : -1);
    } else if (e->code == KEY_VOLUME_UP || e->code == KEY_VOLUME_DOWN || e->code == KEY_MUTE) {
        if (e->pressed && mixer >= 0) {
            set_volume(volume + (e->code == KEY_VOLUME_UP ? VOLUME_STEP : e->code == KEY_VOLUME_DOWN ? -VOLUME_STEP : 0),
                       e->code == KEY_MUTE ? !muted : e->code == KEY_VOLUME_UP ? false : muted);
            show_osd(true);
        }
    } else if (e->code == KEY_SWIPE_LEFT || e->code == KEY_SWIPE_RIGHT || e->code == KEY_SWIPE_UP) {
        if (!e->pressed || locked)
            return true;
        printf("wm: swipe %s\n", e->code == KEY_SWIPE_UP ? "up" : e->code == KEY_SWIPE_LEFT ? "left" : "right");
        if (e->code == KEY_SWIPE_UP)
            set_overview(!overview);
        else if (!overview) // fingers going left bring the workspace on the right, as a page turns
            go_to_workspace((workspace + (e->code == KEY_SWIPE_LEFT ? 1 : WORKSPACES - 1)) % WORKSPACES);
    } else {
        return false;
    }
    return true;
}

static void handle_key(const struct key_event *e) {
    bool super = e->modifiers & KEY_MOD_SUPER;

    modifiers = e->modifiers;

    // Ctrl+Alt+F1 to F10 show that console, even while locked, as the text ones log in for themselves.
    if ((e->modifiers & (KEY_MOD_CTRL | KEY_MOD_ALT)) == (KEY_MOD_CTRL | KEY_MOD_ALT) && e->code >= KEY_F1
        && e->code <= KEY_F10 && display->switch_session) {
        if (e->pressed)
            display->switch_session(e->code - KEY_F1 + 1);
        return;
    }
    if (laptop_key(e))
        return;
    if (auth_shown) {
        auth_key(e);
        return;
    }
    if (desktop_renaming[0] && !locked) {
        desktop_rename_key(e);
        return;
    }
    if (locked) {
        if (focused && focused->login)
            send_to(focused, (struct gui_message){.type = GUI_KEY, .key = *e});
        return;
    }
    super_key(e);
    if (choosing.pixels) {
        if (e->pressed && e->code == KEY_ESCAPE) {
            printf("wm: screenshot stopped\n");
            stop_choosing();
        }
        return;
    }
    if (overview) {
        overview_key(e);
        return;
    }
    if (wifi_key(e))
        return;
    if (panel >= 0 && e->code == KEY_ESCAPE) {
        if (e->pressed)
            close_panel();
        return;
    }
    if (switch_key(e) || search_key(e) || menu_key(e))
        return;
    if (e->code == KEY_PRINT_SCREEN || e->code == KEY_SYSRQ
        || (super && (e->modifiers & KEY_MOD_SHIFT) && e->code == KEY_S)) { // SysRq is Alt+Print Screen
        if (e->pressed && (e->code == KEY_S || (e->modifiers & KEY_MOD_SHIFT)))
            start_choosing();
        else if (e->pressed)
            screenshot(e->code == KEY_SYSRQ || (e->modifiers & KEY_MOD_ALT));
        return;
    }
    if (super && e->code == KEY_TAB) {
        if (e->pressed)
            set_overview(true);
        return;
    }
    if (super && e->code == KEY_Q) {
        if (e->pressed && (e->modifiers & KEY_MOD_SHIFT))
            quitting = true;
        else if (e->pressed && focused)
            send_to(focused, (struct gui_message){.type = GUI_CLOSE});
        return;
    }
    if (super && e->code >= KEY_1 && e->code < KEY_1 + WORKSPACES) {
        if (e->pressed && (e->modifiers & KEY_MOD_SHIFT))
            send_to_workspace(focused, e->code - KEY_1);
        else if (e->pressed)
            go_to_workspace(e->code - KEY_1);
        return;
    }
    if (e->code == KEY_ESCAPE && (e->modifiers & KEY_MOD_CTRL) && (e->modifiers & KEY_MOD_SHIFT)) {
        if (e->pressed)
            launch(TASKS_COMMAND);
        return;
    }
    if (super && e->code == KEY_L && can_lock()) {
        if (e->pressed)
            lock();
        return;
    }
    if (super && (e->code == KEY_ENTER || e->code == KEY_R)) {
        if (e->pressed)
            launch(e->code == KEY_R ? RUN_COMMAND : LAUNCH_COMMAND);
        return;
    }

    // Up maximizes, left and right take half the screen, up and down from a side
    // take a quarter of it, and down goes back or minimizes.
    if (super && (e->code == KEY_UP || e->code == KEY_DOWN || e->code == KEY_LEFT || e->code == KEY_RIGHT)) {
        enum snap now = focused && focused->tiled ? focused->snapped : SNAP_NONE;
        enum snap to = e->code == KEY_UP ? snap_up(now) : e->code == KEY_DOWN ? snap_down(now) : SNAP_NONE;

        if (!e->pressed || !focused)
            return;
        if (to)
            snap_to(focused, to);
        else if (e->code == KEY_UP)
            toggle_maximized(focused);
        else if (e->code == KEY_DOWN && focused->tiled)
            snap_to(focused, SNAP_NONE);
        else if (e->code == KEY_DOWN)
            minimize(focused);
        else
            snap_to(focused, e->code == KEY_RIGHT ? SNAP_RIGHT : SNAP_LEFT);
        if (to)
            printf("wm: snapped %s %s\n", focused->title, snap_names[to]);
        return;
    }
    if (focused)
        send_to(focused, (struct gui_message){.type = GUI_KEY, .key = *e});
}

// Scaled to cover the screen, keeping its shape, with what's left over either side
// cut off equally. It's only loaded again when it or the screen's size changes.
static void load_wallpaper(void) {
    char path[GUI_PATH_MAX];
    struct gui_image image;

    gui_wallpaper_get(path);
    if (!strcmp(path, wallpaper_path) && wallpaper.width == screen.width && wallpaper.height == screen.height
        && wallpaper.scale == scale)
        return;
    snprintf(wallpaper_path, sizeof(wallpaper_path), "%s", path);
    gui_image_free(&wallpaper_image);
    wallpaper = (struct gui_canvas){0};
    if (!strcmp(path, "none"))
        return;
    if (!gui_image_load(path, &image)) {
        printf("wm: wallpaper %s: %s\n", path, gui_image_error());
        return;
    }

    // To cover the display, in its own pixels.
    int sw = screen.width * scale, sh = screen.height * scale;
    bool wider = (long)sw * image.height >= (long)sh * image.width;
    int width = wider ? sw : (int)((long)image.width * sh / image.height);
    int height = wider ? (int)((long)image.height * sw / image.width) : sh;
    bool ok = gui_image_scale(&image, width, height, &wallpaper_image);

    gui_image_free(&image);
    if (!ok) {
        printf("wm: wallpaper %s: %s\n", path, gui_image_error());
        return;
    }
    for (long i = 0; i < (long)width * height; i++)
        wallpaper_image.pixels[i] &= 0xFFFFFF;
    wallpaper = (struct gui_canvas){
        wallpaper_image.pixels + (long)(height - sh) / 2 * width + (width - sw) / 2,
        screen.width, screen.height, width, scale,
    };
}

// When the desktop changes, tiled windows keep their share of it, and the others
// move to stay on it.
static void fit_windows(struct rect was) {
    struct rect now = desktop();

    for (int i = 0; i < window_count; i++) {
        struct window *w = windows[i];
        struct rect f = frame_of(w);

        if (w->login)
            center(w);
        if (w->bare)
            continue;
        if (w->tiled) {
            place(w, (struct rect){(f.x - was.x) * now.width / was.width + now.x,
                                   (f.y - was.y) * now.height / was.height + now.y, f.width * now.width / was.width,
                                   f.height * now.height / was.height});
            continue;
        }
        f.x = f.x + f.width > now.x + now.width ? now.x + now.width - f.width : f.x;
        f.y = f.y + f.height > now.y + now.height ? now.y + now.height - f.height : f.y;
        move_window(w, (f.x > now.x ? f.x : now.x) + BORDER, (f.y > now.y ? f.y : now.y) + TITLE_HEIGHT + BORDER);
    }
}

// The scale the settings ask for, or the one the screen's size suits.
static int wanted_scale(void) {
    return settings.scale ? settings.scale : output.width >= GUI_SCALE_WIDTH ? 2 : 1;
}

// Makes the screen the display's size over the scale, if it isn't, keeping the
// windows and the mouse on it, and tells every program.
static void fit_screen(void) {
    int to = wanted_scale(), width = output.width / to, height = output.height / to;
    struct rect was = desktop();
    uint32_t *pixels;

    if (screen.pixels && to == scale && width == screen.width && height == screen.height)
        return;
    if (!(pixels = malloc((size_t)width * height * to * to * sizeof(uint32_t)))) {
        quitting = true; // the old screen could run off the display's
        return;
    }
    cursor_x = screen.pixels ? (int)((long)cursor_x * width / screen.width) : width / 2;
    cursor_y = screen.pixels ? (int)((long)cursor_y * height / screen.height) : height / 2;
    free(screen.pixels);
    screen = (struct gui_canvas){pixels, width, height, width * to, to};
    if (scale != to) { // icons for it, and corners as smooth as it can show
        scale = to;
        gui_use_scale(scale);
        forget_icons();
        make_coverage();
    }
    damage_count = 0; // clipped to the old size, so they could run off this one
    add_damage((struct rect){0, 0, width, height});
    if (was.width > 0) {
        fit_windows(was);
        load_desktop(true);
    }
    display->set_scale(scale);
    server_broadcast(&(struct gui_message){.type = GUI_SCREEN});
    printf("wm: running at %dx%d%s%s\n", output.width, output.height, scale > 1 ? " at 2x" : "",
           output.can_set_mode ? "" : ", which can't change");
}

// Changes the screen's resolution.
static void change_resolution(int width, int height) {
    if (display->set_mode(&output, width, height))
        fit_screen();
    else
        printf("wm: can't change to %dx%d\n", width, height);
}

// Settings change the colours, shadows, clock, resolution and wallpaper, so
// everything is drawn again.
static void load_settings(void) {
    struct rect was = desktop();
    bool icons_were_hidden = settings.hide_icons;

    gui_settings_load(&settings);
    make_coverage();
    theme_now = *(settings.light ? &light : &dark);
    theme_now.title_focused = gui_accent_colors[settings.accent];
    theme_now.on_accent = gui_on_accent(settings.accent);
    theme = &theme_now;
    make_night();
    autohide();
    if (desktop().y != was.y || desktop().height != was.height) {
        fit_windows(was);
        load_desktop(true); // on a grid that's moved
    }
    if (icons_were_hidden != settings.hide_icons)
        desktop_selected = -1;
    display->set_repeat(settings.repeat_delay, settings.repeat_rate);

    // The volume may have been set elsewhere, as Settings does.
    if (output.can_set_mode && settings.width && (settings.width != output.width || settings.height != output.height))
        change_resolution(settings.width, settings.height);
    fit_screen();
    load_wallpaper();
    add_damage((struct rect){0, 0, screen.width, screen.height});
}

// wm, then every program, loads the settings again.
static void settings_changed(void) {
    load_settings();
    server_broadcast(&(struct gui_message){.type = GUI_SETTINGS});
}

static void quit(void) {
    quitting = true;
}

// SIGUSR1 saves the screen as it's shown, to $XDG_RUNTIME_DIR/wm-screen.png, for the tests.
static volatile sig_atomic_t dump_asked;

static void ask_dump(int signal) {
    (void)signal;
    dump_asked = 1;
}

static void dump_screen(void) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    char path[GUI_PATH_MAX];

    dump_asked = 0;
    snprintf(path, sizeof(path), "%s/wm-screen.png", runtime ? runtime : "/tmp");
    printf(gui_image_save(path, &screen) ? "wm: saved the screen to %s\n" : "wm: can't save the screen to %s\n", path);
}

// Updates: checkupdates says what's waiting, Arch's and AximOS's, a few minutes
// after logging in, then each day, and on SIGUSR2, for the tests. When there are
// some, and more than were said last, a notice says so, which opens Settings' Updates.
#define UPDATE_FIRST_MS (5 * 60 * 1000)
#define UPDATE_EVERY_MS (24 * 60 * 60 * 1000)
static int64_t update_due = -1;
static int update_fd = -1, updates_told;
static char update_said[16384];
static size_t update_said_length;
static volatile sig_atomic_t update_asked;

static void ask_updates(int signal) {
    (void)signal;
    update_asked = 1;
}

static void check_updates(void) {
    int out[2];
    pid_t child;

    update_asked = 0;
    update_due = now_ms() + UPDATE_EVERY_MS;
    if (update_fd >= 0 || pipe2(out, O_CLOEXEC) < 0)
        return;
    if ((child = fork()) == 0) {
        dup2(out[1], STDOUT_FILENO);
        execlp("checkupdates", "checkupdates", (char *)NULL);
        _exit(127);
    }
    close(out[1]);
    if (child < 0) {
        close(out[0]);
        return;
    }
    update_fd = out[0];
    update_said_length = 0;
    fcntl(update_fd, F_SETFL, O_NONBLOCK);
    printf("wm: looking for updates\n");
}

static void read_updates(void) {
    ssize_t n;
    int count = 0;

    while ((n = read(update_fd, update_said + update_said_length, sizeof(update_said) - 1 - update_said_length)) > 0)
        update_said_length += n;
    if (n < 0 && errno == EAGAIN)
        return;
    close(update_fd);
    update_fd = -1;
    update_said[update_said_length] = '\0';
    for (const char *at = update_said; (at = strstr(at, " -> ")); at += 4)
        count++;
    printf("wm: %d updates\n", count);
    if (count > updates_told) {
        struct notice *n = add_notice(count == 1 ? "An update is ready" : "Updates are ready", "Settings can install them", "settings");

        if (n)
            notice_action(n, "Open", GUI_APPS "/System/settings Updates");
        printf("wm: told of %d updates\n", count);
    }
    updates_told = count;
}

static int fail(const char *what) {
    dprintf(STDERR_FILENO, "wm: %s: %s\n", what, strerror(errno));
    return 1;
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: wm [--login] [command]...\n"
                            "Runs the window system, starting each command in it. Super+Shift+Q quits.\n"
                            "With --login, it starts locked, behind the log-in screen, and the commands\n"
                            "start once someone's logged in. Super+L locks it again.\n"
                            "It runs on the screen itself, or in a window when started on another desktop.\n");
    display = &wm_server;
    net = network_now();
    bt = bluetooth_now();
    setvbuf(stdout, NULL, _IOLBF, 0); // as the tests read it
    login_mode = argc > 1 && !strcmp(argv[1], "--login");
    commands = argv + 1 + login_mode;
    command_count = argc - 1 - login_mode;
    signal(SIGPIPE, SIG_IGN);
    sigaction(SIGUSR1, &(struct sigaction){.sa_handler = ask_dump}, NULL);
    sigaction(SIGUSR2, &(struct sigaction){.sa_handler = ask_updates}, NULL);

    if (!display->open(&output))
        return 1;

    // The apps run by name, as Super+Enter's terminal does, from their folders.
    char path[4 * GUI_PATH_MAX];
    int used = snprintf(path, sizeof(path), "%s", GUI_APPS);
    struct dirent *d;
    DIR *apps = opendir(GUI_APPS);

    while (apps && (d = readdir(apps)) && used < (int)sizeof(path)) {
        if (d->d_type == DT_DIR && d->d_name[0] != '.')
            used += snprintf(path + used, sizeof(path) - used, ":%s/%s", GUI_APPS, d->d_name);
    }
    if (apps)
        closedir(apps);
    snprintf(path + used, sizeof(path) - used, ":%s", getenv("PATH") ? getenv("PATH") : "/usr/bin");
    setenv("PATH", path, 1);

    // Programs start in the home folder, which has the desktop's.
    const char *home = getenv("HOME");

    snprintf(desktop_dir, sizeof(desktop_dir), "%s/Desktop", home && strcmp(home, "/") ? home : "");
    if (home && chdir(home) < 0)
        printf("wm: can't go to %s\n", home);
    mkdir(desktop_dir, 0755);
    // What apps put in the desktop's folder, or take from it, shows straight away.
    int desktop_watch = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);

    if (desktop_watch >= 0 && inotify_add_watch(desktop_watch, desktop_dir,
                                                IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO) < 0) {
        close(desktop_watch);
        desktop_watch = -1;
    }
    fit_screen();
    if (!screen.pixels)
        return fail("screen");
    make_falloff();
    load_settings();
    load_volume();
    load_desktop(false);
    if (login_mode)
        lock();
    else
        unlock();
    fflush(stdout);

    char clock[16], shown_clock[16];
    int64_t net_checked = now_ms();

    snprintf(shown_clock, sizeof(shown_clock), "%s", clock_text(clock));
    if (!network_open())
        printf("wm: no D-Bus, so no network\n");
    if (!login_mode && !bluetooth_open())
        printf("wm: no D-Bus, so no Bluetooth\n");
    if (!login_mode && !polkit_open())
        printf("wm: no polkit agent, so apps can't ask for an admin's password\n");
    find_backlight();
    check_battery();
    report_tray();
    while (!quitting) {
        struct pollfd fds[7 + DISPLAY_FDS] = {{.fd = audio_watch, .events = POLLIN}};
        struct pollfd *input = fds + 7;
        int network_wait, polkit_wait, bluetooth_wait, fd_count = 7 + display->poll_fds(input);

        fds[4] = (struct pollfd){.fd = update_fd, .events = POLLIN};
        fds[5] = (struct pollfd){.fd = desktop_watch, .events = POLLIN};
        if (update_due < 0 && logged_in && !login_mode)
            update_due = now_ms() + UPDATE_FIRST_MS;
        if (update_asked || (update_due >= 0 && now_ms() >= update_due))
            check_updates();

        network_poll(&fds[1], &network_wait);
        polkit_poll(&fds[2], &polkit_wait);
        bluetooth_poll(&fds[6], &bluetooth_wait);

        int preview = check_preview(); // before the flyouts, so their own start moving
        int layout_wait = check_layouts();
        bool fading = fade_shadows() | animate() | animate_flyouts() | animate_screen();
        int notice = expire_notices(); // before drawing, so those that went are gone

        redraw();
        report_taskbar();

        // The clock changes at the start of each minute, fading shadows each frame, and
        // the network's looked at every NET_CHECK_MS.
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        int until_minute = (60 - now.tv_sec % 60) * 1000 - now.tv_nsec / 1000000;
        int timeout = fading ? FRAME_MS : until_minute > 0 ? until_minute : 1;
        int64_t until_check = net_checked + NET_CHECK_MS - now_ms();

        timeout = until_check < timeout ? (until_check > 0 ? (int)until_check : 0) : timeout;

        timeout = notice >= 0 && notice < timeout ? (notice > 0 ? notice : 1) : timeout;
        timeout = preview >= 0 && preview < timeout ? preview : timeout;
        timeout = layout_wait >= 0 && layout_wait < timeout ? layout_wait : timeout;

        int leave_wait = check_leaving();

        timeout = leave_wait >= 0 && leave_wait < timeout ? leave_wait : timeout;

        int laptop_wait = check_laptop();

        timeout = laptop_wait >= 0 && (timeout < 0 || laptop_wait < timeout) ? laptop_wait : timeout;
        if (login_due >= 0) {
            int64_t wait = login_due - now_ms();

            if (wait <= 0)
                start_login();
            else if (wait < timeout)
                timeout = (int)wait;
        }

        timeout = display->max_wait >= 0 && (timeout < 0 || timeout > display->max_wait) ? display->max_wait : timeout;
        timeout = network_wait >= 0 && (timeout < 0 || network_wait < timeout) ? network_wait : timeout;
        timeout = polkit_wait >= 0 && (timeout < 0 || polkit_wait < timeout) ? polkit_wait : timeout;
        timeout = bluetooth_wait >= 0 && (timeout < 0 || bluetooth_wait < timeout) ? bluetooth_wait : timeout;
        if (update_due >= 0) {
            int64_t wait = update_due - now_ms();

            timeout = timeout < 0 || wait < timeout ? (wait > 0 ? (int)wait : 0) : timeout;
        }

        int ready = poll(fds, fd_count, timeout);

        if (ready < 0 && errno != EINTR)
            return fail("poll");
        if (strcmp(clock_text(clock), shown_clock)) {
            snprintf(shown_clock, sizeof(shown_clock), "%s", clock_text(clock));
            damage_taskbar();
        }
        if (now_ms() >= net_checked + NET_CHECK_MS) {
            check_battery();
            load_desktop(false);
            load_volume(); // unless it's watched already
            net_checked = now_ms();
        }

        if (fds[0].revents)
            mixer_changed();

        char was_joining[SSID_MAX];
        int joined;

        snprintf(was_joining, sizeof(was_joining), "%s", net->joining);
        if (network_read(&joined))
            network_changed(was_joining, joined);
        if (polkit_read())
            auth_changed();

        int bt_done;

        if (bluetooth_read(&bt_done))
            bluetooth_changed(bt_done);
        if (update_fd >= 0 && fds[4].revents)
            read_updates();
        if (desktop_watch >= 0 && fds[5].revents) {
            char events[4096] __attribute__((aligned(__alignof__(struct inotify_event))));

            while (read(desktop_watch, events, sizeof(events)) > 0)
                ;
            load_desktop(false);
        }

        if (display->read(input, &output, &(struct input){handle_mouse, handle_key, quit}))
            fit_screen();
        if (dump_asked)
            dump_screen();
        while (waitpid(-1, NULL, WNOHANG) > 0)
            ;
    }
    printf("wm: quit\n");
    return 0;
}
