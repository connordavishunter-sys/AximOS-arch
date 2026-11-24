#include "font.h"
#include <font8x16.h>
#include <gui.h>
#include <string.h>

// Shrinks a rectangle to what's on the canvas, returning false if nothing is.
static bool clip(const struct gui_canvas *canvas, int *x, int *y, int *width, int *height) {
    if (*x < 0) {
        *width += *x;
        *x = 0;
    }
    if (*y < 0) {
        *height += *y;
        *y = 0;
    }
    if (*width > canvas->width - *x)
        *width = canvas->width - *x;
    if (*height > canvas->height - *y)
        *height = canvas->height - *y;
    return *width > 0 && *height > 0;
}

void gui_fill(struct gui_canvas *canvas, int x, int y, int width, int height, uint32_t color) {
    if (!clip(canvas, &x, &y, &width, &height))
        return;

    for (int row = y; row < y + height; row++) {
        uint32_t *p = canvas->pixels + (long)row * canvas->stride + x;

        for (int i = 0; i < width; i++)
            p[i] = color;
    }
}

void gui_copy(struct gui_canvas *canvas, int x, int y, const struct gui_canvas *from, int from_x, int from_y,
              int width, int height) {
    // Clip to the source first, then the destination, moving the other to match.
    int sx = from_x, sy = from_y;

    if (!clip(from, &sx, &sy, &width, &height))
        return;
    x += sx - from_x;
    y += sy - from_y;

    int dx = x, dy = y;

    if (!clip(canvas, &dx, &dy, &width, &height))
        return;
    sx += dx - x;
    sy += dy - y;

    for (int row = 0; row < height; row++)
        memcpy(canvas->pixels + (long)(dy + row) * canvas->stride + dx,
               from->pixels + (long)(sy + row) * from->stride + sx, width * sizeof(uint32_t));
}

// The bitmap font: each glyph that's on the canvas, and of it only the rows and
// columns that are, pixel by pixel when it isn't scaled.
static void bitmap_text(struct gui_canvas *canvas, int x, int y, const char *text, uint32_t color, int scale) {

    int width = GUI_CHAR_WIDTH * scale, height = GUI_CHAR_HEIGHT * scale;
    int first_row = y < 0 ? -y / scale : 0;
    int last_row = (canvas->height - y + scale - 1) / scale;

    if (y >= canvas->height || y + height <= 0)
        return;
    last_row = last_row < GUI_CHAR_HEIGHT ? last_row : GUI_CHAR_HEIGHT;
    for (; *text && x < canvas->width; text++, x += width) {
        unsigned char c = (unsigned char)*text < 128 ? (unsigned char)*text : '?';

        if (x + width <= 0)
            continue;
        for (int row = first_row; row < last_row; row++) {
            unsigned char bits = font8x16[c][row];

            for (int col = 0; bits && col < GUI_CHAR_WIDTH; col++, bits <<= 1) {
                int px = x + col * scale, py = y + row * scale;

                if (!(bits & 0x80))
                    continue;
                if (scale == 1 && px >= 0 && px < canvas->width)
                    canvas->pixels[(long)py * canvas->stride + px] = color;
                else if (scale > 1)
                    gui_fill(canvas, px, py, scale, scale, color);
            }
        }
    }
}

void gui_text(struct gui_canvas *canvas, int x, int y, const char *text, uint32_t color, int scale) {
    if (!font_sans_text(canvas, x, y, text, color, scale))
        bitmap_text(canvas, x, y, text, color, scale);
}

void gui_mono_text(struct gui_canvas *canvas, int x, int y, const char *text, uint32_t color, int scale) {
    if (!font_text(canvas, x, y, text, color, scale))
        bitmap_text(canvas, x, y, text, color, scale);
}

static int advance(unsigned char c, int scale) {
    int width = font_sans_advance(c, scale);

    return width >= 0 ? width : GUI_CHAR_WIDTH * scale;
}

int gui_text_width(const char *text, int scale) {
    int width = 0;

    for (; *text; text++)
        width += advance((unsigned char)*text, scale);
    return width;
}

size_t gui_text_fit(const char *text, size_t length, int width) {
    size_t fits = 0;

    for (int used = 0; fits < length && text[fits] && (used += advance((unsigned char)text[fits], 1)) <= width; fits++)
        ;
    return fits;
}

// How much of a pixel, from 0 to 256, is inside a rounded corner: the pixel x, y
// in from the corner of a square radius across, by 16 points in it.
int gui_corner_coverage(int x, int y, int radius) {
    int inside = 0;

    for (int sy = 0; sy < 4; sy++) {
        for (int sx = 0; sx < 4; sx++) {
            int dx = radius * 8 - (x * 8 + sx * 2 + 1), dy = radius * 8 - (y * 8 + sy * 2 + 1);

            inside += dx <= 0 || dy <= 0 || dx * dx + dy * dy <= radius * radius * 64;
        }
    }
    return inside * 16;
}

// How much of a pixel in a rectangle is inside its rounded edge.
static int inside_rounded(int x, int y, int width, int height, int radius) {
    int cx = x < width - 1 - x ? x : width - 1 - x, cy = y < height - 1 - y ? y : height - 1 - y;

    if (cx < 0 || cy < 0)
        return 0;
    return cx < radius && cy < radius ? gui_corner_coverage(cx, cy, radius) : 256;
}

static int fit_radius(int radius, int width, int height) {
    radius = radius < width / 2 ? radius : width / 2;
    radius = radius < height / 2 ? radius : height / 2;
    return radius > GUI_MAX_RADIUS ? GUI_MAX_RADIUS : radius > 0 ? radius : 0;
}

void gui_frame_rounded(struct gui_canvas *canvas, int x, int y, int width, int height, int radius, uint32_t fill,
                       uint32_t line) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + width < canvas->width ? x + width : canvas->width;
    int y1 = y + height < canvas->height ? y + height : canvas->height;
    int inner = radius - 1 > 0 ? radius - 1 : 0;

    radius = fit_radius(radius, width, height);
    inner = fit_radius(inner, width - 2, height - 2);
    for (int row = y0; row < y1; row++) {
        uint32_t *p = canvas->pixels + (long)row * canvas->stride;

        for (int col = x0; col < x1; col++) {
            int px = col - x, py = row - y;
            int out = inside_rounded(px, py, width, height, radius);
            int in = line == GUI_NONE ? out : inside_rounded(px - 1, py - 1, width - 2, height - 2, inner);

            if (!out)
                continue;
            if (line == GUI_NONE)
                p[col] = gui_mix(p[col], fill, out);
            else if (fill == GUI_NONE)
                p[col] = gui_mix(p[col], line, out - in);
            else
                p[col] = gui_mix(gui_mix(p[col], line, out), fill, in * 256 / out);
        }
    }
}

void gui_fill_rounded(struct gui_canvas *canvas, int x, int y, int width, int height, int radius, uint32_t color) {
    gui_frame_rounded(canvas, x, y, width, height, radius, color, GUI_NONE);
}

// Each corner's pixels, from the corner, k being 0 to 3 for top left, top right,
// bottom left and bottom right.
static uint32_t *corner_pixel(const struct gui_canvas *c, int x, int y, int width, int height, int k, int px, int py) {
    int col = k & 1 ? x + width - 1 - px : x + px, row = k & 2 ? y + height - 1 - py : y + py;

    if (col < 0 || row < 0 || col >= c->width || row >= c->height)
        return NULL;
    return c->pixels + (long)row * c->stride + col;
}

void gui_keep_corners(const struct gui_canvas *c, int x, int y, int width, int height, int radius,
                      uint32_t kept[4][GUI_MAX_RADIUS][GUI_MAX_RADIUS]) {
    radius = fit_radius(radius, width, height);
    for (int k = 0; k < 4; k++) {
        for (int py = 0; py < radius; py++) {
            for (int px = 0; px < radius; px++) {
                uint32_t *p = corner_pixel(c, x, y, width, height, k, px, py);

                if (p)
                    kept[k][py][px] = *p;
            }
        }
    }
}

// What was kept goes back outside the rounded edge, then the line along it, then
// what was drawn inside it.
void gui_round_corners(struct gui_canvas *c, int x, int y, int width, int height, int radius,
                       uint32_t kept[4][GUI_MAX_RADIUS][GUI_MAX_RADIUS], uint32_t line) {
    radius = fit_radius(radius, width, height);
    for (int k = 0; k < 4; k++) {
        for (int py = 0; py < radius; py++) {
            for (int px = 0; px < radius; px++) {
                uint32_t *p = corner_pixel(c, x, y, width, height, k, px, py);
                int all = gui_corner_coverage(px, py, radius);
                int in = line == GUI_NONE ? all : px && py ? gui_corner_coverage(px - 1, py - 1, radius - 1) : 0;

                if (!p)
                    continue;
                *p = all ? gui_mix(kept[k][py][px], line == GUI_NONE ? *p : gui_mix(line, *p, in * 256 / all), all)
                         : kept[k][py][px];
            }
        }
    }
}
