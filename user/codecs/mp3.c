// MP3: read with mpg123, written with LAME.
#include <audio_codec.h>
#include <fcntl.h>
#include <lame/lame.h>
#include <mpg123.h>
#include <stdlib.h>
#include <unistd.h>

#define DEFAULT_BITRATE 128

static void *mp3_open(const char *path, struct audio_info *info) {
    mpg123_handle *h = mpg123_new(NULL, NULL);
    struct mpg123_frameinfo2 frame;
    long rate;
    int channels, encoding;

    if (!h)
        return audio_codec_error("out of memory");
    mpg123_param2(h, MPG123_ADD_FLAGS, MPG123_QUIET, 0);
    if (mpg123_open(h, path) != MPG123_OK || mpg123_getformat(h, &rate, &channels, &encoding) != MPG123_OK) {
        mpg123_delete(h);
        return audio_codec_error("%s isn't an MP3 this can read", path);
    }
    // Always 16-bit, at the file's own rate, so the format can't change part way.
    mpg123_format_none(h);
    mpg123_format(h, rate, channels, MPG123_ENC_SIGNED_16);
    mpg123_scan(h); // for the length, and seeking
    info->rate = (int)rate;
    info->channels = channels > 2 ? 2 : channels;
    info->frames = mpg123_length(h);
    info->bitrate = mpg123_info2(h, &frame) == MPG123_OK ? frame.bitrate : 0;
    return h;
}

static long mp3_read(void *state, int16_t *samples, long frames) {
    mpg123_handle *h = state;
    long rate;
    int channels, encoding;
    size_t done = 0;

    mpg123_getformat(h, &rate, &channels, &encoding);
    int result = mpg123_read(h, samples, (size_t)frames * channels * sizeof(int16_t), &done);

    if (result != MPG123_OK && result != MPG123_DONE && result != MPG123_NEW_FORMAT)
        return audio_codec_error("the MP3 is damaged"), -1;
    return (long)(done / (channels * sizeof(int16_t)));
}

static bool mp3_seek(void *state, int64_t frame) {
    return mpg123_seek(state, frame, SEEK_SET) >= 0;
}

static void mp3_close(void *state) {
    mpg123_close(state);
    mpg123_delete(state);
}

struct writer {
    int fd, channels;
    lame_global_flags *lame;
    unsigned char out[16384];
};

static void *mp3_create(const char *path, const struct audio_info *info) {
    struct writer *w = calloc(1, sizeof(*w));

    if (!w)
        return audio_codec_error("out of memory");
    w->channels = info->channels;
    w->lame = lame_init();
    if (w->lame) {
        lame_set_in_samplerate(w->lame, info->rate);
        lame_set_num_channels(w->lame, info->channels);
        lame_set_brate(w->lame, info->bitrate ? info->bitrate : DEFAULT_BITRATE);
        lame_set_mode(w->lame, info->channels == 1 ? MONO : JOINT_STEREO);
    }
    if (!w->lame || lame_init_params(w->lame) < 0) {
        if (w->lame)
            lame_close(w->lame);
        free(w);
        return audio_codec_error("MP3 can't be %d samples a second at that bitrate", info->rate);
    }
    if ((w->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
        lame_close(w->lame);
        free(w);
        return audio_codec_error("can't write %s", path);
    }
    return w;
}

static bool put(struct writer *w, int length) {
    return length >= 0 && (length == 0 || write(w->fd, w->out, length) == length);
}

// LAME takes as many samples as fit what it can give back each time.
static bool mp3_write(void *state, const int16_t *samples, long frames) {
    struct writer *w = state;

    for (long i = 0; i < frames;) {
        long n = frames - i < 4096 ? frames - i : 4096;
        int length = w->channels == 1
                       ? lame_encode_buffer(w->lame, (short *)samples + i, (short *)samples + i, n, w->out, sizeof(w->out))
                       : lame_encode_buffer_interleaved(w->lame, (short *)samples + i * 2, n, w->out, sizeof(w->out));

        if (!put(w, length))
            return audio_codec_error("can't write the MP3") != NULL;
        i += n;
    }
    return true;
}

static bool mp3_finish(void *state) {
    struct writer *w = state;
    bool ok = put(w, lame_encode_flush(w->lame, w->out, sizeof(w->out)));

    ok = close(w->fd) == 0 && ok;
    lame_close(w->lame);
    free(w);
    return ok ? true : audio_codec_error("couldn't finish the MP3") != NULL;
}

const struct audio_codec audio_codec = {
    mp3_open, mp3_read, mp3_seek, mp3_close, mp3_create, mp3_write, mp3_finish,
};
