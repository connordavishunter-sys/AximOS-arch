#pragma once

#include "display.h"
#include <gui.h>
#include <stddef.h>
#include <stdint.h>

// wm is a Wayland compositor, on wlroots. server.c has the screen, the input and the
// apps' windows, Wayland's own and X's through XWayland, and wm.c frames, places
// and draws them, as it always has: each app's window is a struct window, whose
// canvas is a copy of what the app last drew.

struct surface; // an app's window, as the server has it
struct window;  // and as wm has it

// What wm.c does for the server.

// A new window, width by height, or NULL if there's no room for it. A popup, such as
// a menu, has no frame, and is at x, y on the desktop.
struct window *wm_window_new(struct surface *s, int width, int height, bool popup, int x, int y);
// What the app drew: width by height of the desktop's pixels, at the display's scale,
// 0xAARRGGBB, with alpha only if it's see-through, stride of the display's pixels
// from one row to the next, of which a part changed, in the desktop's pixels.
void wm_window_pixels(struct window *w, const uint32_t *pixels, int width, int height, int stride, int x, int y,
                      int changed_width, int changed_height);
void wm_window_title(struct window *w, const char *title);
void wm_window_app(struct window *w, const char *app); // which picks its icon
void wm_window_cursor(struct window *w, enum gui_cursor cursor);
void wm_window_translucent(struct window *w, bool on);
void wm_window_move(struct window *w, int x, int y); // a popup, to x, y on the desktop
void wm_window_raise(struct window *w);              // as the app asked
void wm_window_gone(struct window *w);
void wm_window_position(const struct window *w, int *x, int *y); // where its contents are on the desktop
void wm_window_size(struct window *w, int width, int height); // the app drew at a new size
void wm_window_changed(struct window *w);                     // it drew, so wm's copy's out of date
void wm_room(int *width, int *height); // the most a window's contents can be, on the desktop, framed

void wm_notify(struct window *from, const char *title, const char *body, const char *label, const char *command);
void wm_settings_changed(void); // an app saved them
void wm_unlock(void);           // the log-in or lock screen says someone's in
void wm_login_gone(void);       // and it's stopped
// What's on the clipboard now, text, or a picture width by height, which wm keeps.
void wm_clipboard_changed(char *data, size_t length, int width, int height);

// A file an app dragged, let go of over the desktop, which copies it there.
void wm_desktop_drop(const char *path);

// The window at a point on the desktop, for dragging and dropping.
struct surface *wm_surface_at(int x, int y, int *inside_x, int *inside_y);

// What the server does for wm.c.

extern const struct display wm_server;

// Gives a window what wm would have said to it: GUI_FOCUS, GUI_KEY, GUI_MOUSE_MOVE,
// GUI_MOUSE_BUTTON, GUI_MOUSE_WHEEL, GUI_CLOSE and GUI_RESIZE.
void surface_send(struct surface *s, const struct gui_message *m);

// Whether it's the log-in or lock screen's, which wm started.
bool surface_is_login(const struct surface *s);

// Tells every app of libgui's of a GUI_SCREEN or GUI_SETTINGS.
void server_broadcast(const struct gui_message *m);

// Starts the log-in or lock screen, as a program with a connection of its own, so
// its windows are known from any other's. Returns false if it couldn't.
bool server_start_login(const char *path, const char *option);

// Puts what wm has on the clipboard: text, or a picture width by height, 0x00RRGGBB.
void server_set_clipboard(const char *data, size_t length, int width, int height);

// The scene, which the GPU draws: from the bottom, the desktop, the windows, what's
// over them, and the cursor. What wm draws itself goes in parts, each a buffer of its
// own, uploaded only when it changes, and apps' windows straight from their buffers.
enum server_layer { LAYER_DESKTOP, LAYER_WINDOWS, LAYER_OVER, LAYER_CURSOR, SERVER_LAYERS };

struct scene_part;
struct scene_part *server_part_new(enum server_layer layer);
void server_part_free(struct scene_part *p);
// Where it is, in the desktop's pixels, giving the canvas wm draws it on, at the
// display's scale, which starts see-through when its size changes, or NULL if there's
// no room for it.
struct gui_canvas *server_part_place(struct scene_part *p, int x, int y, int width, int height);
void server_part_show(struct scene_part *p, bool shown);
void server_part_raise(struct scene_part *p); // over the rest of its layer
// Glass: a blur behind it, alpha of it, 0 for none, with corners of radius; and how
// solid all of it is.
void server_part_look(struct scene_part *p, int radius, float blur, float opacity);
// What wm drew, in part of it, from its top left, in the desktop's pixels, to show.
void server_part_drawn(struct scene_part *p, int x, int y, int width, int height);

// A window, as wm places and draws it: its contents, from the app, and around them
// its frame, a part wm draws, its shadow, and a blur behind its title bar, or behind
// contents that are see-through. While it moves between places, a copy of it, which
// wm draws from wm_window_pixels' copy, stands in for all of it.
struct window_look {
    bool shown;
    int x, y; // its contents' top left
    int frame_x, frame_y, frame_width, frame_height;
    int radius;          // of the frame's corners, 0 for square
    float shadow;        // how dark its shadow is, 0 for none
    int shadow_size, shadow_drop; // how far around the frame it spreads, and how far down it's moved
    int title_height;    // its title bar's, which the frame part is, from the frame's top
    bool blur_title;     // blurred behind, as glass
    bool blur_contents;  // see-through contents, blurred behind
    int border;          // its width, down the sides and along the bottom, under the title bar
    uint32_t border_color; // 0xRRGGBB
    bool moving;         // shown by its copy instead
};
void server_window_look(struct surface *s, const struct window_look *look);
struct scene_part *server_window_frame(struct surface *s);
struct scene_part *server_window_moving(struct surface *s);
void server_windows_order(struct surface *const *bottom_up, int count);
// The window's contents, read back from the GPU into wm's copy, by wm_window_pixels.
void server_window_read(struct surface *s);
// The night light: strength out of 100 of the blue taken away, and less of the green,
// from all of the screen, by the GPU, or 0 for none.
void server_night(int strength);

// The screen as it's shown, into a canvas its size, at its scale.
bool server_read_screen(struct gui_canvas *into);

// Gives the file wm's dragging from the desktop to the app whose window the cursor's
// over, as that app would have it from another's drag, or false if there's none.
bool server_drag_file(const char *path);

// Closes every app's connection, as logging out does.
void server_close_apps(void);
