# betterbanana - BetterBanana for Linux
# No cmake dependency: Qt's moc is invoked directly.

CXX      ?= g++
# /usr/bin/moc is often Qt5's, and Qt6 puts its own moc in a different place on
# every distribution. Ask Qt where it lives, then fall back to known locations.
RCC ?= $(shell \
  for c in "$$(qmake6 -query QT_HOST_LIBEXECS 2>/dev/null)/rcc" \
           "$$(qmake6 -query QT_HOST_BINS 2>/dev/null)/rcc" \
           /usr/lib/qt6/rcc /usr/lib/qt6/libexec/rcc \
           /usr/lib/x86_64-linux-gnu/qt6/libexec/rcc \
           /usr/lib64/qt6/libexec/rcc /usr/bin/rcc-qt6; do \
    [ -x "$$c" ] && echo "$$c" && exit 0; done; echo rcc)
MOC ?= $(shell \
  for c in "$$(qmake6 -query QT_HOST_LIBEXECS 2>/dev/null)/moc" \
           "$$(qmake6 -query QT_HOST_BINS 2>/dev/null)/moc" \
           "$$(qtpaths6 -query QT_HOST_LIBEXECS 2>/dev/null)/moc" \
           /usr/lib/qt6/moc /usr/lib/qt6/libexec/moc \
           /usr/lib/x86_64-linux-gnu/qt6/libexec/moc \
           /usr/lib64/qt6/libexec/moc /usr/bin/moc-qt6; do \
    [ -x "$$c" ] && echo "$$c" && exit 0; done; echo moc)
BUILD    := build
HDRS     := common/protocol.h common/preset.h common/eqprofile.h common/fxpreset.h \
            engine/dsp.h engine/spectrum.h engine/voicefx.h engine/formant.h \
            engine/pitchtrack.h engine/autotune.h
GUI_HDRS := gui/mainwindow.h gui/eqdialog.h gui/fxdialog.h gui/calibdialog.h \
            gui/theme.h gui/knob.h gui/widgets.h gui/color.h gui/metrics.h \
            gui/dialogbits.h
VERSION := 0.2.0
CXXFLAGS := -O2 -std=c++20 -DBB_VERSION='"$(VERSION)"' -Wall -Wextra -Wno-unused-parameter -ffp-contract=fast -Wno-missing-field-initializers
PW_CF    := $(shell pkg-config --cflags libpipewire-0.3)
PW_LD    := $(shell pkg-config --libs libpipewire-0.3)
# Qt6Network is only used by the AutoEq headphone browser in the EQ editor.
QT_CF    := $(shell pkg-config --cflags Qt6Widgets Qt6Network)
QT_LD    := $(shell pkg-config --libs Qt6Widgets Qt6Network)
SND_CF   := $(shell pkg-config --cflags sndfile)
SND_LD   := $(shell pkg-config --libs sndfile)

.PHONY: all engine gui ctl tests check clean run install install-system check-deps dist

all: engine gui ctl tests

DISTNAME := betterbanana-$(VERSION)

# Fail early and legibly instead of part-way through a build.
check-deps:
	@ok=1; \
	for m in libpipewire-0.3 sndfile Qt6Widgets Qt6Network; do \
	  pkg-config --exists $$m 2>/dev/null \
	    && printf '  ok   %-18s %s\n' "$$m" "$$(pkg-config --modversion $$m)" \
	    || { printf '  MISSING  %s\n' "$$m"; ok=0; }; \
	done; \
	v=$$($(MOC) --version 2>/dev/null); \
	case "$$v" in \
	  *"moc 6"*) printf '  ok   %-18s %s\n' moc "$$($(MOC) --version)";; \
	  "")        printf '  MISSING  Qt6 moc (set MOC=/path/to/moc)\n'; ok=0;; \
	  *)         printf '  WRONG    moc is not Qt6: %s\n' "$$v"; ok=0;; \
	esac; \
	command -v pactl >/dev/null \
	  && printf '  ok   %-18s runtime\n' pactl \
	  || printf '  WARN     pactl not found; the GUI needs it to list devices\n'; \
	[ $$ok = 1 ] || { echo; echo "Install the missing packages, then run make again."; exit 1; }

# Source tarball for copying to another machine.
dist:
	@rm -rf $(DISTNAME) $(DISTNAME).tar.gz
	@mkdir -p $(DISTNAME)
	@cp -r Makefile README.md LICENSE common engine gui tools tests packaging docs $(DISTNAME)/
	@tar czf $(DISTNAME).tar.gz $(DISTNAME)
	@rm -rf $(DISTNAME)
	@echo "$(DISTNAME).tar.gz  ($$(du -h $(DISTNAME).tar.gz | cut -f1))"


engine: $(BUILD)/bb-engine
gui:    $(BUILD)/bb-gui
tests:  $(BUILD)/test_dsp $(BUILD)/test_eq $(BUILD)/test_fader \
        $(BUILD)/test_preset $(BUILD)/test_spectrum $(BUILD)/test_voicefx \
        $(BUILD)/test_pitch $(BUILD)/test_autotune $(BUILD)/test_contrast
ctl:    $(BUILD)/bb-ctl

$(BUILD)/bb-engine: | check-deps
$(BUILD)/bb-engine: engine/engine.cpp engine/dsp.h $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(PW_CF) $(SND_CF) $< -o $@ $(PW_LD) $(SND_LD) -lrt -lpthread

$(BUILD)/test_fader: tests/test_fader.cpp gui/knob.cpp gui/theme.cpp \
                     $(BUILD)/moc_knob.cpp $(GUI_HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -fPIC $(QT_CF) \
	  tests/test_fader.cpp gui/knob.cpp gui/theme.cpp $(BUILD)/moc_knob.cpp \
	  -o $@ $(QT_LD)

# Needs Qt for QColor only; no display, no event loop.
$(BUILD)/test_contrast: tests/test_contrast.cpp gui/theme.cpp $(GUI_HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -fPIC $(QT_CF) \
	  tests/test_contrast.cpp gui/theme.cpp -o $@ $(QT_LD)

$(BUILD)/bb-ctl: tools/bb-ctl.cpp $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@ -lrt -lpthread

$(BUILD)/test_dsp: tests/test_dsp.cpp engine/dsp.h | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BUILD)/test_eq: tests/test_eq.cpp $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BUILD)/test_preset: tests/test_preset.cpp $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BUILD)/test_spectrum: tests/test_spectrum.cpp $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BUILD)/test_voicefx: tests/test_voicefx.cpp $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BUILD)/test_pitch: tests/test_pitch.cpp $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BUILD)/test_autotune: tests/test_autotune.cpp $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

# GUI: moc the header that declares QObject subclasses, compile alongside.
$(BUILD)/moc_mainwindow.cpp: gui/mainwindow.h | $(BUILD)
	$(MOC) $< -o $@

$(BUILD)/moc_knob.cpp: gui/knob.h | $(BUILD)
	$(MOC) $< -o $@

# The window icon, compiled in so it works before the app is installed.
$(BUILD)/qrc_betterbanana.cpp: packaging/betterbanana.qrc packaging/betterbanana.svg | $(BUILD)
	$(RCC) $< -o $@

$(BUILD)/moc_eqdialog.cpp: gui/eqdialog.h | $(BUILD)
	$(MOC) $< -o $@

$(BUILD)/moc_fxdialog.cpp: gui/fxdialog.h | $(BUILD)
	$(MOC) $< -o $@

$(BUILD)/moc_calibdialog.cpp: gui/calibdialog.h | $(BUILD)
	$(MOC) $< -o $@

$(BUILD)/bb-gui: gui/main.cpp gui/mainwindow.cpp gui/eqdialog.cpp gui/fxdialog.cpp \
                  gui/calibdialog.cpp gui/widgets.cpp gui/theme.cpp gui/knob.cpp \
                  gui/dialogbits.cpp \
                  $(BUILD)/moc_mainwindow.cpp $(BUILD)/moc_eqdialog.cpp \
                  $(BUILD)/moc_fxdialog.cpp $(BUILD)/moc_calibdialog.cpp \
                  $(BUILD)/moc_knob.cpp $(BUILD)/qrc_betterbanana.cpp \
                  $(GUI_HDRS) $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -fPIC $(QT_CF) \
	  gui/main.cpp gui/mainwindow.cpp gui/eqdialog.cpp gui/fxdialog.cpp \
	  gui/calibdialog.cpp gui/widgets.cpp gui/theme.cpp gui/knob.cpp \
	  gui/dialogbits.cpp \
	  $(BUILD)/moc_mainwindow.cpp $(BUILD)/moc_eqdialog.cpp \
	  $(BUILD)/moc_fxdialog.cpp $(BUILD)/moc_calibdialog.cpp $(BUILD)/moc_knob.cpp \
	  $(BUILD)/qrc_betterbanana.cpp \
	  -o $@ $(QT_LD) -lrt -lpthread

$(BUILD):
	@mkdir -p $(BUILD)

# Rebuild everything and refresh the copies in ~/.local/bin, so the installed
# binaries never lag behind the build directory.
install: engine gui ctl
	./packaging/install.sh

# System-wide install, for packaging. The systemd units go to the user-unit
# directory so each user still runs their own engine.
#
# This target used to install three binaries, one icon and one unit: a mixer
# with no watchdog, no stream guard, no PipeWire drop-in and no AutoEq helper -
# not the program packaging/install.sh puts on a machine, and this is the path
# the PKGBUILD takes. The two install the same set now. The differences that
# remain are the ones that have to be: system paths instead of $$HOME, and no
# systemctl, because a package is built in a chroot and must not enable a
# service on the builder's machine.
PREFIX  ?= /usr/local
BINDIR   = $(DESTDIR)$(PREFIX)/bin
DATADIR  = $(DESTDIR)$(PREFIX)/share
ICONDIR  = $(DATADIR)/icons/hicolor
UNITDIR  = $(DESTDIR)$(PREFIX)/lib/systemd/user
ICONSIZES = 16 24 32 48 64 128 256

install-system: engine gui ctl
	install -Dm755 $(BUILD)/bb-engine $(BINDIR)/bb-engine
	install -Dm755 $(BUILD)/bb-gui    $(BINDIR)/bb-gui
	install -Dm755 $(BUILD)/bb-ctl    $(BINDIR)/bb-ctl
	install -Dm755 -t $(BINDIR) tools/bb-stream-guard tools/bb-health \
	                            tools/bb-stream-setup tools/bb-autoeq
	install -Dm644 packaging/betterbanana.svg \
	  $(ICONDIR)/scalable/apps/betterbanana.svg
	@# A panel that does not link librsvg only reads the sized directories, and
	@# with scalable/ alone it falls back to the generic placeholder.
	for s in $(ICONSIZES); do \
	  install -Dm644 packaging/icons/$${s}x$${s}/apps/betterbanana.png \
	    $(ICONDIR)/$${s}x$${s}/apps/betterbanana.png; \
	done
	install -Dm644 packaging/betterbanana.desktop \
	  $(DATADIR)/applications/betterbanana.desktop
	install -Dm644 packaging/io.github.zykoraa.betterbanana.metainfo.xml \
	  $(DATADIR)/metainfo/io.github.zykoraa.betterbanana.metainfo.xml
	install -Dm644 packaging/betterbanana.xml \
	  $(DATADIR)/mime/packages/betterbanana.xml
	install -Dm644 packaging/99-bb-stream.conf \
	  $(DATADIR)/pipewire/pipewire.conf.d/99-bb-stream.conf
	install -Dm644 LICENSE $(DATADIR)/licenses/betterbanana/LICENSE
	@# Every unit runs the binary out of ~/.local/bin, which is where the
	@# per-user installer puts it; all three need repointing, not just the
	@# engine's.
	for u in engine stream-guard health; do \
	  install -Dm644 packaging/betterbanana-$$u.service \
	    $(UNITDIR)/betterbanana-$$u.service; \
	  sed -i 's|%h/.local/bin/|$(PREFIX)/bin/|' \
	    $(UNITDIR)/betterbanana-$$u.service; \
	done
	@# No trailing $$ on the pattern: the entry's Exec now carries %f and the
	@# four Desktop Actions carry their own flags, and an unanchored launcher
	@# only resolves if the *graphical* session has the directory on PATH.
	@# Re-running is safe because install overwrites the file first.
	sed -i 's|^Exec=bb-gui|Exec=$(PREFIX)/bin/bb-gui|' \
	  $(DATADIR)/applications/betterbanana.desktop
	@# Caches, only when installing for real. Under DESTDIR this is a package
	@# build: the files are not at these paths yet and the distribution's own
	@# hooks refresh the caches at install time.
	@if [ -z "$(DESTDIR)" ]; then \
	  command -v update-desktop-database >/dev/null \
	    && update-desktop-database $(DATADIR)/applications || true; \
	  command -v gtk-update-icon-cache >/dev/null \
	    && gtk-update-icon-cache -f -t $(ICONDIR) 2>/dev/null || true; \
	  command -v update-mime-database >/dev/null \
	    && update-mime-database $(DATADIR)/mime || true; \
	fi

# Run every unit test and say plainly which one broke. The integration script
# is deliberately not part of this: it needs a running engine and it moves the
# caller's applications around for half a minute.
UNIT := test_autotune test_contrast test_dsp test_eq test_fader test_pitch \
        test_preset test_spectrum test_voicefx

check: tests
	@fail=0; \
	for t in $(UNIT); do \
	  printf '  %-14s ' "$$t"; \
	  if ./$(BUILD)/$$t >/dev/null 2>&1; then echo ok; \
	  else echo FAILED; fail=1; fi; \
	done; \
	if [ $$fail != 0 ]; then \
	  echo; echo "Run the failing one directly to see the assertions."; exit 1; \
	fi; \
	echo; echo "All unit tests passed."; \
	echo "./tests/integration.sh drives real audio through a running engine."

run: engine
	./$(BUILD)/bb-engine

clean:
	rm -rf $(BUILD)
