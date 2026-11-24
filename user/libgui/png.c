#include "image.h"
#include <stdlib.h>
#include <string.h>

// Deflate, as zlib wraps it, for what's in a PNG's IDAT chunks: blocks stored
// as they are, or Huffman coded with fixed or given codes.

struct inflater {
    const uint8_t *in;
    size_t in_size, in_at;
    uint32_t bits;
    int bit_count;
    uint8_t *out;
    size_t out_size, out_at;
    bool failed;
};

// Canonical Huffman codes, as how many codes there are of each length and the
// symbols in order of their codes.
struct huffman {
    uint16_t count[16];
    uint16_t symbol[288];
};

static int take_bits(struct inflater *s, int n) {
    while (s->bit_count < n) {
        if (s->in_at == s->in_size) {
            s->failed = true;
            return 0;
        }
        s->bits |= (uint32_t)s->in[s->in_at++] << s->bit_count;
        s->bit_count += 8;
    }

    int value = s->bits & ((1u << n) - 1);

    s->bits >>= n;
    s->bit_count -= n;
    return value;
}

// Codes are read a bit at a time, most significant first, until one is complete.
static int decode(struct inflater *s, const struct huffman *h) {
    int code = 0, first = 0, index = 0;

    for (int length = 1; length < 16; length++) {
        code |= take_bits(s, 1);

        int count = h->count[length];

        if (code - count < first)
            return h->symbol[index + code - first];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    s->failed = true;
    return 0;
}

static void build(struct huffman *h, const uint8_t *lengths, int n) {
    uint16_t offsets[16] = {0};

    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; i++)
        h->count[lengths[i]]++;
    h->count[0] = 0;
    for (int length = 1; length < 16; length++)
        offsets[length] = offsets[length - 1] + h->count[length - 1];
    for (int i = 0; i < n; i++) {
        if (lengths[i])
            h->symbol[offsets[lengths[i]]++] = i;
    }
}

static const uint16_t length_base[] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                       31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t length_extra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t distance_base[] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                         193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t distance_extra[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static void put_byte(struct inflater *s, uint8_t byte) {
    if (s->out_at == s->out_size)
        s->failed = true;
    else
        s->out[s->out_at++] = byte;
}

static void inflate_codes(struct inflater *s, const struct huffman *lengths, const struct huffman *distances) {
    while (!s->failed) {
        int symbol = decode(s, lengths);

        if (symbol < 256) {
            put_byte(s, symbol);
            continue;
        }
        if (symbol == 256)
            return;
        symbol -= 257;
        if (symbol >= 29 || s->failed) {
            s->failed = true;
            return;
        }

        int length = length_base[symbol] + take_bits(s, length_extra[symbol]);
        int code = decode(s, distances);

        if (code >= 30) {
            s->failed = true;
            return;
        }

        size_t distance = distance_base[code] + take_bits(s, distance_extra[code]);

        if (distance > s->out_at) {
            s->failed = true;
            return;
        }
        while (length-- > 0)
            put_byte(s, s->out[s->out_at - distance]);
    }
}

static void inflate_stored(struct inflater *s) {
    s->bits = 0;
    s->bit_count = 0; // to the next whole byte
    if (s->in_size - s->in_at < 4) {
        s->failed = true;
        return;
    }

    size_t length = s->in[s->in_at] | s->in[s->in_at + 1] << 8;

    s->in_at += 4;
    if (s->in_size - s->in_at < length || s->out_size - s->out_at < length) {
        s->failed = true;
        return;
    }
    memcpy(s->out + s->out_at, s->in + s->in_at, length);
    s->in_at += length;
    s->out_at += length;
}

static void inflate_fixed(struct inflater *s) {
    static struct huffman lengths, distances;
    static bool built;

    if (!built) {
        uint8_t l[288];

        memset(l, 8, 144);
        memset(l + 144, 9, 112);
        memset(l + 256, 7, 24);
        memset(l + 280, 8, 8);
        build(&lengths, l, 288);
        memset(l, 5, 30);
        build(&distances, l, 30);
        built = true;
    }
    inflate_codes(s, &lengths, &distances);
}

// The codes come first, coded themselves with code length codes.
static void inflate_dynamic(struct inflater *s) {
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int literals = take_bits(s, 5) + 257, distances = take_bits(s, 5) + 1, code_codes = take_bits(s, 4) + 4;
    uint8_t lengths[320] = {0};
    struct huffman codes, literal_codes, distance_codes;

    for (int i = 0; i < code_codes; i++)
        lengths[order[i]] = take_bits(s, 3);
    build(&codes, lengths, 19);
    memset(lengths, 0, 19);
    for (int i = 0; i < literals + distances && !s->failed;) {
        int symbol = decode(s, &codes), repeat = 0, value = 0;

        if (symbol < 16) {
            lengths[i++] = symbol;
            continue;
        }
        if (symbol == 16 && i > 0) {
            value = lengths[i - 1];
            repeat = 3 + take_bits(s, 2);
        } else if (symbol == 17) {
            repeat = 3 + take_bits(s, 3);
        } else if (symbol == 18) {
            repeat = 11 + take_bits(s, 7);
        } else {
            s->failed = true;
        }
        if (i + repeat > literals + distances)
            s->failed = true;
        while (!s->failed && repeat--)
            lengths[i++] = value;
    }
    build(&literal_codes, lengths, literals);
    build(&distance_codes, lengths + literals, distances);
    inflate_codes(s, &literal_codes, &distance_codes);
}

// Returns how many bytes came out, or -1 if the data is broken or too long.
static long zlib_inflate(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size) {
    struct inflater s = {.in = in, .in_size = in_size, .in_at = 2, .out = out, .out_size = out_size};
    int last;

    if (in_size < 2 || (in[0] & 0x0F) != 8 || (in[0] << 8 | in[1]) % 31 || (in[1] & 0x20))
        return -1;
    do {
        last = take_bits(&s, 1);

        int type = take_bits(&s, 2);

        if (type == 0)
            inflate_stored(&s);
        else if (type == 1)
            inflate_fixed(&s);
        else if (type == 2)
            inflate_dynamic(&s);
        else
            s.failed = true;
    } while (!last && !s.failed);
    return s.failed ? -1 : (long)s.out_at;
}

// PNG itself.

enum { GRAY = 0, RGB = 2, PALETTE = 3, GRAY_ALPHA = 4, RGBA = 6 };

struct png {
    uint32_t width, height;
    int depth, type, channels, interlaced;
    uint32_t palette[256]; // with alpha from tRNS
    bool has_key;
    uint16_t key[3]; // the gray or colour that's transparent
};

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);

    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

// Undoes each row's filter in place, given the bytes a pixel takes, at least one.
static bool unfilter(uint8_t *data, uint32_t rows, size_t row_bytes, int pixel_bytes) {
    uint8_t *previous = NULL;

    for (uint32_t y = 0; y < rows; y++) {
        uint8_t filter = data[0], *row = data + 1;

        for (size_t i = 0; i < row_bytes; i++) {
            int a = i >= (size_t)pixel_bytes ? row[i - pixel_bytes] : 0, b = previous ? previous[i] : 0;
            int c = previous && i >= (size_t)pixel_bytes ? previous[i - pixel_bytes] : 0;

            switch (filter) {
            case 0: break;
            case 1: row[i] += a; break;
            case 2: row[i] += b; break;
            case 3: row[i] += (a + b) / 2; break;
            case 4: row[i] += paeth(a, b, c); break;
            default: return false;
            }
        }
        previous = row;
        data += row_bytes + 1;
    }
    return true;
}

// Sample i of a row, as it's stored.
static int raw_sample(const struct png *p, const uint8_t *row, size_t i) {
    if (p->depth == 8)
        return row[i];
    if (p->depth == 16)
        return row[2 * i] << 8 | row[2 * i + 1];

    int per_byte = 8 / p->depth, shift = 8 - p->depth * (1 + i % per_byte);

    return row[i / per_byte] >> shift & ((1 << p->depth) - 1);
}

// Sample i scaled to 8 bits, except palette indexes.
static int sample(const struct png *p, const uint8_t *row, size_t i) {
    int value = raw_sample(p, row, i);

    return p->depth == 16 ? value >> 8 : p->depth == 8 || p->type == PALETTE ? value : value * 255 / ((1 << p->depth) - 1);
}

// Whether a pixel is the one colour tRNS says is transparent.
static bool keyed(const struct png *p, const uint8_t *row, size_t i, int channels) {
    for (int c = 0; p->has_key && c < channels; c++) {
        if (raw_sample(p, row, i + c) != p->key[c])
            return false;
    }
    return p->has_key;
}

static uint32_t pixel_of(const struct png *p, const uint8_t *row, uint32_t x) {
    size_t i = (size_t)x * p->channels;
    int a = 255, r, g, b;

    switch (p->type) {
    case PALETTE:
        return p->palette[sample(p, row, i)];
    case GRAY:
    case GRAY_ALPHA:
        r = g = b = sample(p, row, i);
        a = p->type == GRAY_ALPHA ? sample(p, row, i + 1) : keyed(p, row, i, 1) ? 0 : 255;
        break;
    default:
        r = sample(p, row, i);
        g = sample(p, row, i + 1);
        b = sample(p, row, i + 2);
        a = p->type == RGBA ? sample(p, row, i + 3) : keyed(p, row, i, 3) ? 0 : 255;
    }
    return (uint32_t)a << 24 | r << 16 | g << 8 | b;
}

static size_t row_bytes(const struct png *p, uint32_t width) {
    return ((size_t)width * p->channels * p->depth + 7) / 8;
}

// Adam7 puts the image in seven smaller ones, each every so many pixels from a start.
static const uint8_t pass_x[7] = {0, 4, 0, 2, 0, 1, 0}, pass_y[7] = {0, 0, 4, 0, 2, 0, 1};
static const uint8_t pass_dx[7] = {8, 8, 4, 4, 2, 2, 1}, pass_dy[7] = {8, 8, 8, 4, 4, 2, 2};

static uint32_t pass_size(uint32_t size, int start, int step) {
    return size > (uint32_t)start ? (size - start + step - 1) / step : 0;
}

// The decompressed size, with a filter byte for each row of each pass.
static size_t raw_size(const struct png *p) {
    size_t total = 0;

    for (int pass = 0; pass < (p->interlaced ? 7 : 1); pass++) {
        uint32_t w = p->interlaced ? pass_size(p->width, pass_x[pass], pass_dx[pass]) : p->width;
        uint32_t h = p->interlaced ? pass_size(p->height, pass_y[pass], pass_dy[pass]) : p->height;

        if (w && h)
            total += h * (row_bytes(p, w) + 1);
    }
    return total;
}

static bool place_pixels(const struct png *p, uint8_t *raw, struct gui_image *image) {
    int pixel_bytes = (p->channels * p->depth + 7) / 8;

    for (int pass = 0; pass < (p->interlaced ? 7 : 1); pass++) {
        int x0 = p->interlaced ? pass_x[pass] : 0, y0 = p->interlaced ? pass_y[pass] : 0;
        int dx = p->interlaced ? pass_dx[pass] : 1, dy = p->interlaced ? pass_dy[pass] : 1;
        uint32_t w = pass_size(p->width, x0, dx), h = pass_size(p->height, y0, dy);
        size_t bytes = row_bytes(p, w);

        if (!w || !h)
            continue;
        if (!unfilter(raw, h, bytes, pixel_bytes))
            return image_fail("a PNG row has an unknown filter");
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++)
                image->pixels[(size_t)(y0 + y * dy) * p->width + x0 + x * dx] = pixel_of(p, raw + 1 + y * (bytes + 1), x);
        }
        raw += h * (bytes + 1);
    }
    return true;
}

bool png_decode(const uint8_t *data, size_t size, struct gui_image *image) {
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    struct png p = {0};
    uint8_t *compressed = NULL, *raw = NULL;
    size_t compressed_size = 0, at = 8;
    bool ok = false;

    if (size < 8 || memcmp(data, signature, 8))
        return image_fail("not a PNG");
    for (int i = 0; i < 256; i++)
        p.palette[i] = 0xFF000000;

    // The chunks, gathering the compressed data from every IDAT.
    while (at + 12 <= size) {
        uint32_t length = be32(data + at);
        const uint8_t *type = data + at + 4, *body = data + at + 8;

        if (length > size - at - 12)
            break;
        if (!memcmp(type, "IHDR", 4) && length >= 13) {
            p.width = be32(body);
            p.height = be32(body + 4);
            p.depth = body[8];
            p.type = body[9];
            p.interlaced = body[12];
        } else if (!memcmp(type, "PLTE", 4)) {
            for (uint32_t i = 0; i < length / 3 && i < 256; i++)
                p.palette[i] = 0xFF000000 | body[3 * i] << 16 | body[3 * i + 1] << 8 | body[3 * i + 2];
        } else if (!memcmp(type, "tRNS", 4)) {
            if (p.type == PALETTE) {
                for (uint32_t i = 0; i < length && i < 256; i++)
                    p.palette[i] = (p.palette[i] & 0xFFFFFF) | (uint32_t)body[i] << 24;
            } else {
                p.has_key = true;
                for (uint32_t i = 0; i < 3 && 2 * i + 1 < length; i++)
                    p.key[i] = body[2 * i] << 8 | body[2 * i + 1];
            }
        } else if (!memcmp(type, "IDAT", 4)) {
            uint8_t *more = realloc(compressed, compressed_size + length);

            if (!more)
                goto done;
            compressed = more;
            memcpy(compressed + compressed_size, body, length);
            compressed_size += length;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        at += length + 12;
    }

    static const uint8_t channels[7] = {[GRAY] = 1, [RGB] = 3, [PALETTE] = 1, [GRAY_ALPHA] = 2, [RGBA] = 4};
    bool depth_ok = p.depth == 8 || p.depth == 16 || ((p.type == GRAY || p.type == PALETTE) && (p.depth == 1 || p.depth == 2 || p.depth == 4));

    if (p.type > RGBA || !channels[p.type] || !depth_ok || (p.type == PALETTE && p.depth == 16) || p.interlaced > 1) {
        image_fail("a kind of PNG that isn't supported");
        goto done;
    }
    p.channels = channels[p.type];
    if (!p.width || !p.height || !image_alloc(image, p.width, p.height))
        goto done;

    size_t needed = raw_size(&p);

    raw = malloc(needed);
    if (!raw || zlib_inflate(compressed, compressed_size, raw, needed) != (long)needed) {
        image_fail(raw ? "the PNG's data is broken" : "out of memory");
    } else {
        ok = place_pixels(&p, raw, image);
    }
    if (!ok)
        gui_image_free(image);
done:
    free(compressed);
    free(raw);
    return ok;
}
