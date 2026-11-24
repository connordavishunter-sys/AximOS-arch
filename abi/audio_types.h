#pragma once

// Shared by the kernel and libc. Each file open on /dev/audio plays what's written
// to it, mixed with the others: 16-bit stereo samples, left then right, at 48000 a
// second unless set otherwise.

#define AUDIO_RATE_DEFAULT 48000
#define AUDIO_FRAME_BYTES 4
#define AUDIO_STREAMS 8 // how many can play at once

// ioctl requests on /dev/audio.
#define AUDIO_SET_RATE 0x4101  // int: samples a second, from 8000 to 192000
#define AUDIO_GET_DELAY 0x4102 // int: bytes written that haven't been heard yet
#define AUDIO_FLUSH 0x4103     // forgets what hasn't been mixed yet

// ioctl requests on /dev/mixer, for everything that plays.
#define MIXER_GET_VOLUME 0x4201 // int: 0 to 100
#define MIXER_SET_VOLUME 0x4202
#define MIXER_GET_MUTE 0x4203 // int: 1 when muted
#define MIXER_SET_MUTE 0x4204
