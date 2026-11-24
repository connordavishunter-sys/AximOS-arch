#pragma once

#include <stdbool.h>
#include <stdint.h>

// Sound files, read and written as 16-bit samples, interleaved when there are two
// channels. Each kind is done by its own library in /usr/lib/aximos/codecs, loaded when a
// file of that kind is first opened:
//
//     .wav            libwav.so
//     .mp3            libmp3.so      mpg123 and LAME
//     .flac           libflac.so     libFLAC
//     .ogg .oga       libvorbis.so   libvorbis, or libopus.so for Opus in Ogg
//     .opus           libopus.so     libopus
//     .aac            libaac.so      faad2 and faac, as ADTS

struct audio_info {
    int rate;       // samples a second
    int channels;   // 1 or 2; files with more are mixed down to 2
    int64_t frames; // how many samples each channel has, or -1 when it isn't known
    int bitrate;    // kilobits a second: what to encode at, or what a file averages, or 0
};

struct audio_file;

// Opens a file to read, filling in info.
struct audio_file *audio_open(const char *path, struct audio_info *info);

// Reads up to frames samples a channel, returning how many it read, 0 at the end
// and -1 if the file is damaged.
long audio_read(struct audio_file *file, int16_t *samples, long frames);

// Goes to a sample, counting from the start of each channel.
bool audio_seek(struct audio_file *file, int64_t frame);

// Makes a file to write, encoded at info's rate and bitrate, or a default one when
// that's 0. Lossless kinds ignore the bitrate.
struct audio_file *audio_create(const char *path, const struct audio_info *info);
bool audio_write(struct audio_file *file, const int16_t *samples, long frames);

// Finishes writing, and frees either kind. Returns false if the file couldn't be finished.
bool audio_close(struct audio_file *file);

// What went wrong last.
const char *audio_error(void);

// Whether a file's name says it's one of the kinds above.
bool audio_known(const char *path);

// Changes the rate of interleaved samples, carrying on from one call to the next.
// Each call takes all of in, so out must have room for in_frames * to / from + 2.
// Returns how many it wrote.
struct audio_resampler;
struct audio_resampler *audio_resampler_new(int from, int to, int channels);
long audio_resample(struct audio_resampler *r, const int16_t *in, long in_frames, int16_t *out, long out_frames);
void audio_resampler_free(struct audio_resampler *r);

// Playing, through PipeWire, which mixes everything that's playing. A stream takes
// interleaved 16-bit samples, one or two channels, at any rate from 8000 to 192000 a second.
struct audio_stream;

// Opens a stream, or returns NULL without PipeWire, saying why in audio_error.
struct audio_stream *audio_play(int rate, int channels);

// Queues samples, waiting while PipeWire has plenty to play already.
bool audio_stream_write(struct audio_stream *stream, const int16_t *samples, long frames);

// How many frames written haven't been heard yet.
long audio_stream_delay(struct audio_stream *stream);

// Forgets what hasn't been heard.
void audio_stream_flush(struct audio_stream *stream);

// Closes it. What's written still plays to its end.
void audio_stream_close(struct audio_stream *stream);

// The volume of where sound plays now, 0 to 100, which PipeWire keeps across restarts,
// and whether it's muted, plays with low latency, and has a sound card.
struct audio_mixer {
    int volume;
    bool muted, low_latency, card;
};

bool audio_mixer_get(struct audio_mixer *mixer);
bool audio_set_volume(int volume);
bool audio_set_muted(bool muted);
bool audio_set_low_latency(bool low);

// What can be played through: the computer's own speakers and headphones, USB cards
// by their names, and HDMI with a monitor on it. Each keeps its own volume.
struct audio_output {
    char key[64], name[80]; // what stays the same each time it's plugged in, and what to show
    bool current;           // whether it's what's playing
};

// Fills in up to max, returning how many there are, or -1 without PipeWire.
int audio_outputs(struct audio_output *outputs, int max);

// Plays through the output with that key from now on.
bool audio_set_output(const char *key);

// A file that's readable, for poll, when the mixer or the outputs change, filling in
// mixer as it is now, or -1 without PipeWire. audio_mixer_changed reads it, and what
// the mixer is then, returning false once PipeWire's gone.
int audio_mixer_watch(struct audio_mixer *mixer);
bool audio_mixer_changed(int fd, struct audio_mixer *mixer);
