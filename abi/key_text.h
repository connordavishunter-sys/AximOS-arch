#pragma once

#include <key_event.h>
#include <stdint.h>

// What a key types, as the console and key_event.text have it: a US layout, shared
// by the kernel and libgui on Linux.

// Scancode set 1. The keypad acts as if num lock were always on.
static const char keymap[] = {
    0,    0x1B, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0x7F, '\t',
    'q',  'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\r', 0,  'a',  's',
    'd',  'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,  '\\', 'z', 'x', 'c',  'v',
    'b',  'n', 'm', ',', '.', '/', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,    0,
    0,    0,   0,   0,   0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+',  '1',
    '2',  '3', '0', '.',
};

static const char keymap_shift[] = {
    0,    0x1B, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0x7F, '\t',
    'Q',  'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\r', 0,  'A',  'S',
    'D',  'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,   '|', 'Z', 'X', 'C',  'V',
    'B',  'N', 'M', '<', '>', '?', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,    0,
    0,    0,   0,   0,   0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+',  '1',
    '2',  '3', '0', '.',
};

// The character a key types with modifiers held, or 0.
static inline char key_event_text(uint16_t code, uint8_t modifiers) {
    if (code == KEY_EXTENDED + KEY_ENTER)
        return '\r';
    if (code == (KEY_EXTENDED | 0x35))
        return '/';
    if (code >= sizeof(keymap))
        return 0;

    char c = modifiers & KEY_MOD_SHIFT ? keymap_shift[code] : keymap[code];
    bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');

    if (letter && (modifiers & KEY_MOD_CAPS_LOCK))
        c ^= 0x20;
    if (letter && (modifiers & KEY_MOD_CTRL))
        c &= 0x1F;
    return c;
}
