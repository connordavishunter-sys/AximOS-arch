# Working on AximOS-arch

AximOS's desktop (wm, libgui and its apps) on Arch. README.md covers how it fits
together; docs/scene.md covers the GPU compositing. This file is how we work on it.

## The loop for every change

1. Build and check it on the host: `make -j20` (see "Building on the host").
2. Rebuild the packages and ISO, and test it in the QEMU VM:
   `make packages > build/packages.log && make iso`, then `python3 tests/<name>_test.py`,
   at 1x and at 2x (`TEST_SCALE=2 python3 tests/<name>_test.py`). `python3 tests/run.py`
   runs them all. Look at the screenshots the tests save, too.
3. Ask "Commit, push and publish as rN?". Never commit or push without that yes.
4. On "Yes, publish rN": commit, `git push`, then
   `make packages > build/packages.log && make publish`. The laptop updates from that
   through Settings → Updates.

Publishing needs AximOS's signing key, so only the maintainer does step 4. Contributors
stop at a tested change, and open a pull request.

rN is the commit count (`git rev-list --count HEAD`, the `.rN.` in `VERSION`), so a
release made of two commits skips a number. Say the number it'll actually be.

## Building on the host

SceneFX 0.5 isn't installed on the host, only in the chroot. It's extracted into
`build/host-scenefx` already, so `PKG_CONFIG_PATH=$PWD/build/host-scenefx/pc make -j20`
works. If `build/` is cleaned, extract it again from the package cache:

```bash
mkdir -p build/host-scenefx && bsdtar -xf build/package-cache/scenefx-0.5-*.pkg.tar.zst -C build/host-scenefx usr
```

Then write `build/host-scenefx/pc/scenefx-0.5.pc` with `prefix` set to
`build/host-scenefx/usr` (absolute), `Cflags: -I${includedir}/scenefx-0.5`,
`Libs: -L${libdir} -lscenefx-0.5`, and build with
`PKG_CONFIG_PATH=$PWD/build/host-scenefx/pc make -j20`.

`./build.sh` installs what's missing and builds everything in one go (`iso`, the
default, `packages` or `all`). Without the signing key, packages build unsigned.

## Tests

- `tests/vm.py` boots `build/aximos.iso` in QEMU. Its serial port is a root shell:
  `vm.run(cmd)`, `vm.keys(text)`, `vm.key('ctrl-shift-t')`, `vm.pointer(x, y, buttons)`
  (right button is 2), `vm.click`, `vm.command(...)` for the QEMU monitor (such as
  plugging in USB drives), `vm.save_screenshot(name)`.
- Apps and wm log what tests check for (`/tmp/<app>.log`, `vm.wm_log()`). `said()`
  waits for a line, `window_at()` finds a window from wm's log. Apps print their state
  on SIGUSR1 (the terminal prints its screen, row by row).
- wm logs a window by its title (the terminal's is "aximos@aximos:~"), not by app name.
- A test's docstring says, in plain words, what the user sees working; each `check()`
  is one sentence of it.

## Conventions

- Comments and docs are plain prose, saying what something is or does for the user, in
  short sentences, British spelling (colour). Match the style of what's around.
- Icons are drawn by `tools/icons.py` at 32px (`name@2x.png`), with new ones also
  downscaled to 16px: add the name to the "new ones" tuple there. Run it after adding
  drawings; both PNGs go in `share/icons/`.
- libgui is immediate-mode (`gui_ui_begin`/`gui_ui_end`). Menu items with icons use
  `gui_menu_icon_item(ui, label, gui_icon("name"))`.
- New runtime dependencies go in `packaging/PKGBUILD` and `iso/packages.x86_64`; new
  link flags in the Makefile's `LIBS_<app>`.
- Update README.md's Status and docs/scene.md when a feature lands.

## Ground rules

- Never write to the host's own drives (on the maintainer's laptop, nvme0n1) without
  asking. The installer and its tests only ever touch the VM's disks.
- sudo without a password is fine for reads and the build's own steps (mkarchroot,
  mkarchiso); ask before anything else.
- Keep personal details out of commits, issues and anything sent outside; the repo is
  public.

## What's been done lately (r16–r24)

Corners and icon fringes, build.sh, GPU animations (window copies sharing textures),
fullscreen windows (Super+F), USB drives in Files over udisks2, the closing-window flash
fix, terminal line reflow on resize, the terminal on libgui's tabs, find bar and menus,
and icons in every right-click menu and menu bar.

## Ideas for next

1. Video in the Player: FFmpeg decoding, frames straight to the GPU.
2. A PDF viewer on Poppler: scrolling, zoom, find.
3. Typed expressions in the Calculator, with brackets, powers and a scientific mode.
4. Terminal: split panes, Ctrl+= / Ctrl+- text size, Ctrl+click links.
5. Taskbar: a calendar flyout from the clock, and notification history.
6. Variable refresh rate for fullscreen games and video.
7. Screens beyond the first (README's "still to come").
