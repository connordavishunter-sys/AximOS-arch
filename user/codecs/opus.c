// Opus in Ogg, read and written with libopus, with libogg for the pages. Opus is
// always 48000 samples a second, so other rates are changed to that to write.
#include <audio_codec.h>
#include <fcntl.h>
#include <ogg/ogg.h>
#include <opus.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RATE 48000
#define MAX_PACKET 5760 // samples, 120 ms
#define PREROLL 3840    // decoded before a place it seeks to, for the decoder to settle
#define FRAME 960       // 20 ms, what it writes
#define DEFAULT_BITRATE 96
#define READ_SIZE 4096
#define MAX_BYTES 4000

struct reader {
    int fd, channels;
    off_t audio, size;    // where the first page of audio is, and how big the file is
    int64_t preskip, end; // samples to leave out at the start, and where it ends
    int64_t at, skip_to;  // the granule of the next sample decoded, and what's left out before it
    ogg_sync_state sync;
    ogg_stream_state stream;
    bool have_stream, ended;
    OpusDecoder *decoder;
    int16_t pcm[MAX_PACKET * 2];
    int have, used;
};

// The next page, reading more of the file as it's needed. False at the end.
static bool next_page(struct reader *r, ogg_page *page) {
    for (;;) {
        int status = ogg_sync_pageout(&r->sync, page);

        if (status > 0)
            return true;
        if (status < 0)
            continue; // skipped what wasn't a page

        char *buffer = ogg_sync_buffer(&r->sync, READ_SIZE);
        ssize_t n = read(r->fd, buffer, READ_SIZE);

        if (n <= 0)
            return false;
        ogg_sync_wrote(&r->sync, n);
    }
}

// Starts reading pages from a place in the file.
static void read_from(struct reader *r, off_t offset) {
    lseek(r->fd, offset, SEEK_SET);
    ogg_sync_reset(&r->sync);
    ogg_stream_reset(&r->stream);
}

static bool read_headers(struct reader *r) {
    ogg_page page;
    ogg_packet packet;
    int packets = 0;

    while (packets < 2 && next_page(r, &page)) {
        if (!r->have_stream) {
            ogg_stream_init(&r->stream, ogg_page_serialno(&page));
            r->have_stream = true;
        }
        if (ogg_page_serialno(&page) != r->stream.serialno)
            continue;
        ogg_stream_pagein(&r->stream, &page);
        while (packets < 2 && ogg_stream_packetout(&r->stream, &packet) == 1) {
            if (packets++ > 0)
                continue; // the tags
            if (packet.bytes < 19 || memcmp(packet.packet, "OpusHead", 8))
                return audio_codec_error("not an Ogg Opus file");
            r->channels = packet.packet[9];
            r->preskip = packet.packet[10] | packet.packet[11] << 8;
            if (packet.packet[18] > 1 || r->channels < 1 || r->channels > 2)
                return audio_codec_error("only mono and stereo Opus can be read");
        }
    }
    if (packets < 2)
        return audio_codec_error("not an Ogg Opus file");
    r->audio = lseek(r->fd, 0, SEEK_CUR) - r->sync.fill + r->sync.returned;
    return true;
}

// The last page's granule says where it ends, so the end of the file is looked at.
static int64_t last_granule(struct reader *r) {
    ogg_page page;
    int64_t last = -1;

    for (off_t back = READ_SIZE * 16; last < 0; back *= 2) {
        read_from(r, r->size > back ? r->size - back : 0);
        while (next_page(r, &page)) {
            if (ogg_page_serialno(&page) == r->stream.serialno && ogg_page_granulepos(&page) >= 0)
                last = ogg_page_granulepos(&page);
        }
        if (back >= r->size)
            break;
    }
    return last;
}

static void *opus_open(const char *path, struct audio_info *info) {
    struct reader *r = calloc(1, sizeof(*r));
    int error;

    if (!r)
        return audio_codec_error("out of memory");
    ogg_sync_init(&r->sync);
    r->fd = open(path, O_RDONLY);
    if (r->fd < 0 || !read_headers(r)) {
        if (r->fd < 0)
            audio_codec_error("can't open %s", path);
        else
            close(r->fd);
        if (r->have_stream)
            ogg_stream_clear(&r->stream);
        ogg_sync_clear(&r->sync);
        free(r);
        return NULL;
    }
    r->size = lseek(r->fd, 0, SEEK_END);
    r->end = last_granule(r);
    r->decoder = opus_decoder_create(RATE, r->channels, &error);
    read_from(r, r->audio);
    r->skip_to = r->preskip;
    info->rate = RATE;
    info->channels = r->channels;
    info->frames = r->end > r->preskip ? r->end - r->preskip : -1;
    if (info->frames > 0)
        info->bitrate = (int)((r->size - r->audio) * 8 * RATE / info->frames / 1000);
    return r;
}

// Decodes the next packet, returning false at the end.
static bool decode_packet(struct reader *r) {
    ogg_packet packet;
    ogg_page page;

    while (ogg_stream_packetout(&r->stream, &packet) != 1) {
        if (r->ended || !next_page(r, &page))
            return false;
        if (ogg_page_serialno(&page) == r->stream.serialno)
            ogg_stream_pagein(&r->stream, &page);
    }

    int n = opus_decode(r->decoder, packet.packet, packet.bytes, r->pcm, MAX_PACKET, 0);

    if (n < 0)
        n = 0; // a damaged packet is left out
    if (packet.e_o_s)
        r->ended = true;

    // What comes before where it's to start, or after the end, is left out.
    int64_t first = r->at, last = r->at + n;

    r->at = last;
    if (r->end >= 0 && last > r->end)
        last = r->end > first ? r->end : first;
    r->used = first < r->skip_to ? (int)(r->skip_to < last ? r->skip_to - first : last - first) : 0;
    r->have = (int)(last - first);
    return true;
}

static long opus_read(void *state, int16_t *samples, long frames) {
    struct reader *r = state;
    long done = 0;

    while (done < frames) {
        if (r->used == r->have) {
            if (!decode_packet(r))
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

// The granule at the end of the first page from offset, or -1.
static int64_t granule_after(struct reader *r, off_t offset) {
    ogg_page page;

    read_from(r, offset);
    while (next_page(r, &page)) {
        if (ogg_page_serialno(&page) == r->stream.serialno && ogg_page_granulepos(&page) >= 0)
            return ogg_page_granulepos(&page);
    }
    return -1;
}

// Looks for the last page ending well before where it's going, then decodes from
// it. The packets on that page say where its first one starts.
static bool opus_seek(void *state, int64_t frame) {
    struct reader *r = state;
    int64_t target = frame + r->preskip, settle = target - PREROLL;
    off_t low = r->audio, high = r->size;
    ogg_page page;
    ogg_packet packet;

    while (settle > 0 && high - low > READ_SIZE) {
        off_t middle = low + (high - low) / 2;
        int64_t g = granule_after(r, middle);

        if (g >= 0 && g <= settle)
            low = middle;
        else
            high = middle;
    }
    opus_decoder_ctl(r->decoder, OPUS_RESET_STATE);
    read_from(r, low);
    r->ended = false;
    r->have = r->used = 0;
    r->skip_to = target;
    r->at = 0;
    if (low == r->audio)
        return true;

    // The page's own packets, which end at its granule, say where they start.
    while (next_page(r, &page) && (ogg_page_serialno(&page) != r->stream.serialno || ogg_page_granulepos(&page) < 0))
        ;
    ogg_stream_pagein(&r->stream, &page);

    int64_t length = 0;
    ogg_stream_state copy;
    int status;

    ogg_stream_init(&copy, r->stream.serialno);
    ogg_stream_pagein(&copy, &page);
    while ((status = ogg_stream_packetout(&copy, &packet)) != 0) {
        if (status > 0) // not the end of a packet from the page before
            length += opus_packet_get_nb_samples(packet.packet, packet.bytes, RATE);
    }
    ogg_stream_clear(&copy);
    r->at = ogg_page_granulepos(&page) - length;
    return true;
}

static void opus_close(void *state) {
    struct reader *r = state;

    opus_decoder_destroy(r->decoder);
    ogg_stream_clear(&r->stream);
    ogg_sync_clear(&r->sync);
    close(r->fd);
    free(r);
}

struct writer {
    int fd, channels;
    OpusEncoder *encoder;
    struct audio_resampler *resampler; // when what's written isn't at 48000
    ogg_stream_state stream;
    int64_t written, granule, packets; // samples a channel given, at 48000, and how far it's encoded
    int lookahead, have;
    int16_t frame[FRAME * 2];
};

static bool put_pages(struct writer *w, bool flush) {
    ogg_page page;

    while (flush ? ogg_stream_flush(&w->stream, &page) : ogg_stream_pageout(&w->stream, &page)) {
        if (write(w->fd, page.header, page.header_len) != page.header_len
            || write(w->fd, page.body, page.body_len) != page.body_len)
            return false;
    }
    return true;
}

static void put_packet(struct writer *w, unsigned char *data, long bytes, int64_t granule, bool last) {
    ogg_packet packet = {data, bytes, w->packets == 0, last, granule, w->packets};

    w->packets++;
    ogg_stream_packetin(&w->stream, &packet);
}

static void *opus_create(const char *path, const struct audio_info *info) {
    struct writer *w = calloc(1, sizeof(*w));
    int error;

    if (!w)
        return audio_codec_error("out of memory");
    w->channels = info->channels;
    w->encoder = opus_encoder_create(RATE, info->channels, OPUS_APPLICATION_AUDIO, &error);
    w->resampler = info->rate == RATE ? NULL : audio_resampler_new(info->rate, RATE, info->channels);
    w->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (!w->encoder || w->fd < 0 || (info->rate != RATE && !w->resampler)) {
        if (w->encoder)
            opus_encoder_destroy(w->encoder);
        if (w->fd >= 0)
            close(w->fd);
        audio_resampler_free(w->resampler);
        free(w);
        return audio_codec_error("can't write %s", path);
    }
    opus_encoder_ctl(w->encoder, OPUS_SET_BITRATE((info->bitrate ? info->bitrate : DEFAULT_BITRATE) * 1000));
    opus_encoder_ctl(w->encoder, OPUS_GET_LOOKAHEAD(&w->lookahead));
    srand(time(NULL));
    ogg_stream_init(&w->stream, rand());

    unsigned char head[19] = "OpusHead\x01", tags[] = "OpusTags\x06\0\0\0AximOS\0\0\0\0";

    head[9] = info->channels;
    head[10] = w->lookahead;
    head[11] = w->lookahead >> 8;
    for (int i = 0; i < 4; i++)
        head[12 + i] = info->rate >> (8 * i); // the rate it was, which a player may go back to
    put_packet(w, head, sizeof(head), 0, false);
    put_pages(w, true);
    put_packet(w, tags, sizeof(tags) - 1, 0, false);
    put_pages(w, true);
    return w;
}

static bool encode_frame(struct writer *w, bool last) {
    unsigned char data[MAX_BYTES];
    int bytes = opus_encode(w->encoder, w->frame, FRAME, data, sizeof(data));

    if (bytes < 0)
        return false;
    // A packet's granule is where it ends, and the last one's where the sound does,
    // so a player leaves out the padding.
    w->granule += FRAME;
    put_packet(w, data, bytes, last ? w->lookahead + w->written : w->granule, last);
    w->have = 0;
    return put_pages(w, false);
}

// Gathers samples at 48000 into frames of 20 ms.
static bool take(struct writer *w, const int16_t *samples, long frames) {
    for (long i = 0; i < frames;) {
        long n = FRAME - w->have < frames - i ? FRAME - w->have : frames - i;

        memcpy(w->frame + w->have * w->channels, samples + i * w->channels, n * w->channels * sizeof(int16_t));
        w->have += n;
        w->written += n;
        i += n;
        if (w->have == FRAME && !encode_frame(w, false))
            return false;
    }
    return true;
}

static bool opus_write(void *state, const int16_t *samples, long frames) {
    struct writer *w = state;

    if (!w->resampler)
        return take(w, samples, frames) ? true : audio_codec_error("can't write the Opus file") != NULL;

    int16_t out[(1024 * RATE / 8000 + 2) * 2];

    for (long i = 0; i < frames; i += 1024) {
        long n = frames - i < 1024 ? frames - i : 1024;
        long made = audio_resample(w->resampler, samples + i * w->channels, n, out, sizeof(out) / 4);

        if (!take(w, out, made))
            return audio_codec_error("can't write the Opus file") != NULL;
    }
    return true;
}

// The encoder runs behind by its lookahead, so silence after the end lets it
// catch up, and the last frame is padded with silence too.
static bool opus_finish(void *state) {
    struct writer *w = state;
    int64_t real = w->written;
    bool ok = true;

    for (int left = w->lookahead; ok && left > 0; left -= FRAME) {
        int16_t silence[FRAME * 2] = {0};

        ok = take(w, silence, left < FRAME ? left : FRAME);
    }
    w->written = real;
    memset(w->frame + w->have * w->channels, 0, (FRAME - w->have) * w->channels * sizeof(int16_t));
    ok = ok && encode_frame(w, true) && put_pages(w, true);
    ok = close(w->fd) == 0 && ok;
    opus_encoder_destroy(w->encoder);
    ogg_stream_clear(&w->stream);
    audio_resampler_free(w->resampler);
    free(w);
    return ok ? true : audio_codec_error("couldn't finish the Opus file") != NULL;
}

const struct audio_codec audio_codec = {
    opus_open, opus_read, opus_seek, opus_close, opus_create, opus_write, opus_finish,
};
