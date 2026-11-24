// FLAC, read and written with libFLAC.
#include <audio_codec.h>
#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>
#include <stdlib.h>
#include <string.h>

#define COMPRESSION 5
#define MAX_CHANNELS 8
#define CHUNK 4096

// The decoder hands over a frame at a time, which is kept until it's read.
struct reader {
    FLAC__StreamDecoder *decoder;
    int channels, bits, rate;
    int64_t frames;
    int16_t *frame; // in stereo or mono
    long have, used, size;
    bool failed;
};

static FLAC__StreamDecoderWriteStatus got_frame(const FLAC__StreamDecoder *d, const FLAC__Frame *frame,
                                                const FLAC__int32 *const buffers[], void *user) {
    struct reader *r = user;
    long n = frame->header.blocksize;
    int channels = frame->header.channels, out = channels > 2 ? 2 : channels, shift = r->bits - 16;

    (void)d;
    if (n > r->size) {
        int16_t *bigger = realloc(r->frame, sizeof(int16_t) * n * MAX_CHANNELS);

        if (!bigger)
            return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        r->frame = bigger;
        r->size = n;
    }

    int16_t all[MAX_CHANNELS];

    for (long i = 0; i < n; i++) {
        for (int c = 0; c < channels; c++)
            all[c] = shift >= 0 ? buffers[c][i] >> shift : buffers[c][i] << -shift;
        audio_mix_down(all, channels, 1, r->frame + i * out);
    }
    r->have = n;
    r->used = 0;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void got_metadata(const FLAC__StreamDecoder *d, const FLAC__StreamMetadata *m, void *user) {
    struct reader *r = user;

    (void)d;
    if (m->type == FLAC__METADATA_TYPE_STREAMINFO) {
        r->bits = m->data.stream_info.bits_per_sample;
        r->rate = m->data.stream_info.sample_rate;
        r->channels = m->data.stream_info.channels;
        r->frames = m->data.stream_info.total_samples;
    }
}

static void got_error(const FLAC__StreamDecoder *d, FLAC__StreamDecoderErrorStatus status, void *user) {
    (void)d, (void)status;
    ((struct reader *)user)->failed = true;
}

static void *flac_open(const char *path, struct audio_info *info) {
    struct reader *r = calloc(1, sizeof(*r));

    if (!r || !(r->decoder = FLAC__stream_decoder_new())) {
        free(r);
        return audio_codec_error("out of memory");
    }
    if (FLAC__stream_decoder_init_file(r->decoder, path, got_frame, got_metadata, got_error, r)
            != FLAC__STREAM_DECODER_INIT_STATUS_OK
        || !FLAC__stream_decoder_process_until_end_of_metadata(r->decoder) || !r->bits) {
        FLAC__stream_decoder_delete(r->decoder);
        free(r);
        return audio_codec_error("%s isn't a FLAC file this can read", path);
    }
    info->rate = r->rate;
    info->channels = r->channels > 2 ? 2 : r->channels;
    info->frames = r->frames ? r->frames : -1;
    info->bitrate = 0;
    return r;
}

static long flac_read(void *state, int16_t *samples, long frames) {
    struct reader *r = state;
    int out = r->channels > 2 ? 2 : r->channels;
    long done = 0;

    while (done < frames) {
        if (r->used == r->have) {
            if (FLAC__stream_decoder_get_state(r->decoder) == FLAC__STREAM_DECODER_END_OF_STREAM)
                break;
            if (!FLAC__stream_decoder_process_single(r->decoder) || r->failed) {
                audio_codec_error("the FLAC file is damaged");
                return done ? done : -1;
            }
            continue;
        }

        long n = r->have - r->used < frames - done ? r->have - r->used : frames - done;

        memcpy(samples + done * out, r->frame + r->used * out, n * out * sizeof(int16_t));
        r->used += n;
        done += n;
    }
    return done;
}

// Seeking decodes the frame it lands in, which is kept from there on.
static bool flac_seek(void *state, int64_t frame) {
    struct reader *r = state;

    r->have = r->used = 0;
    if (!FLAC__stream_decoder_seek_absolute(r->decoder, frame)) {
        FLAC__stream_decoder_flush(r->decoder);
        return false;
    }
    return true;
}

static void flac_close(void *state) {
    struct reader *r = state;

    FLAC__stream_decoder_delete(r->decoder);
    free(r->frame);
    free(r);
}

struct writer {
    FLAC__StreamEncoder *encoder;
    int channels;
    FLAC__int32 wide[CHUNK * 2];
};

static void *flac_create(const char *path, const struct audio_info *info) {
    struct writer *w = calloc(1, sizeof(*w));

    if (!w || !(w->encoder = FLAC__stream_encoder_new())) {
        free(w);
        return audio_codec_error("out of memory");
    }
    w->channels = info->channels;
    FLAC__stream_encoder_set_channels(w->encoder, info->channels);
    FLAC__stream_encoder_set_bits_per_sample(w->encoder, 16);
    FLAC__stream_encoder_set_sample_rate(w->encoder, info->rate);
    FLAC__stream_encoder_set_compression_level(w->encoder, COMPRESSION);
    if (FLAC__stream_encoder_init_file(w->encoder, path, NULL, NULL) != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        FLAC__stream_encoder_delete(w->encoder);
        free(w);
        return audio_codec_error("can't write %s as FLAC", path);
    }
    return w;
}

static bool flac_write(void *state, const int16_t *samples, long frames) {
    struct writer *w = state;

    for (long i = 0; i < frames; i += CHUNK) {
        long n = frames - i < CHUNK ? frames - i : CHUNK;

        for (long k = 0; k < n * w->channels; k++)
            w->wide[k] = samples[i * w->channels + k];
        if (!FLAC__stream_encoder_process_interleaved(w->encoder, w->wide, n))
            return audio_codec_error("can't write the FLAC file") != NULL;
    }
    return true;
}

static bool flac_finish(void *state) {
    struct writer *w = state;
    bool ok = FLAC__stream_encoder_finish(w->encoder);

    FLAC__stream_encoder_delete(w->encoder);
    free(w);
    return ok ? true : audio_codec_error("couldn't finish the FLAC file") != NULL;
}

const struct audio_codec audio_codec = {
    flac_open, flac_read, flac_seek, flac_close, flac_create, flac_write, flac_finish,
};
