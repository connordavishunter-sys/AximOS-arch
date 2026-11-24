#pragma once

#include <key_event.h>
#include <stddef.h>
#include <stdint.h>

// Programs draw windows as Wayland clients, on wm, AximOS's compositor, or any other.
// Each draws on a canvas of its own, and updates copy what changed to shared memory
// the compositor shows. Pixels are 0x00RRGGBB, like the screen's.

// Where AximOS's files are installed: under /usr, or wherever the build says.
#ifndef GUI_PREFIX
#define GUI_PREFIX "/usr"
#endif
#define GUI_SHARE GUI_PREFIX "/share/aximos"  // icons, fonts, wallpapers and sounds
#define GUI_LIBEXEC GUI_PREFIX "/lib/aximos"  // what only the desktop runs, such as the log-in screen
#define GUI_APPS GUI_LIBEXEC "/apps"          // the apps, in the start menu's folders
#define GUI_BIN GUI_PREFIX "/bin"
#define GUI_TITLE_MAX 64
#define GUI_PATH_MAX 1024

// The events a program gets, as libgui has them: from the compositor, as Wayland's
// events are put, and from libgui itself. Mouse positions are inside the window.
enum gui_message_type {
    GUI_SETTINGS = 1, // the settings have changed, and should be loaded again
    GUI_MOUSE_MOVE,   // x, y, buttons held
    GUI_MOUSE_BUTTON, // x, y, buttons held, and changed says which changed
    GUI_MOUSE_WHEEL,  // x, y and wheel
    GUI_KEY,          // key
    GUI_FOCUS,        // focused is whether the window now has the keyboard
    GUI_CLOSE,        // the close button was clicked, or Super+Q pressed
    GUI_RESIZE,       // width and height the compositor would like the window to be
    GUI_SCREEN,       // width and height of the screen, with x 1 if its resolution can change,
                      // and y its scale: the screen's pixels are y times as wide and high.
                      // After the window's made, and whenever it changes.
    GUI_DROP,         // x, y, as a file's dropped on the window, which gui_dropped_file gives

    // From libgui itself, through gui_post: draw the window again.
    GUI_REDRAW,
    GUI_TIMER, // x is the timer that's due
};

// The shapes of mouse cursor wm draws.
enum gui_cursor {
    GUI_CURSOR_ARROW,
    GUI_CURSOR_TEXT,
    GUI_CURSOR_RESIZE_H,    // left and right
    GUI_CURSOR_RESIZE_V,    // up and down
    GUI_CURSOR_RESIZE_NWSE, // top left and bottom right
    GUI_CURSOR_RESIZE_NESW, // top right and bottom left
};

struct gui_message {
    uint32_t type;
    uint32_t window;
    int32_t x, y, width, height;
    uint32_t buttons, changed;
    int32_t wheel;
    uint32_t focused;
    struct key_event key;
    char text[GUI_TITLE_MAX];
    char buffer[GUI_TITLE_MAX];
};

// Something to draw on: the screen's copy, or a window.
struct gui_canvas {
    uint32_t *pixels;
    int width, height;
    int stride; // pixels from one row to the next
};

// Programs draw on canvas, which is private, and updates copy what changed to the
// pixels the compositor shows, so it never shows a half-drawn frame.
struct gui_backend_window;
struct gui_window {
    uint32_t id;
    struct gui_canvas canvas;
    struct gui_backend_window *backend;
};

// Connects to the compositor, returning false if there's none. The connection is
// closed when the program exits.
bool gui_connect(void);

// The connection, for poll, which a program waiting on other files too polls after
// gui_ready says nothing's waiting already.
int gui_fd(void);
bool gui_ready(void);
// How long until libgui has something of its own due, a timer or a key repeating,
// in milliseconds, or -1 for nothing.
int gui_due_ms(void);

// From the log-in or lock screen wm started, once someone's in.
bool gui_unlock(void);

// Returns NULL if wm refused or memory ran out.
struct gui_window *gui_create_window(const char *title, int width, int height);
void gui_destroy_window(struct gui_window *window);

// Gives the window a new size, usually when GUI_RESIZE asks, with a blank canvas to
// draw on. wm goes on showing the old size until the next update. Returns
// false, leaving the window as it was, if memory ran out.
bool gui_resize(struct gui_window *window, int width, int height);

void gui_set_title(struct gui_window *window, const char *title);

// Icons are GUI_SHARE/icons/apps/name.png, and a new window has the one named
// after the program, as it was run.
void gui_set_icon(struct gui_window *window, const char *name);
void gui_set_cursor(struct gui_window *window, enum gui_cursor cursor);
// With it on, wm shows what's under the window through its pixels, blurred, as
// much as each one's top byte says: 0 not at all, up to 255, nearly all.
void gui_set_translucent(struct gui_window *window, bool on);

// Shows a notification in the corner of the screen for a few seconds, with the
// window's icon, if it's given. What doesn't fit in GUI_TITLE_MAX is cut short.
bool gui_notify(struct gui_window *window, const char *title, const char *text);
// The same, with a button labelled label that runs command, as Super+R would.
bool gui_notify_action(struct gui_window *window, const char *title, const char *text, const char *label,
                       const char *command);

// Puts text on the clipboard, returning false if memory ran out.
bool gui_clipboard_set(const char *text, size_t length);

// What's on the clipboard, ending in a zero byte, to be freed, or NULL if it's
// empty or a picture. length, if not NULL, gets its length.
char *gui_clipboard_get(size_t *length);

// Puts a picture on the clipboard, or gets one, to be freed, returning false if
// there's none.
struct gui_image;
bool gui_clipboard_set_image(const struct gui_canvas *from);
bool gui_clipboard_get_image(struct gui_image *image);

// Drags a file from the window the mouse is held in, which wm shows by the cursor
// and gives to the window it's let go over, as a GUI_DROP.
bool gui_drag_file(const char *path);

// The path a GUI_DROP gives, or false if it couldn't be had.
bool gui_dropped_file(const struct gui_message *drop, char path[GUI_PATH_MAX]);

// Shows what was drawn on part of a window's canvas, or all of it with gui_show.
void gui_update(struct gui_window *window, int x, int y, int width, int height);
void gui_show(struct gui_window *window);

// Waits for the next event, returning false when wm has gone.
bool gui_next_event(struct gui_message *event);
// The same, waiting no more than ms milliseconds, or forever if it's negative:
// 1 with an event, 0 if none came in time, and -1 when wm has gone.
int gui_wait_event(struct gui_message *event, int ms);

// Puts an event at the end of those gui_next_event gives, as if it came from wm.
void gui_post(const struct gui_message *event);

// gui_next_event gives a GUI_TIMER every ms milliseconds, or only once without
// repeat, with x the number this returns. A timer that's fallen behind doesn't
// make up what it missed. Returns -1 when GUI_MAX_TIMERS are running.
#define GUI_MAX_TIMERS 16
int gui_timer_start(int ms, bool repeat);
void gui_timer_stop(int timer);

// Drawing, clipped to the canvas.
void gui_fill(struct gui_canvas *canvas, int x, int y, int width, int height, uint32_t color);
void gui_copy(struct gui_canvas *canvas, int x, int y, const struct gui_canvas *from, int from_x, int from_y,
              int width, int height);

// Rectangles with rounded corners, smoothed: filled, and filled with a line round
// them, either of which can be GUI_NONE, which no pixel is.
#define GUI_NONE 0xFFFFFFFF
#define GUI_MAX_RADIUS 16
#define GUI_CORNERS 8 // windows' corner radius unless the settings say otherwise
void gui_fill_rounded(struct gui_canvas *canvas, int x, int y, int width, int height, int radius, uint32_t color);
void gui_frame_rounded(struct gui_canvas *canvas, int x, int y, int width, int height, int radius, uint32_t fill,
                       uint32_t line);

// Rounding what's drawn some other way: the pixels in its corners kept before it's
// drawn, then put back outside its rounded edge, with a line along it, or GUI_NONE.
void gui_keep_corners(const struct gui_canvas *canvas, int x, int y, int width, int height, int radius,
                      uint32_t kept[4][GUI_MAX_RADIUS][GUI_MAX_RADIUS]);
void gui_round_corners(struct gui_canvas *canvas, int x, int y, int width, int height, int radius,
                       uint32_t kept[4][GUI_MAX_RADIUS][GUI_MAX_RADIUS], uint32_t line);

// How much of a pixel, out of 256, is inside a corner rounded to radius, x and y
// in from the corner.
int gui_corner_coverage(int x, int y, int radius);

// Text 16 pixels high, scaled up by scale, in DejaVu Sans, each character as wide
// as it is, or with sharp text, the 8x16 bitmap font. Monospaced text, for what's
// in columns, has every character GUI_CHAR_WIDTH * scale wide.
#define GUI_CHAR_WIDTH 8
#define GUI_CHAR_HEIGHT 16
void gui_text(struct gui_canvas *canvas, int x, int y, const char *text, uint32_t color, int scale);
void gui_mono_text(struct gui_canvas *canvas, int x, int y, const char *text, uint32_t color, int scale);

// A character in a terminal's cell, or two for a wide one: box-drawing and block
// characters drawn to meet their neighbours, others from the monospaced font, its
// bold, or DejaVu Sans for those it hasn't, and a box for those none has.
void gui_cell_char(struct gui_canvas *canvas, int x, int y, uint32_t codepoint, int cells, bool bold, uint32_t color);
int gui_text_width(const char *text, int scale);
// How many of the first length bytes of text fit in width, at scale 1.
size_t gui_text_fit(const char *text, size_t length, int width);

// Files by what they hold, each opened in an app the settings choose.
enum { GUI_KIND_FOLDER, GUI_KIND_PICTURE, GUI_KIND_SOUND, GUI_KIND_PAGE, GUI_KIND_TEXT, GUI_KINDS };
extern const char *const gui_kinds[GUI_KINDS]; // their names in the settings

// The desktop's settings, kept in ~/.config/aximos/gui.conf.
struct gui_settings {
    bool light;         // the light theme, rather than the dark one
    bool shadows;       // under windows
    bool clock_12_hour; // on the taskbar, rather than 24 hour
    int width, height;  // the screen's resolution, or 0 to leave it as it started
    int repeat_delay;   // milliseconds a key's held before it repeats
    int repeat_rate;    // times a second it repeats then, or 0 for never
    bool sharp_text;    // the bitmap font, rather than text smoothed from a TrueType one
    int mouse_speed;    // 1 to 10, with 5 as the mouse moves it
    char apps[GUI_KINDS][32]; // which app in GUI_APPS opens each kind of file
    int accent;         // one of gui_accent_names
    bool taskbar_top;   // rather than along the bottom
    bool autohide;      // the taskbar, until the mouse goes to its edge of the screen
    bool hide_icons;    // on the desktop
    bool night_light;   // less blue on the screen, by night_strength, from 10 to 100
    int night_strength;
    bool airplane;      // the network card sends and takes nothing
    bool animations;    // windows grow in, shrink away, and minimize to the taskbar
    bool quiet;         // do not disturb: notifications go straight to the tray, without showing
    bool sounds;        // wm's own: a chime for notifications, logging in and out, the volume
    bool transparency;  // the taskbar, title bars, menus and the terminal show what's under them
    bool blur;          // and blur it
    int corners;        // windows' corner radius, 0 for square, up to GUI_MAX_RADIUS, with
                        // smaller ones, like buttons', rounded in proportion
    int scale;          // 1 or 2 screen pixels to each of the desktop's, or 0 for 2 on screens
                        // GUI_SCALE_WIDTH wide or more, and 1 on smaller ones
};

#define GUI_SCALE_WIDTH 2560

// The accent colours there are, as Settings names them, and what goes on each.
#define GUI_ACCENTS 8
extern const char *const gui_accent_names[GUI_ACCENTS];
extern const uint32_t gui_accent_colors[GUI_ACCENTS];
uint32_t gui_on_accent(int accent);

// a to b by amount out of 256.
uint32_t gui_mix(uint32_t a, uint32_t b, int amount);

void gui_settings_load(struct gui_settings *settings);
bool gui_settings_save(const struct gui_settings *settings);

// What kind of file is at path, and the app the settings say opens it.
int gui_kind_of(const char *path);
const char *gui_app_for(const char *path);

// Apps are in GUI_APPS, or a folder in it, as the start menu shows them.
// Finds one by name, returning false if there's none.
bool gui_app_path(const char *name, char path[GUI_PATH_MAX]);

// Every app's name, sorted, up to max, returning how many there are.
int gui_app_names(char (*names)[32], int max);

// Whether a file is a program: an ELF file, which opening runs.
bool gui_is_program(const char *path);

// What's each person's own, such as their settings, the files they opened lately and
// their trash, is in ~/.config/aximos, which is made the first time it's needed.
// Each path is good until a few more are asked for.
const char *gui_own_path(const char *name);

// Files opened lately, newest first, a path to a line in GUI_RECENT.
#define GUI_RECENT gui_own_path("recent")
#define GUI_RECENT_MAX 20
void gui_recent_add(const char *path);
int gui_recent_list(char (*paths)[GUI_PATH_MAX], int max);
void gui_recent_clear(void);

// The trash is GUI_TRASH, where each thing has a random name, with what it was
// called and where it was in GUI_TRASH_DB, a line each: the name, a tab, the path.
#define GUI_TRASH gui_own_path("trash")
#define GUI_TRASH_DB gui_own_path("trash.db")

// Moves a file, or a folder and all in it, to the trash.
bool gui_trash(const char *path);

// Where something in the trash, by its name there, came from, or false if it's not known.
bool gui_trash_origin(const char *name, char path[GUI_PATH_MAX]);

// Puts it back where it was, as near as it can, saying where in path.
bool gui_trash_restore(const char *name, char path[GUI_PATH_MAX]);

// Deletes it, or everything in the trash, for good.
bool gui_trash_delete(const char *name);
bool gui_trash_empty(void);

// Deletes a file, or a folder and all in it, and copies one.
bool gui_delete_tree(const char *path);
bool gui_copy_tree(const char *from, const char *to);

// Tells wm the settings have changed, and through it every program with a window.
void gui_settings_changed(void);

// The screen, as of the last GUI_SCREEN.
struct gui_screen {
    int width, height; // in the desktop's pixels, scale screen pixels each way
    bool can_resize;   // whether its resolution can change
    int scale;
};

const struct gui_screen *gui_screen(void);

// The wallpaper, an image's path or "none", kept in ~/.config/aximos/wallpaper, and the
// ones that come with the system.
#define GUI_WALLPAPERS GUI_SHARE "/wallpapers"
void gui_wallpaper_get(char path[GUI_PATH_MAX]);
bool gui_wallpaper_set(const char *path);

// The time zones there are, sorted, from malloc, returning how many; the one
// chosen, or UTC if none is; and choosing one, returning false if it couldn't be.
int gui_time_zones(char ***names);
void gui_time_zone_get(char name[GUI_PATH_MAX]);
bool gui_time_zone_set(const char *name);

// Images, as pixels of 0xAARRGGBB, from PNG, JPEG, BMP, GIF (its first frame) and
// WebP files. Each returns false if it couldn't, with gui_image_error saying why.
struct gui_image {
    uint32_t *pixels;
    int width, height;
};

bool gui_image_load(const char *path, struct gui_image *image);
bool gui_image_decode(const void *data, size_t size, struct gui_image *image);
const char *gui_image_error(void);
void gui_image_free(struct gui_image *image);

// A smoothly scaled copy, to be freed as well.
bool gui_image_scale(const struct gui_image *from, int width, int height, struct gui_image *to);

// Draws an image, blending what's partly transparent with what's under it.
void gui_draw_image(struct gui_canvas *canvas, int x, int y, const struct gui_image *image);
// Saves pixels as a PNG file, returning false, with gui_image_error saying why, if it couldn't.
bool gui_image_save(const char *path, const struct gui_canvas *from);

// Icons, 16 pixels square, from GUI_SHARE/icons/name.png, loaded once and
// kept. NULL if there's no such icon.
#define GUI_ICON_SIZE 16
const struct gui_image *gui_icon(const char *name);

// The icon for a file by what its name ends in, or for a folder.
const struct gui_image *gui_file_icon(const char *name, bool folder);

// Widgets, drawn immediately in the style of Dear ImGui. Each frame, a program
// calls them in order, top to bottom, and each draws itself and says what was done
// to it. Frames are drawn after events:
//
//     while (gui_next_event(&e)) {
//         if (e.type == GUI_CLOSE)
//             break;
//         if (gui_ui_event(&ui, &e)) {
//             gui_ui_begin(&ui);
//             if (gui_button(&ui, "Save"))
//                 save();
//             gui_ui_end(&ui);
//         }
//     }
//
// Labels name widgets as well as showing on them, so two with the same label need
// "##" after it and something different, which isn't shown. Tab and Shift+Tab move
// the keyboard between widgets, and Enter or Space presses what it's on.

struct gui_rect {
    int x, y, width, height;
};

#define GUI_UI_MAX_KEYS 16
#define GUI_UI_MAX_FOCUSABLE 64
#define GUI_UI_MAX_SCROLLS 16
#define GUI_UI_MAX_DEPTH 4
#define GUI_UI_MAX_MENUS 8

// Where widgets go: across from left to right, and down from y, inside clip.
struct gui_layout {
    int left, right;
    int x, y, line_height, line_end;
    bool same_line;
    struct gui_rect clip;
};

// How far down something scrolling is, and how tall what's in it was last frame.
struct gui_scroll {
    uint32_t id;
    int offset, content;
};

struct gui_ui {
    struct gui_window *window;
    bool window_focused;

    // This frame's input.
    int mouse_x, mouse_y;
    bool mouse_down, mouse_pressed, mouse_released;
    bool double_click; // the press was the second in quick succession
    bool right_pressed;
    uint16_t click_modifiers; // the keys held at the last press
    bool *marks;       // for the next list, from gui_list_marks
    long last_press;   // when the last press was, in milliseconds
    int wheel;
    struct key_event keys[GUI_UI_MAX_KEYS];
    int key_count;

    // Widgets by the hash of their label: under the mouse, being pressed or
    // dragged, and with the keyboard.
    uint32_t hot, active, focus;
    enum gui_cursor cursor, shown_cursor; // what the widget under the mouse wants, and what wm shows
    size_t text_cursor, text_anchor; // in the text box that last had the keyboard, and
    uint32_t text_owner;             // where what's selected starts
    int blink_timer;                 // while a text box has the keyboard, the cursor blinks
    bool cursor_hidden, text_drawn;
    int drag_offset; // from the top of a scroll bar's thumb to the mouse

    // The widgets that can have the keyboard, in order, as of the last frame and
    // so far in this one, for Tab and Shift+Tab.
    uint32_t order[GUI_UI_MAX_FOCUSABLE], next_order[GUI_UI_MAX_FOCUSABLE];
    int order_count, next_count;

    // What scrolls, and which of it was innermost under the mouse last frame, so
    // gets the wheel.
    struct gui_scroll scrolls[GUI_UI_MAX_SCROLLS];
    uint32_t wheel_owner, next_wheel_owner;

    // The open dropdown or menu, which is drawn over everything else, and gets
    // the mouse when it's over it.
    uint32_t popup;
    bool popup_is_menu, popup_drawn, in_popup, mouse_in_popup;
    struct gui_rect popup_rect, outside_popup;
    uint32_t *popup_pixels;
    uint32_t popup_corners[4][GUI_MAX_RADIUS][GUI_MAX_RADIUS]; // what was under its rounded corners
    int item_y, item_width, item_index, bar_x;

    // The menus in the bar, as of the last frame and so far in this one. An open
    // menu has the keyboard: Up and Down move menu_choice through its items, of
    // which it had menu_items last frame, and Left and Right go to the next menu.
    uint32_t menus[GUI_UI_MAX_MENUS], next_menus[GUI_UI_MAX_MENUS];
    int menu_count, next_menu_count, menu_choice, menu_items, menu_height;
    int menu_x, menu_y; // where the mouse was when a context menu opened

    // The item in a list the last right-click was on, or -1 if none was.
    int clicked_item;
    struct key_event menu_keys[GUI_UI_MAX_KEYS];
    int menu_key_count;

    // A widget Tab moved the keyboard to, which the areas it's in scroll to show.
    uint32_t reveal;

    // A file dropped at drop_x, drop_y, from gui_ui_drop, and the item of a list or
    // grid, and the tab, it was dropped on that frame, or -1.
    bool dropping;
    int drop_x, drop_y, drop_item, drop_tab;

    // Where the next widget goes, with what it was before each scrolling area.
    struct gui_layout layout;
    struct {
        struct gui_layout layout;
        struct gui_rect area, view;
        uint32_t id;
        int top;
    } saved[GUI_UI_MAX_DEPTH];
    int depth;
    struct gui_rect last;
    bool dialog_shown, dimmed, help_open;
    int dialog_choice;
    struct gui_layout column_saved; // the layout outside a column, and where it starts
    int column_top;
    uint32_t tab_owner, next_tab_owner; // code that takes Tab to indent, rather than moving on
    bool again; // something changed that was already drawn
};

void gui_ui_init(struct gui_ui *ui, struct gui_window *window);

// Takes an event's input, and resizes the window when wm asks. Returns whether the
// frame should be drawn again, which is false for events it has no use for.
bool gui_ui_event(struct gui_ui *ui, const struct gui_message *event);

// Says where a file was dropped, for the next frame to find what it's on.
void gui_ui_drop(struct gui_ui *ui, int x, int y);

// A frame starts with a blank window, and ends by showing it.
void gui_ui_begin(struct gui_ui *ui);
void gui_ui_end(struct gui_ui *ui);

// Puts the next widget to the right of the last, rather than under it.
void gui_same_line(struct gui_ui *ui);
// Room for something the program draws itself, width by height, or across the
// rest of the line if width is 0, returning where it is in the window.
struct gui_rect gui_space(struct gui_ui *ui, int width, int height);

// How much of the line is left, from where the next widget would go.
int gui_room(const struct gui_ui *ui);

// Where the last widget went, in the window.
struct gui_rect gui_last_rect(const struct gui_ui *ui);

void gui_heading(struct gui_ui *ui, const char *text);
void gui_label(struct gui_ui *ui, const char *format, ...) __attribute__((format(printf, 2, 3)));
void gui_separator(struct gui_ui *ui);
void gui_progress(struct gui_ui *ui, float fraction);

// Whether Ctrl and a letter were pressed this frame, for shortcuts. A capital
// letter is with Shift as well.
bool gui_shortcut(const struct gui_ui *ui, char letter);

// Whether a key was pressed this frame.
bool gui_key(const struct gui_ui *ui, uint16_t code);

// Whether the widget with label has the keyboard.
bool gui_focused(const struct gui_ui *ui, const char *label);

// Gives the widget with label the keyboard, and if it holds text, selects all of it.
void gui_set_focus(struct gui_ui *ui, const char *label, const char *text);

// These return true when they're pressed, or when what they hold changes.
bool gui_button(struct gui_ui *ui, const char *label);
// A button width by height, with its label in the middle, or as wide as the label if
// width is 0, and the usual height if height is 0.
bool gui_button_sized(struct gui_ui *ui, const char *label, int width, int height);
// A panel across the line, height tall, with text twice the usual size at its right.
void gui_display(struct gui_ui *ui, const char *text, int height);
bool gui_checkbox(struct gui_ui *ui, const char *label, bool *value);

// A picture, with the label under it, framed in the accent colour when selected.
bool gui_image_button(struct gui_ui *ui, const char *label, const struct gui_image *image, bool selected);
bool gui_slider(struct gui_ui *ui, const char *label, int *value, int min, int max);
bool gui_textbox(struct gui_ui *ui, const char *label, char *text, size_t size);

// A text box that shows a star for each character, and can't be copied from.
bool gui_password(struct gui_ui *ui, const char *label, char *text, size_t size);

// Text of several lines, lines tall, or down to the bottom of the window if lines
// is 0, scrolling to keep the cursor in sight. Enter starts a new line.
bool gui_textarea(struct gui_ui *ui, const char *label, char *text, size_t size, int lines);

// The same, with its text coloured as code: highlight gives each character of text
// its kind, in kinds, which is as long.
enum gui_syntax { GUI_SYNTAX_PLAIN, GUI_SYNTAX_KEYWORD, GUI_SYNTAX_TYPE, GUI_SYNTAX_STRING, GUI_SYNTAX_NUMBER,
                  GUI_SYNTAX_COMMENT, GUI_SYNTAX_DIRECTIVE, GUI_SYNTAX_KINDS };
// flags add numbers down the left of each line, wrap long lines rather than
// scrolling sideways, and have Tab and Shift+Tab indent and Enter keep the indent.
typedef void gui_highlighter(const char *text, uint8_t *kinds);
enum { GUI_CODE_LINE_NUMBERS = 1, GUI_CODE_WRAP = 2, GUI_CODE_INDENT = 4 };
bool gui_code_area(struct gui_ui *ui, const char *label, char *text, size_t size, int lines, gui_highlighter *highlight,
                   int flags);

// Gives the text widget with label the keyboard, selecting from one place in its
// text to another, where the cursor goes, and scrolls to show it.
void gui_select_text(struct gui_ui *ui, const char *label, size_t from, size_t to);

// Picks one of count items, shown when clicked in a list under it, which scrolls
// when there are too many. Up and Down change it, open or not.
bool gui_dropdown(struct gui_ui *ui, const char *label, const char *const *items, int count, int *selected);

// All of count items in a list rows tall, or down to the bottom of the window if
// rows is 0, which scrolls, with selected highlighted, or none if it's -1. Returns GUI_LIST_CHANGED
// when the selection changes, and GUI_LIST_ACTIVATED when the selected item is
// double-clicked or Enter is pressed on it.
enum { GUI_LIST_CHANGED = 1, GUI_LIST_ACTIVATED };
int gui_list(struct gui_ui *ui, const char *label, const char *const *items, int count, int *selected, int rows);

// The next list or grid lets several items be chosen, which marks says, as long
// as it: Ctrl+click adds or takes one away, Shift+click those from the selected
// one, and Ctrl+A all of them. The selected one is the last clicked.
void gui_list_marks(struct gui_ui *ui, bool *marks);

// The same, with an icon before each item, where icons has one that isn't NULL.
int gui_list_icons(struct gui_ui *ui, const char *label, const char *const *items, const struct gui_image *const *icons,
                   int count, int *selected, int rows);

// All of count items in a grid, each with its picture, up to GUI_THUMBNAIL square,
// over its name, down to the bottom of the window, which scrolls. It's chosen from
// like gui_list, and the arrows move about it.
#define GUI_THUMBNAIL 64
int gui_grid(struct gui_ui *ui, const char *label, const char *const *items, const struct gui_image *const *images,
             int count, int *selected);

// A bar of menus across the top, which goes first in a frame. Each gui_menu that
// returns true is open, and has its items next, then gui_menu_end. An item returns
// true when it's chosen. F10 opens the first menu from the keyboard, the arrows move
// about, and Enter chooses.
void gui_menu_bar(struct gui_ui *ui);
bool gui_menu(struct gui_ui *ui, const char *label);
bool gui_menu_item(struct gui_ui *ui, const char *label);
void gui_menu_separator(struct gui_ui *ui);
void gui_menu_end(struct gui_ui *ui);

// A menu that opens where area is right-clicked, returning true while it's open,
// to be followed by its items and gui_menu_end like gui_menu. Right-clicking a list
// selects the item clicked, and says which in clicked_item.
bool gui_context_menu(struct gui_ui *ui, const char *label, struct gui_rect area);

// A bar of tabs across the top, which goes first in a frame, or after the menu bar, named names, with
// active the one shown. Clicking a tab, or Ctrl+Tab and Ctrl+Shift+Tab, changes
// active and returns GUI_TABS_CHANGED. A tab's x, or Ctrl+W for the active one,
// returns GUI_TABS_CLOSE with closing set to it, and + or Ctrl+T GUI_TABS_NEW, for
// the program to add or take away the tab. With closing NULL, there's no x or +.
enum { GUI_TABS_CHANGED = 1, GUI_TABS_NEW, GUI_TABS_CLOSE };
// A message, of lines split by \n, in a box over the middle of the window with count
// buttons under it, which goes first in a frame, leaving everything after it dimmed
// and without the mouse or keyboard. Returns the button pressed, from 1, with
// Escape pressing the last, or 0 until then.
int gui_dialog(struct gui_ui *ui, const char *message, const char *const *buttons, int count);

// The program's help, its keyboard shortcuts, shown in a dialog when F1 is pressed
// or gui_help_open called, until it's closed. Goes first in a frame, and returns
// whether it's showing.
bool gui_help(struct gui_ui *ui, const char *help);
void gui_help_open(struct gui_ui *ui);

int gui_tabs(struct gui_ui *ui, const char *const *names, int count, int *active, int *closing);

// The widgets between these go in a column width wide, one under another, with
// what comes after beside it, its top level with the column's.
void gui_column_begin(struct gui_ui *ui, int width);
void gui_column_end(struct gui_ui *ui);

// The widgets between these go in an area height tall, or down to the bottom of
// the window if height is 0, which the wheel and a scroll bar scroll when they
// don't fit, or Tab moves to something in them that can't be seen. Areas can go
// inside each other, GUI_UI_MAX_DEPTH deep.
void gui_scroll_begin(struct gui_ui *ui, const char *label, int height);
void gui_scroll_end(struct gui_ui *ui);

// A file picker, which fills the window while it's open, in place of what's
// usually there. It shows a folder's files, with folders first, to choose one to
// open, or to choose a name to save as. Typing a folder's path in the name box
// goes there.

struct gui_file_picker {
    bool open, saving, focus_name;
    char folder[GUI_PATH_MAX], name[256];
    char path[GUI_PATH_MAX]; // what was chosen
    char status[GUI_PATH_MAX + 64], replacing[GUI_PATH_MAX];
    char **items;
    const struct gui_image **icons;
    int count, selected;
};

enum { GUI_PICKED = 1, GUI_PICK_CANCELLED };

// Opens it on folder, with name in the name box.
void gui_picker_open(struct gui_file_picker *picker, const char *folder, const char *name, bool saving);

// Draws it, between gui_ui_begin and gui_ui_end. Returns GUI_PICKED with the
// chosen file's path in picker->path, GUI_PICK_CANCELLED when Cancel or Escape
// is pressed, and 0 until then. Either closes it.
int gui_file_picker(struct gui_ui *ui, struct gui_file_picker *picker);
