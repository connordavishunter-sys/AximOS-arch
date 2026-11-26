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

// Gives the file wm's dragging from the desktop to the app whose window the cursor's
// over, as that app would have it from another's drag, or false if there's none.
bool server_drag_file(const char *path);

// Closes every app's connection, as logging out does.
void server_close_apps(void);
