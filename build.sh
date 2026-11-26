#!/usr/bin/env bash
# Builds AximOS in one go, installing whatever the build needs first, on Arch or a
# system based on it.
#   ./build.sh          the live ISO, build/aximos.iso (the packages, then the ISO)
#   ./build.sh packages just the pacman packages, into build/repo
#   ./build.sh all      wm and the apps on this system, into build/
# Packages are signed with AximOS's key when its secret half is here, and otherwise
# built unsigned, which the ISO takes, as it installs them from build/repo itself.
set -euo pipefail
cd "$(dirname "$0")"

target=${1:-iso}
case $target in iso|packages|all) ;; *) echo "usage: $0 [iso|packages|all]" >&2; exit 1 ;; esac

command -v pacman >/dev/null || { echo "build.sh needs Arch Linux or a system based on it." >&2; exit 1; }
[ "$EUID" -ne 0 ] || { echo "Run build.sh as yourself, not root: it uses sudo where it must." >&2; exit 1; }

# Packages are built in a clean chroot, and the ISO by archiso, so those need only
# the tools; building here needs what the PKGBUILD builds with.
needed=(base-devel git gnupg)
case $target in
    iso) needed+=(devtools archiso) ;;
    packages) needed+=(devtools) ;;
    all) mapfile -t -O ${#needed[@]} needed < <(source packaging/PKGBUILD && printf '%s\n' "${makedepends[@]}") ;;
esac
mapfile -t missing < <(pacman -T "${needed[@]}" || true)
if [ ${#missing[@]} -gt 0 ]; then
    echo "Installing ${missing[*]}"
    sudo pacman -S --needed --noconfirm "${missing[@]}"
fi

key=$(sed -n 's/^SIGNING_KEY ?= //p' Makefile)
if ! gpg --list-secret-keys "$key" >/dev/null 2>&1; then
    echo "AximOS's signing key isn't here: building unsigned packages."
    export SIGNING_KEY=
fi

jobs=$(nproc)
case $target in
    iso) make -j"$jobs" packages && make iso ;;
    *) make -j"$jobs" "$target" ;;
esac
echo "Done: $(case $target in iso) echo build/aximos.iso ;; packages) echo build/repo ;; all) echo build/ ;; esac)"
