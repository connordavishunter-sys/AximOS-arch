#include "aximos-shell-v1-client-protocol.h"
#include "backend.h"
#include "cursor-shape-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <key_linux.h>
#include <key_text.h>
#include <mouse_event.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

// libgui as a Wayland client. Each window is an xdg-shell toplevel, shown from shared
// memory buffers, and Wayland's events become libgui's messages, as wm sent them
// before. What only AximOS's desktop does, such as notifications, goes through its
// own protocol, aximos-shell, and elsewhere is printed or left undone.

#define BUFFERS 3 // shown, waiting to be, and being drawn
#define TEXT_TYPE "text/plain;charset=utf-8"
#define IMAGE_TYPE "image/png"
#define URI_TYPE "text/uri-list"
#define CLIPBOARD_MAX (64 * 1024 * 1024)

struct buffer {
    struct wl_buffer *buffer;
    uint32_t *pixels;
    size_t size;
    bool busy; // with the compositor, until it lets it go
    int x0, y0, x1, y1; // what's changed since it was last brought up to date, or none if x1 <= x0
};

struct gui_backend_window {
    struct gui_window *window;
    struct wl_surface *surface;
    struct xdg_surface *xdg;
    struct xdg_toplevel *toplevel;
    struct buffer buffers[BUFFERS];
    uint32_t *shown; // what was last given, to send only what's changed
    int width, height; // of the buffers
    bool configured, translucent;
    int asked_width, asked_height; // what the compositor last asked for
    enum gui_cursor cursor;
};

static struct {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct wl_output *output;
    struct xdg_wm_base *wm_base;
    struct zxdg_decoration_manager_v1 *decorations;
    struct wp_cursor_shape_manager_v1 *cursor_shapes;
    struct wp_cursor_shape_device_v1 *cursor_device;
    struct wl_data_device_manager *data_manager;
    struct wl_data_device *data_device;
    struct aximos_shell_v1 *shell;
    struct gui_window *windows[16];
    int output_width, output_height, scale;
    bool can_resize;

    // The pointer: which window it's over, where, and what's held.
    struct gui_window *pointed;
    int x, y;
    uint32_t buttons, enter_serial, press_serial;
    double scrolled;

    // The keyboard, which repeats keys itself as Wayland leaves that to apps.
    struct gui_window *focused;
    struct xkb_context *xkb;
    struct xkb_keymap *keymap;
    xkb_mod_index_t shift, ctrl, alt, super, caps;
    uint8_t modifiers;
    uint32_t key_serial;
    int repeat_rate, repeat_delay;
    struct key_event repeating;
    int64_t repeat_due;

    // The clipboard: what was last offered, and what's offered now.
    struct wl_data_offer *selection, *dragged;
    char *own; // what this program put there itself, while it's there
    size_t own_length;
    bool own_image;
    struct wl_data_source *own_source;
    char dropped[GUI_PATH_MAX];
    struct gui_window *drop_window;
} wl;

static int slot_of(const struct gui_window *window) {
    for (int i = 0; i < (int)(sizeof(wl.windows) / sizeof(wl.windows[0])); i++) {
        if (wl.windows[i] == window)
            return i;
    }
    return -1;
}

static struct gui_window *window_of(struct wl_surface *surface) {
    for (int i = 0; surface && i < (int)(sizeof(wl.windows) / sizeof(wl.windows[0])); i++) {
        if (wl.windows[i] && wl.windows[i]->backend->surface == surface)
            return wl.windows[i];
    }
    return NULL;
}

static void post_to(struct gui_window *window, struct gui_message m) {
    if (!window)
        return;
    m.window = window->id;
    gui_post(&m);
}

// Every window hears of the screen, and of the settings changing.
static void post_to_all(struct gui_message m) {
    for (int i = 0; i < (int)(sizeof(wl.windows) / sizeof(wl.windows[0])); i++)
        post_to(wl.windows[i], m);
}

static void post_screen(int width, int height, bool can_resize, int scale) {
    post_to_all((struct gui_message){.type = GUI_SCREEN, .width = width, .height = height, .x = can_resize, .y = scale});
}

// aximos-shell's events.
static void shell_screen(void *data, struct aximos_shell_v1 *shell, int32_t width, int32_t height, uint32_t can_set_mode,
                         uint32_t scale) {
    (void)data, (void)shell;
    wl.output_width = width, wl.output_height = height;
    wl.can_resize = can_set_mode, wl.scale = scale;
    post_screen(width, height, can_set_mode, scale);
}

static void shell_settings(void *data, struct aximos_shell_v1 *shell) {
    (void)data, (void)shell;
    post_to_all((struct gui_message){.type = GUI_SETTINGS});
}

static const struct aximos_shell_v1_listener shell_listener = {.screen = shell_screen, .settings_changed = shell_settings};

// Elsewhere, the screen is the output's.
static void output_mode(void *data, struct wl_output *output, uint32_t flags, int32_t width, int32_t height,
                        int32_t refresh) {
    (void)data, (void)output, (void)refresh;
    if ((flags & WL_OUTPUT_MODE_CURRENT) && !wl.shell)
        wl.output_width = width, wl.output_height = height;
}

static void output_ignore(void) {
}

static const struct wl_output_listener output_listener = {
    .geometry = (void *)output_ignore, .mode = output_mode, .done = (void *)output_ignore, .scale = (void *)output_ignore,
    .name = (void *)output_ignore, .description = (void *)output_ignore};

static void ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {.ping = ping};

// The pointer.

static void set_shape(enum gui_cursor cursor) {
    static const uint32_t shapes[] = {
        [GUI_CURSOR_ARROW] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT,
        [GUI_CURSOR_TEXT] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT,
        [GUI_CURSOR_RESIZE_H] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE,
        [GUI_CURSOR_RESIZE_V] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE,
        [GUI_CURSOR_RESIZE_NWSE] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE,
        [GUI_CURSOR_RESIZE_NESW] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE,
    };

    if (wl.cursor_device && cursor < sizeof(shapes) / sizeof(shapes[0]))
        wp_cursor_shape_device_v1_set_shape(wl.cursor_device, wl.enter_serial, shapes[cursor]);
}

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t x, wl_fixed_t y) {
    (void)data, (void)pointer;
    wl.pointed = window_of(surface);
    wl.enter_serial = serial;
    wl.x = wl_fixed_to_int(x), wl.y = wl_fixed_to_int(y);
    if (wl.pointed)
        set_shape(wl.pointed->backend->cursor);
    post_to(wl.pointed, (struct gui_message){.type = GUI_MOUSE_MOVE, .x = wl.x, .y = wl.y, .buttons = wl.buttons});
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface) {
    (void)data, (void)pointer, (void)serial, (void)surface;
    wl.pointed = NULL;
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    (void)data, (void)pointer, (void)time;
    wl.x = wl_fixed_to_int(x), wl.y = wl_fixed_to_int(y);
    post_to(wl.pointed, (struct gui_message){.type = GUI_MOUSE_MOVE, .x = wl.x, .y = wl.y, .buttons = wl.buttons,
                                             .key.modifiers = wl.modifiers});
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time, uint32_t button,
                           uint32_t state) {
    uint32_t mask = button == 0x110 ? MOUSE_BUTTON_LEFT : button == 0x111 ? MOUSE_BUTTON_RIGHT
                  : button == 0x112 ? MOUSE_BUTTON_MIDDLE : 0;

    (void)data, (void)pointer, (void)time;
    if (!mask)
        return;
    if (state == WL_POINTER_BUTTON_STATE_PRESSED)
        wl.press_serial = serial;
    wl.buttons = state == WL_POINTER_BUTTON_STATE_PRESSED ? wl.buttons | mask : wl.buttons & ~mask;
    post_to(wl.pointed, (struct gui_message){.type = GUI_MOUSE_BUTTON, .x = wl.x, .y = wl.y, .buttons = wl.buttons,
                                             .changed = mask, .key.modifiers = wl.modifiers});
}

// Towards you is down the page, and positive, as wm counts the wheel's notches.
static void wheel(int notches) {
    if (notches)
        post_to(wl.pointed, (struct gui_message){.type = GUI_MOUSE_WHEEL, .x = wl.x, .y = wl.y, .wheel = notches,
                                                 .key.modifiers = wl.modifiers});
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value) {
    (void)data, (void)pointer, (void)time;
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
        return;
    wl.scrolled += wl_fixed_to_double(value) / 15;
    wheel((int)wl.scrolled);
    wl.scrolled -= (int)wl.scrolled;
}

static void pointer_value120(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t value) {
    (void)data, (void)pointer, (void)axis, (void)value;
    wl.scrolled = 0; // axis counts it too
}

static void pointer_nothing(void) {
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter, .leave = pointer_leave, .motion = pointer_motion, .button = pointer_button,
    .axis = pointer_axis, .frame = (void *)pointer_nothing, .axis_source = (void *)pointer_nothing,
    .axis_stop = (void *)pointer_nothing, .axis_discrete = (void *)pointer_nothing, .axis_value120 = pointer_value120,
    .axis_relative_direction = (void *)pointer_nothing};

// The keyboard. Keys are AximOS's codes, typing as a US keyboard does, as on AximOS.

static void got_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int fd, uint32_t size) {
    char *map = format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 ? mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;

    (void)data, (void)keyboard;
    close(fd);
    if (map == MAP_FAILED)
        return;
    if (!wl.xkb)
        wl.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_keymap_unref(wl.keymap);
    wl.keymap = wl.xkb ? xkb_keymap_new_from_string(wl.xkb, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS)
                       : NULL;
    munmap(map, size);
    if (wl.keymap) {
        wl.shift = xkb_keymap_mod_get_index(wl.keymap, XKB_MOD_NAME_SHIFT);
        wl.ctrl = xkb_keymap_mod_get_index(wl.keymap, XKB_MOD_NAME_CTRL);
        wl.alt = xkb_keymap_mod_get_index(wl.keymap, XKB_MOD_NAME_ALT);
        wl.super = xkb_keymap_mod_get_index(wl.keymap, XKB_MOD_NAME_LOGO);
        wl.caps = xkb_keymap_mod_get_index(wl.keymap, XKB_MOD_NAME_CAPS);
    }
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys) {
    (void)data, (void)keyboard, (void)keys;
    wl.key_serial = serial;
    wl.focused = window_of(surface);
    post_to(wl.focused, (struct gui_message){.type = GUI_FOCUS, .focused = 1});
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface) {
    (void)data, (void)keyboard, (void)serial;
    post_to(window_of(surface), (struct gui_message){.type = GUI_FOCUS, .focused = 0});
    wl.focused = NULL;
    wl.repeating.code = 0;
}

static bool held(xkb_mod_mask_t mask, xkb_mod_index_t index) {
    return index != XKB_MOD_INVALID && mask & 1u << index;
}

static void modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t depressed, uint32_t latched,
                      uint32_t locked, uint32_t group) {
    xkb_mod_mask_t mask = depressed | latched | locked;

    (void)data, (void)keyboard, (void)serial, (void)group;
    wl.modifiers = (held(mask, wl.shift) ? KEY_MOD_SHIFT : 0) | (held(mask, wl.ctrl) ? KEY_MOD_CTRL : 0)
                 | (held(mask, wl.alt) ? KEY_MOD_ALT : 0) | (held(mask, wl.super) ? KEY_MOD_SUPER : 0)
                 | (held(locked, wl.caps) ? KEY_MOD_CAPS_LOCK : 0);
}

static void post_key(uint16_t code, bool pressed) {
    post_to(wl.focused, (struct gui_message){.type = GUI_KEY, .key = {.code = code, .pressed = pressed,
                                                                      .modifiers = wl.modifiers,
                                                                      .text = pressed ? key_event_text(code, wl.modifiers) : 0}});
}

static bool is_modifier(uint16_t code) {
    return code == KEY_LEFT_SHIFT || code == KEY_RIGHT_SHIFT || code == KEY_LEFT_CTRL || code == KEY_RIGHT_CTRL
        || code == KEY_LEFT_ALT || code == KEY_RIGHT_ALT || code == KEY_LEFT_SUPER || code == KEY_RIGHT_SUPER
        || code == KEY_CAPS_LOCK;
}

static void key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t linux_code,
                uint32_t state) {
    uint16_t code = key_from_linux(linux_code);
    bool pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;

    (void)data, (void)keyboard, (void)time;
    wl.key_serial = serial;
    if (!code)
        return;
    // A modifier's change comes after its key, so it's counted here, for this key.
    if (is_modifier(code) && code != KEY_CAPS_LOCK) {
        uint8_t bit = code == KEY_LEFT_SHIFT || code == KEY_RIGHT_SHIFT ? KEY_MOD_SHIFT
                    : code == KEY_LEFT_CTRL || code == KEY_RIGHT_CTRL   ? KEY_MOD_CTRL
                    : code == KEY_LEFT_ALT || code == KEY_RIGHT_ALT     ? KEY_MOD_ALT
                                                                        : KEY_MOD_SUPER;

        wl.modifiers = pressed ? wl.modifiers | bit : wl.modifiers & ~bit;
    }
    post_key(code, pressed);
    if (pressed && !is_modifier(code) && wl.repeat_rate > 0) {
        wl.repeating = (struct key_event){.code = code, .pressed = 1};
        wl.repeat_due = gui_now_ms() + wl.repeat_delay;
    } else if (!pressed && code == wl.repeating.code) {
        wl.repeating.code = 0;
    }
}

static void repeat_info(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay) {
    (void)data, (void)keyboard;
    wl.repeat_rate = rate;
    wl.repeat_delay = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = got_keymap, .enter = keyboard_enter, .leave = keyboard_leave, .key = key, .modifiers = modifiers,
    .repeat_info = repeat_info};

// A key held down types again, every so often, until it's let go.
static void repeat_keys(void) {
    if (!wl.repeating.code || gui_now_ms() < wl.repeat_due)
        return;
    post_key(wl.repeating.code, true);
    wl.repeat_due = gui_now_ms() + 1000 / wl.repeat_rate;
}

// The clipboard, and dropping files, which come as offers.

struct offer {
    bool text, plain, image, uris;
};

static void offer_type(void *data, struct wl_data_offer *offer, const char *type) {
    struct offer *o = wl_data_offer_get_user_data(offer);

    (void)data;
    if (!o)
        return;
    o->text |= !strcmp(type, TEXT_TYPE);
    o->plain |= !strcmp(type, "text/plain");
    o->image |= !strcmp(type, IMAGE_TYPE);
    o->uris |= !strcmp(type, URI_TYPE);
}

static const struct wl_data_offer_listener offer_listener = {
    .offer = offer_type, .source_actions = (void *)pointer_nothing, .action = (void *)pointer_nothing};

static void forget_offer(struct wl_data_offer **offer) {
    if (!*offer)
        return;
    free(wl_data_offer_get_user_data(*offer));
    wl_data_offer_destroy(*offer);
    *offer = NULL;
}

static void data_offer(void *data, struct wl_data_device *device, struct wl_data_offer *offer) {
    (void)data, (void)device;
    wl_data_offer_add_listener(offer, &offer_listener, NULL);
    wl_data_offer_set_user_data(offer, calloc(1, sizeof(struct offer)));
}

static void selection(void *data, struct wl_data_device *device, struct wl_data_offer *offer) {
    (void)data, (void)device;
    if (wl.selection != offer)
        forget_offer(&wl.selection);
    wl.selection = offer;
}

static void drag_enter(void *data, struct wl_data_device *device, uint32_t serial, struct wl_surface *surface,
                       wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *offer) {
    struct offer *o = offer ? wl_data_offer_get_user_data(offer) : NULL;

    (void)data, (void)device;
    forget_offer(&wl.dragged);
    wl.dragged = offer;
    wl.drop_window = window_of(surface);
    wl.x = wl_fixed_to_int(x), wl.y = wl_fixed_to_int(y);
    if (!offer)
        return;
    wl_data_offer_accept(offer, serial, o && o->uris ? URI_TYPE : NULL);
    wl_data_offer_set_actions(offer, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
}

static void drag_leave(void *data, struct wl_data_device *device) {
    (void)data, (void)device;
    forget_offer(&wl.dragged);
    wl.drop_window = NULL;
}

static void drag_motion(void *data, struct wl_data_device *device, uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    (void)data, (void)device, (void)time;
    wl.x = wl_fixed_to_int(x), wl.y = wl_fixed_to_int(y);
}

// Reads what an offer has of a type, to be freed, ending in a zero byte.
static char *receive(struct wl_data_offer *offer, const char *type, size_t *length) {
    int fds[2];
    char *data = NULL;
    size_t have = 0;

    if (pipe2(fds, O_CLOEXEC) < 0)
        return NULL;
    wl_data_offer_receive(offer, type, fds[1]);
    close(fds[1]);
    wl_display_flush(wl.display);
    for (;;) {
        char chunk[65536];
        ssize_t n = read(fds[0], chunk, sizeof(chunk));
        char *bigger;

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0 || have + n > CLIPBOARD_MAX || !(bigger = realloc(data, have + n + 1)))
            break;
        data = bigger;
        memcpy(data + have, chunk, n);
        have += n;
        data[have] = '\0';
    }
    close(fds[0]);
    *length = have;
    return data;
}

// The first file of a list of URIs, as a path, undoing %-escapes.
static bool first_path(const char *uris, char path[GUI_PATH_MAX]) {
    const char *at = strncmp(uris, "file://", 7) ? NULL : strchr(uris + 7, '/');
    size_t n = 0;

    for (; at && *at && *at != '\r' && *at != '\n' && n < GUI_PATH_MAX - 1; at++) {
        unsigned value;

        if (*at == '%' && sscanf(at + 1, "%2x", &value) == 1) {
            path[n++] = (char)value;
            at += 2;
        } else {
            path[n++] = *at;
        }
    }
    path[n] = '\0';
    return n > 0;
}

static void drop(void *data, struct wl_data_device *device) {
    size_t length;
    char *uris = wl.dragged ? receive(wl.dragged, URI_TYPE, &length) : NULL;

    (void)data, (void)device;
    if (uris && first_path(uris, wl.dropped))
        post_to(wl.drop_window, (struct gui_message){.type = GUI_DROP, .x = wl.x, .y = wl.y,
                                                     .key.modifiers = wl.modifiers});
    free(uris);
    if (wl.dragged)
        wl_data_offer_finish(wl.dragged);
    forget_offer(&wl.dragged);
}

static const struct wl_data_device_listener data_device_listener = {
    .data_offer = data_offer, .enter = drag_enter, .leave = drag_leave, .motion = drag_motion, .drop = drop,
    .selection = selection};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities) {
    (void)data;
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !wl.pointer) {
        wl.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(wl.pointer, &pointer_listener, NULL);
        if (wl.cursor_shapes)
            wl.cursor_device = wp_cursor_shape_manager_v1_get_pointer(wl.cursor_shapes, wl.pointer);
    }
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !wl.keyboard) {
        wl.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(wl.keyboard, &keyboard_listener, NULL);
    }
}

static const struct wl_seat_listener seat_listener = {.capabilities = seat_capabilities, .name = (void *)pointer_nothing};

static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    (void)data;
#define BIND(what, iface, most) wl_registry_bind(registry, name, &iface, version < (most) ? version : (most))
    if (!strcmp(interface, wl_compositor_interface.name))
        wl.compositor = BIND(compositor, wl_compositor_interface, 4);
    else if (!strcmp(interface, wl_shm_interface.name))
        wl.shm = BIND(shm, wl_shm_interface, 1);
    else if (!strcmp(interface, wl_seat_interface.name) && !wl.seat) {
        wl.seat = BIND(seat, wl_seat_interface, 9);
        wl_seat_add_listener(wl.seat, &seat_listener, NULL);
    } else if (!strcmp(interface, wl_output_interface.name) && !wl.output) {
        wl.output = BIND(output, wl_output_interface, 4);
        wl_output_add_listener(wl.output, &output_listener, NULL);
    } else if (!strcmp(interface, xdg_wm_base_interface.name)) {
        wl.wm_base = BIND(wm_base, xdg_wm_base_interface, 6);
        xdg_wm_base_add_listener(wl.wm_base, &wm_base_listener, NULL);
    } else if (!strcmp(interface, zxdg_decoration_manager_v1_interface.name))
        wl.decorations = BIND(decorations, zxdg_decoration_manager_v1_interface, 1);
    else if (!strcmp(interface, wp_cursor_shape_manager_v1_interface.name))
        wl.cursor_shapes = BIND(cursor_shapes, wp_cursor_shape_manager_v1_interface, 1);
    else if (!strcmp(interface, wl_data_device_manager_interface.name))
        wl.data_manager = BIND(data_manager, wl_data_device_manager_interface, 3);
    else if (!strcmp(interface, aximos_shell_v1_interface.name)) {
        wl.shell = BIND(shell, aximos_shell_v1_interface, 1);
        aximos_shell_v1_add_listener(wl.shell, &shell_listener, NULL);
    }
#undef BIND
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data, (void)registry, (void)name;
}

static const struct wl_registry_listener registry_listener = {.global = global, .global_remove = global_remove};

bool gui_connect(void) {
    if (wl.display)
        return true;
    if (!(wl.display = wl_display_connect(NULL)))
        return false;
    wl.registry = wl_display_get_registry(wl.display);
    wl_registry_add_listener(wl.registry, &registry_listener, NULL);
    wl_display_roundtrip(wl.display); // the globals
    if (!wl.compositor || !wl.shm || !wl.wm_base) {
        fprintf(stderr, "gui: the desktop isn't one libgui can show windows on\n");
        wl_display_disconnect(wl.display);
        wl.display = NULL;
        return false;
    }
    if (wl.seat && wl.data_manager) {
        wl.data_device = wl_data_device_manager_get_data_device(wl.data_manager, wl.seat);
        wl_data_device_add_listener(wl.data_device, &data_device_listener, NULL);
    }
    wl_display_roundtrip(wl.display); // the seat's and output's own
    return true;
}

int gui_fd(void) {
    return wl.display ? wl_display_get_fd(wl.display) : -1;
}

// Windows.

static void buffer_release(void *data, struct wl_buffer *buffer) {
    struct buffer *b = data;

    (void)buffer;
    b->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {.release = buffer_release};

static void free_buffers(struct gui_backend_window *b) {
    for (int i = 0; i < BUFFERS; i++) {
        if (b->buffers[i].buffer) {
            wl_buffer_destroy(b->buffers[i].buffer);
            munmap(b->buffers[i].pixels, b->buffers[i].size);
        }
        b->buffers[i] = (struct buffer){0};
    }
}

static bool new_buffer(struct gui_backend_window *b, struct buffer *out) {
    int stride = b->width * 4;
    size_t size = (size_t)stride * b->height;
    int fd = memfd_create("gui-window", MFD_CLOEXEC);
    struct wl_shm_pool *pool;

    if (fd < 0 || ftruncate(fd, size) < 0) {
        if (fd >= 0)
            close(fd);
        return false;
    }
    out->pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (out->pixels == MAP_FAILED) {
        close(fd);
        return false;
    }
    pool = wl_shm_create_pool(wl.shm, fd, size);
    out->buffer = wl_shm_pool_create_buffer(pool, 0, b->width, b->height, stride,
                                            b->translucent ? WL_SHM_FORMAT_ARGB8888 : WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    out->size = size;
    out->busy = false;
    out->x0 = out->y0 = 0, out->x1 = b->width, out->y1 = b->height; // all of it, the first time
    wl_buffer_add_listener(out->buffer, &buffer_listener, out);
    return true;
}

static void xdg_configure(void *data, struct xdg_surface *xdg, uint32_t serial) {
    struct gui_window *window = data;
    struct gui_backend_window *b = window->backend;

    xdg_surface_ack_configure(xdg, serial);
    b->configured = true;
    if (b->asked_width > 0 && b->asked_height > 0
        && (b->asked_width != window->canvas.width || b->asked_height != window->canvas.height))
        post_to(window, (struct gui_message){.type = GUI_RESIZE, .width = b->asked_width, .height = b->asked_height});
}

static const struct xdg_surface_listener xdg_listener = {.configure = xdg_configure};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height,
                               struct wl_array *states) {
    struct gui_window *window = data;

    (void)toplevel, (void)states;
    window->backend->asked_width = width;
    window->backend->asked_height = height;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    (void)toplevel;
    post_to(data, (struct gui_message){.type = GUI_CLOSE});
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure, .close = toplevel_close, .configure_bounds = (void *)pointer_nothing,
    .wm_capabilities = (void *)pointer_nothing};

static bool new_canvas(struct gui_window *window, int width, int height) {
    uint32_t *pixels = calloc((size_t)width * height, sizeof(uint32_t));
    uint32_t *shown = calloc((size_t)width * height, sizeof(uint32_t));

    if (!pixels || !shown) {
        free(pixels);
        free(shown);
        return false;
    }
    free(window->canvas.pixels);
    free(window->backend->shown);
    window->canvas = (struct gui_canvas){.pixels = pixels, .width = width, .height = height, .stride = width};
    window->backend->shown = shown;
    return true;
}

static void destroy(struct gui_window *window) {
    struct gui_backend_window *b = window->backend;

    if (b) {
        free_buffers(b);
        if (b->toplevel)
            xdg_toplevel_destroy(b->toplevel);
        if (b->xdg)
            xdg_surface_destroy(b->xdg);
        if (b->surface)
            wl_surface_destroy(b->surface);
        free(b->shown);
        free(b);
    }
    free(window->canvas.pixels);
    free(window);
}

struct gui_window *gui_create_window(const char *title, int width, int height) {
    static uint32_t next_id = 1;
    int slot = slot_of(NULL);
    struct gui_window *window = slot >= 0 ? calloc(1, sizeof(*window)) : NULL;
    struct gui_backend_window *b = window ? calloc(1, sizeof(*b)) : NULL;
    const char *name = program_invocation_short_name;

    if (!b || !(window->backend = b, new_canvas(window, width, height))) {
        if (window)
            destroy(window);
        return NULL;
    }
    window->id = next_id++;
    b->window = window;
    b->surface = wl_compositor_create_surface(wl.compositor);
    b->xdg = xdg_wm_base_get_xdg_surface(wl.wm_base, b->surface);
    xdg_surface_add_listener(b->xdg, &xdg_listener, window);
    b->toplevel = xdg_surface_get_toplevel(b->xdg);
    xdg_toplevel_add_listener(b->toplevel, &toplevel_listener, window);
    xdg_toplevel_set_title(b->toplevel, title);
    xdg_toplevel_set_app_id(b->toplevel, name);
    if (wl.decorations)
        zxdg_toplevel_decoration_v1_set_mode(zxdg_decoration_manager_v1_get_toplevel_decoration(wl.decorations, b->toplevel),
                                             ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    wl.windows[slot] = window;
    wl_surface_commit(b->surface);
    while (!b->configured && wl_display_dispatch(wl.display) >= 0)
        ;
    if (!b->configured) {
        wl.windows[slot] = NULL;
        destroy(window);
        return NULL;
    }
    gui_post(&(struct gui_message){.type = GUI_SCREEN, .window = window->id, .width = wl.output_width,
                                   .height = wl.output_height, .x = wl.can_resize, .y = wl.scale > 0 ? wl.scale : 1});
    return window;
}

bool gui_resize(struct gui_window *window, int width, int height) {
    if (width <= 0 || height <= 0 || !new_canvas(window, width, height))
        return false;
    return true;
}

void gui_destroy_window(struct gui_window *window) {
    int slot = slot_of(window);

    if (slot >= 0)
        wl.windows[slot] = NULL;
    if (wl.pointed == window)
        wl.pointed = NULL;
    if (wl.focused == window)
        wl.focused = NULL;
    destroy(window);
    wl_display_flush(wl.display);
}

// What changed in part of the canvas, put in shown, row by row, narrowed to what
// really did. Returns false if nothing did.
static bool copy_changes(struct gui_window *window, int *x, int *y, int *width, int *height) {
    const struct gui_canvas *c = &window->canvas;
    uint32_t *shown = window->backend->shown;
    int x0 = *x < 0 ? 0 : *x, y0 = *y < 0 ? 0 : *y;
    int right = *x + *width < c->width ? *x + *width : c->width;
    int bottom = *y + *height < c->height ? *y + *height : c->height;
    int top = -1, last = 0, left = right, end = x0;

    for (int row = y0; row < bottom; row++) {
        const uint32_t *from = c->pixels + (long)row * c->stride;
        uint32_t *to = shown + (long)row * c->width;
        int l = x0, r = right - 1;

        while (l < right && from[l] == to[l])
            l++;
        if (l == right)
            continue;
        while (from[r] == to[r])
            r--;
        memcpy(to + l, from + l, (r - l + 1) * sizeof(uint32_t));
        top = top < 0 ? row : top;
        last = row;
        left = l < left ? l : left;
        end = r + 1 > end ? r + 1 : end;
    }
    *x = left, *y = top, *width = end - left, *height = last - top + 1;
    return top >= 0;
}

// A buffer the compositor isn't using, or NULL if it has them all.
static struct buffer *free_buffer(struct gui_backend_window *b) {
    for (int i = 0; i < BUFFERS; i++) {
        if (!b->buffers[i].buffer && !new_buffer(b, &b->buffers[i]))
            return NULL;
        if (!b->buffers[i].busy)
            return &b->buffers[i];
    }
    return NULL;
}

// Brings a buffer up to date with shown, where it's behind. Translucent windows' top
// bytes say how see-through each pixel is, which Wayland has as how solid, with the
// colour multiplied by it.
static void bring_up_to_date(struct gui_backend_window *b, struct buffer *buffer) {
    for (int y = buffer->y0; y < buffer->y1; y++) {
        const uint32_t *from = b->shown + (size_t)y * b->width;
        uint32_t *to = buffer->pixels + (size_t)y * b->width;

        if (!b->translucent) {
            memcpy(to + buffer->x0, from + buffer->x0, (buffer->x1 - buffer->x0) * sizeof(uint32_t));
            continue;
        }
        for (int x = buffer->x0; x < buffer->x1; x++) {
            uint32_t p = from[x], a = 255 - (p >> 24);

            to[x] = a << 24 | ((p >> 16 & 0xFF) * a / 255) << 16 | ((p >> 8 & 0xFF) * a / 255) << 8 | (p & 0xFF) * a / 255;
        }
    }
    buffer->x1 = buffer->x0; // none behind now
}

static void add_behind(struct buffer *buffer, int x, int y, int width, int height) {
    if (buffer->x1 <= buffer->x0) {
        buffer->x0 = x, buffer->y0 = y, buffer->x1 = x + width, buffer->y1 = y + height;
        return;
    }
    buffer->x0 = x < buffer->x0 ? x : buffer->x0;
    buffer->y0 = y < buffer->y0 ? y : buffer->y0;
    buffer->x1 = x + width > buffer->x1 ? x + width : buffer->x1;
    buffer->y1 = y + height > buffer->y1 ? y + height : buffer->y1;
}

void gui_update(struct gui_window *window, int x, int y, int width, int height) {
    struct gui_backend_window *b = window->backend;
    struct buffer *buffer;

    // A new size means new buffers, all of whose pixels are new.
    if (b->width != window->canvas.width || b->height != window->canvas.height) {
        free_buffers(b);
        b->width = window->canvas.width, b->height = window->canvas.height;
        x = y = 0, width = b->width, height = b->height;
        memset(b->shown, 0xFF, (size_t)b->width * b->height * sizeof(uint32_t)); // so all of it differs
    }
    if (!copy_changes(window, &x, &y, &width, &height))
        return;
    for (int i = 0; i < BUFFERS; i++)
        add_behind(&b->buffers[i], x, y, width, height);
    while (!(buffer = free_buffer(b)) && wl_display_dispatch(wl.display) >= 0)
        ;
    if (!buffer)
        return;
    bring_up_to_date(b, buffer);
    buffer->busy = true;
    wl_surface_attach(b->surface, buffer->buffer, 0, 0);
    wl_surface_damage_buffer(b->surface, x, y, width, height);
    wl_surface_commit(b->surface);
    wl_display_flush(wl.display);
}

void gui_show(struct gui_window *window) {
    gui_update(window, 0, 0, window->canvas.width, window->canvas.height);
}

void gui_set_title(struct gui_window *window, const char *title) {
    xdg_toplevel_set_title(window->backend->toplevel, title);
    wl_display_flush(wl.display);
}

void gui_set_icon(struct gui_window *window, const char *name) {
    xdg_toplevel_set_app_id(window->backend->toplevel, name);
    wl_display_flush(wl.display);
}

void gui_set_cursor(struct gui_window *window, enum gui_cursor cursor) {
    window->backend->cursor = cursor;
    if (wl.pointed == window)
        set_shape(cursor);
    wl_display_flush(wl.display);
}

// Buffers of the other kind, with alpha or without, are made with the next update.
void gui_set_translucent(struct gui_window *window, bool on) {
    struct gui_backend_window *b = window->backend;

    if (b->translucent == on)
        return;
    b->translucent = on;
    free_buffers(b);
    b->width = b->height = 0;
    if (wl.shell)
        aximos_shell_v1_set_translucent(wl.shell, b->surface, on);
}

bool gui_notify_action(struct gui_window *window, const char *title, const char *text, const char *label,
                       const char *command) {
    if (!wl.shell) {
        fprintf(stderr, "%s: %s%s%s\n", program_invocation_short_name, title, text ? ": " : "", text ? text : "");
        return true;
    }
    aximos_shell_v1_notify(wl.shell, window ? window->backend->surface : NULL, title, text ? text : "", label,
                           label ? command : NULL);
    wl_display_flush(wl.display);
    return true;
}

bool gui_notify(struct gui_window *window, const char *title, const char *text) {
    return gui_notify_action(window, title, text, NULL, NULL);
}

void gui_backend_settings_changed(void) {
    if (wl.shell) {
        aximos_shell_v1_settings_changed(wl.shell);
        wl_display_flush(wl.display);
    }
}

bool gui_unlock(void) {
    if (!wl.shell)
        return false;
    aximos_shell_v1_unlock(wl.shell);
    return wl_display_roundtrip(wl.display) >= 0;
}

// What this program offers others, the clipboard's or a dragged file's.

static void source_send(void *data, struct wl_data_source *source, const char *type, int fd) {
    const char *uris = data;
    const char *from = uris ? uris : wl.own;
    size_t length = uris ? strlen(uris) : wl.own_length;

    (void)source, (void)type;
    for (size_t done = 0; from && done < length;) {
        ssize_t n = write(fd, from + done, length - done);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        done += n;
    }
    close(fd);
}

static void source_cancelled(void *data, struct wl_data_source *source) {
    free(data);
    if (source == wl.own_source) {
        free(wl.own);
        wl.own = NULL;
        wl.own_source = NULL;
    }
    wl_data_source_destroy(source);
}

static const struct wl_data_source_listener source_listener = {
    .target = (void *)pointer_nothing, .send = source_send, .cancelled = source_cancelled,
    .dnd_drop_performed = (void *)pointer_nothing, .dnd_finished = source_cancelled, .action = (void *)pointer_nothing};

static bool offer_own(char *data, size_t length, bool image) {
    struct wl_data_source *source;

    if (!wl.data_device) {
        free(data);
        return false;
    }
    source = wl_data_device_manager_create_data_source(wl.data_manager);
    wl_data_source_add_listener(source, &source_listener, NULL);
    if (image)
        wl_data_source_offer(source, IMAGE_TYPE);
    else {
        wl_data_source_offer(source, TEXT_TYPE);
        wl_data_source_offer(source, "text/plain");
    }
    free(wl.own);
    wl.own = data, wl.own_length = length, wl.own_image = image;
    wl.own_source = source;
    wl_data_device_set_selection(wl.data_device, source, wl.key_serial > wl.press_serial ? wl.key_serial : wl.press_serial);
    wl_display_flush(wl.display);
    return true;
}

bool gui_clipboard_set(const char *text, size_t length) {
    char *copy = malloc(length + 1);

    if (!copy)
        return false;
    memcpy(copy, text, length);
    copy[length] = '\0';
    return offer_own(copy, length, false);
}

// A picture goes as a PNG, which libgui writes to a file in memory.
bool gui_clipboard_set_image(const struct gui_canvas *from) {
    int fd = memfd_create("clipboard", MFD_CLOEXEC);
    char path[64], *png = NULL;
    off_t size = 0;

    if (fd < 0)
        return false;
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    if (gui_image_save(path, from) && (size = lseek(fd, 0, SEEK_END)) > 0 && (png = malloc(size))
        && pread(fd, png, size, 0) != size) {
        free(png);
        png = NULL;
    }
    close(fd);
    return png && offer_own(png, size, true);
}

char *gui_clipboard_get(size_t *length) {
    struct offer *o = wl.selection ? wl_data_offer_get_user_data(wl.selection) : NULL;
    size_t have = 0;
    char *text = NULL;

    if (wl.own && !wl.own_image)
        text = strndup(wl.own, wl.own_length), have = wl.own_length;
    else
        wl_display_roundtrip(wl.display); // the newest selection, and its types
    o = wl.selection ? wl_data_offer_get_user_data(wl.selection) : NULL;
    if (!text && o && (o->text || o->plain))
        text = receive(wl.selection, o->text ? TEXT_TYPE : "text/plain", &have);
    if (text && !have) {
        free(text);
        text = NULL;
    }
    if (length)
        *length = text ? have : 0;
    return text;
}

bool gui_clipboard_get_image(struct gui_image *image) {
    struct offer *o;
    size_t length = 0;
    char *png = NULL;
    bool ok;

    if (wl.own && wl.own_image) {
        png = malloc(wl.own_length);
        if (png)
            memcpy(png, wl.own, length = wl.own_length);
    } else {
        wl_display_roundtrip(wl.display);
        o = wl.selection ? wl_data_offer_get_user_data(wl.selection) : NULL;
        if (o && o->image)
            png = receive(wl.selection, IMAGE_TYPE, &length);
    }
    ok = png && gui_image_decode(png, length, image);
    free(png);
    return ok;
}

// Dragging a file from the window the mouse is held in, as a URI.
bool gui_drag_file(const char *path) {
    struct wl_data_source *source;
    char *uri;

    if (!wl.data_device || !wl.pointed || !wl.buttons || asprintf(&uri, "file://%s\r\n", path) < 0)
        return false;
    source = wl_data_device_manager_create_data_source(wl.data_manager);
    wl_data_source_add_listener(source, &source_listener, uri);
    wl_data_source_offer(source, URI_TYPE);
    wl_data_source_set_actions(source, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    wl_data_device_start_drag(wl.data_device, source, wl.pointed->backend->surface, NULL, wl.press_serial);
    wl_display_flush(wl.display);
    return true;
}

bool gui_dropped_file(const struct gui_message *drop, char path[GUI_PATH_MAX]) {
    if (drop->type != GUI_DROP || !wl.dropped[0])
        return false;
    strcpy(path, wl.dropped);
    return true;
}

// Reads what the compositor's sent, without waiting, saying whether there are events.
bool gui_ready(void) {
    return wl.display && gui_backend_wait(gui_now_ms()) && gui_queued() > 0;
}

int64_t gui_backend_due(void) {
    return wl.repeating.code ? wl.repeat_due : -1;
}

// Waits for the compositor, until due if it isn't -1, or a key's due to repeat,
// putting what it sends with the events.
bool gui_backend_wait(int64_t due) {
    struct pollfd fd = {.fd = wl_display_get_fd(wl.display), .events = POLLIN};
    int queued = gui_queued();

    while (wl_display_prepare_read(wl.display) != 0)
        wl_display_dispatch_pending(wl.display);
    if (gui_queued() > queued) { // what was waiting already
        wl_display_cancel_read(wl.display);
        return true;
    }
    wl_display_flush(wl.display);
    if (wl.repeating.code && (due < 0 || wl.repeat_due < due))
        due = wl.repeat_due;

    int64_t wait = due - gui_now_ms();
    int ready = poll(&fd, 1, due < 0 ? -1 : wait > 0 ? (int)wait : 0);

    if (ready > 0) {
        if (wl_display_read_events(wl.display) < 0)
            return false;
    } else {
        wl_display_cancel_read(wl.display);
    }
    if (wl_display_dispatch_pending(wl.display) < 0)
        return false;
    repeat_keys();
    return true;
}
