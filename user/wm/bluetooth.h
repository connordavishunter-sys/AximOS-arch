#pragma once

#include <poll.h>
#include <stdbool.h>
#include <stdint.h>

// Bluetooth, as BlueZ has it, over D-Bus: the computer's adapter, turned on and off
// as airplane mode does, through rfkill, and the devices it knows of, paired or found
// nearby while it looks. Connecting and pairing take a while, so they're asked for and
// answered later. wm is BlueZ's agent, so pairing that needs a code shown or
// confirmed asks wm, through bluetooth_request.

#define BT_MAX 32

struct bt_device {
    char path[64], name[64], address[18];
    char icon[32];  // what BlueZ says it is: "audio-headphones", "input-mouse" and the like, or ""
    bool paired, connected;
    int battery;    // percent, or -1
    int16_t rssi;   // how strong it's heard, while looking, or 0
};

struct bluetooth {
    bool running;       // BlueZ is
    bool adapter;       // there's an adapter
    bool powered, blocked, discovering;
    struct bt_device devices[BT_MAX]; // connected first, then paired, then the rest by strength
    int count;
    char busy[64];      // the device being connected, disconnected or paired, or ""
    char doing[16];     // "Connecting", "Disconnecting" or "Pairing"
};

// What pairing wants of whoever's at the computer: a code to check matches the
// device's, or to type on it, or just a yes.
enum bt_asking { BT_NOTHING, BT_CONFIRM, BT_AUTHORIZE, BT_SHOW_CODE };

struct bt_request {
    enum bt_asking asking;
    char device[64]; // its name
    char code[16];
};

bool bluetooth_open(void);

// What to poll, and how long poll may wait, -1 for as long as it likes.
void bluetooth_poll(struct pollfd *fd, int *timeout);

// Takes what's come, after poll, returning true if anything shown has changed. done
// is 1 when what was busy worked, -1 when it didn't, with why in bluetooth_error.
bool bluetooth_read(int *done);

const struct bluetooth *bluetooth_now(void);
const struct bt_request *bluetooth_request(void);
const char *bluetooth_error(void);

bool bluetooth_set_on(bool on);
void bluetooth_look(bool on); // for devices nearby, while the panel's open

// Each of these starts, and bluetooth_read says how it went.
bool bluetooth_connect(const char *path);    // pairing it first, if it isn't
bool bluetooth_disconnect(const char *path);
bool bluetooth_forget(const char *path);     // straight away

void bluetooth_answer(bool yes); // to what bluetooth_request asks
