#include <aximos.h>
#include <audio.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gui.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_TRACKS 256
#define CHUNK 1024 // samples a channel decoded at a time, about 20 ms
#define TICK_MS 200
#define SEEK_STEP 5 // seconds, for Left and Right
#define PLAYLIST "##playlist"
#define POSITION "##position"
#define VOLUME "Volume##volume"
#define NOW_PLAYING "/tmp/now-playing" // for wm's sound panel

// What the window and the thread that plays share, under lock. The window asks
// for things, and the thread says how it's going.
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static struct {
    char path[GUI_PATH_MAX]; // what to play, or nothing to stop
    unsigned asked;          // goes up each time path is set
    bool paused;
    int64_t seek; // where to go, in samples, or -1
    int volume;   // 0 to 100

    unsigned playing;              // the ask what follows is about
    int64_t position, length;      // in samples
    int rate, channels, bitrate;
    bool finished;                 // it played to the end
    char error[GUI_PATH_MAX + 64];
} shared = {.seek = -1, .volume = 80};

static char tracks[MAX_TRACKS][GUI_PATH_MAX];
static const char *names[MAX_TRACKS];
static int track_count, current = -1, selected;
static bool shuffle;
static enum { REPEAT_OFF, REPEAT_ALL, REPEAT_ONE } repeat;
static const char *const repeat_names[] = {"Repeat: off", "Repeat: all", "Repeat: one"};

// Says what's playing, for the sound panel, or that nothing is.
static void tell_wm(bool paused) {
    FILE *f;

    if (current < 0) {
        unlink(NOW_PLAYING);
    } else if ((f = fopen(NOW_PLAYING, "w"))) {
        fprintf(f, "%s %s\n", paused ? "Paused:" : "Playing:", names[current]);
        fclose(f);
    } else {
        printf("player: can't write %s: %s\n", NOW_PLAYING, strerror(errno));
    }
}

static struct gui_window *window;
static struct gui_file_picker picker;

// The volume follows the square of where it's set, which sounds more even.
static void apply_volume(int16_t *samples, long count, int volume) {
    int scale = volume * volume; // out of 10000

    for (long i = 0; volume < 100 && i < count; i++)
        samples[i] = (int16_t)(samples[i] * scale / 10000);
}

// How much of what's been written is still to be heard, in the file's samples.
static int64_t delay(struct audio_stream *out) {
    return audio_stream_delay(out);
}

// Waits while paused, returning false if something else was asked for meanwhile.
// Pausing forgets what's waiting to be mixed, and carries on after what was mixed
// already, which is still heard.
static bool wait_while_paused(unsigned ask, struct audio_stream *out, struct audio_file *f, int64_t *base,
                              int64_t *written) {
    int64_t waiting = delay(out), at;

    audio_stream_flush(out);
    at = *base + *written - waiting + delay(out);
    shared.position = at;
    while (shared.paused && shared.asked == ask)
        pthread_cond_wait(&changed, &lock);
    audio_seek(f, at);
    *base = at;
    *written = 0;
    return shared.asked == ask;
}

static void play(const char *path, unsigned ask) {
    struct audio_info info;
    struct audio_file *f = audio_open(path, &info);
    struct audio_stream *out = f ? audio_play(info.rate, 2) : NULL;
    int64_t base = 0, written = 0;
    static int16_t in[CHUNK * 2], stereo[CHUNK * 2];

    pthread_mutex_lock(&lock);
    if (!out) {
        snprintf(shared.error, sizeof(shared.error), "Can't play it: %s", audio_error());
        printf("player: %s\n", shared.error);
        fflush(stdout);
        shared.finished = true;
        pthread_mutex_unlock(&lock);
        if (f)
            audio_close(f);
        return;
    }
    shared.length = info.frames;
    shared.rate = info.rate;
    shared.channels = info.channels;
    shared.bitrate = info.bitrate;
    pthread_mutex_unlock(&lock);

    for (;;) {
        pthread_mutex_lock(&lock);
        bool going = shared.asked == ask
                     && (!shared.paused || wait_while_paused(ask, out, f, &base, &written));

        if (!going) {
            pthread_mutex_unlock(&lock);
            audio_stream_flush(out); // it's stopped, or something else is to play
            break;
        }
        if (shared.seek >= 0) {
            audio_stream_flush(out);
            audio_seek(f, shared.seek);
            base = shared.seek;
            written = 0;
            shared.seek = -1;
        }
        shared.position = base + written - delay(out);

        int volume = shared.volume;

        pthread_mutex_unlock(&lock);

        long n = audio_read(f, in, CHUNK);

        if (n <= 0) {
            // The end: once what's waiting has been heard, it's finished.
            if (delay(out) > 0) {
                usleep(20000);
                continue;
            }
            pthread_mutex_lock(&lock);
            if (n < 0)
                snprintf(shared.error, sizeof(shared.error), "%s", audio_error());
            shared.finished = shared.asked == ask;
            shared.position = base + written;
            pthread_mutex_unlock(&lock);
            break;
        }
        written += n;
        if (info.channels == 1) {
            for (long i = 0; i < n; i++)
                stereo[2 * i] = stereo[2 * i + 1] = in[i];
        } else {
            memcpy(stereo, in, n * 4);
        }
        apply_volume(stereo, n * 2, volume);
        audio_stream_write(out, stereo, n);
    }
    audio_close(f);
    audio_stream_close(out); // what's been written still plays, unless it was flushed
}

// Plays whatever it's asked to, one at a time.
static void *player(void *unused) {
    unsigned done = 0;

    (void)unused;
    for (;;) {
        char path[GUI_PATH_MAX];
        unsigned ask;

        pthread_mutex_lock(&lock);
        while (shared.asked == done || !shared.path[0]) {
            done = shared.asked;
            pthread_cond_wait(&changed, &lock);
        }
        ask = done = shared.asked;
        snprintf(path, sizeof(path), "%s", shared.path);
        shared.playing = ask;
        shared.position = 0;
        shared.length = -1;
        shared.finished = false;
        shared.error[0] = '\0';
        pthread_mutex_unlock(&lock);
        printf("player: playing %s\n", path);
        fflush(stdout);
        play(path, ask);
    }
    return NULL;
}

// Asks for a track to be played, or for it to stop with -1.
static void choose(int track) {
    pthread_mutex_lock(&lock);
    current = track;
    snprintf(shared.path, sizeof(shared.path), "%s", track >= 0 ? tracks[track] : "");
    shared.asked++;
    shared.paused = false;
    shared.seek = -1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    if (track >= 0) {
        char title[GUI_TITLE_MAX];

        selected = track;
        snprintf(title, sizeof(title), "%s - Player", names[track]);
        gui_set_title(window, title);
    }
    tell_wm(false);
}

// The track after this one: any other at random when shuffling, round to the
// first after the last when repeating all, or -1 at the end.
static int next_track(void) {
    if (shuffle && track_count > 1) {
        int next = rand() % (track_count - 1);

        return next >= current ? next + 1 : next;
    }
    if (current + 1 < track_count)
        return current + 1;
    return repeat == REPEAT_ALL && track_count ? 0 : -1;
}

static void add_track(const char *path) {
    for (int i = 0; i < track_count; i++) {
        if (!strcmp(tracks[i], path))
            return;
    }
    if (track_count == MAX_TRACKS)
        return;
    snprintf(tracks[track_count], GUI_PATH_MAX, "%s", path);
    const char *slash = strrchr(tracks[track_count], '/');

    names[track_count] = slash ? slash + 1 : tracks[track_count];
    track_count++;
}

static int by_name(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

// Opening a file puts the other sounds in its folder in the list too, in order.
// Returns where it is in the list.
static int open_file(const char *path) {
    char folder[GUI_PATH_MAX], *found[MAX_TRACKS];
    const char *slash = strrchr(path, '/');
    int count = 0;
    DIR *dir;
    struct dirent *d;

    snprintf(folder, sizeof(folder), "%.*s", slash ? (int)(slash - path) : 1, slash ? path : ".");
    if ((dir = opendir(folder[0] ? folder : "/"))) {
        while ((d = readdir(dir)) && count < MAX_TRACKS) {
            if (audio_known(d->d_name) && (found[count] = malloc(GUI_PATH_MAX)))
                snprintf(found[count++], GUI_PATH_MAX, "%s/%s", folder, d->d_name);
        }
        closedir(dir);
    }
    qsort(found, count, sizeof(*found), by_name);
    for (int i = 0; i < count; i++) {
        add_track(found[i]);
        free(found[i]);
    }
    add_track(path);
    for (int i = 0; i < track_count; i++) {
        if (!strcmp(tracks[i], path))
            return i;
    }
    return -1;
}

static void format_time(char *out, size_t size, int64_t samples, int rate) {
    long seconds = rate > 0 && samples > 0 ? (long)(samples / rate) : 0;

    snprintf(out, size, "%ld:%02ld", seconds / 60, seconds % 60);
}

static void seek_to(int64_t sample) {
    pthread_mutex_lock(&lock);
    shared.seek = sample < 0 ? 0 : shared.length > 0 && sample > shared.length ? shared.length : sample;
    shared.position = shared.seek;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
}

static void toggle_pause(void) {
    if (current < 0) {
        if (track_count)
            choose(selected);
        return;
    }
    pthread_mutex_lock(&lock);
    shared.paused = !shared.paused;
    printf("player: %s\n", shared.paused ? "paused" : "playing again");
    tell_wm(shared.paused);
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
}

// Says where the controls are, and what's playing, when they change, for the tests.
static void report(struct gui_ui *ui, const char *what) {
    static char last[16][96];
    static const char *whats[16];
    struct gui_rect r = gui_last_rect(ui);
    char now[96];
    int i = 0;

    while (i < 16 && whats[i] && strcmp(whats[i], what))
        i++;
    if (i == 16)
        return;
    whats[i] = what;
    snprintf(now, sizeof(now), "%d %d %d %d", r.x, r.y, r.width, r.height);
    if (strcmp(last[i], now))
        printf("player: at %s %s\n", what, now);
    snprintf(last[i], sizeof(last[i]), "%s", now);
}

static void frame(struct gui_ui *ui) {
    char at[16], length[16];

    gui_ui_begin(ui);
    if (picker.open) {
        int picked = gui_file_picker(ui, &picker);

        if (picked == GUI_PICKED && (picked = open_file(picker.path)) >= 0)
            choose(picked);
        if (picked)
            ui->again = true;
        gui_ui_end(ui);
        return;
    }

    pthread_mutex_lock(&lock);
    bool paused = shared.paused, playing = current >= 0 && !shared.finished;
    int64_t position = shared.position, total = shared.length;
    int rate = shared.rate, channels = shared.channels, bitrate = shared.bitrate, volume = shared.volume;
    char error[sizeof(shared.error)];

    snprintf(error, sizeof(error), "%s", shared.error);
    pthread_mutex_unlock(&lock);

    gui_heading(ui, current >= 0 ? names[current] : "Nothing playing");
    format_time(at, sizeof(at), position, rate);
    format_time(length, sizeof(length), total, rate);
    if (current >= 0) {
        char about[64];

        snprintf(about, sizeof(about), "%d Hz, %s", rate, channels == 1 ? "mono" : "stereo");
        if (bitrate)
            snprintf(about + strlen(about), sizeof(about) - strlen(about), ", %d kbit/s", bitrate);
        gui_label(ui, "%s / %s    %s", at, length, about);
    } else {
        gui_label(ui, "Open a sound file to play it.");
    }

    int seconds = rate ? (int)(position / rate) : 0;

    if (gui_slider(ui, POSITION, &seconds, 0, total > 0 && rate ? (int)(total / rate) : 0) && current >= 0)
        seek_to((int64_t)seconds * rate);
    report(ui, "position");

    if (gui_button(ui, "Previous") && track_count)
        choose(current > 0 ? current - 1 : 0);
    report(ui, "previous");
    gui_same_line(ui);
    if (gui_button(ui, playing && !paused ? "Pause" : "Play"))
        toggle_pause();
    report(ui, "play");
    gui_same_line(ui);
    if (gui_button(ui, "Stop"))
        choose(-1);
    report(ui, "stop");
    gui_same_line(ui);
    if (gui_button(ui, "Next") && next_track() >= 0)
        choose(next_track());
    report(ui, "next");
    gui_same_line(ui);
    if (gui_button(ui, "Open...")) {
        char folder[GUI_PATH_MAX] = "/";

        const char *slash = current >= 0 ? strrchr(tracks[current], '/') : NULL;

        if (slash) // where what's playing is
            snprintf(folder, sizeof(folder), "%.*s", (int)(slash - tracks[current]), tracks[current]);
        gui_picker_open(&picker, folder, NULL, false);
        ui->again = true;
    }
    report(ui, "open");
    if (gui_checkbox(ui, "Shuffle", &shuffle))
        printf("player: shuffle %s\n", shuffle ? "on" : "off");
    report(ui, "shuffle");
    gui_same_line(ui);
    if (gui_button_sized(ui, repeat_names[repeat], 12 * GUI_CHAR_WIDTH + 14, 0)) {
        repeat = (repeat + 1) % 3;
        printf("player: %s\n", repeat_names[repeat]);
    }
    report(ui, "repeat");
    gui_same_line(ui);
    if (gui_slider(ui, VOLUME, &volume, 0, 100)) {
        pthread_mutex_lock(&lock);
        shared.volume = volume;
        pthread_mutex_unlock(&lock);
    }
    report(ui, "volume");
    if (error[0])
        gui_label(ui, "%s", error);

    int result = gui_list(ui, PLAYLIST, names, track_count, &selected, 0);

    report(ui, "playlist");
    if (result == GUI_LIST_ACTIVATED)
        choose(selected);

    // Space pauses, and Left and Right go back and on, when nothing else takes them.
    if (!gui_focused(ui, POSITION) && !gui_focused(ui, VOLUME)) {
        if (gui_key(ui, KEY_SPACE) && !gui_focused(ui, PLAYLIST))
            toggle_pause();
        if (gui_key(ui, KEY_LEFT) && current >= 0)
            seek_to(position - (int64_t)SEEK_STEP * rate);
        if (gui_key(ui, KEY_RIGHT) && current >= 0)
            seek_to(position + (int64_t)SEEK_STEP * rate);
    }
    gui_ui_end(ui);
    fflush(stdout);
}

// What's playing has finished: on to the next, or stopping after the last.
static void next_if_finished(void) {
    pthread_mutex_lock(&lock);
    bool finished = shared.finished && shared.playing == shared.asked && current >= 0 && !shared.error[0];

    pthread_mutex_unlock(&lock);
    if (!finished)
        return;
    printf("player: finished %s\n", names[current]);

    int next = repeat == REPEAT_ONE ? current : next_track();

    if (next >= 0) {
        choose(next);
        if (next != current || repeat != REPEAT_ONE)
            gui_notify(window, "Now playing", names[current]);
    } else {
        pthread_mutex_lock(&lock);
        shared.path[0] = '\0';
        shared.asked++;
        pthread_mutex_unlock(&lock);
        current = -1;
        tell_wm(false);
    }
}

// A window that plays sound files, one after another, from a list.
int main(int argc, char **argv) {
    help_option(argc, argv, "usage: player [file]...\n"
                            "Opens a window that plays sound files: WAV, MP3, FLAC, Ogg Vorbis, Opus and AAC.\n"
                            "Each file given goes in its list, with the others in its folder, and the first plays.\n");
    if (!gui_connect()) {
        dprintf(STDERR_FILENO, "player: wm isn't running\n");
        return 1;
    }

    struct gui_ui ui;
    struct gui_message e;
    pthread_t thread;

    window = gui_create_window("Player", 520, 400);
    if (!window || pthread_create(&thread, NULL, player, NULL)) {
        dprintf(STDERR_FILENO, "player: couldn't start\n");
        return 1;
    }
    gui_ui_init(&ui, window);

    int first = -1;

    for (int i = 1; i < argc; i++) {
        int at = open_file(argv[i]);

        first = first < 0 ? at : first;
    }
    if (first >= 0)
        choose(first);
    gui_timer_start(TICK_MS, true);
    frame(&ui);
    printf("player: ready\n");
    fflush(stdout);
    srand((unsigned)getpid());
    while (gui_next_event(&e) && e.type != GUI_CLOSE) {
        if (e.type == GUI_TIMER)
            next_if_finished();

        char path[GUI_PATH_MAX];
        int at = gui_dropped_file(&e, path) && audio_known(path) ? open_file(path) : -1;

        if (at >= 0) {
            printf("player: dropped %s\n", path);
            choose(at);
            frame(&ui);
            fflush(stdout);
        }
        if (gui_ui_event(&ui, &e))
            frame(&ui);
    }
    current = -1;
    tell_wm(false);
    gui_destroy_window(window);
    return 0;
}
