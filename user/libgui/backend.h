#pragma once

#include <gui.h>

// What events.c needs from whatever shows the windows, which is Wayland. It waits
// until something arrives, or due, in milliseconds by gui_now_ms, if that isn't -1,
// and gives what arrives to gui_post. False means the compositor's gone.
bool gui_backend_wait(int64_t due);

// When the backend has something of its own due, by gui_now_ms, or -1.
int64_t gui_backend_due(void);

// How many events are waiting for gui_next_event.
int gui_queued(void);

// Tells the compositor, and through it every program, that the settings were saved.
void gui_backend_settings_changed(void);

int64_t gui_now_ms(void);
