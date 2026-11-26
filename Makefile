# AximOS's desktop on Arch: wm, a Wayland compositor on wlroots, libgui and its apps,
# built against Arch's own packages. make builds them into build/, make install puts
# them in DESTDIR, as the PKGBUILDs do, and make iso makes a live ISO of Arch with them.

PREFIX ?= /usr
DESTDIR ?=
BUILD := build
PROTOCOLS := $(BUILD)/protocol
WAYLAND_PROTOCOLS := $(shell pkg-config --variable=pkgdatadir wayland-protocols)
WLR := wlroots-0.20

CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=gnu23 -Wall -Wextra -Wno-format-truncation -Wno-unused-parameter -D_GNU_SOURCE -fPIC \
	-Iabi -Iuser/libgui/include -Iuser/libaximos/include -Iuser/libaudio/include -I$(PROTOCOLS) -isystem /usr/include/stb \
	-DGUI_PREFIX='"$(PREFIX)"' -DAUDIO_CODECS='"$(PREFIX)/lib/aximos/codecs/"'
LDFLAGS += -L$(BUILD)/lib -Wl,-rpath-link,$(BUILD)/lib

# What libgui, wm and the codecs need from Arch.
GUI_PACKAGES := wayland-client xkbcommon
WM_PACKAGES := $(WLR) wayland-server xkbcommon libinput pixman-1 libdrm libsystemd
AUDIO_PACKAGES := libpulse libpulse-simple
CODECS := wav mp3 flac vorbis opus aac
CODEC_LIBS_mp3 := -lmpg123 -lmp3lame
CODEC_LIBS_flac := -lFLAC
CODEC_LIBS_vorbis := -lvorbisfile -lvorbisenc -lvorbis -logg
CODEC_LIBS_opus := -lopus -logg
CODEC_LIBS_aac := -lfaad -lfaac
CODEC_FLAGS_opus := -I/usr/include/opus

# Protocols: Wayland's own, for libgui and wm, and AximOS's.
CLIENT_PROTOCOLS := xdg-shell xdg-decoration-unstable-v1 cursor-shape-v1 tablet-unstable-v2 aximos-shell-v1
SERVER_PROTOCOLS := xdg-shell cursor-shape-v1 tablet-unstable-v2 aximos-shell-v1
protocol_xml = $(if $(filter aximos-%,$(1)),protocol/$(1).xml,$(firstword $(wildcard \
	$(WAYLAND_PROTOCOLS)/stable/$(1:-v1=)/$(1).xml $(WAYLAND_PROTOCOLS)/stable/xdg-shell/$(1).xml \
	$(WAYLAND_PROTOCOLS)/staging/$(1:-v1=)/$(1).xml $(WAYLAND_PROTOCOLS)/unstable/$(firstword $(subst -unstable, ,$(1)))/$(1).xml)))
CLIENT_HEADERS := $(CLIENT_PROTOCOLS:%=$(PROTOCOLS)/%-client-protocol.h)
SERVER_HEADERS := $(SERVER_PROTOCOLS:%=$(PROTOCOLS)/%-protocol.h)
CLIENT_CODE := $(CLIENT_PROTOCOLS:%=$(PROTOCOLS)/%-client.c)
SERVER_CODE := $(PROTOCOLS)/aximos-shell-v1-server.c

# The apps, each from its folder in user/, in the start menu's folders, or at the top
# for those in none. GUI_TOOLS use libgui without being in the start menu.
APP_FOLDERS := Accessories Games System Demos
APPS_Accessories := calculator editor paint viewer player
APPS_Games := snake
APPS_System := settings tasks installer
APPS_Demos := guidemo widgets
APPS := term files $(foreach f,$(APP_FOLDERS),$(APPS_$(f)))
app_dir = $(strip $(foreach f,$(APP_FOLDERS),$(if $(filter $(1),$(APPS_$(f))),$(f)/)))
GUI_TOOLS := run notify
LIBEXEC_TOOLS := login
LIBS_player := -laudio -lpthread
LIBS_settings := -laudio -lsystemd
EXTRA_settings := user/wm/network.c # the network, as wm's tray has it
FLAGS_settings := -Iuser/wm
LIBS_login := -lpam

LIBAXIMOS := $(BUILD)/lib/libaximos.so
LIBGUI := $(BUILD)/lib/libgui.so
LIBAUDIO := $(BUILD)/lib/libaudio.so
CODEC_PATHS := $(CODECS:%=$(BUILD)/lib/codecs/lib%.so)
WM := $(BUILD)/bin/wm
APP_PATHS := $(APPS:%=$(BUILD)/apps/%)
TOOL_PATHS := $(GUI_TOOLS:%=$(BUILD)/bin/%) $(LIBEXEC_TOOLS:%=$(BUILD)/libexec/%)

.PHONY: all install iso packages publish test clean help
.SECONDEXPANSION:

all: $(WM) $(APP_PATHS) $(TOOL_PATHS) $(CODEC_PATHS)

$(PROTOCOLS)/%-client-protocol.h: $$(call protocol_xml,$$*)
	@mkdir -p $(@D)
	wayland-scanner client-header $< $@
$(PROTOCOLS)/%-protocol.h: $$(call protocol_xml,$$*)
	@mkdir -p $(@D)
	wayland-scanner server-header $< $@
$(PROTOCOLS)/%-client.c: $$(call protocol_xml,$$*)
	@mkdir -p $(@D)
	wayland-scanner private-code $< $@
$(PROTOCOLS)/%-server.c: $$(call protocol_xml,$$*)
	@mkdir -p $(@D)
	wayland-scanner private-code $< $@

$(LIBAXIMOS): $(wildcard user/libaximos/*.c user/libaximos/include/*.h)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -shared -Wl,-soname,libaximos.so $(filter %.c,$^) $(LDFLAGS) -lcrypt -o $@

$(LIBAUDIO): $(wildcard user/libaudio/*.c user/libaudio/*.h user/libaudio/include/*.h)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(shell pkg-config --cflags $(AUDIO_PACKAGES)) -shared -Wl,-soname,libaudio.so $(filter %.c,$^) \
		$(LDFLAGS) $(shell pkg-config --libs $(AUDIO_PACKAGES)) -ldl -lm -o $@

# Each kind of sound file is read and written by a library of its own, which libaudio
# loads when one's first opened.
$(CODEC_PATHS): $(BUILD)/lib/codecs/lib%.so: user/codecs/%.c $(LIBAUDIO)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CODEC_FLAGS_$*) -shared -Wl,-soname,lib$*.so $< $(LDFLAGS) $(CODEC_LIBS_$*) -laudio -lm -o $@

$(LIBGUI): $(wildcard user/libgui/*.c user/libgui/*.h user/libgui/include/*.h) $(CLIENT_HEADERS) $(CLIENT_CODE) \
		$(LIBAXIMOS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(shell pkg-config --cflags $(GUI_PACKAGES)) -shared -Wl,-soname,libgui.so \
		$(filter %.c,$^) $(LDFLAGS) $(shell pkg-config --libs $(GUI_PACKAGES)) -ldl -lm -o $@

$(APP_PATHS): $(BUILD)/apps/%: $$(wildcard user/$$*/*.c user/$$*/*.h) $$(EXTRA_$$*) $(LIBGUI) $(LIBAUDIO)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(FLAGS_$*) $(filter %.c,$^) $(LDFLAGS) -lgui -laximos $(LIBS_$*) -lm -o $@

$(TOOL_PATHS): $$(wildcard user/$$(notdir $$@)/*.c) $(LIBGUI)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(filter %.c,$^) $(LDFLAGS) -lgui -laximos $(LIBS_$(notdir $@)) -lm -o $@

$(WM): $(wildcard user/wm/*.c user/wm/*.h) $(SERVER_HEADERS) $(SERVER_CODE) $(LIBGUI) $(LIBAUDIO)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -DWLR_USE_UNSTABLE $(shell pkg-config --cflags $(WM_PACKAGES)) $(filter %.c,$^) $(LDFLAGS) \
		$(shell pkg-config --libs $(WM_PACKAGES)) -lgui -laximos -laudio -lm -o $@

# Where each part goes, under PREFIX: wm and the tools in bin, the libraries in lib,
# the apps and what only the desktop runs in lib/aximos, and the rest in share/aximos.
install: all
	install -Dm755 -t $(DESTDIR)$(PREFIX)/bin $(WM) $(GUI_TOOLS:%=$(BUILD)/bin/%)
	install -Dm755 -t $(DESTDIR)$(PREFIX)/lib $(LIBAXIMOS) $(LIBGUI) $(LIBAUDIO)
	install -Dm755 -t $(DESTDIR)$(PREFIX)/lib/aximos/codecs $(CODEC_PATHS)
	install -Dm755 -t $(DESTDIR)$(PREFIX)/lib/aximos $(LIBEXEC_TOOLS:%=$(BUILD)/libexec/%)
	$(foreach a,$(APPS),install -Dm755 $(BUILD)/apps/$(a) $(DESTDIR)$(PREFIX)/lib/aximos/apps/$(call app_dir,$(a))$(a) &&) true
	install -d $(DESTDIR)$(PREFIX)/share/aximos
	cp -r share/. $(DESTDIR)$(PREFIX)/share/aximos/
	# Text is DejaVu Sans, and terminals' bold DejaVu Sans Mono's, from ttf-dejavu.
	ln -sf /usr/share/fonts/TTF/DejaVuSans.ttf /usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf $(DESTDIR)$(PREFIX)/share/aximos/fonts/
	install -Dm644 -t $(DESTDIR)$(PREFIX)/include/aximos user/libgui/include/gui.h abi/key_event.h abi/mouse_event.h
	install -Dm755 system/aximos-install $(DESTDIR)$(PREFIX)/bin/aximos-install
	install -Dm755 system/aximos-update $(DESTDIR)$(PREFIX)/bin/aximos-update
	install -Dm644 system/org.aximos.update.policy $(DESTDIR)$(PREFIX)/share/polkit-1/actions/org.aximos.update.policy
	install -Dm644 system/aximos.desktop $(DESTDIR)$(PREFIX)/share/wayland-sessions/aximos.desktop
	install -Dm644 system/logind.conf $(DESTDIR)$(PREFIX)/lib/systemd/logind.conf.d/aximos.conf
	install -Dm644 system/pam.d/aximos-lock $(DESTDIR)/etc/pam.d/aximos-lock
	install -Dm644 system/aximos-recovery.sh $(DESTDIR)/etc/profile.d/aximos-recovery.sh

# The pacman packages, built in a clean chroot of Arch's own core and extra, from a
# tarball of the tree as it is, into a repository the ISO installs them from. Arch's
# packages are cached apart from the host's, as another distribution's, such as
# CachyOS's, can have the same names with other signatures.
CHROOT := $(BUILD)/chroot
REPO := $(BUILD)/repo
PACKAGING := $(BUILD)/packaging
PACKAGE_CACHE := $(abspath $(BUILD)/package-cache)
# Each build's version comes from git, as how many commits there are and the last's
# hash, so every build is newer than the one before. Packages and the repository's
# database are signed with SIGNING_KEY, which installed systems trust.
VERSION ?= 0.1.0.r$(shell git rev-list --count HEAD).g$(shell git rev-parse --short HEAD)
SIGNING_KEY ?= D0EB0C9AA46C7C675C8306B1F344BF999C1E9042
packages:
	rm -rf $(PACKAGING) && mkdir -p $(PACKAGING) $(REPO) $(CHROOT) $(PACKAGE_CACHE)
	sed 's/^pkgver=.*/pkgver=$(VERSION)/' packaging/PKGBUILD > $(PACKAGING)/PKGBUILD
	cp packaging/keyring/aximos-keyring.install packaging/aximos-desktop.install $(PACKAGING)/
	tar -czf $(PACKAGING)/keyring.tar.gz -C packaging keyring
	tar -czf $(PACKAGING)/aximos.tar.gz --transform 's,^\.,aximos,' --exclude=./build --exclude=./.git .
	[ -d $(CHROOT)/root ] || sudo mkarchroot -C /usr/share/devtools/pacman.conf.d/extra.conf \
		-M /usr/share/devtools/makepkg.conf.d/x86_64.conf -c $(PACKAGE_CACHE) $(CHROOT)/root base-devel
	cd $(PACKAGING) && makechrootpkg -c -u -r $(abspath $(CHROOT))
	rm -f $(REPO)/*.pkg.tar.zst $(REPO)/*.sig $(REPO)/aximos.*
	cp $(PACKAGING)/*.pkg.tar.zst $(REPO)/
	for p in $(REPO)/*.pkg.tar.zst; do gpg --batch --yes -u $(SIGNING_KEY) --detach-sign "$$p"; done
	repo-add -q -s -k $(SIGNING_KEY) $(REPO)/aximos.db.tar.gz $(REPO)/*.pkg.tar.zst

# The packages, on AximOS-arch's packages release, where installed systems update
# from: what's there is replaced by what make packages made. The database goes as
# aximos.db, which is what pacman asks for, as a release can't hold a link.
RELEASE := packages
publish:
	gh release view $(RELEASE) >/dev/null 2>&1 || gh release create $(RELEASE) --title "AximOS packages" \
		--notes "The pacman repository installed AximOS systems update from." --latest=false
	cp -L $(REPO)/aximos.db.tar.gz $(BUILD)/aximos.db && cp -L $(REPO)/aximos.db.tar.gz.sig $(BUILD)/aximos.db.sig
	cp -L $(REPO)/aximos.files.tar.gz $(BUILD)/aximos.files && cp -L $(REPO)/aximos.files.tar.gz.sig $(BUILD)/aximos.files.sig
	# What's there but for the newest version before this one, which an update can be
	# undone to, goes.
	gh release view $(RELEASE) --json assets -q '.assets[].name' > $(BUILD)/release-assets
	keep=$$(sed -nE 's/^aximos-[a-z]+-(.+-[0-9]+)-(x86_64|any)\.pkg\.tar\.zst$$/\1/p' $(BUILD)/release-assets \
		| grep -vxF '$(VERSION)-1' | sort -uV | tail -1); \
	grep -v -x -F -e aximos.db -e aximos.db.sig -e aximos.files -e aximos.files.sig \
		$(foreach p,$(notdir $(wildcard $(REPO)/*.pkg.tar.zst)),-e $(p) -e $(p).sig) $(BUILD)/release-assets \
		| { if [ -n "$$keep" ]; then grep -vF -- "-$$keep-"; else cat; fi; } \
		| xargs -r -n1 gh release delete-asset $(RELEASE) -y
	gh release upload $(RELEASE) --clobber $(REPO)/*.pkg.tar.zst $(REPO)/*.pkg.tar.zst.sig \
		$(BUILD)/aximos.db $(BUILD)/aximos.db.sig $(BUILD)/aximos.files $(BUILD)/aximos.files.sig

# The live ISO, from iso/, with Arch's packages and AximOS's from the repository
# make packages makes, built as root by mkarchiso.
ISO := $(BUILD)/aximos.iso
ISO_PROFILE := $(BUILD)/iso-profile
ISO_WORK := $(abspath $(BUILD)/iso-work)
iso: $(ISO)
$(ISO): $(shell find iso -type f) $(wildcard $(REPO)/*.pkg.tar.zst)
	rm -rf $(ISO_PROFILE) && cp -r iso $(ISO_PROFILE) && rm $(ISO_PROFILE)/pacman.conf.in
	sed 's|@REPO@|$(abspath $(REPO))|; s|@CACHE@|$(PACKAGE_CACHE)|' iso/pacman.conf.in > $(ISO_PROFILE)/pacman.conf
	sudo rm -rf $(ISO_WORK)
	rm -f $(PACKAGE_CACHE)/aximos-* # always from the repository, as a rebuild can keep its version
	sudo mkarchiso -w $(ISO_WORK) -o $(abspath $(BUILD))/iso-out $(ISO_PROFILE)
	sudo rm -rf $(ISO_WORK)
	sudo chown -R $(shell id -u):$(shell id -g) $(BUILD)/iso-out
	mv $(BUILD)/iso-out/aximos-*.iso $@ && rmdir $(BUILD)/iso-out

test: $(ISO)
	python3 tests/run.py

clean:
	rm -rf $(BUILD)

help:
	@echo "make            builds wm, libgui and the apps into build/"
	@echo "make install    installs them under DESTDIR and PREFIX, $(PREFIX)"
	@echo "make packages   makes the pacman packages, in a clean Arch chroot"
	@echo "make iso        makes build/aximos.iso, a live Arch with the desktop"
	@echo "make test       boots the ISO in QEMU and runs tests/"
