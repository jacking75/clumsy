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
	$(SRCDIR)/procfilter_linux.cpp

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

.PHONY: all clean test run install-deps

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

clean:
	rm -rf $(OBJDIR) $(BINDIR)

-include $(DEPS)
