#pragma once

#include <poll.h>
#include <stdbool.h>
#include <stdint.h>

// The network, as NetworkManager has it, over D-Bus: Wi-Fi networks nearby, which
// can be joined, left and forgotten, a cable if one's plugged in, and the address
// of whichever's in use. Networks joined from here belong to whoever's logged in,
// as NetworkManager lets them make those without asking for an admin's password.

#define WIFI_MAX 32
#define SSID_MAX 33 // 32 bytes, and the zero after

struct wifi {
    char ssid[SSID_MAX];
    int strength; // out of 100
    bool secured, saved, active;
};

struct network {
    bool running;           // NetworkManager is
    bool wifi_card, wifi_on; // there's a Wi-Fi card, and Wi-Fi's on, which airplane mode turns off
    bool cable, wired;       // a wired card, and whether it's connected
    char wired_name[16];     // that card's
    uint32_t address, netmask, gateway, dns; // of what's in use, or 0
    uint64_t rx_bytes, tx_bytes;
    struct wifi wifi[WIFI_MAX]; // the joined one first, then the strongest
    int wifi_count;
    char joining[SSID_MAX]; // the network being joined, or ""
};

// Connects to the system's D-Bus, returning false without it.
bool network_open(void);

// The connection, for poll, filled in with what to poll it for and how long poll
// may wait, in milliseconds, which is -1 for as long as it likes.
void network_poll(struct pollfd *fd, int *timeout);

// Takes what's come, after poll, returning true if the network's changed. done is
// set to what joining just came to: 1 joined, -1 couldn't, or 0 for neither.
bool network_read(int *done);

const struct network *network_now(void);

void network_scan(void); // for networks nearby, which come in as they're found
// Joins a network, with its password if it has one and isn't saved. A hidden one is
// asked for by name, as it doesn't say it's there. False says why in network_error.
bool network_join(const char *ssid, const char *password, bool hidden);
bool network_leave(void); // the Wi-Fi network joined
bool network_forget(const char *ssid);
bool network_set_wifi(bool on);
const char *network_error(void);
