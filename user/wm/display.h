#pragma once

#include <key_event.h>
#include <mouse_event.h>
#include <poll.h>
#include <stdint.h>

// Where wm shows the screen and takes its input from: wlroots, which puts it on the
// screen itself through DRM, or in a window on another Wayland or X desktop.

#define DISPLAY_FDS 32

struct screen {
    uint8_t *pixels; // 32-bit XRGB, pitch bytes to a row
    int width, height, pitch;
    bool can_set_mode;
};

struct input {
    void (*mouse)(const struct mouse_event *e);
    void (*key)(const struct key_event *e);
    void (*quit)(void); // as a window being closed asks
};

struct display {
    bool (*open)(struct screen *screen);
    bool (*set_mode)(struct screen *screen, int width, int height); // false leaves it as it was
    void (*changed)(int x, int y, int width, int height);           // pixels drawn since the last show
    void (*show)(void);
    int (*poll_fds)(struct pollfd *fds); // up to DISPLAY_FDS of them, returning how many
    // Takes what input there is, after poll. True if the screen changed size.
    bool (*read)(const struct pollfd *fds, struct screen *screen, const struct input *input);
    void (*set_repeat)(int delay, int rate); // as KEYBOARD_SET_REPEAT_DELAY and _RATE
    void (*set_scale)(int scale);            // how many of the screen's pixels each of the desktop's is across
    int max_wait; // the longest poll may wait, in milliseconds, for input that can't be polled, or -1
    void (*switch_session)(int number); // to show another console, or NULL where there are none
};
