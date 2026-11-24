#include <audio.h>
#include <audio_codec.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Playing, and the volume, through PipeWire, which mixes everything that's playing.
// It's spoken to as PulseAudio, which pipewire-pulse serves: a simple connection
// for each stream, and one for the mixer, whose sink is wherever sound plays now.

#define APP_NAME "AximOS"
#define DEFAULT_SINK "@DEFAULT_SINK@"

struct audio_stream {
    pa_simple *simple;
    int rate, channels;
};

static pa_sample_spec spec_of(int rate, int channels) {
    return (pa_sample_spec){.format = PA_SAMPLE_S16LE, .rate = rate, .channels = channels};
}

struct audio_stream *audio_play(int rate, int channels) {
    struct audio_stream *s = malloc(sizeof(*s));
    pa_sample_spec spec = spec_of(rate, channels);
    int error;

    if (!s)
        return NULL;
    *s = (struct audio_stream){.rate = rate, .channels = channels};
    s->simple = pa_simple_new(NULL, APP_NAME, PA_STREAM_PLAYBACK, NULL, program_invocation_short_name, &spec, NULL,
                              NULL, &error);
    if (!s->simple) {
        audio_codec_error("can't play through PipeWire: %s", pa_strerror(error));
        free(s);
        return NULL;
    }
    return s;
}

// Waits while PipeWire has plenty to play already.
bool audio_stream_write(struct audio_stream *s, const int16_t *samples, long frames) {
    return pa_simple_write(s->simple, samples, (size_t)frames * s->channels * sizeof(int16_t), NULL) == 0;
}

long audio_stream_delay(struct audio_stream *s) {
    pa_usec_t latency = pa_simple_get_latency(s->simple, NULL);

    return latency == (pa_usec_t)-1 ? 0 : (long)(latency * s->rate / 1000000);
}

void audio_stream_flush(struct audio_stream *s) {
    pa_simple_flush(s->simple, NULL);
}

void audio_stream_close(struct audio_stream *s) {
    if (!s)
        return;
    pa_simple_drain(s->simple, NULL);
    pa_simple_free(s->simple);
    free(s);
}

// The mixer: a connection kept for asking and telling, each a round trip on its own loop.

static pa_mainloop *loop;
static pa_context *context;

static void disconnect(void) {
    if (context) {
        pa_context_disconnect(context);
        pa_context_unref(context);
    }
    if (loop)
        pa_mainloop_free(loop);
    context = NULL;
    loop = NULL;
}

static bool connected(void) {
    pa_context_state_t state;

    if (context && pa_context_get_state(context) == PA_CONTEXT_READY)
        return true;
    disconnect();
    if (!(loop = pa_mainloop_new()) || !(context = pa_context_new(pa_mainloop_get_api(loop), APP_NAME))
        || pa_context_connect(context, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) < 0) {
        disconnect();
        return false;
    }
    while ((state = pa_context_get_state(context)) != PA_CONTEXT_READY) {
        if (!PA_CONTEXT_IS_GOOD(state) || pa_mainloop_iterate(loop, 1, NULL) < 0) {
            disconnect();
            return false;
        }
    }
    return true;
}

// Runs the loop until an operation's done, returning false if it couldn't be.
static bool finish(pa_operation *op) {
    bool ok = op != NULL;

    while (op && pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
        if (pa_mainloop_iterate(loop, 1, NULL) < 0) {
            ok = false;
            break;
        }
    }
    if (op)
        pa_operation_unref(op);
    return ok;
}

struct found {
    struct audio_mixer *mixer;
    pa_cvolume volume;
    char default_sink[256];
    struct audio_output *outputs;
    int count, max;
    bool done;
};

static void got_server(pa_context *c, const pa_server_info *info, void *data) {
    struct found *f = data;

    (void)c;
    snprintf(f->default_sink, sizeof(f->default_sink), "%s", info && info->default_sink_name ? info->default_sink_name : "");
}

static void got_default_sink(pa_context *c, const pa_sink_info *info, int last, void *data) {
    struct found *f = data;

    (void)c;
    if (last || !info)
        return;
    f->mixer->volume = (int)((pa_cvolume_avg(&info->volume) * 100 + PA_VOLUME_NORM / 2) / PA_VOLUME_NORM);
    f->mixer->muted = info->mute;
    f->mixer->card = true;
    f->volume = info->volume;
}

bool audio_mixer_get(struct audio_mixer *mixer) {
    struct found f = {.mixer = mixer};

    *mixer = (struct audio_mixer){0};
    return connected() && finish(pa_context_get_sink_info_by_name(context, DEFAULT_SINK, got_default_sink, &f));
}

bool audio_set_volume(int volume) {
    struct audio_mixer m;
    struct found f = {.mixer = &m};

    volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
    if (!connected() || !finish(pa_context_get_sink_info_by_name(context, DEFAULT_SINK, got_default_sink, &f)) || !m.card)
        return false;
    pa_cvolume_set(&f.volume, f.volume.channels, (pa_volume_t)((uint64_t)volume * PA_VOLUME_NORM / 100));
    return finish(pa_context_set_sink_volume_by_name(context, DEFAULT_SINK, &f.volume, NULL, NULL));
}

bool audio_set_muted(bool muted) {
    return connected() && finish(pa_context_set_sink_mute_by_name(context, DEFAULT_SINK, muted, NULL, NULL));
}

// PipeWire chooses its own latency, low for those who ask.
bool audio_set_low_latency(bool low) {
    (void)low;
    return true;
}

static void got_sink(pa_context *c, const pa_sink_info *info, int last, void *data) {
    struct found *f = data;

    (void)c;
    if (last || !info)
        return;
    if (f->count < f->max) {
        struct audio_output *o = &f->outputs[f->count];

        snprintf(o->key, sizeof(o->key), "%s", info->name);
        snprintf(o->name, sizeof(o->name), "%s", info->description ? info->description : info->name);
        o->current = !strcmp(info->name, f->default_sink);
    }
    f->count++;
}

int audio_outputs(struct audio_output *outputs, int max) {
    struct found f = {.outputs = outputs, .max = max};

    if (!connected() || !finish(pa_context_get_server_info(context, got_server, &f))
        || !finish(pa_context_get_sink_info_list(context, got_sink, &f)))
        return -1;
    return f.count;
}

bool audio_set_output(const char *key) {
    return connected() && finish(pa_context_set_default_sink(context, key, NULL, NULL));
}

// Watching: a connection of its own, on a thread, which writes a byte to a pipe for
// each change to the sinks or which is the default, and the pipe's other end is
// what's polled.

static void changed(pa_context *c, pa_subscription_event_type_t type, uint32_t index, void *data) {
    int *fd = data;

    (void)c, (void)type, (void)index;
    if (write(*fd, "", 1) < 0) {
        // full already, which says the same
    }
}

static void watch_state(pa_context *c, void *data) {
    pa_threaded_mainloop *watcher = data;

    if (!PA_CONTEXT_IS_GOOD(pa_context_get_state(c)) || pa_context_get_state(c) == PA_CONTEXT_READY)
        pa_threaded_mainloop_signal(watcher, 0);
}

int audio_mixer_watch(struct audio_mixer *mixer) {
    static int fds[2] = {-1, -1};
    pa_threaded_mainloop *watcher;
    pa_context *c;

    if (fds[0] >= 0) // one watch is enough for everyone
        return audio_mixer_get(mixer) ? fds[0] : -1;
    if (!audio_mixer_get(mixer) || pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0 || !(watcher = pa_threaded_mainloop_new()))
        return -1;
    c = pa_context_new(pa_threaded_mainloop_get_api(watcher), APP_NAME);
    pa_context_set_state_callback(c, watch_state, watcher);
    pa_threaded_mainloop_lock(watcher);
    if (pa_context_connect(c, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) < 0 || pa_threaded_mainloop_start(watcher) < 0) {
        pa_threaded_mainloop_unlock(watcher);
        return -1;
    }
    while (pa_context_get_state(c) != PA_CONTEXT_READY && PA_CONTEXT_IS_GOOD(pa_context_get_state(c)))
        pa_threaded_mainloop_wait(watcher);
    if (pa_context_get_state(c) == PA_CONTEXT_READY) {
        pa_context_set_subscribe_callback(c, changed, &fds[1]);
        pa_operation_unref(pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER, NULL, NULL));
    }
    pa_threaded_mainloop_unlock(watcher);
    return fds[0];
}

bool audio_mixer_changed(int fd, struct audio_mixer *mixer) {
    char drained[64];

    while (read(fd, drained, sizeof(drained)) > 0)
        ;
    return audio_mixer_get(mixer);
}
