#include "image.h"
#include <stdlib.h>
#include <string.h>

// JPEG, baseline and progressive, in grey or YCbCr with any subsampling. Every
// block's coefficients are kept until the last scan, then turned into pixels.

#define MAX_COMPONENTS 3

static const uint8_t zigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

// cos((2x + 1)u pi / 16) / 2, with u = 0 scaled by 1 / sqrt(2), by x then u.
static const float idct_table[8][8] = {
    {0.353553391f, 0.490392640f, 0.461939766f, 0.415734806f, 0.353553391f, 0.277785117f, 0.191341716f, 0.097545161f},
    {0.353553391f, 0.415734806f, 0.191341716f, -0.097545161f, -0.353553391f, -0.490392640f, -0.461939766f, -0.277785117f},
    {0.353553391f, 0.277785117f, -0.191341716f, -0.490392640f, -0.353553391f, 0.097545161f, 0.461939766f, 0.415734806f},
    {0.353553391f, 0.097545161f, -0.461939766f, -0.277785117f, 0.353553391f, 0.415734806f, -0.191341716f, -0.490392640f},
    {0.353553391f, -0.097545161f, -0.461939766f, 0.277785117f, 0.353553391f, -0.415734806f, -0.191341716f, 0.490392640f},
    {0.353553391f, -0.277785117f, -0.191341716f, 0.490392640f, -0.353553391f, -0.097545161f, 0.461939766f, -0.415734806f},
    {0.353553391f, -0.415734806f, 0.191341716f, 0.097545161f, -0.353553391f, 0.490392640f, -0.461939766f, 0.277785117f},
    {0.353553391f, -0.490392640f, 0.461939766f, -0.415734806f, 0.353553391f, -0.277785117f, 0.191341716f, -0.097545161f},
};

// A Huffman table, as the largest code of each length and where its symbols start.
struct huffman {
    uint8_t symbols[256];
    int max_code[18], offset[17];
    bool present;
};

struct component {
    int id, h, v, quant;
    int dc_table, ac_table;
    int blocks_w, blocks_h; // padded out to whole MCUs
    int16_t *coefficients;  // 64 for each block, in natural order
    int dc;                 // the last DC value, which the next is coded against
};

struct jpeg {
    const uint8_t *data;
    size_t size, at;
    uint32_t bits;
    int bit_count;
    bool failed, progressive;

    int width, height, count, h_max, v_max, mcus_x, mcus_y;
    uint16_t quant[4][64];
    struct huffman dc[4], ac[4];
    struct component components[MAX_COMPONENTS];
    int restart_interval, eob_run;
    bool adobe_rgb;

    // The scan being decoded.
    int scan_count, scan[MAX_COMPONENTS], start, end, high, low;
};

// Entropy coded data, with each 0xFF followed by a 0 that isn't data. A marker
// ends it, after which it gives zeros.
static void fill_bits(struct jpeg *j, int n) {
    while (j->bit_count < n) {
        int byte = 0;

        if (j->at < j->size && j->data[j->at] != 0xFF) {
            byte = j->data[j->at++];
        } else if (j->at + 1 < j->size && j->data[j->at + 1] == 0) {
            byte = 0xFF;
            j->at += 2;
        }
        j->bits |= (uint32_t)byte << (24 - j->bit_count);
        j->bit_count += 8;
    }
}

static int get_bits(struct jpeg *j, int n) {
    if (!n)
        return 0;
    fill_bits(j, n);

    int value = j->bits >> (32 - n);

    j->bits <<= n;
    j->bit_count -= n;
    return value;
}

// n bits as a signed value: the top bit clear means negative.
static int extend(int value, int n) {
    return n && value < 1 << (n - 1) ? value - (1 << n) + 1 : value;
}

static int decode(struct jpeg *j, const struct huffman *h) {
    int code = 0;

    for (int length = 1; length <= 16; length++) {
        code = code << 1 | get_bits(j, 1);
        if (code <= h->max_code[length])
            return h->symbols[h->offset[length] + code];
    }
    j->failed = true;
    return 0;
}

static uint16_t be16(const uint8_t *p) {
    return p[0] << 8 | p[1];
}

static void read_huffman(struct jpeg *j, const uint8_t *p, size_t length) {
    while (length >= 17 && !j->failed) {
        int class = p[0] >> 4, id = p[0] & 3, total = 0, code = 0;
        struct huffman *h = class ? &j->ac[id] : &j->dc[id];

        for (int i = 0; i < 16; i++)
            total += p[1 + i];
        if (total > 256 || length < 17 + (size_t)total) {
            j->failed = true;
            return;
        }
        memcpy(h->symbols, p + 17, total);
        for (int length_ = 1, k = 0; length_ <= 16; length_++) {
            int n = p[length_];

            h->offset[length_] = k - code;
            code += n;
            k += n;
            h->max_code[length_] = n ? code - 1 : -1;
            code <<= 1;
        }
        h->present = true;
        p += 17 + total;
        length -= 17 + total;
    }
}

static void read_quant(struct jpeg *j, const uint8_t *p, size_t length) {
    while (length >= 65) {
        int precision = p[0] >> 4, id = p[0] & 3, bytes = precision ? 2 : 1;

        if (length < 1 + 64 * (size_t)bytes)
            break;
        for (int i = 0; i < 64; i++)
            j->quant[id][zigzag[i]] = precision ? be16(p + 1 + 2 * i) : p[1 + i];
        p += 1 + 64 * bytes;
        length -= 1 + 64 * bytes;
    }
}

static bool read_frame(struct jpeg *j, const uint8_t *p, size_t length) {
    if (length < 6 || p[0] != 8)
        return image_fail("a JPEG of more than 8 bits a sample");
    j->height = be16(p + 1);
    j->width = be16(p + 3);
    j->count = p[5];
    if (j->count != 1 && j->count != 3)
        return image_fail("a JPEG in CMYK or with other components");
    if (length < 6 + 3 * (size_t)j->count || !j->width || !j->height)
        return image_fail("a broken JPEG frame");
    for (int i = 0; i < j->count; i++) {
        struct component *c = &j->components[i];

        c->id = p[6 + 3 * i];
        c->h = p[7 + 3 * i] >> 4;
        c->v = p[7 + 3 * i] & 15;
        c->quant = p[8 + 3 * i] & 3;
        if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4)
            return image_fail("a broken JPEG frame");
        j->h_max = c->h > j->h_max ? c->h : j->h_max;
        j->v_max = c->v > j->v_max ? c->v : j->v_max;
    }
    j->mcus_x = (j->width + 8 * j->h_max - 1) / (8 * j->h_max);
    j->mcus_y = (j->height + 8 * j->v_max - 1) / (8 * j->v_max);
    for (int i = 0; i < j->count; i++) {
        struct component *c = &j->components[i];

        c->blocks_w = j->mcus_x * c->h;
        c->blocks_h = j->mcus_y * c->v;
        c->coefficients = calloc((size_t)c->blocks_w * c->blocks_h, 64 * sizeof(int16_t));
        if (!c->coefficients)
            return image_fail("out of memory");
    }
    return true;
}

// Baseline, and the first scan of a progressive coefficient band: DC as a
// difference from the last block's, then runs of zeros and values.
static void decode_block(struct jpeg *j, struct component *c, int16_t *block) {
    if (j->start == 0 && (!j->progressive || j->high == 0)) {
        int bits = decode(j, &j->dc[c->dc_table]);

        c->dc += extend(get_bits(j, bits), bits);
        block[0] = (int16_t)(c->dc * (1 << j->low));
    } else if (j->start == 0) {
        if (get_bits(j, 1)) // a further bit of DC
            block[0] |= (int16_t)(1 << j->low);
    }
    if (j->progressive && j->start == 0)
        return; // DC scans have no AC

    int k = j->progressive ? j->start : 1, end = j->progressive ? j->end : 63;

    if (j->eob_run > 0) {
        j->eob_run--;
        return;
    }
    while (k <= end) {
        int rs = decode(j, &j->ac[c->ac_table]), run = rs >> 4, bits = rs & 15;

        if (!bits && run < 15) {
            // The end of the block, or with progressive, of this many blocks.
            if (j->progressive)
                j->eob_run = (1 << run) - 1 + get_bits(j, run);
            return;
        }
        k += run;
        if (bits && k <= end)
            block[zigzag[k]] = (int16_t)(extend(get_bits(j, bits), bits) * (1 << j->low));
        k++;
    }
}

// Another bit for a coefficient that's already non-zero, if the data says so.
static void refine(struct jpeg *j, int16_t *coefficient, int bit) {
    if (get_bits(j, 1) && !(*coefficient & bit))
        *coefficient += *coefficient > 0 ? bit : -bit;
}

// A later progressive scan of an AC band, a bit more of each coefficient: one bit
// for each that's already non-zero, and new ones of just that bit, each after a
// run of zeros. A run of blocks with no new ones counts this one in.
static void refine_block(struct jpeg *j, struct component *c, int16_t *block) {
    int bit = 1 << j->low, k = j->start;

    while (!j->eob_run && k <= j->end) {
        int rs = decode(j, &j->ac[c->ac_table]), run = rs >> 4, value = 0;

        if ((rs & 15) == 0 && run < 15) {
            j->eob_run = (1 << run) + get_bits(j, run);
            break;
        }
        if (rs & 15)
            value = get_bits(j, 1) ? bit : -bit;
        for (; k <= j->end; k++) {
            int16_t *coefficient = &block[zigzag[k]];

            if (*coefficient) {
                refine(j, coefficient, bit);
            } else if (run-- == 0) {
                *coefficient = (int16_t)value;
                k++;
                break;
            }
        }
    }
    if (j->eob_run) {
        for (; k <= j->end; k++) {
            if (block[zigzag[k]])
                refine(j, &block[zigzag[k]], bit);
        }
        j->eob_run--;
    }
}

static void block(struct jpeg *j, struct component *c, int bx, int by) {
    int16_t *coefficients = c->coefficients + ((size_t)by * c->blocks_w + bx) * 64;

    if (j->progressive && j->start > 0 && j->high > 0)
        refine_block(j, c, coefficients);
    else
        decode_block(j, c, coefficients);
}

// After so many MCUs comes a restart marker, and coding starts afresh.
static void restart(struct jpeg *j) {
    j->bits = 0;
    j->bit_count = 0;
    while (j->at + 1 < j->size && !(j->data[j->at] == 0xFF && j->data[j->at + 1] >= 0xD0 && j->data[j->at + 1] <= 0xD7))
        j->at++;
    j->at += 2;
    j->eob_run = 0;
    for (int i = 0; i < j->count; i++)
        j->components[i].dc = 0;
}

static void decode_scan(struct jpeg *j) {
    int done = 0;

    j->bits = 0;
    j->bit_count = 0;
    j->eob_run = 0;
    for (int i = 0; i < j->count; i++)
        j->components[i].dc = 0;

    // One component goes block by block over just its own blocks, and several go
    // MCU by MCU, each MCU having h by v blocks of each.
    if (j->scan_count == 1) {
        struct component *c = &j->components[j->scan[0]];
        int w = (j->width * c->h / j->h_max + 7) / 8, h = (j->height * c->v / j->v_max + 7) / 8;

        for (int by = 0; by < h && !j->failed; by++) {
            for (int bx = 0; bx < w && !j->failed; bx++) {
                if (j->restart_interval && done && done % j->restart_interval == 0)
                    restart(j);
                block(j, c, bx, by);
                done++;
            }
        }
        return;
    }
    for (int my = 0; my < j->mcus_y && !j->failed; my++) {
        for (int mx = 0; mx < j->mcus_x && !j->failed; mx++) {
            if (j->restart_interval && done && done % j->restart_interval == 0)
                restart(j);
            for (int i = 0; i < j->scan_count; i++) {
                struct component *c = &j->components[j->scan[i]];

                for (int y = 0; y < c->v; y++) {
                    for (int x = 0; x < c->h; x++)
                        block(j, c, mx * c->h + x, my * c->v + y);
                }
            }
            done++;
        }
    }
}

// The next marker after where entropy coded data ended.
static void skip_to_marker(struct jpeg *j) {
    while (j->at + 1 < j->size && !(j->data[j->at] == 0xFF && j->data[j->at + 1] != 0 &&
                                     !(j->data[j->at + 1] >= 0xD0 && j->data[j->at + 1] <= 0xD7)))
        j->at++;
}

static bool read_scan(struct jpeg *j, const uint8_t *p, size_t length) {
    j->scan_count = p[0];
    if (!j->count || j->scan_count < 1 || j->scan_count > j->count || length < 4 + 2 * (size_t)j->scan_count)
        return image_fail("a broken JPEG scan");
    for (int i = 0; i < j->scan_count; i++) {
        int id = p[1 + 2 * i], found = -1;

        for (int c = 0; c < j->count; c++) {
            if (j->components[c].id == id)
                found = c;
        }
        if (found < 0)
            return image_fail("a broken JPEG scan");
        j->scan[i] = found;
        j->components[found].dc_table = p[2 + 2 * i] >> 4 & 3;
        j->components[found].ac_table = p[2 + 2 * i] & 3;
    }
    p += 1 + 2 * j->scan_count;
    j->start = p[0];
    j->end = p[1];
    j->high = p[2] >> 4;
    j->low = p[2] & 15;
    if (!j->progressive) {
        j->start = 0;
        j->end = 63;
        j->high = j->low = 0;
    }
    if (j->start > j->end || j->end > 63)
        return image_fail("a broken JPEG scan");
    decode_scan(j);
    skip_to_marker(j);
    return !j->failed || image_fail("the JPEG's data is broken");
}

// Coefficients, dequantized, to 8 by 8 samples, a row at a time then a column.
static void idct(const int16_t *in, const uint16_t *quant, uint8_t *out, int stride) {
    float row[64];

    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            float sum = 0;

            for (int u = 0; u < 8; u++)
                sum += idct_table[x][u] * in[y * 8 + u] * quant[y * 8 + u];
            row[y * 8 + x] = sum;
        }
    }
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            float sum = 128.5f;

            for (int v = 0; v < 8; v++)
                sum += idct_table[y][v] * row[v * 8 + x];
            out[y * stride + x] = sum < 0 ? 0 : sum > 255 ? 255 : (uint8_t)sum;
        }
    }
}

static uint8_t clamp(int value) {
    return value < 0 ? 0 : value > 255 ? 255 : value;
}

// A component's sample at a pixel. One with fewer samples blends the four
// nearest, by where the middle of the pixel falls among their middles, as
// libjpeg does.
static int upsampled(const struct jpeg *j, const struct component *c, const uint8_t *plane, int x, int y) {
    int stride = c->blocks_w * 8, w = (j->width * c->h + j->h_max - 1) / j->h_max;
    int h = (j->height * c->v + j->v_max - 1) / j->v_max;

    if (c->h == j->h_max && c->v == j->v_max)
        return plane[(size_t)y * stride + x];

    // In 1/256ths of a sample.
    int sx = ((2 * x + 1) * c->h * 128) / j->h_max - 128, sy = ((2 * y + 1) * c->v * 128) / j->v_max - 128;
    int x0 = sx < 0 ? 0 : sx >> 8, y0 = sy < 0 ? 0 : sy >> 8, fx = sx < 0 ? 0 : sx & 255, fy = sy < 0 ? 0 : sy & 255;
    int x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
    const uint8_t *top = plane + (size_t)y0 * stride, *bottom = plane + (size_t)y1 * stride;
    int upper = top[x0] * (256 - fx) + top[x1] * fx, lower = bottom[x0] * (256 - fx) + bottom[x1] * fx;

    return (upper * (256 - fy) + lower * fy + 32768) >> 16;
}

// Each component's samples, then colours from them, with smaller components
// spread over the pixels they cover.
static bool make_pixels(struct jpeg *j, struct gui_image *image) {
    uint8_t *planes[MAX_COMPONENTS] = {0};
    bool ok = image_alloc(image, j->width, j->height);

    for (int i = 0; ok && i < j->count; i++) {
        struct component *c = &j->components[i];
        int stride = c->blocks_w * 8;

        planes[i] = malloc((size_t)stride * c->blocks_h * 8);
        ok = planes[i] || image_fail("out of memory");
        for (int by = 0; ok && by < c->blocks_h; by++) {
            for (int bx = 0; bx < c->blocks_w; bx++)
                idct(c->coefficients + ((size_t)by * c->blocks_w + bx) * 64, j->quant[c->quant],
                     planes[i] + (size_t)by * 8 * stride + bx * 8, stride);
        }
    }
    for (int y = 0; ok && y < j->height; y++) {
        for (int x = 0; x < j->width; x++) {
            int s[MAX_COMPONENTS];

            for (int i = 0; i < j->count; i++)
                s[i] = upsampled(j, &j->components[i], planes[i], x, y);

            uint32_t pixel;

            if (j->count == 1) {
                pixel = s[0] * 0x010101u;
            } else if (j->adobe_rgb) {
                pixel = s[0] << 16 | s[1] << 8 | s[2];
            } else {
                int cb = s[1] - 128, cr = s[2] - 128;

                pixel = clamp(s[0] + (91881 * cr >> 16)) << 16 | clamp(s[0] - ((22554 * cb + 46802 * cr) >> 16)) << 8
                      | clamp(s[0] + (116130 * cb >> 16));
            }
            image->pixels[(size_t)y * j->width + x] = 0xFF000000 | pixel;
        }
    }
    for (int i = 0; i < j->count; i++)
        free(planes[i]);
    if (!ok)
        gui_image_free(image);
    return ok;
}

bool jpeg_decode(const uint8_t *data, size_t size, struct gui_image *image) {
    struct jpeg j = {.data = data, .size = size, .at = 2};
    bool ok = size > 2 && data[0] == 0xFF && data[1] == 0xD8, frame = false, scanned = false;

    if (!ok)
        return image_fail("not a JPEG");

    // Markers, each with its length after it, until the end of the image.
    while (ok && j.at + 4 <= size) {
        int marker = data[j.at + 1];
        size_t length = be16(data + j.at + 2);
        const uint8_t *p = data + j.at + 4;

        if (data[j.at] != 0xFF) {
            j.at++;
            continue;
        }
        if (marker == 0xD9)
            break;
        if (length < 2 || j.at + 2 + length > size) {
            ok = image_fail("a broken JPEG");
            break;
        }
        length -= 2;
        j.at += 4 + length;
        switch (marker) {
        case 0xC0:
        case 0xC1:
        case 0xC2:
            j.progressive = marker == 0xC2;
            ok = !frame && read_frame(&j, p, length);
            frame = true;
            break;
        case 0xC4:
            read_huffman(&j, p, length);
            break;
        case 0xDB:
            read_quant(&j, p, length);
            break;
        case 0xDD:
            j.restart_interval = length >= 2 ? be16(p) : 0;
            break;
        case 0xEE: // Adobe, saying whether three components are RGB rather than YCbCr
            j.adobe_rgb = length >= 12 && !memcmp(p, "Adobe", 5) && p[11] == 0;
            break;
        case 0xDA:
            ok = frame && read_scan(&j, p, length);
            scanned = true;
            break;
        default:
            if (marker >= 0xC3 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC)
                ok = image_fail("a kind of JPEG that isn't supported");
        }
    }
    ok = ok && (scanned || image_fail("a JPEG with no image")) && make_pixels(&j, image);
    for (int i = 0; i < MAX_COMPONENTS; i++)
        free(j.components[i].coefficients);
    return ok;
}
