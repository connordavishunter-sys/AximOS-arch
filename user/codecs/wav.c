// WAV: integer samples of 8 to 32 bits, or 32-bit floats, read, and 16-bit
// written. Files with more than two channels are mixed down.
#include <audio_codec.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FORMAT_PCM 1
#define FORMAT_FLOAT 3
#define FORMAT_EXTENSIBLE 0xFFFE
#define HEADER 44
#define MAX_CHANNELS 8
#define CHUNK 1024

struct wav {
    int fd, channels, bytes, rate; // bytes in a sample
    bool floats;
    off_t data;              // where the samples start
    int64_t frames, at;
    uint8_t buffer[CHUNK * MAX_CHANNELS * 4];
};

static uint32_t le32(const uint8_t *p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const uint8_t *p) {
    return p[0] | p[1] << 8;
}

static bool read_all(int fd, void *buf, size_t len) {
    return read(fd, buf, len) == (ssize_t)len;
}

// The chunks after the RIFF header, up to and including the samples.
static bool read_chunks(struct wav *w, struct audio_info *info) {
    uint8_t head[12], chunk[8], format[40];
    bool have_format = false;

    if (!read_all(w->fd, head, 12) || memcmp(head, "RIFF", 4) || memcmp(head + 8, "WAVE", 4))
        return audio_codec_error("not a WAV file");
    while (read_all(w->fd, chunk, 8)) {
        uint32_t size = le32(chunk + 4);

        if (!memcmp(chunk, "fmt ", 4)) {
            if (size < 16 || size > sizeof(format) || !read_all(w->fd, format, size))
                return audio_codec_error("a WAV format this can't read");

            int tag = le16(format) == FORMAT_EXTENSIBLE && size >= 26 ? le16(format + 24) : le16(format);

            info->rate = le32(format + 4);
            w->channels = le16(format + 2);
            w->bytes = le16(format + 14) / 8;
            w->floats = tag == FORMAT_FLOAT;
            if ((tag != FORMAT_PCM && !w->floats) || w->bytes < 1 || w->bytes > 4 || (w->floats && w->bytes != 4)
                || w->channels < 1 || w->channels > MAX_CHANNELS)
                return audio_codec_error("a WAV format this can't read");
            have_format = true;
            lseek(w->fd, size & 1, SEEK_CUR);
        } else if (!memcmp(chunk, "data", 4)) {
            if (!have_format)
                return audio_codec_error("a WAV file with samples before their format");
            w->data = lseek(w->fd, 0, SEEK_CUR);

            off_t end = lseek(w->fd, 0, SEEK_END);

            w->frames = ((end - w->data < size ? end - w->data : size)) / (w->bytes * w->channels);
            lseek(w->fd, w->data, SEEK_SET);
            info->channels = w->channels > 2 ? 2 : w->channels;
            info->frames = w->frames;
            info->bitrate = (int)((int64_t)info->rate * w->channels * w->bytes * 8 / 1000);
            return true;
        } else {
            lseek(w->fd, size + (size & 1), SEEK_CUR);
        }
    }
    return audio_codec_error("a WAV file without samples");
}

static void *wav_open(const char *path, struct audio_info *info) {
    struct wav *w = calloc(1, sizeof(*w));

    if (!w)
        return audio_codec_error("out of memory");
    w->fd = open(path, O_RDONLY);
    if (w->fd < 0 || !read_chunks(w, info)) {
        if (w->fd < 0)
            audio_codec_error("can't open %s", path);
        if (w->fd >= 0)
            close(w->fd);
        free(w);
        return NULL;
    }
    return w;
}

static int16_t convert(const struct wav *w, const uint8_t *p) {
    union {
        uint32_t u;
        float f;
    } v;

    switch (w->bytes) {
    case 1:
        return (int16_t)((p[0] - 128) << 8);
    case 2:
        return (int16_t)le16(p);
    case 3:
        return (int16_t)(p[1] | p[2] << 8);
    }
    v.u = le32(p);
    return w->floats ? audio_from_float(v.f) : (int16_t)(v.u >> 16);
}

static long wav_read(void *state, int16_t *samples, long frames) {
    struct wav *w = state;
    long done = 0;

    while (done < frames && w->at < w->frames) {
        long n = frames - done < CHUNK ? frames - done : CHUNK;

        n = n < w->frames - w->at ? n : (long)(w->frames - w->at);

        ssize_t got = read(w->fd, w->buffer, (size_t)n * w->bytes * w->channels);

        if (got <= 0)
            break;
        n = got / (w->bytes * w->channels);

        int16_t all[CHUNK * MAX_CHANNELS];

        for (long i = 0; i < n * w->channels; i++)
            all[i] = convert(w, w->buffer + i * w->bytes);
        audio_mix_down(all, w->channels, n, samples + done * (w->channels > 2 ? 2 : w->channels));
        done += n;
        w->at += n;
    }
    return done;
}

static bool wav_seek(void *state, int64_t frame) {
    struct wav *w = state;

    w->at = frame < w->frames ? frame : w->frames;
    return lseek(w->fd, w->data + w->at * w->bytes * w->channels, SEEK_SET) >= 0;
}

static void wav_close(void *state) {
    struct wav *w = state;

    close(w->fd);
    free(w);
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

// The header, with how long it is filled in once that's known.
static bool write_header(struct wav *w, int rate) {
    uint8_t h[HEADER] = "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0";
    uint32_t bytes = (uint32_t)(w->frames * w->channels * 2);

    put32(h + 4, 36 + bytes);
    h[22] = w->channels;
    put32(h + 24, rate);
    put32(h + 28, rate * w->channels * 2);
    h[32] = w->channels * 2;
    h[34] = 16;
    memcpy(h + 36, "data", 4);
    put32(h + 40, bytes);
    return lseek(w->fd, 0, SEEK_SET) == 0 && write(w->fd, h, HEADER) == HEADER;
}

static void *wav_create(const char *path, const struct audio_info *info) {
    struct wav *w = calloc(1, sizeof(*w));

    if (!w)
        return audio_codec_error("out of memory");
    w->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    w->channels = info->channels;
    w->rate = info->rate;
    if (w->fd < 0 || !write_header(w, info->rate)) {
        if (w->fd >= 0)
            close(w->fd);
        free(w);
        return audio_codec_error("can't write %s", path);
    }
    return w;
}

static bool wav_write(void *state, const int16_t *samples, long frames) {
    struct wav *w = state;
    size_t bytes = (size_t)frames * w->channels * 2;

    w->frames += frames;
    return write(w->fd, samples, bytes) == (ssize_t)bytes; // already little-endian
}

static bool wav_finish(void *state) {
    struct wav *w = state;
    bool ok = write_header(w, w->rate);

    ok = close(w->fd) == 0 && ok;
    free(w);
    return ok ? true : audio_codec_error("couldn't finish the WAV file") != NULL;
}

const struct audio_codec audio_codec = {
    wav_open, wav_read, wav_seek, wav_close, wav_create, wav_write, wav_finish,
};
