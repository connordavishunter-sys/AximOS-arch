// GIF: the first frame, on a canvas the size of the logical screen, with its
// transparent colour, if it has one, see-through.
#include "image.h"
#include <stdlib.h>
#include <string.h>

#define MAX_CODES 4096

struct reader {
    const uint8_t *data, *end;
};

static int byte(struct reader *r) {
    return r->data < r->end ? *r->data++ : -1;
}

static int le16(struct reader *r) {
    int lo = byte(r), hi = byte(r);

    return lo < 0 || hi < 0 ? -1 : lo | hi << 8;
}

// Skips sub-blocks, each its length then that many bytes, up to the empty one.
static bool skip_blocks(struct reader *r) {
    for (int len; (len = byte(r)) > 0;) {
        if (r->end - r->data < len)
            return false;
        r->data += len;
    }
    return r->data <= r->end;
}

// Reads a colour table of count entries as 0xFFRRGGBB.
static bool read_palette(struct reader *r, uint32_t *palette, int count) {
    if (r->end - r->data < count * 3)
        return false;
    for (int i = 0; i < count; i++, r->data += 3)
        palette[i] = 0xFF000000u | r->data[0] << 16 | r->data[1] << 8 | r->data[2];
    return true;
}

// Decodes the LZW codes in the image's sub-blocks into width * height palette
// indices, giving how many it got.
static long lzw(struct reader *r, int min_size, uint8_t *out, long want) {
    static uint16_t prefix[MAX_CODES];
    static uint8_t suffix[MAX_CODES], stack[MAX_CODES];
    int clear = 1 << min_size, size = min_size + 1, next = clear + 2, old = -1, first = 0, bits = 0, left = 0;
    uint32_t buffer = 0;
    long got = 0;

    for (int i = 0; i < clear; i++)
        prefix[i] = 0, suffix[i] = i;
    while (got < want) {
        while (bits < size) {
            if (!left && (left = byte(r)) <= 0)
                return got;
            int b = byte(r);

            if (b < 0)
                return got;
            left--;
            buffer |= (uint32_t)b << bits;
            bits += 8;
        }

        int code = buffer & ((1 << size) - 1), in = code, top = 0;

        buffer >>= size;
        bits -= size;
        if (code == clear) {
            size = min_size + 1, next = clear + 2, old = -1;
            continue;
        }
        if (code == clear + 1)
            break;
        if (old < 0) {
            if (code >= clear)
                return got;
            out[got++] = first = code;
            old = code;
            continue;
        }
        if (code > next)
            return got;
        if (code == next) { // not in the table yet: the last string and its first byte
            stack[top++] = first;
            code = old;
        }
        while (code >= clear && top < MAX_CODES) {
            stack[top++] = suffix[code];
            code = prefix[code];
        }
        stack[top++] = first = code;
        while (top && got < want)
            out[got++] = stack[--top];
        if (next < MAX_CODES) {
            prefix[next] = old, suffix[next] = first;
            if (++next == 1 << size && size < 12)
                size++;
        }
        old = in;
    }
    skip_blocks(r);
    return got;
}

bool gif_decode(const uint8_t *data, size_t size, struct gui_image *image) {
    struct reader r = {data + 6, data + size};
    uint32_t global[256] = {0}, local[256] = {0}, *palette = global;
    int width = le16(&r), height = le16(&r), flags = byte(&r), transparent = -1;

    if (size < 13 || width <= 0 || height <= 0)
        return image_fail("not a GIF");
    r.data += 2; // the background colour and aspect ratio
    if (flags & 0x80 && !read_palette(&r, global, 2 << (flags & 7)))
        return image_fail("a GIF cut short");
    for (;;) {
        int kind = byte(&r);

        if (kind == 0x21) { // an extension: a graphic control one says which colour's see-through
            int label = byte(&r);

            if (label == 0xF9 && r.end - r.data >= 6 && r.data[0] == 4) {
                if (r.data[1] & 1)
                    transparent = r.data[4];
                r.data += 5;
            }
            if (!skip_blocks(&r))
                return image_fail("a GIF cut short");
        } else if (kind == 0x2C) {
            break;
        } else {
            return image_fail(kind == 0x3B ? "a GIF with no pictures" : "a GIF cut short");
        }
    }

    int x = le16(&r), y = le16(&r), w = le16(&r), h = le16(&r), image_flags = byte(&r);

    if (image_flags < 0 || w <= 0 || h <= 0)
        return image_fail("a GIF cut short");
    if (image_flags & 0x80) {
        if (!read_palette(&r, local, 2 << (image_flags & 7)))
            return image_fail("a GIF cut short");
        palette = local;
    } else if (!(flags & 0x80)) {
        return image_fail("a GIF without colours");
    }

    int min_size = byte(&r);
    uint8_t *indices = calloc((size_t)w * h, 1);

    if (min_size < 2 || min_size > 8 || !indices) {
        free(indices);
        return image_fail(indices ? "a GIF that's broken" : "out of memory");
    }
    lzw(&r, min_size, indices, (long)w * h);
    if (!image_alloc(image, width, height)) {
        free(indices);
        return false;
    }
    memset(image->pixels, 0, (size_t)width * height * sizeof(uint32_t));

    // Interlaced rows come every 8th from 0, every 8th from 4, every 4th from 2, then
    // every other from 1.
    static const int start[] = {0, 4, 2, 1}, step[] = {8, 8, 4, 2};
    int row = 0, pass = 0;

    for (int i = 0; i < h; i++) {
        int dest = image_flags & 0x40 ? row : i;

        if (image_flags & 0x40) {
            row += step[pass];
            while (row >= h && pass < 3)
                row = start[++pass];
        }
        if (y + dest >= height)
            continue;
        for (int j = 0; j < w && x + j < width; j++) {
            int index = indices[(long)i * w + j];

            if (index != transparent)
                image->pixels[(long)(y + dest) * width + x + j] = palette[index];
        }
    }
    free(indices);
    return true;
}
