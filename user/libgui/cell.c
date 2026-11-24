#include "font.h"
#include <gui.h>

// Characters in a terminal's cells. Box-drawing and block characters are drawn
// here, filling their cells, so lines and boxes meet their neighbours' with no
// gaps, which fonts' glyphs seldom do. The rest come from the fonts.

#define LIGHT 1
#define HEAVY 2
#define DOUBLE 3

// U+2500 to U+257F, each as its arms, two bits each, up, right, down and left: none,
// light, heavy or double. With 0x100, it's a dashed line: vertical in 0x80, its
// weight in 0x30, and how many dashes in the rest. 0 is from the font.
static const uint16_t box[128] = {
    0x011, 0x022, 0x044, 0x088, 0x113, 0x123, 0x193, 0x1A3,
    0x114, 0x124, 0x194, 0x1A4, 0x014, 0x024, 0x018, 0x028,
    0x005, 0x006, 0x009, 0x00A, 0x050, 0x060, 0x090, 0x0A0,
    0x041, 0x042, 0x081, 0x082, 0x054, 0x064, 0x094, 0x058,
    0x098, 0x0A4, 0x068, 0x0A8, 0x045, 0x046, 0x085, 0x049,
    0x089, 0x086, 0x04A, 0x08A, 0x015, 0x016, 0x025, 0x026,
    0x019, 0x01A, 0x029, 0x02A, 0x051, 0x052, 0x061, 0x062,
    0x091, 0x092, 0x0A1, 0x0A2, 0x055, 0x056, 0x065, 0x066,
    0x095, 0x059, 0x099, 0x096, 0x0A5, 0x05A, 0x069, 0x0A6,
    0x06A, 0x09A, 0x0A9, 0x0AA, 0x112, 0x122, 0x192, 0x1A2,
    0x033, 0x0CC, 0x034, 0x01C, 0x03C, 0x007, 0x00D, 0x00F,
    0x070, 0x0D0, 0x0F0, 0x043, 0x0C1, 0x0C3, 0x074, 0x0DC,
    0x0FC, 0x047, 0x0CD, 0x0CF, 0x037, 0x01D, 0x03F, 0x073,
    0x0D1, 0x0F3, 0x077, 0x0DD, 0x0FF, 0x014, 0x005, 0x041,
    0x050, 0x000, 0x000, 0x000, 0x001, 0x040, 0x010, 0x004,
    0x002, 0x080, 0x020, 0x008, 0x021, 0x048, 0x012, 0x084,
};

static void line(struct gui_canvas *c, int x, int y, int width, int height, uint32_t color) {
    if (width > 0 && height > 0)
        gui_fill(c, x, y, width, height, color);
}

// An arm from the middle of the cell to its edge, as thick as its weight, two thin
// lines for double, overlapping the middle so arms meet.
static void arm(struct gui_canvas *c, int x, int y, int w, int h, int side, int weight, uint32_t color) {
    int cx = x + w / 2, cy = y + h / 2, thick = weight == HEAVY ? 2 : 1;

    for (int pass = 0; pass < (weight == DOUBLE ? 2 : 1); pass++) {
        int shift = weight == DOUBLE ? (pass ? 1 : -1) : 0;

        if (side == 0) // up
            line(c, cx + shift - thick / 2, y, thick, cy - y + thick, color);
        else if (side == 2) // down
            line(c, cx + shift - thick / 2, cy, thick, y + h - cy, color);
        else if (side == 1) // right
            line(c, cx, cy + shift - thick / 2, x + w - cx, thick, color);
        else // left
            line(c, x, cy + shift - thick / 2, cx - x + thick, thick, color);
    }
}

static bool draw_box(struct gui_canvas *c, int x, int y, int w, int h, uint32_t cp, uint32_t color) {
    uint16_t b = cp >= 0x2500 && cp < 0x2580 ? box[cp - 0x2500] : 0;

    if (!b)
        return false;
    if (b & 0x100) { // dashes, with gaps between
        bool vertical = b & 0x80;
        int thick = (b >> 4 & 3) == HEAVY ? 2 : 1, n = b & 0xF, length = vertical ? h : w;

        for (int i = 0; i < n; i++) {
            int from = i * length / n, to = (i + 1) * length / n - (length / n > 2 ? length / n / 3 : 1);

            if (vertical)
                line(c, x + w / 2 - thick / 2, y + from, thick, to - from, color);
            else
                line(c, x + from, y + h / 2 - thick / 2, to - from, thick, color);
        }
        return true;
    }
    for (int side = 0; side < 4; side++) {
        int weight = b >> (6 - 2 * side) & 3;

        if (weight)
            arm(c, x, y, w, h, side, weight, color);
    }
    return true;
}

// Blocks, U+2580 to U+259F: halves, eighths, shades and quadrants.
static bool draw_block(struct gui_canvas *c, int x, int y, int w, int h, uint32_t cp, uint32_t color) {
    static const uint8_t quadrants[10] = {4, 8, 1, 13, 9, 7, 11, 2, 6, 14}; // U+2596..: 1 top left, 2 top right, 4 bottom left, 8 bottom right

    if (cp == 0x2580)
        line(c, x, y, w, h / 2, color);
    else if (cp >= 0x2581 && cp <= 0x2588) // lower eighths, up to the whole cell
        line(c, x, y + h - h * (int)(cp - 0x2580) / 8, w, h * (int)(cp - 0x2580) / 8, color);
    else if (cp >= 0x2589 && cp <= 0x258F) // left eighths, from seven of them down to one
        line(c, x, y, w * (int)(0x2590 - cp) / 8, h, color);
    else if (cp == 0x2590)
        line(c, x + w / 2, y, w - w / 2, h, color);
    else if (cp >= 0x2591 && cp <= 0x2593) { // light, medium and dark shade
        uint32_t alpha = (cp - 0x2590) * 64;

        for (int row = 0; row < h; row++) {
            if (y + row < 0 || y + row >= c->height)
                continue;
            for (int col = 0; col < w; col++) {
                uint32_t *p = &c->pixels[(long)(y + row) * c->stride + x + col];

                if (x + col >= 0 && x + col < c->width) // keeping how see-through it is
                    *p = (*p & 0xFF000000) | gui_mix(*p, color, alpha);
            }
        }
    } else if (cp == 0x2594)
        line(c, x, y, w, h / 8, color);
    else if (cp == 0x2595)
        line(c, x + w - w / 8, y, w / 8, h, color);
    else if (cp >= 0x2596 && cp <= 0x259F) {
        uint8_t q = quadrants[cp - 0x2596];

        if (q & 1)
            line(c, x, y, w / 2, h / 2, color);
        if (q & 2)
            line(c, x + w / 2, y, w - w / 2, h / 2, color);
        if (q & 4)
            line(c, x, y + h / 2, w / 2, h - h / 2, color);
        if (q & 8)
            line(c, x + w / 2, y + h / 2, w - w / 2, h - h / 2, color);
    } else
        return false;
    return true;
}

void gui_cell_char(struct gui_canvas *canvas, int x, int y, uint32_t cp, int cells, bool bold, uint32_t color) {
    int w = GUI_CHAR_WIDTH * cells, h = GUI_CHAR_HEIGHT;

    if (cp <= ' ' || draw_box(canvas, x, y, w, h, cp, color) || draw_block(canvas, x, y, w, h, cp, color))
        return;
    if (font_cell(canvas, x, y, cp, cells, bold, color))
        return;
    if (cp < 0x7F) // no fonts: the bitmap one, which has only ASCII
        gui_mono_text(canvas, x, y, (char[]){(char)cp, '\0'}, color, 1);
    else // a box for what there's no glyph for
        gui_frame_rounded(canvas, x + 1, y + 2, w - 2, h - 4, 0, GUI_NONE, color);
}
