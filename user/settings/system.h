#pragma once

#include <stdbool.h>
#include <stdint.h>

// What Settings shows of the system, from its services on D-Bus: UPower's battery,
// power-profiles-daemon's modes, timedated's clock and hostnamed's name. What
// needs an admin asks polkit, which asks wm for a password, so those calls can
// take as long as typing it does. Each returns false, saying why in system_error,
// if it couldn't.

struct battery {
    bool present;
    double percent;
    char state[24];          // "charging", "discharging", "full" or "not charging"
    int64_t seconds_left;    // to empty or to full, or 0 if it isn't known
    double health;           // what it holds full, out of 100 of what it did new, or 0
    uint32_t cycles;         // or 0 if it isn't known
    char model[64];
};

struct power_modes {
    bool available;
    char active[32];
    char names[3][32]; // power-saver, balanced, performance, as there are
    int count;
};

struct clock_state {
    bool ntp, can_ntp, synced;
    char zone[64];
};

struct about {
    char hostname[64], os[96], kernel[64], cpu[96], gpu[2][96], model[96];
    int gpus;
    uint64_t memory, disk_total, disk_free; // bytes
};

bool system_open(void);
const char *system_error(void);

bool battery_read(struct battery *b);
bool power_modes_read(struct power_modes *m);
bool power_mode_set(const char *name);

bool clock_read(struct clock_state *c);
bool clock_set_ntp(bool on);
bool clock_set(int64_t seconds); // since 1970, UTC
bool clock_set_zone(const char *zone);

void about_read(struct about *a);
bool hostname_set(const char *name);
