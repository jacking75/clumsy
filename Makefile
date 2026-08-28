# clumsy - Linux native build  (Phase 4.5)
#
# genie.lua remains the canonical build definition, but GENie ships as a Windows
# binary in this repo and building it from source just to compile ~30 files is
# not worth the extra step. This Makefile is the tested Linux path; keep the two
# in sync when adding source files (genie.lua globs src/**.cpp, so the only real
# divergence is the platform-specific file lists below).
#
# Requires:
#   sudo apt install g++-16 libnetfilter-queue-dev libmnl-dev iptables
#
# Usage:
#   make              release build -> bin/linux/clumsy
#   make DEBUG=1      debug build with -O0 -g and verbose logging on
#   make test         build and run the packet helper conformance test
#   make install      install to $(PREFIX)/bin (default /usr/local)
#   make package-deb  build a .deb into bin/linux/
#   make package-rpm  build an .rpm (needs rpmbuild)
#   make clean

# make predefines CXX=g++, so ?= would never take effect; only override the
# built-in default and still honour an explicit CXX=... on the command line.
ifeq ($(origin CXX),default)
CXX := g++-16
endif
CXXSTD   := -std=c++23
SRCDIR   := src
OBJDIR   := obj_linux
BINDIR   := bin/linux
TARGET   := $(BINDIR)/clumsy

# Portable core, shared with the Windows build.
SOURCES := \
	$(SRCDIR)/main.cpp \
	$(SRCDIR)/packet.cpp \
	$(SRCDIR)/utils.cpp \
	$(SRCDIR)/lag.cpp \
	$(SRCDIR)/jitter.cpp \
	$(SRCDIR)/drop.cpp \
	$(SRCDIR)/burstloss.cpp \
	$(SRCDIR)/blackout.cpp \
	$(SRCDIR)/throttle.cpp \
	$(SRCDIR)/duplicate.cpp \
	$(SRCDIR)/ood.cpp \
	$(SRCDIR)/tamper.cpp \
	$(SRCDIR)/reset.cpp \
	$(SRCDIR)/bandwidth.cpp \
	$(SRCDIR)/pipe.cpp \
	$(SRCDIR)/statslog.cpp \
	$(SRCDIR)/scenario.cpp \
	$(SRCDIR)/profile.cpp \
	$(SRCDIR)/json.cpp \
	$(SRCDIR)/controlapi.cpp \
	$(SRCDIR)/httpserver.cpp \
	$(SRCDIR)/pcapexport.cpp \
	$(SRCDIR)/report.cpp \
	$(SRCDIR)/plugin.cpp \
	$(SRCDIR)/filterexpr.cpp

# Linux-only backends. Their Windows counterparts (divert.cpp, packetutil_win.cpp,
# elevate.cpp, procfilter.cpp) are never compiled here.
SOURCES += \
	$(SRCDIR)/platform_linux.cpp \
	$(SRCDIR)/divert_linux.cpp \
	$(SRCDIR)/packetutil_linux.cpp \
	$(SRCDIR)/elevate_linux.cpp \
	$(SRCDIR)/procfilter_linux.cpp \
	$(SRCDIR)/iptables_linux.cpp

OBJECTS := $(SOURCES:$(SRCDIR)/%.cpp=$(OBJDIR)/%.o)
DEPS    := $(OBJECTS:.o=.d)

WARNINGS := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers

ifdef DEBUG
  BUILDFLAGS := -O0 -g -D_DEBUG
else
  BUILDFLAGS := -O2 -DNDEBUG
endif

CXXFLAGS := $(CXXSTD) $(WARNINGS) $(BUILDFLAGS) -I$(SRCDIR) \
            $(shell pkg-config --cflags libnetfilter_queue 2>/dev/null)
LDFLAGS  := -pthread -ldl \
            $(shell pkg-config --libs libnetfilter_queue 2>/dev/null || echo -lnetfilter_queue -lnfnetlink)

VERSION := 0.4
DEBARCH := $(shell dpkg --print-architecture 2>/dev/null || echo amd64)
# Staged outside the source tree on purpose: dpkg-deb refuses a control
# directory whose mode is not 0755-0775, and a checkout on a Windows mount
# (WSL /mnt/c) reports everything as 0777. /tmp always honours the mode.
PKGDIR  := $(shell echo $${TMPDIR:-/tmp})/clumsy-pkg-$(shell id -u)

.PHONY: all clean test run install-deps install package-deb package-rpm

all: $(TARGET)

$(TARGET): $(OBJECTS) | $(BINDIR)
	$(CXX) $(OBJECTS) -o $@ $(LDFLAGS)
	@# The dashboard and presets are loaded from next to the executable.
	@mkdir -p $(BINDIR)/web
	@cp -f etc/web/index.html $(BINDIR)/web/
	@cp -f etc/config.json etc/config.txt $(BINDIR)/ 2>/dev/null || true
	@echo "built $@"

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp | $(OBJDIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(OBJDIR) $(BINDIR):
	@mkdir -p $@

# Conformance test for the backend-neutral packet helpers. Runs the Linux
# implementation against the same assertions the Windows one must satisfy.
test: $(OBJDIR)/packetutil_test
	@$(OBJDIR)/packetutil_test

$(OBJDIR)/packetutil_test: tests/packetutil_test.cpp $(SRCDIR)/packetutil_linux.cpp \
                           $(SRCDIR)/platform_linux.cpp | $(OBJDIR)
	$(CXX) $(CXXSTD) $(WARNINGS) -O1 -I$(SRCDIR) $^ -o $@ -pthread

install-deps:
	sudo apt-get install -y g++-16 libnetfilter-queue-dev libmnl-dev iptables

# Plain install, for people who do not want a package.
DESTDIR ?=
PREFIX  ?= /usr/local
install: $(TARGET)
	install -D -m 0755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/clumsy
	install -D -m 0644 etc/web/index.html $(DESTDIR)$(PREFIX)/bin/web/index.html
	install -D -m 0644 etc/config.json $(DESTDIR)$(PREFIX)/bin/config.json
	@echo "installed to $(DESTDIR)$(PREFIX)/bin/clumsy"
	@echo "grant capabilities with:"
	@echo "  sudo setcap cap_net_admin,cap_net_raw+ep $(DESTDIR)$(PREFIX)/bin/clumsy"

# ---------------------------------------------------------------------------
# Packaging
#
# clumsy looks for config.json and web/ next to its own executable, so the data
# files are symlinked from /usr/bin back into /usr/share/clumsy rather than
# duplicated.
# ---------------------------------------------------------------------------
package-deb: $(TARGET)
	@command -v dpkg-deb >/dev/null || { echo "dpkg-deb not found"; exit 1; }
	rm -rf $(PKGDIR)
	mkdir -p $(PKGDIR)/DEBIAN
	mkdir -p $(PKGDIR)/usr/bin/web
	mkdir -p $(PKGDIR)/usr/share/clumsy/web
	mkdir -p $(PKGDIR)/usr/share/doc/clumsy
	sed -e 's/@VERSION@/$(VERSION)/' -e 's/@ARCH@/$(DEBARCH)/' 	    packaging/deb/control.in > $(PKGDIR)/DEBIAN/control
	install -m 0755 packaging/deb/postinst $(PKGDIR)/DEBIAN/postinst
	install -m 0755 $(TARGET) $(PKGDIR)/usr/bin/clumsy
	install -m 0644 etc/config.json etc/config.txt etc/scenario-example.json 	    $(PKGDIR)/usr/share/clumsy/
	install -m 0644 etc/web/index.html $(PKGDIR)/usr/share/clumsy/web/
	install -m 0644 docs/LINUX.md README.md $(PKGDIR)/usr/share/doc/clumsy/
	ln -sf /usr/share/clumsy/config.json     $(PKGDIR)/usr/bin/config.json
	ln -sf /usr/share/clumsy/web/index.html  $(PKGDIR)/usr/bin/web/index.html
	chmod 0755 $(PKGDIR)/DEBIAN
	dpkg-deb --build --root-owner-group $(PKGDIR) $(BINDIR)/clumsy_$(VERSION)_$(DEBARCH).deb
	rm -rf $(PKGDIR)
	@echo "built $(BINDIR)/clumsy_$(VERSION)_$(DEBARCH).deb"

package-rpm:
	@command -v rpmbuild >/dev/null || { echo "rpmbuild not found (install rpm-build)"; exit 1; }
	rm -rf $(OBJDIR)/rpmbuild
	mkdir -p $(OBJDIR)/rpmbuild/SOURCES
	git archive --format=tar.gz --prefix=clumsy-$(VERSION)/ 	    -o $(OBJDIR)/rpmbuild/SOURCES/clumsy-$(VERSION).tar.gz HEAD
	rpmbuild -bb --define "_topdir $(CURDIR)/$(OBJDIR)/rpmbuild" packaging/clumsy.spec
	@echo "rpm(s) under $(OBJDIR)/rpmbuild/RPMS/"

clean:
	rm -rf $(OBJDIR) $(BINDIR)

-include $(DEPS)
