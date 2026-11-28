# Contributing to AximOS

Thanks for wanting to help. AximOS is a desktop for Arch Linux: its own window manager,
wm, on wlroots, and its own apps, on libgui. README.md says how it all fits together.

## Questions, ideas and bugs

- **Bugs** go in [Issues](https://github.com/connordavishunter-sys/AximOS-arch/issues).
  Say what you did, what you expected and what happened. Say too whether it was the
  live ISO, an installed system or the VM, and at 1x or 2x. A screenshot helps,
  and so does wm's log. Save the screen with `kill -USR1` on wm, which writes
  `$XDG_RUNTIME_DIR/wm-screen.png`.
- **Questions and ideas** go in
  [Discussions](https://github.com/connordavishunter-sys/AximOS-arch/discussions).
  An idea that's agreed on there becomes an issue.

## Sending a change

1. Fork the repository, and make a branch off `main` for your change.
2. Build it, and test it in the VM (below).
3. Push the branch to your fork, and open a pull request against `main`.

Keep each pull request to one change. Small fixes are welcome as they are. For a big
change, start a discussion first, so nobody spends a week on something that won't
fit.

Releases are published from `main` by the maintainer, as they're signed with
AximOS's key. Once your change is merged, it goes out with the next release.

## Building

On Arch, or anything based on it:

```bash
./build.sh
```

That installs what the build needs, builds the packages in a clean chroot and makes
the live ISO, `build/aximos.iso`. Without AximOS's signing key the packages are
unsigned, which is fine for testing. `make` alone builds wm and the apps into
`build/`, for a quick compile check.

## Testing

The tests boot the ISO in QEMU and drive it, with the keyboard and mouse, through
the serial port's root shell:

```bash
python3 tests/run.py
```

Name a test to run just that one, for example `python3 tests/term_test.py`. Set
`TEST_SCALE=2` to run it at 2560x1600, which wm draws at 2x.

A pull request needs:

- **A test for a new feature.** Add it in `tests/`, or extend the test that's
  closest. It should check what someone using AximOS would see working. Look at
  `tests/drives_test.py` or `tests/term_test.py` to see how.
- **Tests passing at 1x and 2x.** Run the ones your change touches both ways.
- **Screenshots for anything that looks different.** Add before and after shots to
  the pull request. The tests save theirs with `vm.save_screenshot()`.
- **Docs updated.** When a feature lands, update README.md's Status, and `docs/` if it
  changes how something works.

## How the code's written

- It's C, built by the Makefile, with no other build system. Match the style of
  the code around yours.
- Comments and docs are plain prose. They say what something does for the person
  using it, in short sentences, with British spelling (colour).
- libgui is immediate-mode: a window's frame is drawn between `gui_ui_begin` and
  `gui_ui_end`. Use its widgets instead of drawing your own.
- Icons are drawn in `tools/icons.py`, which writes `share/icons/name.png` and
  `name@2x.png`. Add new ones there, and don't add PNGs from elsewhere.
- Ask in the pull request before adding a new dependency. When one's agreed, add it to
  `packaging/PKGBUILD` and `iso/packages.x86_64`.

## AI tools

You're welcome to use AI tools such as Claude Code. If you do, say so in the pull
request, and say which tools. You're still the one sending the change. Test it, and
make sure you understand it, as you would any code of your own. CLAUDE.md tells Claude
Code how this project works, so it's read by itself in every session.

## License

AximOS is under the GNU General Public License, version 3 or later (LICENSE). By
sending a change, you agree to it being under that license too.
