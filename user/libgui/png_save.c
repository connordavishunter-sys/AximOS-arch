#include "image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// PNG files of RGB pixels, compressed with deflate's fixed codes and matches found
// through a table of where each three bytes were last seen, which is quick and does
// well on screens of flat colour.
#define WINDOW 32768
#define MIN_MATCH 3
#define MAX_MATCH 258
#define HASH_BITS 15

struct bits {
    uint8_t *out;
    size_t used;
    uint32_t buffer;
    int count;
};

// Least significant bit first, as deflate packs them.
static void put_bits(struct bits *b, uint32_t value, int count) {
    b->buffer |= value << b->count;
    b->count += count;
    while (b->count >= 8) {
        b->out[b->used++] = (uint8_t)b->buffer;
        b->buffer >>= 8;
        b->count -= 8;
    }
}

// Huffman codes go most significant bit first, so they're turned round.
static void put_code(struct bits *b, uint32_t code, int length) {
    uint32_t reversed = 0;

    for (int i = 0; i < length; i++)
        reversed |= (code >> i & 1) << (length - 1 - i);
    put_bits(b, reversed, length);
}

static void put_symbol(struct bits *b, int symbol) {
    if (symbol < 144)
        put_code(b, 0x30 + symbol, 8);
    else if (symbol < 256)
        put_code(b, 0x190 + symbol - 144, 9);
    else if (symbol < 280)
        put_code(b, symbol - 256, 7);
    else
        put_code(b, 0xC0 + symbol - 280, 8);
}

static const uint16_t length_base[] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                       31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t length_extra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t distance_base[] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                         193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t distance_extra[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static void put_match(struct bits *b, int length, int distance) {
    int l = 28, d = 29;

    while (length_base[l] > length)
        l--;
    while (distance_base[d] > distance)
        d--;
    put_symbol(b, 257 + l);
    put_bits(b, length - length_base[l], length_extra[l]);
    put_code(b, d, 5);
    put_bits(b, distance - distance_base[d], distance_extra[d]);
}

static uint32_t hash3(const uint8_t *p) {
    return (p[0] << 16 | p[1] << 8 | p[2]) * 2654435761u >> (32 - HASH_BITS);
}

// A zlib stream of one fixed block. out must have room for size * 9 / 8 + 16.
static size_t zlib_deflate(const uint8_t *in, size_t size, uint8_t *out) {
    struct bits b = {out, 0, 0, 0};
    int32_t *last = malloc(sizeof(int32_t) << HASH_BITS);
    uint32_t a = 1, s = 0;

    if (!last)
        return 0;
    memset(last, 0xFF, sizeof(int32_t) << HASH_BITS);
    out[b.used++] = 0x78;
    out[b.used++] = 0x01;
    put_bits(&b, 1, 1); // the last block
    put_bits(&b, 1, 2); // with the fixed codes
    for (size_t i = 0; i < size;) {
        int length = 0;

        if (i + MIN_MATCH <= size) {
            uint32_t h = hash3(in + i);
            long from = last[h];
            size_t most = size - i < MAX_MATCH ? size - i : MAX_MATCH;

            last[h] = (int32_t)i;
            if (from >= 0 && i - from <= WINDOW)
                while ((size_t)length < most && in[from + length] == in[i + length])
                    length++;
            if (length >= MIN_MATCH) {
                put_match(&b, length, (int)(i - from));
                i += length;
                continue;
            }
        }
        put_symbol(&b, in[i++]);
    }
    put_symbol(&b, 256);
    put_bits(&b, 0, 7); // to the end of the byte
    free(last);
    for (size_t i = 0; i < size; i++) {
        a = (a + in[i]) % 65521;
        s = (s + a) % 65521;
    }
    for (int i = 3; i >= 0; i--)
        out[b.used++] = (uint8_t)((s << 16 | a) >> (8 * i));
    return b.used;
}

static uint32_t crc32(uint32_t crc, const uint8_t *p, size_t n) {
    static uint32_t table[256];

    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;

            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320 ^ c >> 1 : c >> 1;
            table[i] = c;
        }
    }
    crc = ~crc;
    while (n--)
        crc = table[(crc ^ *p++) & 0xFF] ^ crc >> 8;
    return ~crc;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static bool chunk(FILE *f, const char *type, const uint8_t *data, size_t size) {
    uint8_t head[8], tail[4];

    put32(head, (uint32_t)size);
    memcpy(head + 4, type, 4);
    put32(tail, crc32(crc32(0, head + 4, 4), data, size));
    return fwrite(head, 1, 8, f) == 8 && fwrite(data, 1, size, f) == size && fwrite(tail, 1, 4, f) == 4;
}

// Every one of the canvas's own pixels, at whatever scale it is.
bool gui_image_save(const char *path, const struct gui_canvas *from) {
    int width = from->width * gui_scale_of(from), height = from->height * gui_scale_of(from);
    size_t row = 1 + (size_t)width * 3, raw_size = row * height;
    uint8_t *raw = malloc(raw_size), *packed = malloc(raw_size + raw_size / 8 + 16), header[13] = {0};
    size_t packed_size = 0;
    FILE *f = NULL;
    bool ok = false;

    if (!raw || !packed) {
        image_fail("out of memory");
        goto done;
    }
    for (int y = 0; y < height; y++) {
        uint8_t *r = raw + y * row;
        const uint32_t *p = from->pixels + (long)y * from->stride;

        *r++ = 0; // no filter
        for (int x = 0; x < width; x++, r += 3) {
            r[0] = p[x] >> 16;
            r[1] = p[x] >> 8;
            r[2] = p[x];
        }
    }
    if (!(packed_size = zlib_deflate(raw, raw_size, packed))) {
        image_fail("out of memory");
        goto done;
    }
    put32(header, width);
    put32(header + 4, height);
    header[8] = 8; // bits a sample
    header[9] = 2; // RGB
    if (!(f = fopen(path, "wb"))) {
        image_fail("can't create the file");
        goto done;
    }
    ok = fwrite("\x89PNG\r\n\x1a\n", 1, 8, f) == 8 && chunk(f, "IHDR", header, 13)
      && chunk(f, "IDAT", packed, packed_size) && chunk(f, "IEND", NULL, 0);
    ok = (fclose(f) == 0 && ok) || image_fail("couldn't write the file");
done:
    free(raw);
    free(packed);
    return ok;
}
