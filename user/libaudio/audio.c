#include <audio_codec.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#ifndef AUDIO_CODECS
#define AUDIO_CODECS "/usr/lib/aximos/codecs/"
#endif

struct audio_file {
    const struct audio_codec *codec;
    void *state;
    bool writing;
};

static char error[256];

void *audio_codec_error(const char *format, ...) {
    va_list args;

    va_start(args, format);
    vsnprintf(error, sizeof(error), format, args);
    va_end(args);
    return NULL;
}

const char *audio_error(void) {
    return error;
}

static const struct {
    const char *extension, *codec;
} kinds[] = {
    {".wav", "wav"}, {".mp3", "mp3"},  {".flac", "flac"}, {".ogg", "vorbis"},
    {".oga", "vorbis"}, {".opus", "opus"}, {".aac", "aac"},
};

// Ogg holds Opus as well as Vorbis, and its first page says which.
static bool ogg_has_opus(const char *path) {
    char start[64] = "";
    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return false;
    read(fd, start, sizeof(start));
    close(fd);
    return memmem(start, sizeof(start), "OpusHead", 8) != NULL;
}

static const char *codec_for(const char *path, bool reading) {
    const char *dot = strrchr(path, '.');

    for (size_t i = 0; dot && i < sizeof(kinds) / sizeof(*kinds); i++) {
        if (!strcasecmp(dot, kinds[i].extension))
            return !strcmp(kinds[i].codec, "vorbis") && reading && ogg_has_opus(path) ? "opus" : kinds[i].codec;
    }
    return NULL;
}

bool audio_known(const char *path) {
    return codec_for(path, false) != NULL;
}

// Codecs stay loaded once they're needed.
static const struct audio_codec *load(const char *path, bool reading) {
    const char *name = codec_for(path, reading);
    char library[64];
    void *handle;
    const struct audio_codec *codec;

    if (!name)
        return audio_codec_error("%s isn't a kind of sound file this knows", path);
    snprintf(library, sizeof(library), AUDIO_CODECS "lib%s.so", name);
    handle = dlopen(library, RTLD_NOW);
    codec = handle ? dlsym(handle, "audio_codec") : NULL;
    if (!codec)
        return audio_codec_error("can't load %s: %s", library, dlerror());
    return codec;
}

static struct audio_file *wrap(const struct audio_codec *codec, void *state, bool writing) {
    struct audio_file *file = state ? malloc(sizeof(*file)) : NULL;

    if (file)
        *file = (struct audio_file){codec, state, writing};
    else if (state)
        writing ? codec->finish(state) : codec->close(state);
    return file;
}

struct audio_file *audio_open(const char *path, struct audio_info *info) {
    const struct audio_codec *codec = load(path, true);

    if (!codec)
        return NULL;
    if (!codec->open)
        return audio_codec_error("can't read %s", path);
    *info = (struct audio_info){.frames = -1};
    return wrap(codec, codec->open(path, info), false);
}

long audio_read(struct audio_file *file, int16_t *samples, long frames) {
    return file->writing ? -1 : file->codec->read(file->state, samples, frames);
}

bool audio_seek(struct audio_file *file, int64_t frame) {
    if (file->writing || !file->codec->seek)
        return audio_codec_error("can't go to a place in this file") != NULL;
    return file->codec->seek(file->state, frame < 0 ? 0 : frame);
}

struct audio_file *audio_create(const char *path, const struct audio_info *info) {
    const struct audio_codec *codec = load(path, false);

    if (!codec)
        return NULL;
    if (!codec->create)
        return audio_codec_error("can't write %s", path);
    if (info->channels < 1 || info->channels > 2 || info->rate <= 0)
        return audio_codec_error("can only write 1 or 2 channels, at a rate above 0");
    return wrap(codec, codec->create(path, info), true);
}

bool audio_write(struct audio_file *file, const int16_t *samples, long frames) {
    return file->writing && file->codec->write(file->state, samples, frames);
}

bool audio_close(struct audio_file *file) {
    bool ok = true;

    if (!file)
        return false;
    if (file->writing)
        ok = file->codec->finish(file->state);
    else
        file->codec->close(file->state);
    free(file);
    return ok;
}

void audio_mix_down(const int16_t *in, int channels, long frames, int16_t *out) {
    for (long i = 0; i < frames; i++) {
        if (channels == 1) {
            out[i] = in[i];
            continue;
        }

        // The front pair, with the others' average added to both at 0.7.
        const int16_t *f = in + i * channels;
        int rest = 0, left, right;

        for (int c = 2; c < channels; c++)
            rest += f[c];
        rest = channels > 2 ? rest * 7 / (10 * (channels - 2)) : 0;
        left = f[0] + rest;
        right = f[1] + rest;
        out[2 * i] = left < -32768 ? -32768 : left > 32767 ? 32767 : left;
        out[2 * i + 1] = right < -32768 ? -32768 : right > 32767 ? 32767 : right;
    }
}

int16_t audio_from_float(float x) {
    long v = lrintf(x * 32768.0f);

    return v < -32768 ? -32768 : v > 32767 ? 32767 : (int16_t)v;
}

// Cubic interpolation between samples. The last three of each call are kept for
// the next, as the samples before the first of it.
struct audio_resampler {
    double step, at; // input samples an output apart, and where the next output is
    int channels;
    int16_t history[3][2];
};

struct audio_resampler *audio_resampler_new(int from, int to, int channels) {
    struct audio_resampler *r = calloc(1, sizeof(*r));

    if (r)
        *r = (struct audio_resampler){(double)from / to, 3, channels, {{0}}};
    return r;
}

// Sample n of the history and then in, one after the other.
static int sample_at(const struct audio_resampler *r, const int16_t *in, long n, int c) {
    return n < 3 ? r->history[n][c] : in[(n - 3) * r->channels + c];
}

long audio_resample(struct audio_resampler *r, const int16_t *in, long in_frames, int16_t *out, long out_frames) {
    long total = 3 + in_frames, made = 0;

    for (long n; made < out_frames && (n = (long)r->at) + 2 < total; made++, r->at += r->step) {
        double t = r->at - n;

        for (int c = 0; c < r->channels; c++) {
            double p0 = sample_at(r, in, n - 1, c), p1 = sample_at(r, in, n, c), p2 = sample_at(r, in, n + 1, c),
                   p3 = sample_at(r, in, n + 2, c);
            double v = p1 + 0.5 * t * (p2 - p0 + t * (2 * p0 - 5 * p1 + 4 * p2 - p3 + t * (3 * (p1 - p2) + p3 - p0)));

            out[made * r->channels + c] = v < -32768 ? -32768 : v > 32767 ? 32767 : (int16_t)lrint(v);
        }
    }

    int16_t keep[3][2] = {{0}};

    for (int k = 0; k < 3; k++)
        for (int c = 0; c < r->channels; c++)
            keep[k][c] = sample_at(r, in, total - 3 + k, c);
    memcpy(r->history, keep, sizeof(keep));
    r->at -= total - 3;
    return made;
}

void audio_resampler_free(struct audio_resampler *r) {
    free(r);
}
