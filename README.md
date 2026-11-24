# AximOS on Arch

AximOS's desktop on Arch Linux: **wm**, AximOS's window manager, as a Wayland compositor
on wlroots, with XWayland, so ordinary Linux apps open in its windows beside AximOS's
own. It keeps everything that made it AximOS's: its frames, taskbar, tray, start menu,
workspaces, animations, glass and drawing. Arch does the rest: systemd, NetworkManager,
PipeWire and UPower in place of AximOS's own init, netd and audiod.

AximOS began with a kernel of its own, then moved onto an LFS-style Linux with its own
userland, in the AximOS repository. This repository takes its desktop and apps onto Arch.

## Status

Phase one: wm on wlroots, libgui's apps as Wayland clients, Wayland and X apps in wm's
windows, PipeWire for sound, greetd with wm's own log-in screen, pacman packages, and a
live ISO that boots straight into the desktop, tested in QEMU.

Still to come: Settings and the tray as front ends for NetworkManager, UPower and
timedated; an installer; dragging files between wm's desktop and apps; screens beyond
the first; and drawing at the screen's own resolution at 2x, which apps' HiDPI buffers
are scaled down from for now.

## How it fits together

- **wm** (`user/wm`) is `wm.c`, the window manager as it was, and `server.c`, its
  wlroots server. wlroots has the screen, through DRM, the input, through libinput,
  and the session, through libseat. wm draws the whole desktop in memory, as it always
  has, which is put on the screen as a texture each frame. Apps get only shared memory
  buffers, which are copied into wm's windows as they commit, so wm can blur, round and
  animate them like its own.
- **libgui** (`user/libgui`) is a Wayland client: each window is an xdg-shell toplevel,
  drawn from shared memory, with the clipboard and dragging files on Wayland's data
  device. That's cleaner than keeping its own socket protocol beside Wayland: wm has one
  kind of app, not two, and libgui's apps run on any Wayland desktop. What Wayland has no
  protocol for goes through AximOS's own, `protocol/aximos-shell-v1.xml`: notifications
  with buttons, blur behind see-through windows, the screen's size and scale, settings
  changing, and the log-in screen unlocking.
- **libaudio** (`user/libaudio`) plays through PipeWire, as PulseAudio, which
  pipewire-pulse serves, and reads and writes sound files with Arch's codec libraries.
- **Logging in**: greetd runs `wm --login`, whose log-in screen (`user/login`) logs in
  through greetd's socket. The lock screen checks the password with PAM.

## Building

On Arch, or anything with Arch's packages:

```bash
sudo pacman -S --needed base-devel wlroots0.20 wayland-protocols libinput libxkbcommon pixman libpulse pam stb mpg123 lame flac libvorbis opus faad2 faac
```

```bash
make
```

`make install` puts it under `/usr`, or `DESTDIR` and `PREFIX`. `make packages` builds
the pacman packages, `aximos-libgui`, `aximos-apps` and `aximos-desktop`, in a clean
chroot of Arch's own repositories, which needs devtools. `make iso` makes
`build/aximos.iso`, which needs archiso, and `make test` boots it in QEMU and runs the
tests in `tests/`.

wm runs in a window on another Wayland desktop as well as on the screen itself:
`WM_NESTED=1 wm foot` starts it with foot in it, and `WLR_BACKENDS=headless` runs it
with no screen at all. `kill -USR1` saves what it's showing to
`$XDG_RUNTIME_DIR/wm-screen.png`.

## The live ISO

It boots straight into the desktop as `aximos`, whose password, for the lock screen, is
`aximos`. It has Arch's base and kernel, NetworkManager, PipeWire with WirePlumber,
UPower, `nvidia-open` for NVIDIA's GPUs, `foot` and `xterm`. The serial port has a root
shell, for the tests.
