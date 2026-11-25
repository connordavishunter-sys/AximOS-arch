#include "font.h"
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb_truetype.h>

// Text drawn smoothly from TrueType fonts, 16 pixels high, times scale, as the
// bitmap font is. The monospaced one fills the bitmap font's 8 pixel wide cells, so
// what's laid out in columns stays as it is, and only a font whose characters are
// all as wide will do for it. DejaVu Sans is for everything else, each character
// as wide as it is. Without a font, the bitmap one is used.
#define MONO_FONT GUI_SHARE "/fonts/mono.ttf"
#define SANS_FONT GUI_SHARE "/fonts/DejaVuSans.ttf"
#define BOLD_FONT GUI_SHARE "/fonts/DejaVuSansMono-Bold.ttf" // for terminals' bold
#define CELL_CACHE 2048 // characters in cells kept drawn, past ASCII
#define SANS_EM 13 // pixels to its em, which makes it look the size of the monospaced one
#define MAX_FONT (4 * 1024 * 1024)
#define MAX_SCALE 8 // text's own scale, times the canvas's

struct glyph {
    uint8_t *coverage; // how much of each pixel the glyph covers, out of 255
    int width, height, x, y; // where it goes from the pen and the baseline
    int advance;             // how far the pen goes after it
    bool made;
};

struct size {
    float scale;  // from font units to pixels
    int baseline; // down from the top of the line
    struct glyph glyphs[128];
};

struct face {
    const char *path;
    bool mono;
    enum { UNTRIED, LOADED, FAILED } state;
    uint8_t *data;
    stbtt_fontinfo info;
    struct size sizes[MAX_SCALE + 1];
};

bool font_smooth = true;
static bool settings_read;
static struct face faces[] = {{.path = MONO_FONT, .mono = true}, {.path = SANS_FONT}, {.path = BOLD_FONT, .mono = true}};

static bool load(struct face *f) {
    int fd = open(f->path, O_RDONLY | O_CLOEXEC), advance, other, unused;
    struct stat st;
    bool ok = fd >= 0 && fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size <= MAX_FONT
           && (f->data = malloc(st.st_size)) && read(fd, f->data, st.st_size) == st.st_size
           && stbtt_InitFont(&f->info, f->data, stbtt_GetFontOffsetForIndex(f->data, 0));

    if (fd >= 0)
        close(fd);
    if (ok && f->mono) {
        stbtt_GetCodepointHMetrics(&f->info, 'M', &advance, &unused);
        stbtt_GetCodepointHMetrics(&f->info, 'i', &other, &unused);
        ok = advance > 0 && advance == other;
    }
    if (!ok) {
        free(f->data);
        f->data = NULL;
    }
    return ok;
}

// The face, if text is smooth and it could be loaded. The first time, the settings,
// which say whether to use them, are loaded for programs that don't load them themselves.
static struct face *face_numbered(int number, int scale) {
    struct face *f = &faces[number];

    if (!settings_read) {
        struct gui_settings settings;

        settings_read = true;
        gui_settings_load(&settings);
    }
    if (!font_smooth || scale < 1 || scale > MAX_SCALE)
        return NULL;
    if (f->state == UNTRIED)
        f->state = load(f) ? LOADED : FAILED;
    return f->state == LOADED ? f : NULL;
}

static struct face *face_for(bool mono, int scale) {
    return face_numbered(mono ? 0 : 1, scale);
}

static struct size *size_of(struct face *f, int scale) {
    struct size *s = &f->sizes[scale];
    int advance, unused, ascent, descent;

    if (s->scale)
        return s;
    stbtt_GetCodepointHMetrics(&f->info, 'M', &advance, &unused);
    stbtt_GetFontVMetrics(&f->info, &ascent, &descent, &unused);
    s->scale = f->mono ? (float)(GUI_CHAR_WIDTH * scale) / advance : stbtt_ScaleForMappingEmToPixels(&f->info, SANS_EM * scale);
    // Centred in the line's height, from the top of the tallest to the bottom of
    // the lowest.
    s->baseline = (int)((GUI_CHAR_HEIGHT * scale - (ascent - descent) * s->scale) / 2 + ascent * s->scale + 0.5f);
    return s;
}

static const struct glyph *glyph_of(struct face *f, struct size *s, unsigned char c) {
    struct glyph *g = &s->glyphs[c < 128 ? c : '?'];

    if (!g->made) {
        int advance, unused;

        g->made = true;
        stbtt_GetCodepointHMetrics(&f->info, c, &advance, &unused);
        g->advance = (int)(advance * s->scale + 0.5f);
        if (c > ' ')
            g->coverage = stbtt_GetCodepointBitmap(&f->info, s->scale, s->scale, c, &g->width, &g->height, &g->x, &g->y);
    }
    return g;
}

// Mixes color over a pixel by a out of 255, as 256ths so it's a shift. The top
// bytes, how see-through each is in a translucent window, mix too, so the edges of
// letters show as much of what's behind as the background around them.
static uint32_t blend(uint32_t under, uint32_t color, uint32_t a) {
    uint32_t keep;

    a += a >> 7;
    keep = 256 - a;
    return ((((color >> 24) * a + (under >> 24) * keep) >> 8) << 24)
         | ((((color & 0xFF00FF) * a + (under & 0xFF00FF) * keep) >> 8) & 0xFF00FF)
         | ((((color & 0x00FF00) * a + (under & 0x00FF00) * keep) >> 8) & 0x00FF00);
}

// A glyph, with its pen at x and its baseline at y, in the canvas's own pixels.
static void blit(struct gui_canvas *canvas, int x, int y, const struct glyph *g, uint32_t color) {
    int width = canvas->width * gui_scale_of(canvas), height = canvas->height * gui_scale_of(canvas);

    if (!g->coverage || x + g->x + g->width <= 0)
        return;
    for (int row = 0; row < g->height; row++) {
        int py = y + g->y + row;

        if (py < 0 || py >= height)
            continue;

        uint32_t *line = canvas->pixels + (long)py * canvas->stride;
        const uint8_t *cover = g->coverage + row * g->width;

        for (int col = 0; col < g->width; col++) {
            int px = x + g->x + col;

            if (cover[col] && px >= 0 && px < width)
                line[px] = cover[col] == 255 ? color : blend(line[px], color, cover[col]);
        }
    }
}

// Glyphs at the canvas's scale, each where it would be at 1x, so text is as wide.
static bool draw(bool mono, struct gui_canvas *canvas, int x, int y, const char *text, uint32_t color, int scale) {
    int k = gui_scale_of(canvas);
    struct face *f = face_for(mono, scale * k);

    if (!f)
        return false;

    struct size *s = size_of(f, scale * k), *laid = size_of(f, scale);

    if (y >= canvas->height || y + GUI_CHAR_HEIGHT * scale <= 0)
        return true;
    for (; *text && x < canvas->width; text++) {
        unsigned char c = (unsigned char)*text < 128 ? (unsigned char)*text : '?';

        blit(canvas, x * k, y * k + s->baseline, glyph_of(f, s, c), color);
        x += mono ? GUI_CHAR_WIDTH * scale : glyph_of(f, laid, c)->advance;
    }
    return true;
}

bool font_text(struct gui_canvas *canvas, int x, int y, const char *text, uint32_t color, int scale) {
    return draw(true, canvas, x, y, text, color, scale);
}

bool font_sans_text(struct gui_canvas *canvas, int x, int y, const char *text, uint32_t color, int scale) {
    return draw(false, canvas, x, y, text, color, scale);
}

int font_sans_advance(unsigned char c, int scale) {
    struct face *f = face_for(false, scale);

    return f ? glyph_of(f, size_of(f, scale), c < 128 ? c : '?')->advance : -1;
}

// Characters in a terminal's cells, kept once drawn, by character and the face they
// came from: the monospaced one, its bold, or DejaVu Sans for those it hasn't, shrunk
// to fit the cells, and centred in them.
static struct cell_glyph {
    uint32_t cp;
    uint8_t kind; // 1 monospaced, 2 bold, 3 and up DejaVu Sans, as many cells as it's for plus 2
    struct glyph g;
} cell_glyphs[CELL_CACHE];

bool font_cell(struct gui_canvas *canvas, int x, int y, uint32_t cp, int cells, bool bold, uint32_t color) {
    int k = gui_scale_of(canvas);
    struct face *mono = face_numbered(bold ? 2 : 0, k), *f = mono;

    if (!mono && bold)
        mono = f = face_numbered(0, k);
    if (!mono)
        return false;
    if (!stbtt_FindGlyphIndex(&f->info, (int)cp) && !((f = face_numbered(1, k)) && stbtt_FindGlyphIndex(&f->info, (int)cp)))
        return false;

    uint8_t kind = (f == mono ? (bold ? 2 : 1) : 2 + cells) + 8 * k; // at each scale its own
    struct cell_glyph *c = &cell_glyphs[(cp * 31 + kind) % CELL_CACHE];
    struct size *s = size_of(mono, k);

    if (c->cp != cp || c->kind != kind) { // made, or made again in the place of another
        float scale = f == mono ? s->scale : size_of(f, k)->scale;
        int advance, unused;

        free(c->g.coverage);
        *c = (struct cell_glyph){.cp = cp, .kind = kind};
        stbtt_GetCodepointHMetrics(&f->info, (int)cp, &advance, &unused);
        if (advance * scale > GUI_CHAR_WIDTH * cells * k)
            scale = (float)(GUI_CHAR_WIDTH * cells * k) / advance;
        c->g.advance = (int)(advance * scale + 0.5f);
        c->g.coverage = stbtt_GetCodepointBitmap(&f->info, scale, scale, (int)cp, &c->g.width, &c->g.height, &c->g.x, &c->g.y);
        c->g.made = true;
    }
    blit(canvas, x * k + (GUI_CHAR_WIDTH * cells * k - c->g.advance) / 2, y * k + s->baseline, &c->g, color);
    return true;
}
