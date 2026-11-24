#pragma once

#include <audio.h>
#include <stddef.h>

// What each codec library gives libaudio, as the symbol audio_codec. Anything it
// can't do is NULL. Failures say why through audio_codec_error.
struct audio_codec {
    void *(*open)(const char *path, struct audio_info *info);
    long (*read)(void *state, int16_t *samples, long frames);
    bool (*seek)(void *state, int64_t frame);
    void (*close)(void *state);

    void *(*create)(const char *path, const struct audio_info *info);
    bool (*write)(void *state, const int16_t *samples, long frames);
    bool (*finish)(void *state); // frees it too
};

// For codecs, in libaudio: says what went wrong, as printf would, and returns NULL.
void *audio_codec_error(const char *format, ...);

// Mixes channels down to stereo, or keeps mono: out may be in. Takes 16-bit samples.
void audio_mix_down(const int16_t *in, int channels, long frames, int16_t *out);

// Rounds and clips a float, from -1 to 1, to a 16-bit sample.
int16_t audio_from_float(float x);
