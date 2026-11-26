#pragma once

#include <poll.h>
#include <stdbool.h>

// wm as polkit's authentication agent for the session: when an app asks for
// something that needs an admin, such as Settings setting the clock, polkit asks
// wm, which asks for a password, and checks it through polkit's own helper.

struct polkit_request {
    bool asking;          // a password's wanted
    bool checking;        // the one given is being checked
    bool wrong;           // the last one given wasn't right
    char message[256];    // what it's for, as polkit says
    char user[64];        // whose password
};

// Registers with polkit on the system's D-Bus, for the session wm's in, returning
// false where there's no bus, no polkit, or no session.
bool polkit_open(void);

// What to poll, filling two, the bus and the helper, and how long poll may wait.
void polkit_poll(struct pollfd fds[2], int *timeout);

// Takes what's come, after poll, returning true if the request's changed.
bool polkit_read(void);

const struct polkit_request *polkit_now(void);

void polkit_answer(const char *password); // checks it
void polkit_cancel(void);                 // says no
