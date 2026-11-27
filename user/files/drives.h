#pragma once

#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>

// Drives plugged in, USB sticks, cards and discs, from udisks2, which mounts them
// for whoever's logged in, as polkit lets it without a password.

#define DRIVES_MAX 16

struct drive {
    char block[128], drive[128]; // udisks2's objects: its filesystem's, and the stick or disc it's on
    char name[64];               // its label, or its size
    char mount[PATH_MAX];        // where it's mounted, or ""
};

extern struct drive drives[DRIVES_MAX];
extern int drive_count;

// Connects to udisks2, listing what's plugged in, or returns false if it's not there.
bool drives_open(void);
// What to poll for, and when, for drives_read.
struct pollfd drives_poll(int *timeout);
// Takes what udisks2 said, returning whether the list changed.
bool drives_read(void);

// Mounts a drive, as it's opened, or says why it couldn't be into error.
bool drive_mount(struct drive *d, char *error, size_t size);
// Unmounts all of the stick or disc a drive's on, and powers it off, or ejects it, so
// it can be taken out, or says why not into error.
bool drive_eject(const struct drive *d, char *error, size_t size);
