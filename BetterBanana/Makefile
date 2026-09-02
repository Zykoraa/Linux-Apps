# betterbanana - BetterBanana for Linux
# No cmake dependency: Qt's moc is invoked directly.

CXX      ?= g++
# /usr/bin/moc is often Qt5's, and Qt6 puts its own moc in a different place on
# every distribution. Ask Qt where it lives, then fall back to known locations.
MOC ?= $(shell \
  for c in "$$(qmake6 -query QT_HOST_LIBEXECS 2>/dev/null)/moc" \
           "$$(qmake6 -query QT_HOST_BINS 2>/dev/null)/moc" \
           "$$(qtpaths6 -query QT_HOST_LIBEXECS 2>/dev/null)/moc" \
           /usr/lib/qt6/moc /usr/lib/qt6/libexec/moc \
           /usr/lib/x86_64-linux-gnu/qt6/libexec/moc \
           /usr/lib64/qt6/libexec/moc /usr/bin/moc-qt6; do \
    [ -x "$$c" ] && echo "$$c" && exit 0; done; echo moc)
BUILD    := build
HDRS     := common/protocol.h common/preset.h common/eqprofile.h engine/dsp.h engine/spectrum.h
CXXFLAGS := -O2 -std=c++20 -Wall -Wextra -Wno-unused-parameter -ffp-contract=fast -Wno-missing-field-initializers
PW_CF    := $(shell pkg-config --cflags libpipewire-0.3)
PW_LD    := $(shell pkg-config --libs libpipewire-0.3)
# Qt6Network is only used by the AutoEq headphone browser in the EQ editor.
QT_CF    := $(shell pkg-config --cflags Qt6Widgets Qt6Network)
QT_LD    := $(shell pkg-config --libs Qt6Widgets Qt6Network)
SND_CF   := $(shell pkg-config --cflags sndfile)
SND_LD   := $(shell pkg-config --libs sndfile)

.PHONY: all engine gui ctl tests clean run install install-system check-deps dist

all: engine gui ctl tests

VERSION := 0.1.0
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
	@cp -r Makefile README.md common engine gui tools tests packaging $(DISTNAME)/
	@tar czf $(DISTNAME).tar.gz $(DISTNAME)
	@rm -rf $(DISTNAME)
	@echo "$(DISTNAME).tar.gz  ($$(du -h $(DISTNAME).tar.gz | cut -f1))"


engine: $(BUILD)/bb-engine
gui:    $(BUILD)/bb-gui
tests:  $(BUILD)/test_dsp $(BUILD)/test_eq $(BUILD)/test_fader \
        $(BUILD)/test_preset $(BUILD)/test_spectrum
ctl:    $(BUILD)/bb-ctl

$(BUILD)/bb-engine: | check-deps
$(BUILD)/bb-engine: engine/engine.cpp engine/dsp.h $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(PW_CF) $(SND_CF) $< -o $@ $(PW_LD) $(SND_LD) -lrt -lpthread

$(BUILD)/test_fader: tests/test_fader.cpp gui/knob.cpp gui/theme.cpp \
                     $(BUILD)/moc_knob.cpp gui/knob.h gui/theme.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -fPIC $(QT_CF) \
	  tests/test_fader.cpp gui/knob.cpp gui/theme.cpp $(BUILD)/moc_knob.cpp \
	  -o $@ $(QT_LD)

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

# GUI: moc the header that declares QObject subclasses, compile alongside.
$(BUILD)/moc_mainwindow.cpp: gui/mainwindow.h | $(BUILD)
	$(MOC) $< -o $@

$(BUILD)/moc_knob.cpp: gui/knob.h | $(BUILD)
	$(MOC) $< -o $@

$(BUILD)/moc_eqdialog.cpp: gui/eqdialog.h | $(BUILD)
	$(MOC) $< -o $@

$(BUILD)/bb-gui: gui/main.cpp gui/mainwindow.cpp gui/eqdialog.cpp gui/widgets.cpp \
                  gui/theme.cpp gui/knob.cpp \
                  $(BUILD)/moc_mainwindow.cpp $(BUILD)/moc_eqdialog.cpp $(BUILD)/moc_knob.cpp \
                  gui/mainwindow.h gui/eqdialog.h $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -fPIC $(QT_CF) \
	  gui/main.cpp gui/mainwindow.cpp gui/eqdialog.cpp gui/widgets.cpp gui/theme.cpp gui/knob.cpp \
	  $(BUILD)/moc_mainwindow.cpp $(BUILD)/moc_eqdialog.cpp $(BUILD)/moc_knob.cpp \
	  -o $@ $(QT_LD) -lrt -lpthread

$(BUILD):
	@mkdir -p $(BUILD)

# Rebuild everything and refresh the copies in ~/.local/bin, so the installed
# binaries never lag behind the build directory.
install: engine gui ctl
	./packaging/install.sh

# System-wide install, for packaging. The systemd unit goes to the user-unit
# directory so each user still runs their own engine.
PREFIX ?= /usr/local
install-system: engine gui ctl
	install -Dm755 build/bb-engine $(DESTDIR)$(PREFIX)/bin/bb-engine
	install -Dm755 build/bb-gui    $(DESTDIR)$(PREFIX)/bin/bb-gui
	install -Dm755 build/bb-ctl    $(DESTDIR)$(PREFIX)/bin/bb-ctl
	install -Dm644 packaging/betterbanana.svg \
	  $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/betterbanana.svg
	install -Dm644 packaging/betterbanana.desktop \
	  $(DESTDIR)$(PREFIX)/share/applications/betterbanana.desktop
	install -Dm644 packaging/betterbanana-engine.service \
	  $(DESTDIR)$(PREFIX)/lib/systemd/user/betterbanana-engine.service
	sed -i 's|%h/.local/bin/bb-engine|$(PREFIX)/bin/bb-engine|' \
	  $(DESTDIR)$(PREFIX)/lib/systemd/user/betterbanana-engine.service
	sed -i 's|^Exec=bb-gui$$|Exec=$(PREFIX)/bin/bb-gui|' \
	  $(DESTDIR)$(PREFIX)/share/applications/betterbanana.desktop

run: engine
	./$(BUILD)/bb-engine

clean:
	rm -rf $(BUILD)
