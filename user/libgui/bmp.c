#include "image.h"
#include <string.h>

// BMP, uncompressed or with bit fields: palettes of 1, 4 or 8 bits, and 16, 24
// and 32 bit colours. Rows go bottom up, unless the height is negative.

static uint32_t le32(const uint8_t *p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const uint8_t *p) {
    return p[0] | p[1] << 8;
}

// A field of a pixel, picked out by its mask and scaled to 8 bits.
static int field(uint32_t pixel, uint32_t mask) {
    int shift = 0, bits = 0;

    if (!mask)
        return -1;
    while (!(mask >> shift & 1))
        shift++;
    while (shift + bits < 32 && mask >> (shift + bits) & 1)
        bits++;

    uint32_t value = (pixel & mask) >> shift;

    return bits >= 8 ? (int)(value >> (bits - 8)) : (int)(value * 255 / ((1u << bits) - 1));
}

bool bmp_decode(const uint8_t *data, size_t size, struct gui_image *image) {
    if (size < 26 || data[0] != 'B' || data[1] != 'M')
        return image_fail("not a BMP");

    uint32_t offset = le32(data + 10), header = le32(data + 14);
    bool core = header == 12; // the oldest kind, with 16 bit sizes and 3 byte palette entries

    if (header < 12 || 14 + (size_t)header > size || (!core && header < 40))
        return image_fail("a broken BMP");

    long width = core ? le16(data + 18) : (int32_t)le32(data + 18);
    long height = core ? le16(data + 20) : (int32_t)le32(data + 22);
    int bpp = le16(data + (core ? 24 : 28));
    uint32_t compression = core ? 0 : le32(data + 30), colours = core ? 0 : le32(data + 46);
    uint32_t masks[4] = {0x7C00, 0x03E0, 0x001F, 0}; // 16 bit, unless it says otherwise
    bool top_down = height < 0;

    height = top_down ? -height : height;
    if (bpp == 32)
        masks[0] = 0xFF0000, masks[1] = 0xFF00, masks[2] = 0xFF, masks[3] = 0;
    if (compression == 3 || compression == 6) {
        // The masks are in the header when it's big enough, or just after it.
        const uint8_t *m = data + 14 + (header >= 52 ? 40 : header);

        if (m + 12 > data + size)
            return image_fail("a broken BMP");
        for (int i = 0; i < (header >= 56 || compression == 6 ? 4 : 3); i++)
            masks[i] = le32(m + 4 * i);
    } else if (compression != 0) {
        return image_fail("a compressed BMP, which isn't supported");
    } else if (bpp == 32 && header >= 56) {
        masks[3] = le32(data + 14 + 52);
    }
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)
        return image_fail("a kind of BMP that isn't supported");

    // A palette follows the header, for 8 bits and fewer.
    uint32_t palette[256] = {0};
    int entry = core ? 3 : 4;
    size_t palette_at = 14 + header + (compression == 3 && header == 40 ? 12 : 0);

    colours = colours && colours <= 256 ? colours : bpp <= 8 ? 1u << bpp : 0;
    for (uint32_t i = 0; bpp <= 8 && i < colours && palette_at + (i + 1) * entry <= size; i++) {
        const uint8_t *c = data + palette_at + i * entry;

        palette[i] = 0xFF000000 | c[2] << 16 | c[1] << 8 | c[0];
    }

    size_t stride = ((size_t)width * bpp + 31) / 32 * 4;

    if (width <= 0 || height <= 0 || offset > size || stride * height > size - offset)
        return image_fail("a broken BMP");
    if (!image_alloc(image, width, height))
        return false;

    // Alpha only counts if some pixel has some, as many leave it all zero.
    bool any_alpha = false;

    for (long y = 0; y < height; y++) {
        const uint8_t *row = data + offset + (top_down ? y : height - 1 - y) * stride;
        uint32_t *out = image->pixels + y * width;

        for (long x = 0; x < width; x++) {
            uint32_t pixel;

            if (bpp <= 8) {
                int per_byte = 8 / bpp, shift = 8 - bpp * (1 + x % per_byte);

                out[x] = palette[row[x / per_byte] >> shift & ((1 << bpp) - 1)];
                continue;
            }
            pixel = bpp == 16 ? le16(row + 2 * x) : bpp == 24 ? (uint32_t)row[3 * x] | row[3 * x + 1] << 8 | row[3 * x + 2] << 16 : le32(row + 4 * x);
            if (bpp == 24) {
                out[x] = 0xFF000000 | pixel;
                continue;
            }

            int alpha = field(pixel, masks[3]);

            any_alpha |= alpha > 0;
            out[x] = (uint32_t)(alpha < 0 ? 255 : alpha) << 24 | field(pixel, masks[0]) << 16 | field(pixel, masks[1]) << 8
                   | field(pixel, masks[2]);
        }
    }
    for (long i = 0; !any_alpha && bpp >= 16 && bpp != 24 && i < width * height; i++)
        image->pixels[i] |= 0xFF000000;
    return true;
}
