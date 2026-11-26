#include "server.h"
#include "aximos-shell-v1-protocol.h"
#include <drm_fourcc.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <key_linux.h>
#include <key_text.h>
#include <libinput.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/libinput.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_shm.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_switch.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
#include <xkbcommon/xkbcommon.h>

// wm on wlroots. The backend gives the screen, the keyboards and pointers, and the
// session, through libseat. wm draws the whole desktop itself, in memory, which is
// put on the screen as a texture each frame, and apps' windows are copied out of
// their shared memory buffers into wm's windows. Apps get only wl_shm, so their
// pixels can always be read.

#define MAX_KEYS 256 // Linux's key codes that are held down, for each surface
#define WHEEL_STEP 15 // how far a notch of the wheel scrolls, in Wayland's units
#define CLIPBOARD_MAX (64 * 1024 * 1024)
#define TEXT_TYPE "text/plain;charset=utf-8"
#define URI_TYPE "text/uri-list"
#define IMAGE_TYPE "image/png"

struct surface {
    struct wl_list link;
    struct window *window; // wm's, while it's shown
    struct wlr_surface *surface;
    struct wlr_xdg_toplevel *toplevel; // one of these three
    struct wlr_xdg_popup *popup;
    struct wlr_xwayland_surface *xsurface;
    struct surface *parent; // a popup's
    uint32_t *pixels;       // its surfaces, put together, width by height, at scale
    int width, height, scale;
    uint64_t layout; // where its surfaces are, and how big, as last gathered, hashed
    int x, y; // where an X window was last put, or a popup is relative to its parent's contents
    bool translucent;
    bool keys[MAX_KEYS];
    struct wl_listener map, unmap, commit, destroy, title, app_id, configure, activate, associate, dissociate,
        reposition, close;
};

static struct server {
    struct wl_display *display;
    struct wl_event_loop *loop;
    struct wlr_backend *backend;
    struct wlr_session *session;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_compositor *compositor;
    struct wlr_output_layout *layout;
    struct wlr_output *output; // the one wm shows the desktop on
    struct wlr_seat *seat;
    struct wlr_cursor *cursor;
    struct wlr_keyboard *keyboard; // the last one typed on
    struct wlr_xdg_shell *xdg_shell;
    struct wlr_xwayland *xwayland;
    struct wl_global *shell;
    struct wl_list surfaces, shells, keyboards;
    struct wl_client *login; // the log-in screen's connection
    struct wl_listener login_destroy;
    struct wl_listener new_output, new_input, new_toplevel, new_popup, new_xsurface, xwayland_ready, frame,
        output_destroy, request_cursor, request_shape, request_selection, request_primary, request_drag, start_drag,
        set_selection, new_decoration, motion, motion_absolute, button, axis, swipe_begin, swipe_update, swipe_end;

    // The desktop, as wm draws it, and what's changed since the last frame.
    struct screen *screen;
    struct wlr_buffer pixels;
    bool pixels_made;
    struct wlr_texture *texture;
    pixman_region32_t changed;
    bool wanted, waiting; // a frame's to be shown, or one's being shown
    int scale;

    const struct input *input;
    uint32_t buttons;
    bool pointer_sent; // whether wm gave the pointer to an app, this time it moved
    struct wl_event_source *repeat;
    int repeat_delay, repeat_rate;
    struct key_event repeating;
    double swipe_x, swipe_y;
    int swipe_fingers;

    // A selection being read, for wm's clipboard history.
    struct wl_event_source *reading;
    int read_fd;
    char *read_data;
    bool read_drop; // what's read is a dragged file's URI, dropped on the desktop
    size_t read_length;
    bool read_image;
} server;

// What wlroots says: its errors, and why it refused an app's copy, which is otherwise
// only said with everything else, with WM_DEBUG.
static void wlroots_said(enum wlr_log_importance importance, const char *format, va_list args) {
    if (importance > WLR_ERROR && !getenv("WM_DEBUG") && !strstr(format, "Rejecting set_selection"))
        return;
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
}

// Milliseconds, for Wayland's events, which only compare them.
static uint32_t now_ms(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

// wm's own pixels, as a buffer wlroots can make a texture from.
static bool pixels_access(struct wlr_buffer *buffer, uint32_t flags, void **data, uint32_t *format, size_t *stride) {
    (void)buffer, (void)flags;
    *data = server.screen->pixels;
    *format = DRM_FORMAT_XRGB8888;
    *stride = server.screen->pitch;
    return true;
}

static void pixels_end(struct wlr_buffer *buffer) {
    (void)buffer;
}

static void pixels_destroy(struct wlr_buffer *buffer) {
    (void)buffer;
}

static const struct wlr_buffer_impl pixels_impl = {
    .destroy = pixels_destroy, .begin_data_ptr_access = pixels_access, .end_data_ptr_access = pixels_end};

// The desktop's pixels, the output's size. A new texture is made for them when the size changes.
static bool new_pixels(void) {
    struct screen *s = server.screen;
    int width = server.output->width, height = server.output->height;
    uint8_t *pixels = calloc((size_t)width * height, sizeof(uint32_t));

    if (!pixels)
        return false;
    free(s->pixels);
    *s = (struct screen){pixels, width, height, width * 4, !wl_list_empty(&server.output->modes)};
    if (server.texture)
        wlr_texture_destroy(server.texture);
    server.texture = NULL;
    if (server.pixels_made)
        wlr_buffer_finish(&server.pixels);
    wlr_buffer_init(&server.pixels, &pixels_impl, width, height);
    server.pixels_made = true;
    pixman_region32_union_rect(&server.changed, &server.changed, 0, 0, width, height);
    return true;
}

// Puts what changed on the screen, if the last frame's been shown.
static void show_frame(void) {
    struct wlr_output_state state;
    struct wlr_render_pass *pass;

    if (!server.output || server.waiting || !server.wanted)
        return;
    if (server.texture && !wlr_texture_update_from_buffer(server.texture, &server.pixels, &server.changed)) {
        wlr_texture_destroy(server.texture);
        server.texture = NULL;
    }
    if (!server.texture)
        server.texture = wlr_texture_from_buffer(server.renderer, &server.pixels);
    if (!server.texture)
        return;
    wlr_output_state_init(&state);
    pass = wlr_output_begin_render_pass(server.output, &state, NULL);
    if (pass) {
        wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
                                              .texture = server.texture, .blend_mode = WLR_RENDER_BLEND_MODE_NONE});
        if (wlr_render_pass_submit(pass) && wlr_output_commit_state(server.output, &state)) {
            server.waiting = true;
            server.wanted = false;
            pixman_region32_clear(&server.changed);
        }
    }
    wlr_output_state_finish(&state);
}

// Each frame shown lets apps draw their next.
static void frame_done(struct wlr_surface *surface, int sx, int sy, void *now) {
    (void)sx, (void)sy;
    wlr_surface_send_frame_done(surface, now);
}

static void on_frame(struct wl_listener *l, void *data) {
    struct surface *s;
    struct timespec now;

    (void)l, (void)data;
    server.waiting = false;
    show_frame();
    clock_gettime(CLOCK_MONOTONIC, &now);
    wl_list_for_each(s, &server.surfaces, link) {
        if (s->surface)
            wlr_surface_for_each_surface(s->surface, frame_done, &now);
    }
}

static void on_output_destroy(struct wl_listener *l, void *data) {
    (void)l, (void)data;
    wl_list_remove(&server.frame.link);
    wl_list_remove(&server.output_destroy.link);
    server.output = NULL;
    server.waiting = false;
}

// The first screen is wm's, at its preferred mode. Others are left off, for now.
static void on_new_output(struct wl_listener *l, void *data) {
    struct wlr_output *output = data;
    struct wlr_output_state state;
    struct wlr_output_mode *mode = wlr_output_preferred_mode(output);

    (void)l;
    if (server.output || !wlr_output_init_render(output, server.allocator, server.renderer))
        return;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    if (mode)
        wlr_output_state_set_mode(&state, mode);
    wlr_output_state_set_scale(&state, server.scale);
    if (!wlr_output_commit_state(output, &state)) {
        wlr_output_state_finish(&state);
        return;
    }
    wlr_output_state_finish(&state);
    server.output = output;
    server.waiting = true; // until that's shown
    server.frame.notify = on_frame;
    wl_signal_add(&output->events.frame, &server.frame);
    server.output_destroy.notify = on_output_destroy;
    wl_signal_add(&output->events.destroy, &server.output_destroy);
    wlr_output_layout_add_auto(server.layout, output);
    wlr_output_create_global(output, server.display);
}

// An app's surfaces, put together into its pixels, in the part of them that changed.
struct gather {
    struct surface *s;
    pixman_box32_t box; // in the window's contents
    int origin_x, origin_y; // where the contents start in the root surface
};

// Each of the window's pixels, at the desktop's scale, from the nearest of the
// surface's buffer: the part of it the surface shows, which may be cropped, and at
// any size, as an app scales it, by its buffer scale or with viewporter, as Firefox
// does, or an X app's, at 1x, made twice the size. In 16.16 fixed point, a desktop
// pixel's middle, as a buffer's.
static int64_t buffer_at(double start, double size, int surface_size, int at) {
    return (int64_t)((start + (at + 0.5) * size / surface_size) * 65536);
}

static void copy_surface(struct wlr_surface *surface, int sx, int sy, void *data) {
    struct gather *g = data;
    struct surface *s = g->s;
    struct wlr_buffer *buffer = surface->current.buffer;
    int width = surface->current.width, height = surface->current.height, k = s->scale;
    struct wlr_fbox src;
    void *from;
    uint32_t format;
    size_t stride;

    if (!buffer || width <= 0 || height <= 0)
        return;
    wlr_surface_get_buffer_source_box(surface, &src);
    if (src.width <= 0 || src.height <= 0
        || !wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &from, &format, &stride))
        return;
    if (format == DRM_FORMAT_XRGB8888 || format == DRM_FORMAT_ARGB8888) {
        bool alpha = format == DRM_FORMAT_ARGB8888 && s->translucent;
        int left = sx - g->origin_x, top = sy - g->origin_y;
        int x0 = g->box.x1 > left ? g->box.x1 : left, y0 = g->box.y1 > top ? g->box.y1 : top;
        int x1 = left + width, y1 = top + height;
        int64_t step = (int64_t)(src.width / (width * k) * 65536);

        x1 = x1 < g->box.x2 ? x1 : g->box.x2;
        y1 = y1 < g->box.y2 ? y1 : g->box.y2;
        x1 = x1 < s->width ? x1 : s->width;
        y1 = y1 < s->height ? y1 : s->height;
        x0 = x0 > 0 ? x0 : 0, y0 = y0 > 0 ? y0 : 0;

        // A buffer at the display's own scale, as apps draw them, pixel for pixel.
        bool same = src.x == (int)src.x && src.y == (int)src.y && src.width == width * k && src.height == height * k;

        for (int y = y0 * k; same && !alpha && y < y1 * k; y++) {
            const uint32_t *row = (const uint32_t *)((const uint8_t *)from + (size_t)((int)src.y + y - top * k) * stride)
                                + (int)src.x - left * k;
            uint32_t *to = s->pixels + (size_t)y * s->width * k;

            for (int x = x0 * k; x < x1 * k; x++)
                to[x] = row[x] & 0xFFFFFF;
        }
        for (int y = y0 * k; !(same && !alpha) && y < y1 * k; y++) {
            int by = (int)(buffer_at(src.y, src.height, height * k, y - top * k) >> 16);
            const uint32_t *row = (const uint32_t *)((const uint8_t *)from
                                                     + (size_t)(by < buffer->height ? by : buffer->height - 1) * stride);
            uint32_t *to = s->pixels + (size_t)y * s->width * k;
            int64_t bx = buffer_at(src.x, src.width, width * k, x0 * k - left * k);

            for (int x = x0 * k; x < x1 * k; x++, bx += step) {
                uint32_t p = row[(bx >> 16) < buffer->width ? (bx >> 16) : buffer->width - 1];
                uint32_t a = p >> 24;

                // Wayland's alpha is how solid, with the colour already multiplied by it; wm's
                // top byte is how see-through, with the colour as it is.
                if (!alpha || a == 255)
                    to[x] = p & 0xFFFFFF;
                else if (a == 0)
                    to[x] = 0xFF000000;
                else
                    to[x] = (255 - a) << 24 | ((p >> 16 & 0xFF) * 255 / a) << 16 | ((p >> 8 & 0xFF) * 255 / a) << 8
                          | (p & 0xFF) * 255 / a;
            }
        }
    }
    wlr_buffer_end_data_ptr_access(buffer);
}

// The part of the window's contents geometry starts at, in its root surface.
static struct wlr_box geometry_of(const struct surface *s) {
    if (s->toplevel)
        return s->toplevel->base->geometry;
    if (s->popup)
        return s->popup->base->geometry;
    return (struct wlr_box){0}; // an X window is all its surface
}

// Where a window's surfaces are and how big, hashed, and where one of them is.
struct layout {
    struct wlr_surface *find;
    int x, y;
    bool found;
    uint64_t hash;
};

static void lay_out(struct wlr_surface *surface, int sx, int sy, void *data) {
    struct layout *l = data;
    uint64_t parts[] = {(uintptr_t)surface, (uint64_t)(uint32_t)sx, (uint64_t)(uint32_t)sy,
                        (uint64_t)surface->current.width << 32 | (uint32_t)surface->current.height};

    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++)
        l->hash = (l->hash ^ parts[i]) * 0x100000001B3ULL;
    if (surface == l->find)
        l->x = sx, l->y = sy, l->found = true;
}

// Gives wm what changed, after a commit to any of the window's surfaces: only what
// that surface says changed, where it is in the window, unless the window's size, or
// where its surfaces are, changed, when it's all of it.
static void gather(struct surface *s, struct wlr_surface *committed) {
    struct wlr_box g = geometry_of(s);
    int width = g.width > 0 ? g.width : s->surface->current.width, height = g.height > 0 ? g.height : s->surface->current.height;
    struct gather into = {s, {0, 0, width, height}, g.x, g.y};
    struct layout l = {.find = committed, .hash = 0xCBF29CE484222325ULL};

    if (width <= 0 || height <= 0)
        return;
    wlr_surface_for_each_surface(s->surface, lay_out, &l);
    if (width != s->width || height != s->height || s->scale != server.scale) {
        uint32_t *pixels = calloc((size_t)width * height * server.scale * server.scale, sizeof(uint32_t));

        if (!pixels)
            return;
        free(s->pixels);
        s->pixels = pixels, s->width = width, s->height = height, s->scale = server.scale;
    } else if (committed == s->surface && l.hash == s->layout && wl_list_empty(&s->surface->current.subsurfaces_above)
               && wl_list_empty(&s->surface->current.subsurfaces_below)) {
        // With no other surfaces, only what the app says it changed. One with them, such
        // as Firefox's page, in a subsurface synchronized with the window's, is all copied.
        pixman_box32_t *b = pixman_region32_extents(&committed->buffer_damage);
        struct wlr_fbox src;
        double across, down;

        wlr_surface_get_buffer_source_box(committed, &src);
        across = src.width > 0 ? committed->current.width / src.width : 1;
        down = src.height > 0 ? committed->current.height / src.height : 1;
        into.box = (pixman_box32_t){(int)floor((b->x1 - src.x) * across) + l.x - g.x,
                                    (int)floor((b->y1 - src.y) * down) + l.y - g.y,
                                    (int)ceil((b->x2 - src.x) * across) + l.x - g.x,
                                    (int)ceil((b->y2 - src.y) * down) + l.y - g.y};
    }
    s->layout = l.hash;
    into.box.x1 = into.box.x1 > 0 ? into.box.x1 : 0, into.box.y1 = into.box.y1 > 0 ? into.box.y1 : 0;
    into.box.x2 = into.box.x2 < width ? into.box.x2 : width, into.box.y2 = into.box.y2 < height ? into.box.y2 : height;
    if (into.box.x2 <= into.box.x1 || into.box.y2 <= into.box.y1)
        return;
    wlr_surface_for_each_surface(s->surface, copy_surface, &into);
    if (s->window)
        wm_window_pixels(s->window, s->pixels, s->width, s->height, s->width * s->scale, into.box.x1, into.box.y1,
                         into.box.x2 - into.box.x1, into.box.y2 - into.box.y1);
}

static struct surface *surface_of(struct wlr_surface *surface) {
    struct surface *s;

    wl_list_for_each(s, &server.surfaces, link) {
        if (s->surface == surface)
            return s;
    }
    return NULL;
}

// The window a surface is part of, as subsurfaces are.
static struct surface *root_of(struct wlr_surface *surface) {
    struct wlr_surface *root = surface ? wlr_surface_get_root_surface(surface) : NULL;

    return root ? surface_of(root) : NULL;
}

static void on_commit(struct wl_listener *l, void *data) {
    struct surface *s = wl_container_of(l, s, commit);

    (void)data;
    if (s->toplevel && s->toplevel->base->initial_commit) {
        // Apps choose their own first size, and wm draws the frame, within the room
        // there is, which apps that size themselves from the screen, as Firefox does,
        // keep to.
        int width, height;

        wm_room(&width, &height);
        if (wl_resource_get_version(s->toplevel->resource) >= 4) // configure_bounds' version
            wlr_xdg_toplevel_set_bounds(s->toplevel, width, height);
        wlr_xdg_toplevel_set_size(s->toplevel, 0, 0);
        return;
    }
    if (s->popup && s->popup->base->initial_commit) {
        struct wlr_box desktop = {-s->parent->x, -s->parent->y, 0, 0};
        int x, y;

        if (s->parent->window) {
            wm_window_position(s->parent->window, &x, &y);
            desktop = (struct wlr_box){-x, -y, server.screen->width / server.scale, server.screen->height / server.scale};
        }
        wlr_xdg_popup_unconstrain_from_box(s->popup, &desktop);
        return;
    }
    gather(s, s->surface);
    if (server.output)
        wlr_output_schedule_frame(server.output);
}

// A subsurface committing changes its window.
static void on_subsurface_commit(struct wl_listener *l, void *data);

// Every surface, each told the desktop's scale, which apps draw their buffers at.
struct subsurface {
    struct wl_list link;
    struct wlr_surface *surface;
    struct wl_listener commit, destroy;
};

static struct wl_list all_surfaces = {&all_surfaces, &all_surfaces};

static void on_subsurface_destroy(struct wl_listener *l, void *data) {
    struct subsurface *sub = wl_container_of(l, sub, destroy);

    (void)data;
    wl_list_remove(&sub->link);
    wl_list_remove(&sub->commit.link);
    wl_list_remove(&sub->destroy.link);
    free(sub);
}

static void on_subsurface_commit(struct wl_listener *l, void *data) {
    struct subsurface *sub = wl_container_of(l, sub, commit);
    struct surface *s = root_of(sub->surface);

    (void)data;
    if (s && s->surface != sub->surface) {
        gather(s, NULL);
        if (server.output)
            wlr_output_schedule_frame(server.output);
    }
}

static void on_new_surface(struct wl_listener *l, void *data) {
    struct wlr_surface *surface = data;
    struct subsurface *sub = calloc(1, sizeof(*sub));

    (void)l;
    if (!sub)
        return;
    sub->surface = surface;
    wl_list_insert(&all_surfaces, &sub->link);
    wlr_surface_set_preferred_buffer_scale(surface, server.scale);
    if (server.output)
        wlr_surface_send_enter(surface, server.output);
    sub->commit.notify = on_subsurface_commit;
    wl_signal_add(&surface->events.commit, &sub->commit);
    sub->destroy.notify = on_subsurface_destroy;
    wl_signal_add(&surface->events.destroy, &sub->destroy);
}

static const char *title_of(const struct surface *s) {
    const char *title = s->toplevel ? s->toplevel->title : s->xsurface ? s->xsurface->title : NULL;

    return title ? title : "";
}

static const char *app_of(const struct surface *s) {
    const char *app = s->toplevel ? s->toplevel->app_id : s->xsurface ? s->xsurface->class : NULL;

    return app ? app : "";
}

static void on_map(struct wl_listener *l, void *data) {
    struct surface *s = wl_container_of(l, s, map);
    struct wlr_box g = geometry_of(s);
    bool popup = s->popup || (s->xsurface && s->xsurface->override_redirect);
    int x = s->xsurface ? s->xsurface->x : 0, y = s->xsurface ? s->xsurface->y : 0;

    (void)data;
    if (s->popup && s->parent->window) {
        wm_window_position(s->parent->window, &x, &y);
        x += s->popup->current.geometry.x, y += s->popup->current.geometry.y;
    }
    s->width = s->height = 0; // so gather starts with all of it
    s->window = wm_window_new(s, g.width > 0 ? g.width : s->surface->current.width,
                              g.height > 0 ? g.height : s->surface->current.height, popup, x, y);
    if (!s->window) {
        if (s->toplevel)
            wlr_xdg_toplevel_send_close(s->toplevel);
        return;
    }
    wm_window_title(s->window, title_of(s));
    wm_window_app(s->window, app_of(s));
    gather(s, NULL);
    int at_x, at_y;

    wm_window_position(s->window, &at_x, &at_y);
    printf("wm: %s opened a window, %dx%d at %d %d\n", app_of(s), s->width, s->height, at_x, at_y);
}

static void on_unmap(struct wl_listener *l, void *data) {
    struct surface *s = wl_container_of(l, s, unmap);

    (void)data;
    if (s->window)
        wm_window_gone(s->window);
    s->window = NULL;
}

static void on_title(struct wl_listener *l, void *data) {
    struct surface *s = wl_container_of(l, s, title);

    (void)data;
    if (s->window)
        wm_window_title(s->window, title_of(s));
}

static void on_app_id(struct wl_listener *l, void *data) {
    struct surface *s = wl_container_of(l, s, app_id);

    (void)data;
    if (s->window)
        wm_window_app(s->window, app_of(s));
}

static void forget(struct surface *s) {
    wl_list_remove(&s->link);
    wl_list_remove(&s->destroy.link);
    free(s->pixels);
    free(s);
}

static void on_destroy(struct wl_listener *l, void *data) {
    struct surface *s = wl_container_of(l, s, destroy);

    (void)data;
    if (s->window)
        wm_window_gone(s->window);
    if (s->surface) {
        wl_list_remove(&s->map.link);
        wl_list_remove(&s->unmap.link);
        wl_list_remove(&s->commit.link);
    }
    if (s->toplevel) {
        wl_list_remove(&s->title.link);
        wl_list_remove(&s->app_id.link);
    }
    if (s->popup)
        wl_list_remove(&s->reposition.link);
    if (s->xsurface) {
        wl_list_remove(&s->title.link);
        wl_list_remove(&s->app_id.link);
        wl_list_remove(&s->configure.link);
        wl_list_remove(&s->activate.link);
        wl_list_remove(&s->associate.link);
        wl_list_remove(&s->dissociate.link);
        wl_list_remove(&s->close.link);
    }
    forget(s);
}

static void watch_surface(struct surface *s, struct wlr_surface *surface) {
    s->surface = surface;
    s->map.notify = on_map;
    wl_signal_add(&surface->events.map, &s->map);
    s->unmap.notify = on_unmap;
    wl_signal_add(&surface->events.unmap, &s->unmap);
    s->commit.notify = on_commit;
    wl_signal_add(&surface->events.commit, &s->commit);
}

static struct surface *new_surface(struct wl_signal *destroyed) {
    struct surface *s = calloc(1, sizeof(*s));

    if (!s)
        return NULL;
    wl_list_insert(&server.surfaces, &s->link);
    s->destroy.notify = on_destroy;
    wl_signal_add(destroyed, &s->destroy);
    return s;
}

static void on_new_toplevel(struct wl_listener *l, void *data) {
    struct wlr_xdg_toplevel *toplevel = data;
    struct surface *s = new_surface(&toplevel->events.destroy);

    (void)l;
    if (!s)
        return;
    s->toplevel = toplevel;
    watch_surface(s, toplevel->base->surface);
    s->title.notify = on_title;
    wl_signal_add(&toplevel->events.set_title, &s->title);
    s->app_id.notify = on_app_id;
    wl_signal_add(&toplevel->events.set_app_id, &s->app_id);
}

// A popup moved by its app, such as a menu that's grown.
static void on_reposition(struct wl_listener *l, void *data) {
    struct surface *s = wl_container_of(l, s, reposition);
    int x, y;

    (void)data;
    if (!s->window || !s->parent->window)
        return;
    wm_window_position(s->parent->window, &x, &y);
    wm_window_move(s->window, x + s->popup->current.geometry.x, y + s->popup->current.geometry.y);
}

static void on_new_popup(struct wl_listener *l, void *data) {
    struct wlr_xdg_popup *popup = data;
    struct surface *parent = popup->parent ? root_of(popup->parent) : NULL, *s;

    (void)l;
    if (!parent || !(s = new_surface(&popup->events.destroy)))
        return;
    s->popup = popup;
    s->parent = parent;
    watch_surface(s, popup->base->surface);
    s->reposition.notify = on_reposition;
    wl_signal_add(&popup->events.reposition, &s->reposition);
}

// Every toplevel gets wm's frame rather than drawing its own, which it's told once
// it's made its first commit, and whenever it asks.
struct decoration {
    struct wlr_xdg_toplevel_decoration_v1 *decoration;
    struct wl_listener commit, request, destroy;
};

static void decorate(struct decoration *d) {
    if (d->decoration->toplevel->base->initialized)
        wlr_xdg_toplevel_decoration_v1_set_mode(d->decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void on_decoration_commit(struct wl_listener *l, void *data) {
    struct decoration *d = wl_container_of(l, d, commit);

    (void)data;
    if (d->decoration->toplevel->base->initial_commit)
        decorate(d);
}

static void on_decoration_request(struct wl_listener *l, void *data) {
    struct decoration *d = wl_container_of(l, d, request);

    (void)data;
    decorate(d);
}

static void on_decoration_destroy(struct wl_listener *l, void *data) {
    struct decoration *d = wl_container_of(l, d, destroy);

    (void)data;
    wl_list_remove(&d->commit.link);
    wl_list_remove(&d->request.link);
    wl_list_remove(&d->destroy.link);
    free(d);
}

static void on_new_decoration(struct wl_listener *l, void *data) {
    struct decoration *d = calloc(1, sizeof(*d));

    (void)l;
    if (!d)
        return;
    d->decoration = data;
    d->commit.notify = on_decoration_commit;
    wl_signal_add(&d->decoration->toplevel->base->surface->events.commit, &d->commit);
    d->request.notify = on_decoration_request;
    wl_signal_add(&d->decoration->events.request_mode, &d->request);
    d->destroy.notify = on_decoration_destroy;
    wl_signal_add(&d->decoration->events.destroy, &d->destroy);
    decorate(d);
}

// X windows. One that's managed is framed by wm and put where wm says, while one
// that isn't, a menu or tooltip, goes where it asks.
static void on_x_configure(struct wl_listener *l, void *data) {
    struct surface *s = wl_container_of(l, s, configure);
    struct wlr_xwayland_surface_configure_event *e = data;

    if (s->window && !s->xsurface->override_redirect) { // wm says where, keeping the size asked for
        wlr_xwayland_surface_configure(s->xsurface, s->x, s->y, e->width, e->height);
        return;
    }
    wlr_xwayland_surface_configure(s->xsurface, e->x, e->y, e->width, e->height);
    if (s->window)
        wm_window_move(s->window, e->x, e->y);
}

static void on_x_activate(struct wl_listener *l, void *data) {
    struct surface *s = wl_container_of(l, s, activate);

    (void)data;
    if (s->window)
        wm_window_raise(s->window);
}

static void on_x_close(struct wl_listener *l, void *data) {
    struct surface *s = wl_container_of(l, s, close);

    (void)data;
    wlr_xwayland_surface_close(s->xsurface);
}

static void on_x_associate(struct wl_listener *l, void *data) {
    struct surface *s = wl_container_of(l, s, associate);

    (void)data;
    watch_surface(s, s->xsurface->surface);
}

static void on_x_dissociate(struct wl_listener *l, void *data) {
    struct surface *s = wl_container_of(l, s, dissociate);

    (void)data;
    if (s->window)
        wm_window_gone(s->window);
    s->window = NULL;
    wl_list_remove(&s->map.link);
    wl_list_remove(&s->unmap.link);
    wl_list_remove(&s->commit.link);
    s->surface = NULL;
}

static void on_new_xsurface(struct wl_listener *l, void *data) {
    struct wlr_xwayland_surface *xsurface = data;
    struct surface *s = new_surface(&xsurface->events.destroy);

    (void)l;
    if (!s)
        return;
    s->xsurface = xsurface;
    s->x = xsurface->x, s->y = xsurface->y;
    s->title.notify = on_title;
    wl_signal_add(&xsurface->events.set_title, &s->title);
    s->app_id.notify = on_app_id;
    wl_signal_add(&xsurface->events.set_class, &s->app_id);
    s->configure.notify = on_x_configure;
    wl_signal_add(&xsurface->events.request_configure, &s->configure);
    s->activate.notify = on_x_activate;
    wl_signal_add(&xsurface->events.request_activate, &s->activate);
    s->close.notify = on_x_close;
    wl_signal_add(&xsurface->events.request_close, &s->close);
    s->associate.notify = on_x_associate;
    wl_signal_add(&xsurface->events.associate, &s->associate);
    s->dissociate.notify = on_x_dissociate;
    wl_signal_add(&xsurface->events.dissociate, &s->dissociate);
}

static void on_xwayland_ready(struct wl_listener *l, void *data) {
    (void)l, (void)data;
    wlr_xwayland_set_seat(server.xwayland, server.seat);
    printf("wm: XWayland started, on %s\n", server.xwayland->display_name);
}

// Keeps each managed X window told where wm has put it, so its menus open beside it.
static void place_x_windows(void) {
    struct surface *s;
    int x, y;

    wl_list_for_each(s, &server.surfaces, link) {
        if (!s->xsurface || !s->window || s->xsurface->override_redirect)
            continue;
        wm_window_position(s->window, &x, &y);
        if (x != s->x || y != s->y) {
            s->x = x, s->y = y;
            wlr_xwayland_surface_configure(s->xsurface, x, y, s->xsurface->width, s->xsurface->height);
        }
    }
}

// wm's messages, to Wayland's and X's apps.

static void focus(struct surface *s, bool on) {
    struct wlr_keyboard *k = server.keyboard;

    if (s->toplevel)
        wlr_xdg_toplevel_set_activated(s->toplevel, on);
    if (s->xsurface)
        wlr_xwayland_surface_activate(s->xsurface, on);
    if (on && s->surface)
        wlr_seat_keyboard_notify_enter(server.seat, s->surface, k ? k->keycodes : NULL, k ? k->num_keycodes : 0,
                                       k ? &k->modifiers : NULL);
    else if (s->surface && server.seat->keyboard_state.focused_surface == s->surface)
        wlr_seat_keyboard_notify_clear_focus(server.seat);
    memset(s->keys, 0, sizeof(s->keys)); // what's held now isn't the app's to let go
}

// Apps repeat keys themselves, so only the first press goes to them.
static void key(struct surface *s, const struct key_event *e) {
    uint32_t code = key_to_linux(e->code);

    if (!code || code >= MAX_KEYS || !s->surface || s->keys[code] == (bool)e->pressed)
        return;
    s->keys[code] = e->pressed;
    if (server.keyboard)
        wlr_seat_keyboard_notify_modifiers(server.seat, &server.keyboard->modifiers);
    wlr_seat_keyboard_notify_key(server.seat, now_ms(), code,
                                 e->pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
}

// The surface of a window's at a point in its contents, which may be one of its subsurfaces.
static void point_at(struct surface *s, int x, int y) {
    struct wlr_box g = geometry_of(s);
    double sx, sy;
    struct wlr_surface *under = wlr_surface_surface_at(s->surface, x + g.x, y + g.y, &sx, &sy);

    if (!under) { // outside, while a button's held: the root surface still has it
        under = s->surface;
        sx = x + g.x, sy = y + g.y;
    }
    if (server.seat->pointer_state.focused_surface != under)
        wlr_seat_pointer_notify_enter(server.seat, under, sx, sy);
    wlr_seat_pointer_notify_motion(server.seat, now_ms(), sx, sy);
}

// Pressing outside a popup, such as an open menu, closes it, and the popups it opened.
static bool opened(const struct surface *popup, const struct surface *s) {
    for (; s; s = s->parent) {
        if (s == popup)
            return true;
    }
    return false;
}

// Destroying a popup frees it, and the popups it opened, which may be any of the
// list's, so after each the list's looked through again.
static void dismiss_popups(const struct surface *pressed) {
    struct surface *s;
    bool dismissed;

    do {
        dismissed = false;
        wl_list_for_each(s, &server.surfaces, link) {
            if (s->popup && s->window && !opened(s, pressed)) {
                wlr_xdg_popup_destroy(s->popup);
                dismissed = true;
                break;
            }
        }
    } while (dismissed);
}

static void mouse(struct surface *s, const struct gui_message *m) {
    static const struct {
        uint32_t mask, button;
    } buttons[] = {{MOUSE_BUTTON_LEFT, 0x110}, {MOUSE_BUTTON_RIGHT, 0x111}, {MOUSE_BUTTON_MIDDLE, 0x112}};

    if (!s->surface || server.seat->drag)
        return;
    server.pointer_sent = true;
    if (m->type == GUI_MOUSE_BUTTON && (m->buttons & m->changed))
        dismiss_popups(s);
    point_at(s, m->x, m->y);
    for (size_t i = 0; m->type == GUI_MOUSE_BUTTON && i < sizeof(buttons) / sizeof(buttons[0]); i++) {
        if (m->changed & buttons[i].mask)
            wlr_seat_pointer_notify_button(server.seat, now_ms(), buttons[i].button,
                                           m->buttons & buttons[i].mask ? WL_POINTER_BUTTON_STATE_PRESSED
                                                                        : WL_POINTER_BUTTON_STATE_RELEASED);
    }
    if (m->type == GUI_MOUSE_WHEEL)
        wlr_seat_pointer_notify_axis(server.seat, now_ms(), WL_POINTER_AXIS_VERTICAL_SCROLL, m->wheel * WHEEL_STEP,
                                     m->wheel * 120, WL_POINTER_AXIS_SOURCE_WHEEL,
                                     WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    wlr_seat_pointer_notify_frame(server.seat);
}

void surface_send(struct surface *s, const struct gui_message *m) {
    switch (m->type) {
    case GUI_FOCUS:
        focus(s, m->focused);
        break;
    case GUI_KEY:
        key(s, &m->key);
        break;
    case GUI_MOUSE_MOVE:
    case GUI_MOUSE_BUTTON:
    case GUI_MOUSE_WHEEL:
        mouse(s, m);
        break;
    case GUI_CLOSE:
        if (s->toplevel)
            wlr_xdg_toplevel_send_close(s->toplevel);
        else if (s->xsurface)
            wlr_xwayland_surface_close(s->xsurface);
        else if (s->popup)
            wlr_xdg_popup_destroy(s->popup);
        break;
    case GUI_RESIZE:
        if (s->toplevel)
            wlr_xdg_toplevel_set_size(s->toplevel, m->width, m->height);
        else if (s->xsurface)
            wlr_xwayland_surface_configure(s->xsurface, s->x, s->y, m->width, m->height);
        break;
    }
}

bool surface_is_login(const struct surface *s) {
    return server.login && s->surface && wl_resource_get_client(s->surface->resource) == server.login;
}

// libgui's own protocol, for what Wayland hasn't.

static void shell_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void shell_set_translucent(struct wl_client *client, struct wl_resource *resource, struct wl_resource *surface,
                                  uint32_t on) {
    struct surface *s = root_of(wlr_surface_from_resource(surface));

    (void)client, (void)resource;
    if (!s)
        return;
    s->translucent = on;
    if (s->window) {
        wm_window_translucent(s->window, on);
        s->width = 0; // copied again, with its alpha
        gather(s, NULL);
    }
}

static void shell_notify(struct wl_client *client, struct wl_resource *resource, struct wl_resource *surface,
                         const char *title, const char *body, const char *label, const char *command) {
    struct surface *s = surface ? root_of(wlr_surface_from_resource(surface)) : NULL;

    (void)client, (void)resource;
    wm_notify(s ? s->window : NULL, title, body, label, command);
}

static void shell_settings_changed(struct wl_client *client, struct wl_resource *resource) {
    (void)client, (void)resource;
    wm_settings_changed();
}

static void shell_unlock(struct wl_client *client, struct wl_resource *resource) {
    (void)resource;
    if (client == server.login)
        wm_unlock();
}

static const struct aximos_shell_v1_interface shell_impl = {
    .destroy = shell_destroy, .set_translucent = shell_set_translucent, .notify = shell_notify,
    .settings_changed = shell_settings_changed, .unlock = shell_unlock};

static void shell_gone(struct wl_resource *resource) {
    wl_list_remove(wl_resource_get_link(resource));
}

static void send_screen(struct wl_resource *resource) {
    aximos_shell_v1_send_screen(resource, server.screen->width / server.scale, server.screen->height / server.scale,
                                server.screen->can_set_mode, server.scale);
}

static void bind_shell(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client, &aximos_shell_v1_interface, version, id);

    (void)data;
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &shell_impl, NULL, shell_gone);
    wl_list_insert(&server.shells, wl_resource_get_link(resource));
    send_screen(resource);
}

void server_broadcast(const struct gui_message *m) {
    struct wl_resource *resource;

    wl_resource_for_each(resource, &server.shells) {
        if (m->type == GUI_SCREEN)
            send_screen(resource);
        else if (m->type == GUI_SETTINGS)
            aximos_shell_v1_send_settings_changed(resource);
    }
}

static void on_login_destroy(struct wl_listener *l, void *data) {
    (void)l, (void)data;
    wl_list_remove(&server.login_destroy.link);
    server.login = NULL;
    wm_login_gone();
}

bool server_start_login(const char *path, const char *option) {
    int pair[2];
    pid_t pid;

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) < 0)
        return false;
    if ((pid = fork()) == 0) {
        char fd[16];

        snprintf(fd, sizeof(fd), "%d", dup(pair[1])); // without close-on-exec
        setenv("WAYLAND_SOCKET", fd, 1);
        execl(path, "login", option, (char *)NULL);
        _exit(127);
    }
    close(pair[1]);
    if (pid < 0 || !(server.login = wl_client_create(server.display, pair[0]))) {
        close(pair[0]);
        return false;
    }
    server.login_destroy.notify = on_login_destroy;
    wl_client_add_destroy_listener(server.login, &server.login_destroy);
    return true;
}

void server_close_apps(void) {
    struct wl_list *clients = wl_display_get_client_list(server.display), *link, *next;
    struct wl_client *x = server.xwayland && server.xwayland->server ? server.xwayland->server->client : NULL;

    for (link = clients->next; link != clients; link = next) {
        struct wl_client *client = wl_client_from_link(link);

        next = link->next;
        if (client != server.login && client != x)
            wl_client_destroy(client);
    }
}

// The clipboard. Whatever an app copies is read, as text or a PNG, for wm's history,
// and what wm puts there is offered to every app as a source of its own.

struct wm_source {
    struct wlr_data_source base;
    char *data;
    size_t length;
};

static bool encode_png(const char *pixels, int width, int height, char **out, size_t *length) {
    struct gui_canvas c = {(uint32_t *)pixels, width, height, width, 1};
    int fd = memfd_create("clipboard", MFD_CLOEXEC);
    char path[64];
    off_t size;
    bool ok = false;

    if (fd < 0)
        return false;
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    if (gui_image_save(path, &c) && (size = lseek(fd, 0, SEEK_END)) > 0 && (*out = malloc(size))) {
        ok = pread(fd, *out, size, 0) == size;
        *length = size;
    }
    close(fd);
    return ok;
}

static void source_send(struct wlr_data_source *source, const char *type, int fd) {
    struct wm_source *s = wl_container_of(source, s, base);

    (void)type;
    // Small enough for the pipe, mostly; what isn't goes as far as the reader takes it.
    for (size_t done = 0; done < s->length;) {
        ssize_t n = write(fd, s->data + done, s->length - done);

        if (n <= 0 && errno != EINTR)
            break;
        done += n > 0 ? n : 0;
    }
    close(fd);
}

static void source_destroy(struct wlr_data_source *source) {
    struct wm_source *s = wl_container_of(source, s, base);

    free(s->data);
    free(s);
}

static const struct wlr_data_source_impl source_impl = {.send = source_send, .destroy = source_destroy};

void server_set_clipboard(const char *data, size_t length, int width, int height) {
    struct wm_source *s = calloc(1, sizeof(*s));
    char **type;

    if (!s)
        return;
    wlr_data_source_init(&s->base, &source_impl);
    if (width) {
        if (!encode_png(data, width, height, &s->data, &s->length)) {
            free(s);
            return;
        }
    } else if (!(s->data = malloc(length ? length : 1))) {
        free(s);
        return;
    } else {
        memcpy(s->data, data, length);
        s->length = length;
    }
    if ((type = wl_array_add(&s->base.mime_types, sizeof(*type))))
        *type = strdup(width ? IMAGE_TYPE : TEXT_TYPE);
    if (!width && (type = wl_array_add(&s->base.mime_types, sizeof(*type))))
        *type = strdup("text/plain");
    wlr_seat_set_selection(server.seat, &s->base, wl_display_next_serial(server.display));
}

static void stop_reading(void) {
    server.read_drop = false;
    if (server.reading)
        wl_event_source_remove(server.reading);
    if (server.read_fd >= 0)
        close(server.read_fd);
    free(server.read_data);
    server.reading = NULL;
    server.read_fd = -1;
    server.read_data = NULL;
    server.read_length = 0;
}

// The first file of a list of URIs, as a path, undoing %-escapes, or false.
static bool first_path(const char *uris, char path[GUI_PATH_MAX]) {
    const char *at = strncmp(uris, "file://", 7) ? NULL : strchr(uris + 7, '/');
    size_t n = 0;

    for (; at && *at && *at != '\r' && *at != '\n' && n < GUI_PATH_MAX - 1; at++) {
        unsigned value;

        if (*at == '%' && sscanf(at + 1, "%2x", &value) == 1)
            path[n++] = (char)value, at += 2;
        else
            path[n++] = *at;
    }
    path[n] = '\0';
    return n > 0;
}

static void finish_reading(void) {
    struct gui_image image;
    char *data = server.read_data, path[GUI_PATH_MAX];
    size_t length = server.read_length;
    bool drop = server.read_drop;

    server.read_data = NULL;
    stop_reading();
    if (drop) {
        if (data && first_path(data, path))
            wm_desktop_drop(path);
        free(data);
        return;
    }
    if (!server.read_image) {
        wm_clipboard_changed(data, length, 0, 0);
        return;
    }
    if (gui_image_decode(data, length, &image)) {
        wm_clipboard_changed((char *)image.pixels, (size_t)image.width * image.height * 4, image.width, image.height);
    }
    free(data);
}

static int on_readable(int fd, uint32_t mask, void *data) {
    char chunk[65536];
    ssize_t n = read(fd, chunk, sizeof(chunk));

    (void)mask, (void)data;
    if (n > 0 && server.read_length + n <= CLIPBOARD_MAX) {
        char *bigger = realloc(server.read_data, server.read_length + n + 1);

        if (!bigger) {
            stop_reading();
            return 0;
        }
        memcpy(bigger + server.read_length, chunk, n);
        server.read_data = bigger;
        server.read_length += n;
        server.read_data[server.read_length] = '\0';
        return 0;
    }
    if (n == 0)
        finish_reading();
    else if (n > 0 || errno != EAGAIN)
        stop_reading();
    return 0;
}

static bool offers(struct wlr_data_source *source, const char *type) {
    char **t;

    wl_array_for_each(t, &source->mime_types) {
        if (!strcmp(*t, type))
            return true;
    }
    return false;
}

// A new selection, which wm reads unless it's wm's own.
static void on_set_selection(struct wl_listener *l, void *data) {
    struct wlr_data_source *source = server.seat->selection_source;
    int pipe_fds[2];

    (void)l, (void)data;
    stop_reading();
    if (!source || source->impl == &source_impl)
        return;
    server.read_image = !offers(source, TEXT_TYPE) && !offers(source, "text/plain") && offers(source, IMAGE_TYPE);
    if (!server.read_image && !offers(source, TEXT_TYPE) && !offers(source, "text/plain"))
        return;
    if (pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK) < 0)
        return;
    wlr_data_source_send(source, server.read_image ? IMAGE_TYPE : offers(source, TEXT_TYPE) ? TEXT_TYPE : "text/plain",
                         pipe_fds[1]);
    server.read_fd = pipe_fds[0];
    server.reading = wl_event_loop_add_fd(server.loop, pipe_fds[0], WL_EVENT_READABLE, on_readable, NULL);
}

// A drag let go of over the desktop, which no app takes, so wm reads which file it
// was, without waiting for the app, which is told only after.
static void read_drop(struct wlr_data_source *source) {
    int pipe_fds[2];

    if (!source || !offers(source, URI_TYPE) || pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK) < 0)
        return;
    stop_reading();
    wlr_data_source_send(source, URI_TYPE, pipe_fds[1]);
    server.read_drop = true;
    server.read_fd = pipe_fds[0];
    server.reading = wl_event_loop_add_fd(server.loop, pipe_fds[0], WL_EVENT_READABLE, on_readable, NULL);
}

static void on_request_selection(struct wl_listener *l, void *data) {
    struct wlr_seat_request_set_selection_event *e = data;

    (void)l;
    wlr_seat_set_selection(server.seat, e->source, e->serial);
}

static void on_request_primary(struct wl_listener *l, void *data) {
    struct wlr_seat_request_set_primary_selection_event *e = data;

    (void)l;
    wlr_seat_set_primary_selection(server.seat, e->source, e->serial);
}

// Dragging between apps: while it lasts, the pointer goes to whatever's under it.
static void on_request_drag(struct wl_listener *l, void *data) {
    struct wlr_seat_request_start_drag_event *e = data;

    (void)l;
    if (wlr_seat_validate_pointer_grab_serial(server.seat, e->origin, e->serial))
        wlr_seat_start_pointer_drag(server.seat, e->drag, e->serial);
    else
        wlr_data_source_destroy(e->drag->source);
}

static void drag_to(double x, double y) {
    int sx, sy;
    struct surface *s = wm_surface_at((int)x, (int)y, &sx, &sy);

    if (s && s->surface) {
        point_at(s, sx, sy);
        return;
    }
    wlr_seat_pointer_notify_clear_focus(server.seat);
}

// A file wm drags from the desktop, offered to apps as a URI, as their own drags are.
// With dnd_finish, wlroots frees it itself, once it's let go over nothing that takes
// it, or the app it went to is done with it.
static void drag_source_finish(struct wlr_data_source *source) {
    (void)source;
}

static const struct wlr_data_source_impl drag_source_impl = {
    .send = source_send, .destroy = source_destroy, .dnd_finish = drag_source_finish};

static char *file_uri(const char *path) {
    char *uri = malloc(strlen("file://\r\n") + 3 * strlen(path) + 1), *at = uri;

    if (!uri)
        return NULL;
    at += sprintf(at, "file://");
    for (const unsigned char *p = (const unsigned char *)path; *p; p++)
        at += isalnum(*p) || strchr("/-._~", *p) ? sprintf(at, "%c", *p) : sprintf(at, "%%%02X", *p);
    strcpy(at, "\r\n");
    return uri;
}

bool server_drag_file(const char *path) {
    struct wlr_seat_pointer_state *pointer = &server.seat->pointer_state;
    struct wlr_seat_client *client;
    struct wm_source *s;
    struct wlr_drag *drag;
    struct wlr_box box;
    struct surface *under;
    char **type;
    int sx, sy;
    bool held = false;

    if (server.seat->drag || !server.output)
        return false;
    wlr_output_layout_get_box(server.layout, server.output, &box);
    under = wm_surface_at((int)(server.cursor->x - box.x), (int)(server.cursor->y - box.y), &sx, &sy);
    // wlroots wants a client the drag came from, which the app it's over stands in for.
    if (!under || !under->surface
        || !(client = wlr_seat_client_for_wl_client(server.seat, wl_resource_get_client(under->surface->resource))))
        return false;
    if (!(s = calloc(1, sizeof(*s))) || !(s->data = file_uri(path))) {
        free(s);
        return false;
    }
    s->length = strlen(s->data);
    wlr_data_source_init(&s->base, &drag_source_impl);
    s->base.actions = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
    if ((type = wl_array_add(&s->base.mime_types, sizeof(*type))))
        *type = strdup(URI_TYPE);
    if (!(drag = wlr_drag_create(client, &s->base, NULL))) {
        wlr_data_source_destroy(&s->base);
        return false;
    }
    // The press that started it was wm's, on the desktop, so the seat's told of it, as
    // the drag ends when it's let go.
    for (size_t i = 0; i < pointer->button_count; i++)
        held |= pointer->buttons[i].button == 0x110;
    wlr_seat_pointer_notify_clear_focus(server.seat);
    if (!held)
        wlr_seat_pointer_notify_button(server.seat, now_ms(), 0x110, WL_POINTER_BUTTON_STATE_PRESSED);
    wlr_seat_start_pointer_drag(server.seat, drag, wl_display_next_serial(server.display));
    drag_to(server.cursor->x - box.x, server.cursor->y - box.y);
    return true;
}

// Cursors: wm draws its own, in the shapes it has, and apps choose among them.
static void on_request_shape(struct wl_listener *l, void *data) {
    struct wlr_cursor_shape_manager_v1_request_set_shape_event *e = data;
    struct surface *s = root_of(server.seat->pointer_state.focused_surface);
    enum gui_cursor shape = GUI_CURSOR_ARROW;

    (void)l;
    if (!s || !s->window || e->seat_client != server.seat->pointer_state.focused_client)
        return;
    switch (e->shape) {
    case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT:
    case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_VERTICAL_TEXT:
        shape = GUI_CURSOR_TEXT;
        break;
    case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE:
    case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE:
    case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE:
    case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE:
        shape = GUI_CURSOR_RESIZE_H;
        break;
    case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE:
    case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE:
    case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE:
    case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE:
        shape = GUI_CURSOR_RESIZE_V;
        break;
    case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE:
    case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE:
    case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE:
        shape = GUI_CURSOR_RESIZE_NWSE;
        break;
    case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE:
    case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE:
    case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE:
        shape = GUI_CURSOR_RESIZE_NESW;
        break;
    default:
        break;
    }
    wm_window_cursor(s->window, shape);
}

// A cursor image of the app's own: wm keeps to its own, but for the text cursor,
// an app setting none is asking for the arrow back.
static void on_request_cursor(struct wl_listener *l, void *data) {
    (void)l, (void)data;
}

// Input. The pointer is wlr_cursor's, which knows the screen's edges and accelerates
// mice as libinput does, and wm is told where it is.

// After wm's had it, the app whose window the pointer's over keeps the pointer's
// focus, even when wm didn't pass this event on, as for a move too small to be one
// of the desktop's pixels at 2x: leaving and coming back would start a double click
// over. A press wm didn't give an app, outside them all, closes their popups.
static void tell_pointer(bool pressed) {
    double width = server.screen->width, height = server.screen->height;
    struct wlr_box box;

    if (!server.output || !server.input)
        return;
    wlr_output_layout_get_box(server.layout, server.output, &box);
    if (box.width <= 0 || box.height <= 0)
        return;

    // As the screen's pixels, which wm takes as a share of MOUSE_ABSOLUTE_MAX across.
    double x = (server.cursor->x - box.x) * width / box.width, y = (server.cursor->y - box.y) * height / box.height;
    struct mouse_event e = {.buttons = server.buttons, .absolute = 1,
                            .x = (int32_t)(x * (MOUSE_ABSOLUTE_MAX + 1) / width),
                            .y = (int32_t)(y * (MOUSE_ABSOLUTE_MAX + 1) / height)};

    server.pointer_sent = false;
    server.input->mouse(&e);
    if (server.seat->drag)
        drag_to(server.cursor->x - box.x, server.cursor->y - box.y);
    else if (!server.pointer_sent && server.seat->pointer_state.focused_surface && !server.buttons) {
        int sx, sy;

        if (!wm_surface_at((int)(server.cursor->x - box.x), (int)(server.cursor->y - box.y), &sx, &sy))
            wlr_seat_pointer_notify_clear_focus(server.seat);
    }
    if (!server.pointer_sent && pressed)
        dismiss_popups(NULL);
}

static void on_motion(struct wl_listener *l, void *data) {
    struct wlr_pointer_motion_event *e = data;

    (void)l;
    wlr_cursor_move(server.cursor, &e->pointer->base, e->delta_x, e->delta_y);
    tell_pointer(false);
}

static void on_motion_absolute(struct wl_listener *l, void *data) {
    struct wlr_pointer_motion_absolute_event *e = data;

    (void)l;
    wlr_cursor_warp_absolute(server.cursor, &e->pointer->base, e->x, e->y);
    tell_pointer(false);
}

static void on_button(struct wl_listener *l, void *data) {
    struct wlr_pointer_button_event *e = data;
    uint32_t mask = e->button == 0x110 ? MOUSE_BUTTON_LEFT : e->button == 0x111 ? MOUSE_BUTTON_RIGHT
                  : e->button == 0x112 ? MOUSE_BUTTON_MIDDLE : 0;

    (void)l;
    if (server.seat->drag) { // the drag's own grab drops it, when it's let go
        struct wlr_box box;
        int sx, sy;

        wlr_output_layout_get_box(server.layout, server.output, &box);
        if (e->state == WL_POINTER_BUTTON_STATE_RELEASED && server.seat->drag->source
            && server.seat->drag->source->impl != &drag_source_impl // wm's own, which wm.c drops itself
            && !wm_surface_at((int)(server.cursor->x - box.x), (int)(server.cursor->y - box.y), &sx, &sy))
            read_drop(server.seat->drag->source);
        wlr_seat_pointer_notify_button(server.seat, e->time_msec, e->button, e->state);
        wlr_seat_pointer_notify_frame(server.seat);
    }
    if (!mask)
        return;
    server.buttons = e->state == WL_POINTER_BUTTON_STATE_PRESSED ? server.buttons | mask : server.buttons & ~mask;
    tell_pointer(e->state == WL_POINTER_BUTTON_STATE_PRESSED);
}

// Notches of the wheel, as wm counts them: towards the user is down the page and positive.
static void on_axis(struct wl_listener *l, void *data) {
    struct wlr_pointer_axis_event *e = data;
    static double rest;
    struct mouse_event m = {.buttons = server.buttons};
    struct wlr_box box;

    (void)l;
    if (e->orientation != WL_POINTER_AXIS_VERTICAL_SCROLL || !server.input || !server.output)
        return;
    if (e->delta_discrete)
        m.wheel = e->delta_discrete / 120;
    else {
        rest += e->delta / WHEEL_STEP;
        m.wheel = (int)rest;
        rest -= m.wheel;
    }
    if (!m.wheel)
        return;
    wlr_output_layout_get_box(server.layout, server.output, &box);
    m.absolute = 1;
    m.x = (int32_t)((server.cursor->x - box.x) * (MOUSE_ABSOLUTE_MAX + 1) / box.width);
    m.y = (int32_t)((server.cursor->y - box.y) * (MOUSE_ABSOLUTE_MAX + 1) / box.height);
    server.input->mouse(&m);
}

// Three fingers swiping go between workspaces, or up to the overview, as wm's keys.
static void on_swipe_begin(struct wl_listener *l, void *data) {
    struct wlr_pointer_swipe_begin_event *e = data;

    (void)l;
    server.swipe_x = server.swipe_y = 0;
    server.swipe_fingers = e->fingers;
}

static void on_swipe_update(struct wl_listener *l, void *data) {
    struct wlr_pointer_swipe_update_event *e = data;

    (void)l;
    server.swipe_x += e->dx;
    server.swipe_y += e->dy;
}

static void press(uint16_t code) {
    struct key_event e = {.code = code, .pressed = 1};

    if (!server.input)
        return;
    server.input->key(&e);
    e.pressed = 0;
    server.input->key(&e);
}

static void on_swipe_end(struct wl_listener *l, void *data) {
    struct wlr_pointer_swipe_end_event *e = data;
    double x = server.swipe_x, y = server.swipe_y;

    (void)l;
    if (e->cancelled || server.swipe_fingers < 3)
        return;
    if (y < -100 && -y > (x < 0 ? -x : x))
        press(KEY_SWIPE_UP);
    else if (x < -100 || x > 100)
        press(x < 0 ? KEY_SWIPE_LEFT : KEY_SWIPE_RIGHT);
}

struct keyboard {
    struct wl_list link;
    struct wlr_keyboard *keyboard;
    struct wl_listener key, modifiers, destroy;
};

static uint8_t modifiers_now(struct wlr_keyboard *k) {
    uint32_t held = wlr_keyboard_get_modifiers(k);

    return (held & WLR_MODIFIER_SHIFT ? KEY_MOD_SHIFT : 0) | (held & WLR_MODIFIER_CTRL ? KEY_MOD_CTRL : 0)
         | (held & WLR_MODIFIER_ALT ? KEY_MOD_ALT : 0) | (held & WLR_MODIFIER_LOGO ? KEY_MOD_SUPER : 0)
         | (held & WLR_MODIFIER_CAPS ? KEY_MOD_CAPS_LOCK : 0);
}

// Which are held after a key, as wlroots counts it only once the key's been given.
static uint8_t modifier_of(uint16_t code) {
    return code == KEY_LEFT_SHIFT || code == KEY_RIGHT_SHIFT ? KEY_MOD_SHIFT
         : code == KEY_LEFT_CTRL || code == KEY_RIGHT_CTRL   ? KEY_MOD_CTRL
         : code == KEY_LEFT_ALT || code == KEY_RIGHT_ALT     ? KEY_MOD_ALT
         : code == KEY_LEFT_SUPER || code == KEY_RIGHT_SUPER ? KEY_MOD_SUPER
                                                             : 0;
}

static uint8_t modifiers_after(struct wlr_keyboard *k, uint16_t code, bool pressed) {
    uint8_t held = modifiers_now(k), bit = modifier_of(code);

    return bit ? (pressed ? held | bit : held & ~bit) : held;
}

static bool is_modifier(uint16_t code) {
    return code == KEY_LEFT_SHIFT || code == KEY_RIGHT_SHIFT || code == KEY_LEFT_CTRL || code == KEY_RIGHT_CTRL
        || code == KEY_LEFT_ALT || code == KEY_RIGHT_ALT || code == KEY_LEFT_SUPER || code == KEY_RIGHT_SUPER
        || code == KEY_CAPS_LOCK;
}

// wm's own repeat, for what it types itself. Apps repeat for themselves.
static int on_repeat(void *data) {
    (void)data;
    if (!server.repeating.code || !server.input)
        return 0;
    if (server.keyboard)
        server.repeating.modifiers = modifiers_now(server.keyboard);
    server.repeating.text = key_event_text(server.repeating.code, server.repeating.modifiers);
    server.input->key(&server.repeating);
    wl_event_source_timer_update(server.repeat, server.repeat_rate ? 1000 / server.repeat_rate : 0);
    return 0;
}

static void on_key(struct wl_listener *l, void *data) {
    struct keyboard *k = wl_container_of(l, k, key);
    struct wlr_keyboard_key_event *e = data;
    uint16_t code = key_from_linux(e->keycode);
    struct key_event event;

    server.keyboard = k->keyboard;
    wlr_seat_set_keyboard(server.seat, k->keyboard);
    if (!code || !server.input)
        return;
    event = (struct key_event){.code = code, .pressed = e->state == WL_KEYBOARD_KEY_STATE_PRESSED,
                               .modifiers = modifiers_after(k->keyboard, code, e->state == WL_KEYBOARD_KEY_STATE_PRESSED)};
    event.text = event.pressed ? key_event_text(code, event.modifiers) : 0;
    server.input->key(&event);
    if (event.pressed && !is_modifier(code) && server.repeat_rate) {
        server.repeating = event;
        wl_event_source_timer_update(server.repeat, server.repeat_delay);
    } else if (!event.pressed && code == server.repeating.code) {
        server.repeating.code = 0;
        wl_event_source_timer_update(server.repeat, 0);
    }
}

static void on_modifiers(struct wl_listener *l, void *data) {
    struct keyboard *k = wl_container_of(l, k, modifiers);

    (void)data;
    wlr_seat_set_keyboard(server.seat, k->keyboard);
    wlr_seat_keyboard_notify_modifiers(server.seat, &k->keyboard->modifiers);
}

static void on_keyboard_destroy(struct wl_listener *l, void *data) {
    struct keyboard *k = wl_container_of(l, k, destroy);

    (void)data;
    if (server.keyboard == k->keyboard)
        server.keyboard = NULL;
    wl_list_remove(&k->key.link);
    wl_list_remove(&k->modifiers.link);
    wl_list_remove(&k->destroy.link);
    wl_list_remove(&k->link);
    free(k);
}

static void add_keyboard(struct wlr_input_device *device) {
    struct keyboard *k = calloc(1, sizeof(*k));
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = context ? xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;

    if (!k || !keymap) {
        free(k);
        xkb_context_unref(context);
        return;
    }
    k->keyboard = wlr_keyboard_from_input_device(device);
    wlr_keyboard_set_keymap(k->keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    wlr_keyboard_set_repeat_info(k->keyboard, server.repeat_rate, server.repeat_delay);
    k->key.notify = on_key;
    wl_signal_add(&k->keyboard->events.key, &k->key);
    k->modifiers.notify = on_modifiers;
    wl_signal_add(&k->keyboard->events.modifiers, &k->modifiers);
    k->destroy.notify = on_keyboard_destroy;
    wl_signal_add(&device->events.destroy, &k->destroy);
    wl_list_insert(&server.keyboards, &k->link);
    wlr_seat_set_keyboard(server.seat, k->keyboard);
    server.keyboard = k->keyboard;
}

// Closing the lid is a key to wm, pressed as it closes and let go as it opens.
struct lid {
    struct wl_listener toggle, destroy;
};

static void on_lid(struct wl_listener *l, void *data) {
    struct wlr_switch_toggle_event *e = data;
    struct key_event key = {.code = KEY_LID, .pressed = e->switch_state == WLR_SWITCH_STATE_ON};

    (void)l;
    if (e->switch_type == WLR_SWITCH_TYPE_LID && server.input)
        server.input->key(&key);
}

static void on_lid_destroy(struct wl_listener *l, void *data) {
    struct lid *lid = wl_container_of(l, lid, destroy);

    (void)data;
    wl_list_remove(&lid->toggle.link);
    wl_list_remove(&lid->destroy.link);
    free(lid);
}

// Touchpads tap to click, two fingers right-clicking, and scroll the way the page moves.
static void set_up_touchpad(struct wlr_input_device *device) {
    struct libinput_device *d = wlr_input_device_is_libinput(device) ? wlr_libinput_get_device_handle(device) : NULL;

    if (!d || !libinput_device_config_tap_get_finger_count(d))
        return;
    libinput_device_config_tap_set_enabled(d, LIBINPUT_CONFIG_TAP_ENABLED);
    libinput_device_config_tap_set_button_map(d, LIBINPUT_CONFIG_TAP_MAP_LRM);
    if (libinput_device_config_scroll_has_natural_scroll(d))
        libinput_device_config_scroll_set_natural_scroll_enabled(d, true);
}

static void on_new_input(struct wl_listener *l, void *data) {
    struct wlr_input_device *device = data;

    (void)l;
    if (device->type == WLR_INPUT_DEVICE_KEYBOARD)
        add_keyboard(device);
    else if (device->type == WLR_INPUT_DEVICE_POINTER || device->type == WLR_INPUT_DEVICE_TOUCH
             || device->type == WLR_INPUT_DEVICE_TABLET) {
        set_up_touchpad(device);
        wlr_cursor_attach_input_device(server.cursor, device);
    } else if (device->type == WLR_INPUT_DEVICE_SWITCH) {
        struct lid *lid = calloc(1, sizeof(*lid));

        if (lid) {
            lid->toggle.notify = on_lid;
            wl_signal_add(&wlr_switch_from_input_device(device)->events.toggle, &lid->toggle);
            lid->destroy.notify = on_lid_destroy;
            wl_signal_add(&device->events.destroy, &lid->destroy);
        }
    }
    wlr_seat_set_capabilities(server.seat, WL_SEAT_CAPABILITY_POINTER
                                               | (wl_list_empty(&server.keyboards) ? 0 : WL_SEAT_CAPABILITY_KEYBOARD));
}

// The display wm.c uses.

static void watch(struct wl_signal *signal, struct wl_listener *listener, wl_notify_func_t notify) {
    listener->notify = notify;
    wl_signal_add(signal, listener);
}

static bool open_server(struct screen *screen) {
    static struct wl_listener new_surface_listener;
    const char *socket;

    wlr_log_init(WLR_DEBUG, wlroots_said);
    server.screen = screen;
    server.scale = 1;
    server.read_fd = -1;
    server.repeat_delay = KEYBOARD_REPEAT_DELAY_DEFAULT;
    server.repeat_rate = KEYBOARD_REPEAT_RATE_DEFAULT;
    pixman_region32_init(&server.changed);
    wl_list_init(&server.surfaces);
    wl_list_init(&server.shells);
    wl_list_init(&server.keyboards);
    server.display = wl_display_create();
    server.loop = wl_display_get_event_loop(server.display);
    if (!(server.backend = wlr_backend_autocreate(server.loop, &server.session))) {
        fprintf(stderr, "wm: no screen to show it on, nor a seat to take one from\n");
        return false;
    }
    if (!(server.renderer = wlr_renderer_autocreate(server.backend))
        || !(server.allocator = wlr_allocator_autocreate(server.backend, server.renderer))) {
        fprintf(stderr, "wm: can't draw on the screen\n");
        return false;
    }
    wlr_shm_create(server.display, 1, (uint32_t[]){DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888}, 2);
    server.compositor = wlr_compositor_create(server.display, 6, NULL); // apps' pixels are read, not made textures
    watch(&server.compositor->events.new_surface, &new_surface_listener, on_new_surface);
    wlr_subcompositor_create(server.display);
    wlr_data_device_manager_create(server.display);
    wlr_primary_selection_v1_device_manager_create(server.display);
    wlr_viewporter_create(server.display);
    server.layout = wlr_output_layout_create(server.display);
    wlr_xdg_output_manager_v1_create(server.display, server.layout);
    watch(&server.backend->events.new_output, &server.new_output, on_new_output);

    server.xdg_shell = wlr_xdg_shell_create(server.display, 6);
    watch(&server.xdg_shell->events.new_toplevel, &server.new_toplevel, on_new_toplevel);
    watch(&server.xdg_shell->events.new_popup, &server.new_popup, on_new_popup);
    watch(&wlr_xdg_decoration_manager_v1_create(server.display)->events.new_toplevel_decoration,
           &server.new_decoration, on_new_decoration);
    wlr_server_decoration_manager_set_default_mode(wlr_server_decoration_manager_create(server.display),
                                                   WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
    server.shell = wl_global_create(server.display, &aximos_shell_v1_interface, 1, NULL, bind_shell);

    server.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server.cursor, server.layout);
    watch(&server.cursor->events.motion, &server.motion, on_motion);
    watch(&server.cursor->events.motion_absolute, &server.motion_absolute, on_motion_absolute);
    watch(&server.cursor->events.button, &server.button, on_button);
    watch(&server.cursor->events.axis, &server.axis, on_axis);
    watch(&server.cursor->events.swipe_begin, &server.swipe_begin, on_swipe_begin);
    watch(&server.cursor->events.swipe_update, &server.swipe_update, on_swipe_update);
    watch(&server.cursor->events.swipe_end, &server.swipe_end, on_swipe_end);
    watch(&server.backend->events.new_input, &server.new_input, on_new_input);
    server.seat = wlr_seat_create(server.display, "seat0");
    watch(&server.seat->events.request_set_cursor, &server.request_cursor, on_request_cursor);
    watch(&server.seat->events.request_set_selection, &server.request_selection, on_request_selection);
    watch(&server.seat->events.request_set_primary_selection, &server.request_primary, on_request_primary);
    watch(&server.seat->events.request_start_drag, &server.request_drag, on_request_drag);
    watch(&server.seat->events.set_selection, &server.set_selection, on_set_selection);
    watch(&wlr_cursor_shape_manager_v1_create(server.display, 1)->events.request_set_shape, &server.request_shape,
           on_request_shape);
    server.repeat = wl_event_loop_add_timer(server.loop, on_repeat, NULL);

    if (!(socket = wl_display_add_socket_auto(server.display))) {
        fprintf(stderr, "wm: can't make a socket for apps\n");
        return false;
    }
    if (!wlr_backend_start(server.backend)) {
        fprintf(stderr, "wm: can't start the screen and input\n");
        return false;
    }
    setenv("WAYLAND_DISPLAY", socket, 1);
    unsetenv("DISPLAY");
    if ((server.xwayland = wlr_xwayland_create(server.display, server.compositor, true))) {
        watch(&server.xwayland->events.ready, &server.xwayland_ready, on_xwayland_ready);
        watch(&server.xwayland->events.new_surface, &server.new_xsurface, on_new_xsurface);
        setenv("DISPLAY", server.xwayland->display_name, 1);
        printf("wm: X apps connect to %s\n", server.xwayland->display_name);
    }
    printf("wm: apps connect to %s\n", socket);

    // Starting the backend has found the screens. Wait a little for one, as some
    // only appear once the session's been taken.
    for (int i = 0; i < 100 && !server.output; i++)
        wl_event_loop_dispatch(server.loop, 20);
    if (!server.output) {
        fprintf(stderr, "wm: no screen to show it on\n");
        return false;
    }
    return new_pixels();
}

static bool set_mode(struct screen *screen, int width, int height) {
    struct wlr_output_mode *mode, *best = NULL;
    struct wlr_output_state state;
    bool ok;

    (void)screen;
    if (!server.output)
        return false;
    wl_list_for_each(mode, &server.output->modes, link) {
        if (mode->width == width && mode->height == height && (!best || mode->refresh > best->refresh))
            best = mode;
    }
    if (!best)
        return false;
    wlr_output_state_init(&state);
    wlr_output_state_set_mode(&state, best);
    ok = wlr_output_commit_state(server.output, &state);
    wlr_output_state_finish(&state);
    return ok && new_pixels();
}

// Apps hear of it, to draw at it, and wm's windows are copied again at it meanwhile.
static void set_scale(int scale) {
    struct wlr_output_state state;
    struct subsurface *sub;
    struct surface *s;

    if (scale == server.scale || !server.output)
        return;
    server.scale = scale;
    wlr_output_state_init(&state);
    wlr_output_state_set_scale(&state, scale);
    wlr_output_commit_state(server.output, &state);
    wlr_output_state_finish(&state);
    wl_list_for_each(sub, &all_surfaces, link) wlr_surface_set_preferred_buffer_scale(sub->surface, scale);
    wl_list_for_each(s, &server.surfaces, link) {
        if (s->window)
            gather(s, NULL);
    }
}

static void changed(int x, int y, int width, int height) {
    pixman_region32_union_rect(&server.changed, &server.changed, x, y, width, height);
}

static void show(void) {
    server.wanted = true;
    place_x_windows();
    show_frame();
}

static int poll_fds(struct pollfd *fds) {
    wl_display_flush_clients(server.display);
    fds[0] = (struct pollfd){.fd = wl_event_loop_get_fd(server.loop), .events = POLLIN};
    return 1;
}

// Everything that's waiting, then a check that the screen's the size it was.
static bool read_events(const struct pollfd *fds, struct screen *screen, const struct input *input) {
    int width = screen->width, height = screen->height;

    (void)fds;
    server.input = input;
    wl_event_loop_dispatch(server.loop, 0);
    wl_display_flush_clients(server.display);
    if (server.output && (server.output->width != width || server.output->height != height))
        return new_pixels();
    return false;
}

static void set_repeat(int delay, int rate) {
    struct keyboard *k;

    server.repeat_delay = delay > 0 ? delay : KEYBOARD_REPEAT_DELAY_DEFAULT;
    server.repeat_rate = rate;
    wl_list_for_each(k, &server.keyboards, link) wlr_keyboard_set_repeat_info(k->keyboard, rate, server.repeat_delay);
}

// The other consoles, Ctrl+Alt+F1 and on, where there's a session to change.
static void switch_session(int number) {
    if (server.session)
        wlr_session_change_vt(server.session, number);
}

const struct display wm_server = {
    .open = open_server, .set_mode = set_mode, .changed = changed, .show = show, .poll_fds = poll_fds,
    .read = read_events, .set_repeat = set_repeat, .set_scale = set_scale, .max_wait = -1,
    .switch_session = switch_session,
};
