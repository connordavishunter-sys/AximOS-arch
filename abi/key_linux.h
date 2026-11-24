#pragma once

#include <key_event.h>
#include <linux/input-event-codes.h>
#include <stdint.h>

// From Linux's key codes, which Wayland's keyboards send, to the scancode set 1 codes
// AximOS uses, and back. Linux's are set 1's up to F12, and the others have 0xE0
// first, or are AximOS's own past set 1's codes.

static const struct {
    uint16_t linux_code, code;
} key_linux_extended[] = {
    {KEY_KPENTER, KEY_EXTENDED | 0x1C}, {KEY_RIGHTCTRL, KEY_EXTENDED | 0x1D}, {KEY_KPSLASH, KEY_EXTENDED | 0x35},
    {KEY_SYSRQ, KEY_EXTENDED | 0x37},   {KEY_RIGHTALT, KEY_EXTENDED | 0x38},  {KEY_HOME, KEY_EXTENDED | 0x47},
    {KEY_UP, KEY_EXTENDED | 0x48},      {KEY_PAGEUP, KEY_EXTENDED | 0x49},    {KEY_LEFT, KEY_EXTENDED | 0x4B},
    {KEY_RIGHT, KEY_EXTENDED | 0x4D},   {KEY_END, KEY_EXTENDED | 0x4F},       {KEY_DOWN, KEY_EXTENDED | 0x50},
    {KEY_PAGEDOWN, KEY_EXTENDED | 0x51}, {KEY_INSERT, KEY_EXTENDED | 0x52},   {KEY_DELETE, KEY_EXTENDED | 0x53},
    {KEY_LEFTMETA, KEY_EXTENDED | 0x5B}, {KEY_RIGHTMETA, KEY_EXTENDED | 0x5C}, {KEY_COMPOSE, KEY_EXTENDED | 0x5D},
    {KEY_MUTE, KEY_EXTENDED | 0x20},    {KEY_VOLUMEDOWN, KEY_EXTENDED | 0x2E}, {KEY_VOLUMEUP, KEY_EXTENDED | 0x30},
    {KEY_POWER, KEY_EXTENDED | 0x5E},   {KEY_BRIGHTNESSDOWN, 0x200},          {KEY_BRIGHTNESSUP, 0x201},
};
#define KEY_LINUX_EXTENDED (sizeof(key_linux_extended) / sizeof(key_linux_extended[0]))

// AximOS's code for a Linux key, or 0 for one it has none for.
static inline uint16_t key_from_linux(uint32_t linux_code) {
    if (linux_code <= KEY_F12)
        return linux_code;
    for (unsigned i = 0; i < KEY_LINUX_EXTENDED; i++) {
        if (key_linux_extended[i].linux_code == linux_code)
            return key_linux_extended[i].code;
    }
    return 0;
}

// Linux's code for one of AximOS's, or 0.
static inline uint32_t key_to_linux(uint16_t code) {
    if (code <= KEY_F12)
        return code;
    for (unsigned i = 0; i < KEY_LINUX_EXTENDED; i++) {
        if (key_linux_extended[i].code == code)
            return key_linux_extended[i].linux_code;
    }
    return 0;
}
