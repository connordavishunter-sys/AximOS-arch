// Vorbis in Ogg, read and written with libvorbis.
#include <audio_codec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define OV_EXCLUDE_STATIC_CALLBACKS
#include <vorbis/vorbisenc.h>
#include <vorbis/vorbisfile.h>

#define DEFAULT_QUALITY 0.4f // about 128 kbit/s in stereo
#define MAX_CHANNELS 8
#define CHUNK 1024

struct reader {
    OggVorbis_File file;
    int channels;
    int16_t all[CHUNK * MAX_CHANNELS];
};

static void *vorbis_open(const char *path, struct audio_info *info) {
    struct reader *r = calloc(1, sizeof(*r));

    if (!r)
        return audio_codec_error("out of memory");
    if (ov_fopen(path, &r->file) < 0) {
        free(r);
        return audio_codec_error("%s isn't an Ogg Vorbis file this can read", path);
    }

    vorbis_info *vi = ov_info(&r->file, -1);
    ogg_int64_t total = ov_pcm_total(&r->file, -1);

    r->channels = vi->channels;
    if (r->channels > MAX_CHANNELS) {
        ov_clear(&r->file);
        free(r);
        return audio_codec_error("too many channels");
    }
    info->rate = vi->rate;
    info->channels = r->channels > 2 ? 2 : r->channels;
    info->frames = total >= 0 ? total : -1;
    info->bitrate = (int)(ov_bitrate(&r->file, -1) / 1000);
    return r;
}

static long vorbis_read(void *state, int16_t *samples, long frames) {
    struct reader *r = state;
    int out = r->channels > 2 ? 2 : r->channels, section;
    long done = 0;

    while (done < frames) {
        long want = frames - done < CHUNK ? frames - done : CHUNK;
        long got = ov_read(&r->file, (char *)r->all, want * r->channels * 2, 0, 2, 1, &section);

        if (got == OV_HOLE)
            continue; // a gap in the data, which it carries on after
        if (got < 0) {
            audio_codec_error("the Ogg Vorbis file is damaged");
            return done ? done : -1;
        }
        if (got == 0)
            break;
        got /= r->channels * 2;
        audio_mix_down(r->all, r->channels, got, samples + done * out);
        done += got;
    }
    return done;
}

static bool vorbis_seek(void *state, int64_t frame) {
    return ov_pcm_seek(&((struct reader *)state)->file, frame) == 0;
}

static void vorbis_close(void *state) {
    ov_clear(&((struct reader *)state)->file);
    free(state);
}

struct writer {
    FILE *out;
    int channels;
    vorbis_info vi;
    vorbis_comment comment;
    vorbis_dsp_state dsp;
    vorbis_block block;
    ogg_stream_state stream;
};

static bool put_page(struct writer *w, const ogg_page *page) {
    return fwrite(page->header, 1, page->header_len, w->out) == (size_t)page->header_len
           && fwrite(page->body, 1, page->body_len, w->out) == (size_t)page->body_len;
}

// Writes the pages that are full, or everything when flushing.
static bool put_pages(struct writer *w, bool flush) {
    ogg_page page;

    while (flush ? ogg_stream_flush(&w->stream, &page) : ogg_stream_pageout(&w->stream, &page)) {
        if (!put_page(w, &page))
            return false;
    }
    return true;
}

// Takes the packets the encoder has made, as far as what it's been given goes.
static bool encode(struct writer *w) {
    ogg_packet packet;

    while (vorbis_analysis_blockout(&w->dsp, &w->block) == 1) {
        vorbis_analysis(&w->block, NULL);
        vorbis_bitrate_addblock(&w->block);
        while (vorbis_bitrate_flushpacket(&w->dsp, &packet)) {
            ogg_stream_packetin(&w->stream, &packet);
            if (!put_pages(w, false))
                return false;
        }
    }
    return true;
}

static void *vorbis_create(const char *path, const struct audio_info *info) {
    struct writer *w = calloc(1, sizeof(*w));
    ogg_packet header, comments, codebooks;
    int status;

    if (!w)
        return audio_codec_error("out of memory");
    vorbis_info_init(&w->vi);
    status = info->bitrate ? vorbis_encode_init(&w->vi, info->channels, info->rate, -1, info->bitrate * 1000, -1)
                           : vorbis_encode_init_vbr(&w->vi, info->channels, info->rate, DEFAULT_QUALITY);
    if (status || !(w->out = fopen(path, "wb"))) {
        vorbis_info_clear(&w->vi);
        free(w);
        return status ? audio_codec_error("Vorbis can't be %d samples a second at that bitrate", info->rate)
                      : audio_codec_error("can't write %s", path);
    }
    w->channels = info->channels;
    vorbis_comment_init(&w->comment);
    vorbis_comment_add_tag(&w->comment, "ENCODER", "AximOS");
    vorbis_analysis_init(&w->dsp, &w->vi);
    vorbis_block_init(&w->dsp, &w->block);
    srand(time(NULL));
    ogg_stream_init(&w->stream, rand());
    vorbis_analysis_headerout(&w->dsp, &w->comment, &header, &comments, &codebooks);
    ogg_stream_packetin(&w->stream, &header);
    ogg_stream_packetin(&w->stream, &comments);
    ogg_stream_packetin(&w->stream, &codebooks);
    put_pages(w, true); // the audio starts on a page of its own
    return w;
}

static bool vorbis_write(void *state, const int16_t *samples, long frames) {
    struct writer *w = state;

    for (long i = 0; i < frames; i += CHUNK) {
        long n = frames - i < CHUNK ? frames - i : CHUNK;
        float **buffer = vorbis_analysis_buffer(&w->dsp, n);

        for (long k = 0; k < n; k++)
            for (int c = 0; c < w->channels; c++)
                buffer[c][k] = samples[(i + k) * w->channels + c] / 32768.0f;
        vorbis_analysis_wrote(&w->dsp, n);
        if (!encode(w))
            return audio_codec_error("can't write the Ogg Vorbis file") != NULL;
    }
    return true;
}

static bool vorbis_finish(void *state) {
    struct writer *w = state;
    bool ok;

    vorbis_analysis_wrote(&w->dsp, 0);
    ok = encode(w) && put_pages(w, true);
    ok = fclose(w->out) == 0 && ok;
    ogg_stream_clear(&w->stream);
    vorbis_block_clear(&w->block);
    vorbis_dsp_clear(&w->dsp);
    vorbis_comment_clear(&w->comment);
    vorbis_info_clear(&w->vi);
    free(w);
    return ok ? true : audio_codec_error("couldn't finish the Ogg Vorbis file") != NULL;
}

const struct audio_codec audio_codec = {
    vorbis_open, vorbis_read, vorbis_seek, vorbis_close, vorbis_create, vorbis_write, vorbis_finish,
};
