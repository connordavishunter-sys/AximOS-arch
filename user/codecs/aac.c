// AAC in ADTS frames, read with faad2 and written with faac.
#include <audio_codec.h>
#include <faac.h>
#include <fcntl.h>
#include <neaacdec.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_FRAME 8192 // bytes, more than ADTS can say a frame is
#define DEFAULT_BITRATE 128
#define BEFORE 2 // frames decoded before one it seeks to, which overlap it

struct reader {
    int fd, channels;
    NeAACDecHandle decoder;
    off_t *frames; // where each ADTS frame starts
    long count, next, per_frame;
    int64_t skip; // samples a channel still to leave out after seeking
    uint8_t data[MAX_FRAME];
    int16_t *pcm;
    long have, used;
};

// ID3 tags can come before the frames.
static off_t after_tags(int fd) {
    uint8_t h[10];

    if (pread(fd, h, 10, 0) != 10 || memcmp(h, "ID3", 3))
        return 0;
    return 10 + ((h[6] & 0x7F) << 21 | (h[7] & 0x7F) << 14 | (h[8] & 0x7F) << 7 | (h[9] & 0x7F));
}

// Finds every frame, so the length is known and seeking goes straight to one.
static bool index_frames(struct reader *r) {
    off_t at = after_tags(r->fd), size = lseek(r->fd, 0, SEEK_END);
    long capacity = 0;
    uint8_t h[7];

    while (at + 7 <= size && pread(r->fd, h, 7, at) == 7) {
        long length = (h[3] & 3) << 11 | h[4] << 3 | h[5] >> 5;

        if (h[0] != 0xFF || (h[1] & 0xF6) != 0xF0 || length < 7) {
            if (r->count)
                break; // the end, or what isn't AAC
            at++;
            continue;
        }
        if (r->count == capacity) {
            off_t *bigger = realloc(r->frames, sizeof(off_t) * (capacity = capacity ? capacity * 2 : 1024));

            if (!bigger)
                return false;
            r->frames = bigger;
        }
        r->frames[r->count++] = at;
        at += length;
    }
    return r->count > 0;
}

static long read_frame(struct reader *r, long index) {
    off_t end = index + 1 < r->count ? r->frames[index + 1] : r->frames[index] + MAX_FRAME;
    ssize_t n = pread(r->fd, r->data, end - r->frames[index] < MAX_FRAME ? end - r->frames[index] : MAX_FRAME,
                      r->frames[index]);

    return n > 0 ? n : 0;
}

// Decodes the next frame, keeping its samples. False at the end.
static bool decode_frame(struct reader *r) {
    NeAACDecFrameInfo frame;

    while (r->next < r->count) {
        long n = read_frame(r, r->next++);
        int16_t *pcm = NeAACDecDecode(r->decoder, &frame, r->data, n);

        if (frame.error || !pcm || !frame.samples)
            continue; // a damaged frame, or the first, which only primes it
        r->pcm = pcm;
        r->channels = frame.channels; // what it gives, after mixing down
        r->have = frame.samples / frame.channels;
        r->used = r->skip < r->have ? r->skip : r->have;
        r->skip -= r->used;
        return true;
    }
    return false;
}

static void *aac_open(const char *path, struct audio_info *info) {
    struct reader *r = calloc(1, sizeof(*r));
    unsigned long rate;
    unsigned char channels;

    if (!r)
        return audio_codec_error("out of memory");
    r->fd = open(path, O_RDONLY);
    r->decoder = NeAACDecOpen();
    if (r->fd < 0 || !r->decoder || !index_frames(r)) {
        if (r->fd >= 0)
            close(r->fd);
        if (r->decoder)
            NeAACDecClose(r->decoder);
        free(r->frames);
        free(r);
        return audio_codec_error("%s isn't AAC this can read", path);
    }

    NeAACDecConfigurationPtr config = NeAACDecGetCurrentConfiguration(r->decoder);

    config->outputFormat = FAAD_FMT_16BIT;
    config->downMatrix = 1; // to stereo
    NeAACDecSetConfiguration(r->decoder, config);
    NeAACDecInit(r->decoder, r->data, read_frame(r, 0), &rate, &channels);

    // The first frame says how many samples each has, which is twice as many with SBR.
    if (!decode_frame(r)) {
        audio_codec_error("%s isn't AAC this can read", path);
        NeAACDecClose(r->decoder);
        close(r->fd);
        free(r->frames);
        free(r);
        return NULL;
    }
    r->per_frame = r->have;
    info->rate = rate;
    info->channels = r->channels;
    info->frames = (r->count - 1) * r->per_frame; // the first frame only primes it
    info->bitrate = 0;
    return r;
}

static long aac_read(void *state, int16_t *samples, long frames) {
    struct reader *r = state;
    long done = 0;

    while (done < frames) {
        if (r->used == r->have) {
            if (!decode_frame(r))
                break;
            continue;
        }

        long n = r->have - r->used < frames - done ? r->have - r->used : frames - done;

        memcpy(samples + done * r->channels, r->pcm + r->used * r->channels, n * r->channels * sizeof(int16_t));
        r->used += n;
        done += n;
    }
    return done;
}

// The first frame only primes the decoder, so a sample's in the frame after the one
// it'd be in otherwise. Decoding from the start leaves that frame out again.
static bool aac_seek(void *state, int64_t frame) {
    struct reader *r = state;
    int64_t at = frame + r->per_frame;
    long index = at / r->per_frame < r->count ? (long)(at / r->per_frame) : r->count;
    long from = index > BEFORE ? index - BEFORE : 0;

    NeAACDecPostSeekReset(r->decoder, from);
    r->next = from;
    r->skip = at - (int64_t)(from ? from : 1) * r->per_frame;
    r->have = r->used = 0;
    return true;
}

static void aac_close(void *state) {
    struct reader *r = state;

    NeAACDecClose(r->decoder);
    close(r->fd);
    free(r->frames);
    free(r);
}

struct writer {
    int fd, channels;
    faac_encoder *encoder;
    uint32_t per_frame, max_bytes;
    uint8_t *out;
};

static bool encode(struct writer *w, const int16_t *samples, uint32_t count) {
    uint32_t bytes;

    return faac_encoder_encode(w->encoder, samples, count, w->out, w->max_bytes, &bytes) == FAAC_OK
           && write(w->fd, w->out, bytes) == (ssize_t)bytes;
}

static void *aac_create(const char *path, const struct audio_info *info) {
    struct writer *w = calloc(1, sizeof(*w));
    faac_params params;
    faac_encoder_info about = {.struct_size = sizeof(about)};

    if (!w)
        return audio_codec_error("out of memory");
    faac_params_init(&params);
    params.sample_rate = info->rate;
    params.num_channels = info->channels;
    params.mpeg_version = FAAC_MPEG4;
    params.object_type = FAAC_OBJ_LOW;
    params.joint_mode = FAAC_JOINT_MS;
    params.use_tns = true;
    params.bit_rate = (info->bitrate ? info->bitrate : DEFAULT_BITRATE) * 1000 / info->channels;
    params.output_format = FAAC_STREAM_ADTS;
    params.input_format = FAAC_INPUT_16BIT;
    if (faac_encoder_open(&params, &w->encoder) != FAAC_OK || faac_encoder_get_info(w->encoder, &about) != FAAC_OK) {
        faac_encoder_close(&w->encoder);
        free(w);
        return audio_codec_error("AAC can't be %d samples a second at that bitrate", info->rate);
    }
    w->channels = info->channels;
    w->per_frame = about.frame_samples;
    w->max_bytes = about.max_output_bytes;
    w->out = malloc(w->max_bytes);
    w->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (!w->out || w->fd < 0) {
        if (w->fd >= 0)
            close(w->fd);
        faac_encoder_close(&w->encoder);
        free(w->out);
        free(w);
        return audio_codec_error("can't write %s", path);
    }
    return w;
}

// It takes at most a frame each time.
static bool aac_write(void *state, const int16_t *samples, long frames) {
    struct writer *w = state;

    for (long i = 0; i < frames; i += w->per_frame) {
        long n = frames - i < (long)w->per_frame ? frames - i : (long)w->per_frame;

        if (!encode(w, samples + i * w->channels, n * w->channels))
            return audio_codec_error("can't write the AAC file") != NULL;
    }
    return true;
}

// Giving it nothing flushes it, a frame each time, until there's no more.
static bool aac_finish(void *state) {
    struct writer *w = state;
    uint32_t bytes = 1;
    bool ok = true;

    while (ok && bytes) {
        ok = faac_encoder_encode(w->encoder, NULL, 0, w->out, w->max_bytes, &bytes) == FAAC_OK
             && write(w->fd, w->out, bytes) == (ssize_t)bytes;
    }
    ok = close(w->fd) == 0 && ok;
    faac_encoder_close(&w->encoder);
    free(w->out);
    free(w);
    return ok ? true : audio_codec_error("couldn't finish the AAC file") != NULL;
}

const struct audio_codec audio_codec = {
    aac_open, aac_read, aac_seek, aac_close, aac_create, aac_write, aac_finish,
};
