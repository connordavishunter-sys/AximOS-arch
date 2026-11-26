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

Phase two, under way: the tray's network icon opens a Wi-Fi panel, on NetworkManager,
listing the networks nearby and the cable, joining open, secured and hidden networks,
leaving and forgetting them, and turning Wi-Fi off, as airplane mode does too. The
brightness keys and slider set the backlight through logind, as its file is root's. On
screens 2560 wide or more, everything's drawn at 2x, at the screen's own resolution:
wm, libgui's apps, whose canvases are laid out as at 1x and drawn twice as sharp, and
Wayland apps such as foot, at their own 2x. X apps draw at 1x, made twice the size.
The live ISO's installer, in System, puts AximOS on a drive: in place of an AximOS
already on it, in its free space, or over the whole of it. Settings is the system's
front end: its Network, Power, Date & time and About pages are NetworkManager's,
UPower's and power-profiles-daemon's, timedated's and hostnamed's, and wm asks for an
admin's password, as polkit's agent, when a change needs one. Taskbar buttons have a
menu, the desktop's makes new folders and files and opens Settings, and things are
renamed where their names are, on the desktop and in Files.

Still to come: updating an installed system; dragging
files between wm's desktop and apps; and screens beyond the first.

## How it fits together

- **wm** (`user/wm`) is `wm.c`, the window manager as it was, and `server.c`, its
  wlroots server. wlroots has the screen, through DRM, the input, through libinput,
  and the session, through libseat. wm draws the whole desktop in memory, as it always
  has, which is put on the screen as a texture each frame. Apps get only shared memory
  buffers, which are copied into wm's windows as they commit, so wm can blur, round and
  animate them like its own. At 2x, wm and every window are drawn at the screen's own
  resolution, and only laid out in the desktop's pixels, each two of the screen's across.
- **Icons** are 16x16, with a 32x32 one for 2x beside each, `name@2x.png`, which
  `tools/icons.py` draws.
- **libgui** (`user/libgui`) is a Wayland client: each window is an xdg-shell toplevel,
  drawn from shared memory, with the clipboard and dragging files on Wayland's data
  device. That's cleaner than keeping its own socket protocol beside Wayland: wm has one
  kind of app, not two, and libgui's apps run on any Wayland desktop. What Wayland has no
  protocol for goes through AximOS's own, `protocol/aximos-shell-v1.xml`: notifications
  with buttons, blur behind see-through windows, the screen's size and scale, settings
  changing, and the log-in screen unlocking.
- **libaudio** (`user/libaudio`) plays through PipeWire, as PulseAudio, which
  pipewire-pulse serves, and reads and writes sound files with Arch's codec libraries.
- **The network** (`user/wm/network.c`) is NetworkManager's, over D-Bus with sd-bus, on
  wm's own poll loop. Networks joined from the panel belong to whoever's logged in, as
  NetworkManager lets them make those without an admin's password.
- **Admin rights** come from polkit, whose agent wm is (`user/wm/polkit.c`): it
  shows its own dialog for the password, which polkit's helper checks, through its
  socket. Settings' system pages (`user/settings/system.c`) ask for what needs it
  as interactive, so polkit asks wm, and the network's is wm's own `network.c`.
- **Installing**: the installer (`user/installer`) asks which drive, how, the first
  account, the computer's name and the time zone, then runs `aximos-install`
  (`system/aximos-install`) as root. That copies the live system onto an ext4 root,
  takes out what only the live system has (its account, logging in by itself, the
  serial port's root shell, the tests' tools and itself, and the development tools
  unless they were asked for), and makes the account. Limine boots it, from AximOS's
  own boot partition, with a menu of every other system's EFI loader it finds, so a
  system beside it, such as CachyOS, stays in the menu.
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
UPower, `nvidia-open` for NVIDIA's GPUs, `foot`, `xterm` and DejaVu's fonts. The serial port has a root
shell, for the tests, and hostapd and dnsmasq, which the Wi-Fi test makes access points
of simulated radios with.
