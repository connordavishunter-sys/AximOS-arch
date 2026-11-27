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
#include <wlr/render/color.h>
#include <wlr/render/wlr_renderer.h>
#include <scenefx/render/fx_renderer/fx_renderer.h>
#include <scenefx/types/wlr_scene.h>
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

// A part of the screen wm draws, in a buffer of its own in the scene, with a blur
// behind it if it's glass. canvas is what wm draws, in its own format, at the
// display's scale, and argb what the scene's given, as each part of it changes.
struct pixels_buffer {
    struct wlr_buffer base;
    uint32_t *argb;
};

struct scene_part {
    struct wlr_scene_tree *tree;
    struct wlr_scene_blur *blur; // or NULL
    struct wlr_scene_buffer *node;
    struct pixels_buffer *buffer;
    struct gui_canvas canvas;
    int x, y, width, height, scale;
    unsigned drawn; // which drawing of it's in its buffer, of all parts', so copies know theirs is old
};
static unsigned parts_drawn;

struct surface {
    struct wl_list link;
    struct window *window; // wm's, while it's shown
    struct wlr_surface *surface;
    // In the scene, from the bottom: its shadow, a blur behind its title bar, or its
    // contents if they're see-through, its frame, its contents, and while it's moving
    // between places, a copy of it, which stands in for the rest.
    struct wlr_scene_tree *tree, *contents;
    struct wlr_scene_shadow *shadow;
    struct wlr_scene_blur *blur;
    struct wlr_scene_rect *border; // down its sides and along its bottom, a ring around its contents
    struct scene_part *frame;
    struct window_copy *moving;
    struct window_look look; // as wm last laid it out
    int contents_x, contents_y; // where wm last put its contents
    int seen_width, seen_height; // its size, as wm last heard it
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
        reposition, close, fullscreen;
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

    // The scene: the desktop, windows, what's over them, and the cursor, each a tree.
    struct screen *screen;
    struct wlr_scene *scene;
    struct wlr_scene_output *scene_output;
    struct wlr_scene_output_layout *scene_layout;
    struct wlr_scene_tree *layers[SERVER_LAYERS];
    struct wlr_color_transform *night; // or NULL
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

// A part's pixels, as a buffer the scene makes a texture from: premultiplied ARGB.
static bool pixels_access(struct wlr_buffer *buffer, uint32_t flags, void **data, uint32_t *format, size_t *stride) {
    struct pixels_buffer *b = wl_container_of(buffer, b, base);

    (void)flags;
    *data = b->argb;
    *format = DRM_FORMAT_ARGB8888;
    *stride = (size_t)buffer->width * 4;
    return true;
}

static void pixels_end(struct wlr_buffer *buffer) {
    (void)buffer;
}

static void pixels_destroy(struct wlr_buffer *buffer) {
    struct pixels_buffer *b = wl_container_of(buffer, b, base);

    free(b->argb);
    free(b);
}

static const struct wlr_buffer_impl pixels_impl = {
    .destroy = pixels_destroy, .begin_data_ptr_access = pixels_access, .end_data_ptr_access = pixels_end};

// The screen's size, which wm lays the desktop out in, and its modes. Its pixels are
// only for reading the screen back into.
static bool new_pixels(void) {
    struct screen *s = server.screen;
    int width = server.output->width, height = server.output->height;
    uint8_t *pixels = calloc((size_t)width * height, sizeof(uint32_t));

    if (!pixels)
        return false;
    free(s->pixels);
    *s = (struct screen){pixels, width, height, width * 4, !wl_list_empty(&server.output->modes)};
    return true;
}

static struct scene_part *part_in(struct wlr_scene_tree *parent) {
    struct scene_part *p = calloc(1, sizeof(*p));

    if (!p || !(p->tree = wlr_scene_tree_create(parent))) {
        free(p);
        return NULL;
    }
    p->node = wlr_scene_buffer_create(p->tree, NULL);
    wlr_scene_node_set_enabled(&p->tree->node, false);
    return p;
}

struct scene_part *server_part_new(enum server_layer layer) {
    return part_in(server.layers[layer]);
}

void server_part_free(struct scene_part *p) {
    if (!p)
        return;
    wlr_scene_node_destroy(&p->tree->node); // which lets go of its buffer
    if (p->buffer)
        wlr_buffer_drop(&p->buffer->base);
    free(p->canvas.pixels);
    free(p);
}

struct gui_canvas *server_part_place(struct scene_part *p, int x, int y, int width, int height) {
    int k = server.scale;

    if (width <= 0 || height <= 0)
        return NULL;
    wlr_scene_node_set_position(&p->tree->node, x, y);
    p->x = x, p->y = y;
    if (width != p->width || height != p->height || k != p->scale || !p->buffer) {
        struct pixels_buffer *b = calloc(1, sizeof(*b));
        uint32_t *canvas = malloc((size_t)width * height * k * k * sizeof(uint32_t));

        if (!b || !canvas || !(b->argb = calloc((size_t)width * height * k * k, sizeof(uint32_t)))) {
            free(b ? b->argb : NULL), free(b), free(canvas);
            return NULL;
        }
        for (size_t i = 0; i < (size_t)width * height * k * k; i++)
            canvas[i] = 0xFF000000; // see-through, in wm's colours
        wlr_buffer_init(&b->base, &pixels_impl, width * k, height * k);
        wlr_scene_buffer_set_buffer(p->node, &b->base);
        wlr_scene_buffer_set_dest_size(p->node, width, height);
        if (p->buffer)
            wlr_buffer_drop(&p->buffer->base);
        p->buffer = b;
        free(p->canvas.pixels);
        p->canvas = (struct gui_canvas){canvas, width, height, width * k, k};
        p->width = width, p->height = height, p->scale = k;
        if (p->blur)
            wlr_scene_blur_set_size(p->blur, width, height);
    }
    return &p->canvas;
}

void server_part_show(struct scene_part *p, bool shown) {
    wlr_scene_node_set_enabled(&p->tree->node, shown && p->buffer);
}

void server_part_raise(struct scene_part *p) {
    wlr_scene_node_raise_to_top(&p->tree->node);
}

void server_part_look(struct scene_part *p, int radius, float blur, float opacity) {
    if (blur > 0 && !p->blur) {
        p->blur = wlr_scene_blur_create(p->tree, p->width, p->height);
        wlr_scene_node_lower_to_bottom(&p->blur->node);
    }
    if (p->blur) {
        wlr_scene_node_set_enabled(&p->blur->node, blur > 0);
        wlr_scene_blur_set_alpha(p->blur, blur);
        wlr_scene_blur_set_corner_radius(p->blur, radius);
    }
    wlr_scene_buffer_set_opacity(p->node, opacity);
}

// wm's colours, whose top byte is how see-through, as the scene's: premultiplied.
void server_part_drawn(struct scene_part *p, int x, int y, int width, int height) {
    int k = p->scale, x0 = x < 0 ? 0 : x * k, y0 = y < 0 ? 0 : y * k;
    int x1 = (x + width > p->width ? p->width : x + width) * k, y1 = (y + height > p->height ? p->height : y + height) * k;
    pixman_region32_t damage;

    if (!p->buffer || x1 <= x0 || y1 <= y0)
        return;
    for (int row = y0; row < y1; row++) {
        const uint32_t *from = p->canvas.pixels + (size_t)row * p->canvas.stride;
        uint32_t *to = p->buffer->argb + (size_t)row * p->width * k;

        for (int col = x0; col < x1; col++) {
            uint32_t c = from[col], solid = 255 - (c >> 24);

            to[col] = solid == 255 ? 0xFF000000 | c
                    : !solid       ? 0
                                   : solid << 24 | ((c >> 16 & 0xFF) * solid / 255) << 16
                                         | ((c >> 8 & 0xFF) * solid / 255) << 8 | (c & 0xFF) * solid / 255;
        }
    }
    p->drawn = ++parts_drawn;
    pixman_region32_init_rect(&damage, x0, y0, x1 - x0, y1 - y0);
    wlr_scene_buffer_set_buffer_with_damage(p->node, &p->buffer->base, &damage);
    pixman_region32_fini(&damage);
}

// Each frame: the scene, drawn by the GPU where it's changed, then apps told it's
// shown, so they draw their next.
static void on_frame(struct wl_listener *l, void *data) {
    struct timespec now;

    (void)l, (void)data;
    if (!server.scene_output)
        return;
    wlr_scene_output_commit(server.scene_output,
                            &(struct wlr_scene_output_state_options){.color_transform = server.night});
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(server.scene_output, &now);
}

static void on_output_destroy(struct wl_listener *l, void *data) {
    (void)l, (void)data;
    wl_list_remove(&server.frame.link);
    wl_list_remove(&server.output_destroy.link);
    if (server.scene_output)
        wlr_scene_output_destroy(server.scene_output);
    server.scene_output = NULL;
    server.output = NULL;
}

// The first screen is wm's, at its preferred mode. Others are left off, for now.
static void on_new_output(struct wl_listener *l, void *data) {
    struct wlr_output *output = data;
    struct wlr_output_state state;
    struct wlr_output_mode *mode = wlr_output_preferred_mode(output);
    struct wlr_output_layout_output *placed;

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
    server.frame.notify = on_frame;
    wl_signal_add(&output->events.frame, &server.frame);
    server.output_destroy.notify = on_output_destroy;
    wl_signal_add(&output->events.destroy, &server.output_destroy);
    placed = wlr_output_layout_add_auto(server.layout, output);
    server.scene_output = wlr_scene_output_create(server.scene, output);
    if (placed && server.scene_output)
        wlr_scene_output_layout_add_output(server.scene_layout, placed, server.scene_output);
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
    struct wlr_texture *texture = surface->buffer ? surface->buffer->texture : NULL;
    int width = surface->current.width, height = surface->current.height, k = s->scale;
    int buffer_width, buffer_height;
    struct wlr_fbox src;
    void *from = NULL, *read = NULL;
    uint32_t format;
    size_t stride;

    if (width <= 0 || height <= 0)
        return;
    wlr_surface_get_buffer_source_box(surface, &src);
    if (src.width <= 0 || src.height <= 0)
        return;
    // Its buffer while the app's still there, or else what the GPU has, read back.
    if (buffer && wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &from, &format, &stride)) {
        buffer_width = buffer->width, buffer_height = buffer->height;
    } else if (texture && (read = malloc((size_t)texture->width * texture->height * 4))
               && wlr_texture_read_pixels(texture, &(struct wlr_texture_read_pixels_options){
                                                       .data = read, .format = DRM_FORMAT_ARGB8888,
                                                       .stride = texture->width * 4})) {
        from = read, format = DRM_FORMAT_ARGB8888, stride = (size_t)texture->width * 4, buffer = NULL;
        buffer_width = texture->width, buffer_height = texture->height;
    } else {
        free(read);
        return;
    }
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
                                                     + (size_t)(by < buffer_height ? by : buffer_height - 1) * stride);
            uint32_t *to = s->pixels + (size_t)y * s->width * k;
            int64_t bx = buffer_at(src.x, src.width, width * k, x0 * k - left * k);

            for (int x = x0 * k; x < x1 * k; x++, bx += step) {
                uint32_t p = row[(bx >> 16) < buffer_width ? (bx >> 16) : buffer_width - 1];
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
    if (buffer)
        wlr_buffer_end_data_ptr_access(buffer);
    free(read);
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

// Where its contents' surfaces go: from the top left of the part the app says is its
// window, which may leave out a shadow it draws, or all of an X window's surface.
static void place_contents(struct surface *s) {
    struct wlr_box g = geometry_of(s);

    if (s->contents)
        wlr_scene_node_set_position(&s->contents->node, s->contents_x - g.x, s->contents_y - g.y);
}

// The app drew. The scene shows it; wm hears if its size changed, to frame it, and
// that its copy of it's out of date.
static void contents_changed(struct surface *s) {
    struct wlr_box g = geometry_of(s);
    int width = g.width > 0 ? g.width : s->surface->current.width;
    int height = g.height > 0 ? g.height : s->surface->current.height;

    if (!s->window)
        return;
    place_contents(s);
    if (width > 0 && height > 0 && (width != s->seen_width || height != s->seen_height)) {
        s->seen_width = width, s->seen_height = height;
        wm_window_size(s->window, width, height);
    }
    wm_window_changed(s->window);
}

static void scene_window_new(struct surface *s) {
    if (s->tree || !(s->tree = wlr_scene_tree_create(server.layers[LAYER_WINDOWS])))
        return;
    s->shadow = wlr_scene_shadow_create(s->tree, 0, 0, 0, 0, (float[4]){0, 0, 0, 0});
    s->blur = wlr_scene_blur_create(s->tree, 0, 0);
    s->border = wlr_scene_rect_create(s->tree, 0, 0, (float[4]){0, 0, 0, 1});
    s->contents = wlr_scene_subsurface_tree_create(s->tree, s->surface);
    wlr_scene_node_set_enabled(&s->shadow->node, false);
    wlr_scene_node_set_enabled(&s->blur->node, false);
    wlr_scene_node_set_enabled(&s->tree->node, false);
}

static void scene_window_gone(struct surface *s) {
    server_part_free(s->frame);
    server_copy_free(s->moving);
    if (s->tree)
        wlr_scene_node_destroy(&s->tree->node);
    s->tree = s->contents = NULL, s->shadow = NULL, s->blur = NULL, s->frame = NULL, s->moving = NULL;
    s->border = NULL;
}

struct scene_part *server_window_frame(struct surface *s) {
    if (!s->frame && s->tree && (s->frame = part_in(s->tree)))
        wlr_scene_node_place_below(&s->frame->tree->node, &s->contents->node);
    return s->frame;
}


// Its contents' corners rounded, as its frame's are: the buffers that reach them, of
// the contents width by height, whose top left is at x, y in the contents tree.
struct corners {
    int x, y, width, height, radius;
    bool top;
};

static void round_buffer(struct wlr_scene_buffer *buffer, int sx, int sy, void *data) {
    const struct corners *c = data;
    int width = buffer->dst_width ? buffer->dst_width : buffer->buffer ? buffer->buffer->width / server.scale : 0;
    int height = buffer->dst_height ? buffer->dst_height : buffer->buffer ? buffer->buffer->height / server.scale : 0;
    int left = sx - c->x, top = sy - c->y, right = left + width, bottom = top + height;
    bool at_left = left <= 0, at_right = right >= c->width, at_top = c->top && top <= 0, at_bottom = bottom >= c->height;

    wlr_scene_buffer_set_corner_radii(buffer, (struct fx_corner_radii){
                                                  at_top && at_left ? c->radius : 0, at_top && at_right ? c->radius : 0,
                                                  at_bottom && at_right ? c->radius : 0,
                                                  at_bottom && at_left ? c->radius : 0});
}

static void round_contents(struct surface *s, const struct window_look *look) {
    struct wlr_box g = geometry_of(s);
    bool framed = look->title_height > 0;
    struct corners c = {
        .x = g.x, .y = g.y, .top = !framed,
        .width = look->frame_width - 2 * look->border,
        .height = look->frame_height - look->title_height - (framed ? look->border : 0),
        .radius = look->radius > look->border ? look->radius - look->border : 0};

    wlr_scene_node_for_each_buffer(&s->contents->node, round_buffer, &c);
}

// A window's shadow, border and blur, for its frame at f, its title bar title_height
// of it, alpha of them shown: the window's own, or its copy's.
static void lay_shadow(struct wlr_scene_shadow *shadow, const struct window_look *look, struct wlr_box f, float alpha) {
    int spread = look->shadow_size;

    // A soft shadow, a little bigger than the frame, only around it, not under it,
    // where it'd show through what's see-through.
    wlr_scene_node_set_enabled(&shadow->node, look->shadow > 0);
    if (look->shadow <= 0)
        return;
    wlr_scene_node_set_position(&shadow->node, f.x - spread, f.y - spread + look->shadow_drop);
    wlr_scene_shadow_set_size(shadow, f.width + 2 * spread, f.height + 2 * spread);
    wlr_scene_shadow_set_corner_radius(shadow, look->radius);
    wlr_scene_shadow_set_blur_sigma(shadow, spread); // the frame, blurred out to the node's edge
    wlr_scene_shadow_set_color(shadow, (float[4]){0, 0, 0, look->shadow * alpha});
    wlr_scene_shadow_set_clipped_region(shadow, (struct clipped_region){
                                                    {spread, spread - look->shadow_drop, f.width, f.height},
                                                    corner_radii_all(look->radius)});
}

// The border down its sides and along its bottom, its corners rounded as the frame's,
// with its contents' rounded rectangle cut out of it. The title bar's part has the top.
static void lay_border(struct wlr_scene_rect *border, const struct window_look *look, struct wlr_box f, int title_height,
                       float alpha) {
    int top = f.y + title_height, height = f.height - title_height, b = look->border;
    uint32_t c = look->border_color;

    wlr_scene_node_set_enabled(&border->node, b > 0 && height > b && f.width > 2 * b);
    if (b <= 0 || height <= b || f.width <= 2 * b)
        return;
    wlr_scene_node_set_position(&border->node, f.x, top);
    wlr_scene_rect_set_size(border, f.width, height);
    wlr_scene_rect_set_color(border, (float[4]){(c >> 16 & 0xFF) / 255.0f * alpha, (c >> 8 & 0xFF) / 255.0f * alpha,
                                                (c & 0xFF) / 255.0f * alpha, alpha});
    wlr_scene_rect_set_corner_radii(border, corner_radii_bottom(look->radius));
    wlr_scene_rect_set_clipped_region(border, (struct clipped_region){
                                                  {b, 0, f.width - 2 * b, height - b},
                                                  corner_radii_bottom(look->radius > b ? look->radius - b : 0)});
}

// A blur behind its title bar, or its contents if they're see-through.
static void lay_blur(struct wlr_scene_blur *blur, const struct window_look *look, struct wlr_box f, int title_height,
                     float alpha) {
    bool on = (look->blur_title && title_height > 0) || look->blur_contents;

    wlr_scene_node_set_enabled(&blur->node, on);
    if (!on)
        return;
    wlr_scene_node_set_position(&blur->node, f.x, f.y);
    wlr_scene_blur_set_size(blur, f.width, look->blur_contents ? f.height : title_height);
    wlr_scene_blur_set_alpha(blur, alpha);
    wlr_scene_blur_set_corner_radii(blur, (struct fx_corner_radii){look->radius, look->radius,
                                                                   look->blur_contents ? look->radius : 0,
                                                                   look->blur_contents ? look->radius : 0});
}

// A copy of a window: buffers in the scene of its own, showing the same buffers as its
// frame and contents, so the GPU draws them from the textures it has, each where it
// was in the frame, at, in the desktop's pixels, with a shadow, border and blur.
#define COPY_BUFFERS 32
struct window_copy {
    struct wlr_scene_tree *tree;
    struct wlr_scene_shadow *shadow;
    struct wlr_scene_blur *blur;
    struct wlr_scene_rect *border;
    struct window_look look; // the window's, as it was copied
    int count;
    struct {
        struct wlr_scene_buffer *node;
        struct wlr_buffer *from; // the buffer it shows, which may since have been let go of
        unsigned drawn;          // for a part's, which of its drawings
        struct wlr_box at;
    } buffers[COPY_BUFFERS];
};

static void copy_window(struct window_copy *c, struct surface *s);

void server_window_look(struct surface *s, const struct window_look *look) {
    struct wlr_box f = {look->frame_x, look->frame_y, look->frame_width, look->frame_height};

    if (!s->tree)
        return;
    s->look = *look;
    wlr_scene_node_set_enabled(&s->tree->node, look->shown);
    if (!look->shown)
        return;
    s->contents_x = look->x, s->contents_y = look->y;
    place_contents(s);
    round_contents(s, look);
    wlr_scene_node_set_enabled(&s->contents->node, !look->moving);
    if (s->frame)
        wlr_scene_node_set_enabled(&s->frame->tree->node, !look->moving && look->title_height > 0);
    if (look->moving && !s->moving && (s->moving = server_window_copy(s)))
        wlr_scene_node_reparent(&s->moving->tree->node, s->tree); // in the window's place among the others
    if (s->moving && look->moving) {
        copy_window(s->moving, s); // as it is now, as it may have drawn since
        server_copy_place(s->moving, look->moving_x, look->moving_y, look->moving_width, look->moving_height,
                          look->moving_alpha);
    }
    if (s->moving && !look->moving) { // which lets go of the buffers it showed, for the app to draw in again
        server_copy_free(s->moving);
        s->moving = NULL;
    }
    if (look->moving) {
        wlr_scene_node_set_enabled(&s->shadow->node, false);
        wlr_scene_node_set_enabled(&s->border->node, false);
        wlr_scene_node_set_enabled(&s->blur->node, false);
        return;
    }
    lay_shadow(s->shadow, look, f, 1);
    lay_border(s->border, look, f, look->title_height, 1);
    lay_blur(s->blur, look, f, look->title_height, 1);
}

// One of the window's buffers into the copy's index'th, at in its frame, unless it's
// the same buffer, as it was, already.
static void copy_buffer(struct window_copy *c, struct wlr_buffer *buffer, unsigned drawn, struct wlr_box at,
                        const struct wlr_scene_buffer *like) {
    int i = c->count;

    if (i == COPY_BUFFERS || !buffer || at.width <= 0 || at.height <= 0)
        return;
    if (!c->buffers[i].node && !(c->buffers[i].node = wlr_scene_buffer_create(c->tree, NULL)))
        return;
    struct wlr_scene_buffer *node = c->buffers[i].node;

    if (c->buffers[i].from != buffer || c->buffers[i].drawn != drawn)
        wlr_scene_buffer_set_buffer(node, buffer);
    c->buffers[i].from = buffer, c->buffers[i].drawn = drawn, c->buffers[i].at = at;
    if (like) {
        wlr_scene_buffer_set_source_box(node, &like->src_box);
        wlr_scene_buffer_set_transform(node, like->transform);
        wlr_scene_buffer_set_corner_radii(node, like->corners);
    }
    wlr_scene_node_raise_to_top(&node->node); // in the window's order
    wlr_scene_node_set_enabled(&node->node, true);
    c->count++;
}

// The buffers shown in a tree, x, y from the frame's top left, but for those hidden.
static void copy_tree(struct window_copy *c, struct wlr_scene_tree *tree, int x, int y) {
    struct wlr_scene_node *node;

    wl_list_for_each(node, &tree->children, link) {
        if (!node->enabled)
            continue;
        if (node->type == WLR_SCENE_NODE_TREE) {
            copy_tree(c, wlr_scene_tree_from_node(node), x + node->x, y + node->y);
        } else if (node->type == WLR_SCENE_NODE_BUFFER) {
            struct wlr_scene_buffer *b = wlr_scene_buffer_from_node(node);
            int width = b->dst_width ? b->dst_width : b->buffer ? b->buffer->width / server.scale : 0;
            int height = b->dst_height ? b->dst_height : b->buffer ? b->buffer->height / server.scale : 0;

            copy_buffer(c, b->buffer, 0, (struct wlr_box){x + node->x, y + node->y, width, height}, b);
        }
    }
}

// The copy, made again from the window as it is: its frame and its contents.
static void copy_window(struct window_copy *c, struct surface *s) {
    const struct window_look *look = &s->look;

    c->look = *look;
    c->count = 0;
    if (s->frame && s->frame->buffer && look->title_height > 0)
        copy_buffer(c, &s->frame->buffer->base, s->frame->drawn,
                    (struct wlr_box){s->frame->x - look->frame_x, s->frame->y - look->frame_y, s->frame->width,
                                     s->frame->height}, NULL);
    if (s->contents)
        copy_tree(c, s->contents, s->contents->node.x - look->frame_x, s->contents->node.y - look->frame_y);
    for (int i = c->count; i < COPY_BUFFERS && c->buffers[i].node; i++) {
        wlr_scene_node_destroy(&c->buffers[i].node->node);
        c->buffers[i].node = NULL, c->buffers[i].from = NULL;
    }
}

struct window_copy *server_window_copy(struct surface *s) {
    struct window_copy *c = s->tree ? calloc(1, sizeof(*c)) : NULL;

    if (!c || !(c->tree = wlr_scene_tree_create(server.layers[LAYER_OVER]))) {
        free(c);
        return NULL;
    }
    wlr_scene_node_lower_to_bottom(&c->tree->node); // under all else over the windows
    c->shadow = wlr_scene_shadow_create(c->tree, 0, 0, 0, 0, (float[4]){0, 0, 0, 0});
    c->blur = wlr_scene_blur_create(c->tree, 0, 0);
    c->border = wlr_scene_rect_create(c->tree, 0, 0, (float[4]){0, 0, 0, 1});
    copy_window(c, s);
    // Where the window is, until it's placed, as a frame can be shown first.
    server_copy_place(c, c->look.frame_x, c->look.frame_y, c->look.frame_width, c->look.frame_height, 1);
    return c;
}

void server_copy_place(struct window_copy *c, int x, int y, int width, int height, float alpha) {
    const struct window_look *look = &c->look;
    float sx = look->frame_width > 0 ? (float)width / look->frame_width : 1;
    float sy = look->frame_height > 0 ? (float)height / look->frame_height : 1;
    struct wlr_box f = {x, y, width, height};

    if (width <= 0 || height <= 0) {
        wlr_scene_node_set_enabled(&c->tree->node, false);
        return;
    }
    wlr_scene_node_set_enabled(&c->tree->node, true);
    for (int i = 0; i < c->count; i++) {
        struct wlr_box at = c->buffers[i].at;
        int left = x + (int)(at.x * sx + 0.5f), top = y + (int)(at.y * sy + 0.5f);
        int right = x + (int)((at.x + at.width) * sx + 0.5f), bottom = y + (int)((at.y + at.height) * sy + 0.5f);

        wlr_scene_node_set_position(&c->buffers[i].node->node, left, top);
        wlr_scene_buffer_set_dest_size(c->buffers[i].node, right > left ? right - left : 1, bottom > top ? bottom - top : 1);
        wlr_scene_buffer_set_opacity(c->buffers[i].node, alpha);
    }
    lay_shadow(c->shadow, look, f, alpha);
    lay_border(c->border, look, f, (int)(look->title_height * sy + 0.5f), alpha);
    lay_blur(c->blur, look, f, (int)(look->title_height * sy + 0.5f), alpha);
}

void server_copy_free(struct window_copy *c) {
    if (!c)
        return;
    wlr_scene_node_destroy(&c->tree->node); // and the buffers in it, which let go of theirs
    free(c);
}

void server_window_fullscreen(struct surface *s, bool on) {
    if (s->toplevel && s->toplevel->base->initialized)
        wlr_xdg_toplevel_set_fullscreen(s->toplevel, on);
    else if (s->xsurface)
        wlr_xwayland_surface_set_fullscreen(s->xsurface, on);
}

void server_windows_order(struct surface *const *bottom_up, int count) {
    for (int i = 0; i < count; i++) {
        if (bottom_up[i]->tree)
            wlr_scene_node_raise_to_top(&bottom_up[i]->tree->node);
    }
}

void server_night(int strength) {
    uint16_t red[256], green[256], blue[256];

    if (server.night)
        wlr_color_transform_unref(server.night);
    server.night = NULL;
    for (int i = 0; strength > 0 && i < 256; i++) {
        red[i] = i * 257;
        green[i] = (i * (256 - strength * 72 / 100) >> 8) * 257;
        blue[i] = (i * (256 - strength * 184 / 100) >> 8) * 257;
    }
    if (strength > 0)
        server.night = wlr_color_transform_init_lut_3x1d(256, red, green, blue);
    if (server.scene_output)
        wlr_damage_ring_add_whole(&server.scene_output->damage_ring);
    if (server.output)
        wlr_output_schedule_frame(server.output);
}

void server_window_read(struct surface *s) {
    gather(s, NULL);
}

// The scene, all drawn into a buffer that isn't shown, and read back. The next frame
// shown is all drawn again, as this one isn't one the scene can build on.
bool server_read_screen(struct gui_canvas *into) {
    struct wlr_output_state state;
    struct wlr_texture *texture = NULL;
    bool ok = false;

    if (!server.scene_output)
        return false;
    wlr_damage_ring_add_whole(&server.scene_output->damage_ring);
    wlr_output_state_init(&state);
    if (wlr_scene_output_build_state(server.scene_output, &state, NULL) && state.buffer
        && (texture = wlr_texture_from_buffer(server.renderer, state.buffer))) {
        int width = (int)texture->width < into->width * into->scale ? (int)texture->width : into->width * into->scale;
        int height = (int)texture->height < into->height * into->scale ? (int)texture->height : into->height * into->scale;

        ok = wlr_texture_read_pixels(texture, &(struct wlr_texture_read_pixels_options){
                                                  .data = into->pixels, .format = DRM_FORMAT_XRGB8888,
                                                  .stride = into->stride * 4,
                                                  .src_box = {0, 0, width, height}});
        for (int y = 0; ok && y < height; y++) {
            for (int x = 0; x < width; x++)
                into->pixels[(size_t)y * into->stride + x] &= 0xFFFFFF; // solid, in wm's colours
        }
        wlr_texture_destroy(texture);
    }
    wlr_output_state_finish(&state);
    wlr_damage_ring_add_whole(&server.scene_output->damage_ring);
    if (server.output)
        wlr_output_schedule_frame(server.output);
    return ok;
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
    contents_changed(s);
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
    if (s && s->surface != sub->surface && s->window)
        wm_window_changed(s->window);
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

// An app asking to cover the screen, or to stop, which wm says it does, or doesn't.
static bool wants_fullscreen(const struct surface *s) {
    return s->toplevel ? s->toplevel->requested.fullscreen : s->xsurface && s->xsurface->fullscreen;
}

static void on_fullscreen(struct wl_listener *l, void *data) {
    struct surface *s = wl_container_of(l, s, fullscreen);

    (void)data;
    if (s->window)
        wm_window_fullscreen(s->window, wants_fullscreen(s));
    else if (s->toplevel && s->toplevel->base->initialized)
        wlr_xdg_surface_schedule_configure(s->toplevel->base); // it'll be asked once it's shown
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
    s->seen_width = g.width > 0 ? g.width : s->surface->current.width;
    s->seen_height = g.height > 0 ? g.height : s->surface->current.height;
    scene_window_new(s);
    wm_window_position(s->window, &s->contents_x, &s->contents_y);
    place_contents(s);
    printf("wm: %s opened a window, %dx%d at %d %d\n", app_of(s), s->seen_width, s->seen_height, s->contents_x,
           s->contents_y);
    if (!popup && wants_fullscreen(s))
        wm_window_fullscreen(s->window, true);
}

static void on_unmap(struct wl_listener *l, void *data) {
    struct surface *s = wl_container_of(l, s, unmap);

    (void)data;
    if (s->window)
        wm_window_gone(s->window);
    s->window = NULL;
    scene_window_gone(s);
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
    scene_window_gone(s);
    if (s->surface) {
        wl_list_remove(&s->map.link);
        wl_list_remove(&s->unmap.link);
        wl_list_remove(&s->commit.link);
    }
    if (s->toplevel) {
        wl_list_remove(&s->title.link);
        wl_list_remove(&s->app_id.link);
        wl_list_remove(&s->fullscreen.link);
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
        wl_list_remove(&s->fullscreen.link);
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
    s->fullscreen.notify = on_fullscreen;
    wl_signal_add(&toplevel->events.request_fullscreen, &s->fullscreen);
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
    s->fullscreen.notify = on_fullscreen;
    wl_signal_add(&xsurface->events.request_fullscreen, &s->fullscreen);
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
    wl_list_init(&server.surfaces);
    wl_list_init(&server.shells);
    wl_list_init(&server.keyboards);
    server.display = wl_display_create();
    server.loop = wl_display_get_event_loop(server.display);
    if (!(server.backend = wlr_backend_autocreate(server.loop, &server.session))) {
        fprintf(stderr, "wm: no screen to show it on, nor a seat to take one from\n");
        return false;
    }
    // SceneFX's renderer, which draws blur, shadows and rounded corners, and the scene.
    if (!(server.renderer = fx_renderer_create(server.backend))
        || !(server.allocator = wlr_allocator_autocreate(server.backend, server.renderer))) {
        fprintf(stderr, "wm: can't draw on the screen\n");
        return false;
    }
    wlr_renderer_init_wl_display(server.renderer, server.display); // apps' buffers, shared memory or the GPU's
    server.compositor = wlr_compositor_create(server.display, 6, server.renderer);
    server.scene = wlr_scene_create();
    for (int i = 0; i < SERVER_LAYERS; i++)
        server.layers[i] = wlr_scene_tree_create(&server.scene->tree);
    watch(&server.compositor->events.new_surface, &new_surface_listener, on_new_surface);
    wlr_subcompositor_create(server.display);
    wlr_data_device_manager_create(server.display);
    wlr_primary_selection_v1_device_manager_create(server.display);
    wlr_viewporter_create(server.display);
    server.layout = wlr_output_layout_create(server.display);
    server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.layout);
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
            wm_window_changed(s->window);
    }
}

// What wm draws goes to the scene in parts, which shows it when it next draws.
static void changed(int x, int y, int width, int height) {
    (void)x, (void)y, (void)width, (void)height;
}

static void show(void) {
    place_x_windows();
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
