
#SHELL = thirdparty/cosmocc/bin/cocmd
# /bin/sh rather than dash: the recipes here are plain POSIX shell, and dash is
# not installed on Fedora or Arch, where the distribution packages build.
SHELL = /bin/sh

#STATIC = --static
STATIC =

# clang++ is the default, but only when nothing else asked for a compiler. A
# distribution package is built with the compiler that distribution chose --
# gcc, almost always -- and the plain `=` that used to be here silently ignored
# a CXX in the environment, which looks like the build honouring the request
# right up until the binary turns out to have been built with something else.
#
# $(origin) rather than ?=, because make defines CXX itself: ?= would see it
# already set and never fire.
ifeq ($(origin CXX),default)
CXX := $(shell command -v clang++ 2>/dev/null || command -v c++)
endif

# The compiler as it is to be recorded, before ccache is put in front of it:
# compile_commands.json is read by clangd, which wants the compiler and not the
# thing that caches it.
REAL_CXX := $(CXX)

# ccache, when it is installed. It is worth having here for one reason in
# particular: GLEDITOR_ENABLE_VULKAN changes the flags every object is built
# with, so turning it on or off rebuilds all of them -- a hundred seconds of
# recompiling to arrive back at objects that were compiled ten minutes ago.
# With ccache the second flip is a second and a half. Set GLEDITOR_NO_CCACHE=1
# to build without it, and an explicit CXX is left alone: somebody who named a
# compiler meant that command and not a wrapper around it.
ifeq ($(origin CXX),file)
ifndef GLEDITOR_NO_CCACHE
CCACHE := $(shell command -v ccache 2>/dev/null)
ifneq ($(CCACHE),)
CXX := $(CCACHE) $(CXX)
endif
endif
endif
#CXX = thirdparty/cosmocc-4.0.2/bin/cosmoc++ -mclang
#CXX = thirdparty/cosmocc/bin/x86_64-linux-cosmo-gcc
# cocmd gives us a builtin in-process sed hook
#SED = thirdparty/cosmos/bin/sed
SED = sed
# cocmd builtin rm doesn't currently support -r
#RM = thirdparty/cosmos/bin/rm
RM = rm
#CKSUM = thirdparty/cosmos/bin/cksum
CKSUM = cksum
# cocmd builtin mkdir is sufficient for our needs
#MKDIR = thirdparty/cosmos/bin/mkdir
MKDIR = mkdir
# removed spdlog
# SDL major version. SDL3 is what the code is written against and what is used
# when it is installed; SDL2 is still what most distributions ship, so it is
# the fallback rather than an error. Set GLEDITOR_SDL=2 or 3 to choose
# explicitly -- which is what the CI matrix does, since "whatever is installed"
# is not a configuration anyone can reproduce.
ifndef GLEDITOR_SDL
GLEDITOR_SDL := $(shell pkg-config --exists sdl3 && echo 3 || echo 2)
endif
ifeq ($(GLEDITOR_SDL),3)
SDL_PKG := sdl3
SDL_IMAGE_PKG := sdl3-image
else ifeq ($(GLEDITOR_SDL),2)
SDL_PKG := sdl2
SDL_IMAGE_PKG := SDL2_image
else
$(error GLEDITOR_SDL must be 2 or 3, got "$(GLEDITOR_SDL)")
endif
ifneq ($(shell pkg-config --exists $(SDL_PKG) && echo 1),1)
$(error $(SDL_PKG) not found by pkg-config; install it or set GLEDITOR_SDL to the other major version)
endif

# The GL/GLES entry points are resolved at runtime through SDL rather than
# linked, so no GL library is needed here; `gl` is only wanted for the include
# path to GL/glcorearb.h, which the backend reads for its typedefs and enum
# values. MinGW has no gl.pc at all and puts those headers where the compiler
# already looks, so this is used when pkg-config knows it and skipped when not.
PKGS := pangomm-2.48 $(SDL_PKG)
ifeq ($(shell pkg-config --exists gl && echo 1),1)
PKGS += gl
endif
ifdef GLEDITOR_ENABLE_VULKAN
PKGS += vulkan
endif
# SDL_image supplies the window icon and nothing else, and is not packaged
# everywhere SDL itself is. Use it when it is present and carry on without it
# when it is not.
HAVE_SDL_IMAGE := $(shell pkg-config --exists $(SDL_IMAGE_PKG) && echo 1)
ifeq ($(HAVE_SDL_IMAGE),1)
PKGS += $(SDL_IMAGE_PKG)
endif

# Every package, named individually, before anything asks for their flags.
# pkg-config is all-or-nothing: given a set where one member is missing it
# prints a complaint about that one and then returns *nothing at all* for the
# rest, so the build carries on with no include paths and fails on whichever
# header happens to come first. That names a header rather than a package, and
# the header is never the one belonging to what is actually missing. Several of
# these ship under names that differ per distribution -- pangomm has parallel
# ABIs, and vulkan.pc comes from the loader rather than the headers -- so
# getting one wrong is an ordinary packaging mistake to make.
MISSING_PKGS := $(strip $(foreach p,$(PKGS),\
  $(if $(shell pkg-config --exists $(p) && echo 1),,$(p))))
ifneq ($(MISSING_PKGS),)
$(error pkg-config cannot find: $(MISSING_PKGS). Install the development \
packages providing them, or unset GLEDITOR_ENABLE_VULKAN to build without \
the Vulkan backend)
endif
TEST_PKGS := gmock_main
# What the xanalogical engine is allowed to link. glibmm supplies SHA-1, which
# is what a torrent's info hash is, and glibmm is already a hard dependency of
# the library. The point of keeping this list short is that the engine must not
# need a graphics device -- not that it must need nothing at all -- so a
# utility library is fine here and pangomm, cairo and SDL are not.
# libtorrent is required, not optional. It is what lets a reference be fetched
# from peers rather than only from a disk here, and what signs and resolves the
# names a publisher is known by -- so a build without it produces a program
# whose documents cannot leave the machine that wrote them, which is the one
# thing this program is for. Better to fail at configure time than to ship a
# xanadoc editor that quietly cannot publish.
XUDU_PKGS := glibmm-2.68 libtorrent-rasterbar
ifneq ($(shell pkg-config --exists libtorrent-rasterbar && echo 1),1)
$(error libtorrent-rasterbar was not found by pkg-config. It is required: \
install libtorrent-rasterbar-dev (Debian, Ubuntu), libtorrent-rasterbar-devel \
(Fedora), or libtorrent-rasterbar (Arch, Homebrew).)
endif

# The version, which a release tarball has to know without a git history: every
# distribution builds from an unpacked tarball, where `git describe` prints
# nothing at all. VERSION in the tree is the fallback, and setting GLEDITOR_VERSION
# overrides both, which is what a packager does when the package version and
# the tag disagree.
FALLBACK_VERS := $(shell cat VERSION 2>/dev/null || echo 0.0.0)
ifndef GLEDITOR_VERSION
# No --always: a bare commit hash is not a version, and every packaging format
# wants to compare two of them. Without a matching tag this comes back empty
# and VERSION answers instead. The dashes git puts between the tag, the commit
# count and the hash become dots, because a dash separates upstream from
# revision in a Debian version and means nothing good in an RPM one.
GLEDITOR_VERSION := $(shell git describe --tags --match "v[0-9]*.[0-9]*.[0-9]*" HEAD 2>/dev/null | tr -d v | tr - .)
ifeq ($(GLEDITOR_VERSION),)
GLEDITOR_VERSION := $(FALLBACK_VERS)
endif
endif
VERS := $(GLEDITOR_VERSION)

# Which compiler this is, and what it will accept. -std=c++2c is clang's and
# gcc 14's spelling; gcc 13 wants c++2b and rejects the newer name outright, so
# asking beats assuming. -rtlib=compiler-rt is clang-only and gcc fails the
# link on it.
CXX_IS_CLANG := $(shell $(CXX) --version 2>/dev/null | grep -qi clang && echo 1)
STD_FLAG := $(shell $(CXX) -std=c++2c -x c++ -E - </dev/null >/dev/null 2>&1 \
	&& echo -std=c++2c || echo -std=c++2b)

# Where an installed copy lives. Only the install target compiles DATADIR in:
# a plain `make` must not let a build tree quietly read the assets of an older
# installed copy, so the search falls through to ./assets instead.
prefix ?= /usr/local
exec_prefix ?= $(prefix)
bindir ?= $(exec_prefix)/bin
libdir ?= $(exec_prefix)/lib
includedir ?= $(prefix)/include
datarootdir ?= $(prefix)/share
datadir ?= $(datarootdir)
mandir ?= $(datarootdir)/man
appdir := $(datadir)/gleditor
ifdef DEBUG
SANITIZE_ADDR_OPTS := -fsanitize=address,undefined,integer -fno-omit-frame-pointer -fsanitize-address-use-after-return=runtime \
	         -fsanitize-address-use-after-scope 
SANITIZE_THR_OPTS := -fsanitize=thread,undefined,integer -fno-omit-frame-pointer 
SANITIZE_MEM_OPTS := -fsanitize=memory,undefined,integer -fPIE -pie -fno-omit-frame-pointer \
		     -fsanitize-memory-track-origins
DEBUG_OPTS := -g -gembed-source -fdebug-macro -O0
PROFILE_OPTS := -fprofile-instr-generate -fcoverage-mapping -fcoverage-mcdc
else
DEBUG_OPTS := -O3 -g
endif
# -fPIC everywhere rather than only for the library's objects. A shared library
# requires it; a program does not, but compiling the two trees differently
# would mean two object directories and two sets of rules for one flag whose
# cost here is not measurable.
override CXXFLAGS += $(DEBUG_OPTS) $(STD_FLAG) -fPIC -Ibuild/src -Iinclude -Iapps -Ithirdparty/Choreograph/src -Ithirdparty/argparse/include -Wall -Wextra $(shell pkg-config $(STATIC) --cflags $(PKGS))
ifdef GLEDITOR_DATADIR
override CXXFLAGS += -DGLEDITOR_DATADIR='"$(GLEDITOR_DATADIR)"'
endif
override CXXFLAGS += -DGLEDITOR_SDL_MAJOR=$(GLEDITOR_SDL)
# glm/gtx/string_cast.hpp, which is where glm::to_string lives and which three
# files use for their startup diagnostics. GLM 1.0 refuses to compile any gtx
# header without this defined; 0.9, which is what Ubuntu still ships, does not
# ask. Defining it unconditionally satisfies both, and the distributions
# tracking GLM 1.0 -- Arch, Fedora, nixpkgs -- would otherwise all fail here.
override CXXFLAGS += -DGLM_ENABLE_EXPERIMENTAL
override CXXFLAGS += $(shell pkg-config --cflags libtorrent-rasterbar)
ifdef GLEDITOR_ENABLE_VULKAN
override CXXFLAGS += -DGLEDITOR_ENABLE_VULKAN=1
endif
ifeq ($(HAVE_SDL_IMAGE),1)
override CXXFLAGS += -DGLEDITOR_HAVE_SDL_IMAGE=1
endif
override LDFLAGS += $(DEBUG_OPTS) $(findstring $(STATIC),-static)
ifeq ($(CXX_IS_CLANG),1)
override LDFLAGS += -rtlib=compiler-rt
endif
# XXX: work on this in a separate branch, get tests working again for now
#CXXFLAGS += -stdlib=libc++ -fexperimental-library 
#LDFLAGS += -v -stdlib=libc++ -fexperimental-library 
LIBS := $(shell pkg-config $(STATIC) --libs $(PKGS))
XUDU_LIBS := $(shell pkg-config $(STATIC) --libs $(XUDU_PKGS))
# glslangValidator is the traditional name and glslang the current one; which
# of the two a distribution installs varies, so both are tried.
#
# command -v rather than which: `which` is a separate package that neither the
# Nix build sandbox nor a minimal Fedora container has, and its absence looked
# exactly like the compiler being absent -- an empty result either way. It is a
# shell builtin, so it is always there.
GLSLANG := $(shell command -v glslangValidator 2>/dev/null || command -v glslang 2>/dev/null)
# The Vulkan backend only compiles when Vulkan is enabled; everything else is
# backend-neutral or belongs to the GL family backend.
# -- what is a library and what is a program ----------------------------------
#
# Everything under src/ is the shared library: the render devices, the glyph
# cache, the buffer allocator, the document model and the render loop. None of
# it names a document format or an application, which is what lets more than
# one program be built on it.
#
# The programs live under apps/. `gleditor` is the plain editor -- a command
# line, a key map and an event loop over the library. `xudu` is a second one
# that keeps a versioned hypertext and renders it with the same library. The
# library must not need either of them in order to build, and neither program
# may need the other; that is the whole of what the boundary is for.
VK_SRCS := $(shell find src/render/vulkan -name '*.cpp' 2>/dev/null)
LIB_SRCS := $(filter-out $(VK_SRCS),$(shell find thirdparty/Choreograph/src/ src/ -name '*.cpp'))
ifdef GLEDITOR_ENABLE_VULKAN
LIB_SRCS += $(VK_SRCS)
endif
GLEDITOR_SRCS  := $(shell find apps/gleditor -name '*.cpp' 2>/dev/null)
# The xanalogical engine is separated from the program that displays it so that
# it can be tested without a graphics device: every rule about versions, spans
# and links is decidable from the store alone.
XUDU_CORE_SRCS := $(shell find apps/xudu/core -name '*.cpp' 2>/dev/null)
XUDU_SRCS      := $(filter-out $(XUDU_CORE_SRCS),$(shell find apps/xudu -name '*.cpp' 2>/dev/null))
LIB_TEST_SRCS  := $(shell find tests/lib -name '*.cpp' 2>/dev/null)
XUDU_TEST_SRCS := $(shell find tests/xudu -name '*.cpp' 2>/dev/null)

OBJDIR := build/
obj = $(addprefix $(OBJDIR)/,$(patsubst %.cpp,%.o,$(1)))
LIB_OBJS        := $(call obj,$(LIB_SRCS))
GLEDITOR_OBJS   := $(call obj,$(GLEDITOR_SRCS))
XUDU_CORE_OBJS  := $(call obj,$(XUDU_CORE_SRCS))
XUDU_OBJS       := $(call obj,$(XUDU_SRCS))
LIB_TEST_OBJS   := $(call obj,$(LIB_TEST_SRCS))
XUDU_TEST_OBJS  := $(call obj,$(XUDU_TEST_SRCS))
SWARM_PEER_OBJS := $(call obj,tools/xudu-swarm-peer.cpp)

# What a shared library is called, and how a program finds it, differ enough
# between the two targets that both are spelled out rather than guessed at.
#
# On ELF the real name carries the ABI version and the linker name does not:
# `-lgleditor` finds the second, which is a symlink to the first, and a program
# records the first in its DT_NEEDED. That is what lets an incompatible library
# be installed beside this one rather than over it.
#
# Windows has no soname and no symlink. The DLL is what is loaded, an import
# library produced by the same link is what a program links against, and the
# loader searches the executable's own directory -- so there is nothing for a
# run path to do.
SOVERSION := 0
UNAME_S := $(shell uname -s 2>/dev/null || echo unknown)
ifneq (,$(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)))
WINDOWS := 1
endif

ifdef WINDOWS
LIBNAME     := libgleditor.dll
LIBREAL     := $(OBJDIR)/$(LIBNAME)
LIBLINK     := $(OBJDIR)/libgleditor.dll.a
LIB_LINKARG := -Wl,--out-implib,$(LIBLINK)
RPATH_FLAGS :=
else
LIBNAME     := libgleditor.so
LIBREAL     := $(OBJDIR)/$(LIBNAME).$(SOVERSION)
LIBLINK     := $(OBJDIR)/$(LIBNAME)
LIB_LINKARG := -Wl,-soname,$(LIBNAME).$(SOVERSION)
# A program finds the library beside itself, which is what lets one run out of
# build/ with no LD_LIBRARY_PATH. $$ORIGIN reaches the shell as a literal, so
# it stays a run-time lookup rather than a path resolved at link time.
RPATH_FLAGS := -Wl,-rpath,'$$ORIGIN'
# The install directory is recorded as well, but only when the dynamic linker
# would not find it anyway. Under a custom prefix that is the difference
# between a program that starts and one that does not; under /usr it is at best
# noise, and Fedora's check-rpaths fails the package build outright on a
# runpath naming /usr/local/lib. Anything under /usr, and /lib and /lib64
# themselves, count as somewhere the linker already looks -- which covers
# Debian's multiarch directory without naming it.
ifeq (,$(filter /usr/% /lib /lib64,$(libdir)))
RPATH_FLAGS += -Wl,-rpath,$(libdir)
endif
endif

ALL_OBJS := $(sort $(LIB_OBJS) $(GLEDITOR_OBJS) $(XUDU_CORE_OBJS) $(XUDU_OBJS) \
	$(LIB_TEST_OBJS) $(XUDU_TEST_OBJS) $(SWARM_PEER_OBJS))
ALL_OBJ_DIRS := $(sort $(OBJDIR)/ $(OBJDIR)/tmp/ $(dir $(ALL_OBJS)))
DEPS := $(sort $(patsubst %.o,%.dep,$(ALL_OBJS)))
JFILES := $(sort $(patsubst %.o,%.j,$(ALL_OBJS)))

ifneq ($(LANDLOCKMAKE_VERSION),)
.STRICT = 1
.UNVEIL = \
	rwcx:$(OBJDIR)/ \
	rwcx:$(OBJDIR)/src/ \
	rwcx:$(OBJDIR)/tmp/ \
	rx:thirdparty/ \
	include/ src/ apps/ tests/ \
	rw:/dev/null \
	rx:/usr/bin/ \
	rx:/usr/include/ \
	$(shell pkg-config $(STATIC) --cflags-only-I $(PKGS) $(TEST_PKGS) | $(SED) 's/ *-I\([^ ]*\)/\1\n/g')

.PLEDGE = exec proc prot_exec stdio rpath wpath cpath

endif

.FEATURES = output-sync


SPIRV := assets/shaders/vulkan/glyph.vert.spv assets/shaders/vulkan/glyph.frag.spv \
	assets/shaders/vulkan/beam.vert.spv assets/shaders/vulkan/beam.frag.spv

all: lib gleditor xudu gleditor_test xudu_test $(OBJDIR)/compile_commands.json
ifdef GLEDITOR_ENABLE_VULKAN
all: shaders
endif

# cannot unveil a nonexistant directory, have to remove the sandbox for
# just the directory creation
$(ALL_OBJ_DIRS): private .UNSANDBOXED = 1
$(ALL_OBJ_DIRS):
	[ -d "$@" ] || $(MKDIR) -p "$@"

$(ALL_OBJS): | $(ALL_OBJ_DIRS)
$(DEPS) $(JFILES) $(OBJDIR)/src/config.h: | $(ALL_OBJ_DIRS)
$(LIB_TEST_OBJS) $(XUDU_TEST_OBJS): CXXFLAGS += $(shell pkg-config $(STATIC) --cflags $(TEST_PKGS))

ifeq (,$(filter clean,$(MAKECMDGOALS)))
MKCFG = $(SED) 's/\@\@VERS\@\@/$(VERS)/'
# cmd ... | read var is the only way to mimic subshells in cocmd
# only supported elsewhere in zsh and possibly ksh
ifeq ($(findstring cocmd,$(SHELL)),cocmd)
$(OBJDIR)/src/config.h: src/config.h.in $(VERS)
	@$(MKCFG) $< | read cfg; \
	[ -e $@ ] && cat $@ | read origCfg; \
	[ "$$cfg" = "$$origCfg" ] || \
	echo $$cfg > $@
else
$(OBJDIR)/src/config.h: src/config.h.in $(VERS)
	@[ "`$(MKCFG) $< | $(CKSUM)`" = "`cat $@ 2>/dev/null | $(CKSUM)`" ] || \
	$(MKCFG) $< > $@
endif
.PHONY: $(VERS)
endif

# Object files do not otherwise depend on the flags they were compiled with, so
# toggling GLEDITOR_ENABLE_VULKAN or DEBUG left stale objects behind and the
# resulting binary silently disagreed with the build that was asked for.
# Recording the flags in a stamp file makes every object depend on them.
FLAGSTAMP := $(OBJDIR)/.buildflags
.PHONY: FORCE
FORCE:
$(FLAGSTAMP): FORCE | $(OBJDIR)/
	@sig='$(CXXFLAGS) $(LDFLAGS)'; 	[ "`cat $@ 2>/dev/null`" = "$$sig" ] || printf '%s' "$$sig" > $@
$(ALL_OBJS): $(FLAGSTAMP)

$(OBJDIR)/apps/gleditor/main.o $(OBJDIR)/apps/gleditor/main.dep: $(OBJDIR)/src/config.h
$(OBJDIR)/apps/xudu/main.o $(OBJDIR)/apps/xudu/main.dep: $(OBJDIR)/src/config.h

# The SPIR-V the Vulkan backend loads is produced from the same portable shader
# bodies the GL backends compile at runtime, and through the same preamble
# generator, so the two forms cannot drift apart.
$(OBJDIR)/shader_assemble: tools/shader_assemble.cpp src/render/shader_source.cpp src/render/backend.cpp | $(OBJDIR)/
	$(CXX) $(CXXFLAGS) -o $@ $^

assets/shaders/vulkan/%.spv: assets/shaders/%.glsl $(OBJDIR)/shader_assemble
	@[ -n "$(GLSLANG)" ] || { echo "neither glslangValidator nor glslang found on PATH; install glslang-tools (Debian) or glslang (Fedora, Arch)" >&2; exit 1; }
	@$(MKDIR) -p assets/shaders/vulkan $(OBJDIR)/shaders
	$(OBJDIR)/shader_assemble vulkan $(word 2,$(subst ., ,$(notdir $<))) $< $(OBJDIR)/shaders/$(notdir $<)
	$(GLSLANG) -V --target-env vulkan1.0 -S $(word 2,$(subst ., ,$(notdir $<))) $(OBJDIR)/shaders/$(notdir $<) -o $@

shaders: $(SPIRV)
.PHONY: shaders

# -- the library --------------------------------------------------------------

lib: $(LIBLINK)
.PHONY: lib

# Libraries after the objects that need them. GNU ld resolves left to right, so
# the other order only ever worked because clang's linker is forgiving about
# it; gcc with link-time optimisation, which is what Debian builds with,
# reported every pangomm and glibmm symbol as undefined.
$(LIBREAL): $(LIB_OBJS)
	$(CXX) $(LDFLAGS) -shared $(LIB_LINKARG) -o $@ $^ $(LIBS)

ifdef WINDOWS
# The import library falls out of the link above, so asking for it is asking
# for the DLL and there is nothing further to do.
$(LIBLINK): $(LIBREAL)
	@:
else
$(LIBLINK): $(LIBREAL)
	ln -sf $(notdir $(LIBREAL)) $@
endif

APP_LDFLAGS = -L$(OBJDIR) -lgleditor $(RPATH_FLAGS)

# -- the programs -------------------------------------------------------------

gleditor: $(OBJDIR)/gleditor
$(OBJDIR)/gleditor: $(GLEDITOR_OBJS) $(LIBLINK)
	$(CXX) $(LDFLAGS) -o $@ $(GLEDITOR_OBJS) $(APP_LDFLAGS) $(LIBS)
.PHONY: gleditor

xudu: $(OBJDIR)/xudu
$(OBJDIR)/xudu: $(XUDU_OBJS) $(XUDU_CORE_OBJS) $(LIBLINK)
	$(CXX) $(LDFLAGS) -o $@ $(XUDU_OBJS) $(XUDU_CORE_OBJS) $(APP_LDFLAGS) $(LIBS) $(XUDU_LIBS)
.PHONY: xudu

sanitize/address: CXXFLAGS += $(SANITIZE_ADDR_OPTS)
sanitize/address: LDFLAGS += $(SANITIZE_ADDR_OPTS)
sanitize/address: gleditor

sanitize/address/run: sanitize/address
	ASAN_OPTIONS=check_initialization_order=1:detect_leaks=1:strict_string_checks=1 $(OBJDIR)/gleditor

sanitize/thread: CXXFLAGS += $(SANITIZE_THR_OPTS)
sanitize/thread: LDFLAGS += $(SANITIZE_THR_OPTS)
sanitize/thread: gleditor

sanitize/thread/run: sanitize/thread
	TSAN_OPTIONS=second_deadlock_stack=1:detect_leaks=1:strict_string_checks=1 $(OBJDIR)/gleditor

# Only use if your entire library chain has been compiled with MSAN
# otherwise it will generate a neverending wave of false positives from SDL/stdlib
sanitize/memory: CXXFLAGS += $(SANITIZE_MEM_OPTS)
sanitize/memory: LDFLAGS += $(SANITIZE_MEM_OPTS)
sanitize/memory: gleditor

sanitize/memory/run: sanitize/memory
	MSAN_OPTIONS=check_initialization_order=1:detect_leaks=1:strict_string_checks=1 $(OBJDIR)/gleditor


.PHONY: gleditor_test xudu_test
TEST_LIBS = $(shell pkg-config $(STATIC) --libs $(TEST_PKGS))

# The library's own tests, linked against the library the programs link
# against: testing a separately compiled copy of the sources would not notice a
# symbol that failed to be exported.
gleditor_test: $(OBJDIR)/gleditor_test
$(OBJDIR)/gleditor_test: $(LIB_TEST_OBJS) $(LIBLINK)
	$(CXX) $(LDFLAGS) -o $@ $(LIB_TEST_OBJS) $(APP_LDFLAGS) $(LIBS) $(TEST_LIBS)

# The xanalogical engine's tests link the engine and not the library, so they
# run without a graphics device. That is the boundary being checked rather than
# merely asserted: if a rule about versions, links or content addresses ever
# needed a renderer, this would stop linking.
xudu_test: $(OBJDIR)/xudu_test
$(OBJDIR)/xudu_test: $(XUDU_TEST_OBJS) $(XUDU_CORE_OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(XUDU_LIBS) $(TEST_LIBS)

# The other end of the swarm tests: a peer that offers a torrent's content and
# waits to be asked. A separate program because the two peers are meant to be
# separate machines -- tools/swarm-netns-test.sh runs this one in a network
# namespace of its own.
.PHONY: xudu-swarm-peer
xudu-swarm-peer: $(OBJDIR)/xudu-swarm-peer
$(OBJDIR)/xudu-swarm-peer: $(OBJDIR)/tools/xudu-swarm-peer.o $(XUDU_CORE_OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(XUDU_LIBS)

# What the loader pays to lay a page out, against the two ways of asking Pango
# for it. Not part of `all`, because it measures rather than builds anything the
# programs use -- but a target, so that it goes on compiling: a measurement tool
# that no longer builds is worse than none, since it is reached for precisely
# when somebody is already unsure what is slow.
.PHONY: layout-latency-probe
layout-latency-probe: $(OBJDIR)/layout-latency-probe
$(OBJDIR)/layout-latency-probe: $(OBJDIR)/tools/layout-latency-probe.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

# The swarm tests proper, with the two peers on separate network stacks. Needs
# root, so it is not part of `make test`.
.PHONY: test/swarm
test/swarm: $(OBJDIR)/xudu-swarm-peer $(OBJDIR)/xudu_test
	tools/swarm-netns-test.sh

# Suites kept out of the tests a person runs after every change. Not because
# they are unimportant -- they cover the network path, which is where the
# surprises are -- but because they need a peer on another network stack to do
# anything, and take their time deciding there is not one. Without that peer
# they skip, which is worse than not running: a skip reads as a pass.
#
# The pull request checks run them for real; see the swarm job in
# .github/workflows/c-cpp.yml, and `make test/swarm` to run them here.
SLOW_TESTS  := SwarmTest.*:MutableNameTest.*
# Override to run something else, including everything: make test TEST_FILTER='*'
TEST_FILTER ?= -$(SLOW_TESTS)

test: $(OBJDIR)/gleditor_test $(OBJDIR)/xudu_test
	$(OBJDIR)/gleditor_test --gtest_filter='$(TEST_FILTER)'
	$(OBJDIR)/xudu_test --gtest_filter='$(TEST_FILTER)'

# Everything, slow suites included. What the pull request checks run, and what
# to run here before pushing.
.PHONY: test/all
test/all: $(OBJDIR)/gleditor_test $(OBJDIR)/xudu_test
	$(OBJDIR)/gleditor_test
	$(OBJDIR)/xudu_test

# produces gleditor_test.prof (a human-readable code coverage report) and
# coverage.lcov (a coverage report in lcov format) suitable for feeding into other tools like NeoVim
profile: CXXFLAGS += $(PROFILE_OPTS)
profile: LDFLAGS += $(PROFILE_OPTS)
profile: gleditor_test
	set -e; \
	raw=gleditor_test.profraw; data=gleditor_test.profdata; \
	trap "$(RM) -f $${raw} $${data}" EXIT HUP KILL TERM; \
	seq 1 100 | while read f; do \
		echo "*\c"; LLVM_PROFILE_FILE=$${raw} $(OBJDIR)/gleditor_test 2>&1 >/dev/null; \
	done; echo; \
	llvm-profdata merge -sparse $${raw} -o $${data}; \
	llvm-cov show $(OBJDIR)/gleditor_test -instr-profile=$${data} \
		-show-line-counts-or-regions -show-branches=count -show-expansions > gleditor_test.prof; \
	llvm-cov export $(OBJDIR)/gleditor_test --format=lcov --instr-profile=$${data} > coverage.lcov; \
	$(RM) -f $${raw} $${data};

profile/main: CXXFLAGS += $(PROFILE_OPTS)
profile/main: LDFLAGS += $(PROFILE_OPTS)
profile/main: gleditor
	set -e; \
	raw=gleditor.profraw; data=gleditor.profdata; \
	trap "$(RM) -f $${raw} $${data}" EXIT HUP KILL TERM; \
	seq 1 1 | while read f; do \
		echo "*\c"; LLVM_PROFILE_FILE=$${raw} $(OBJDIR)/gleditor --font "Serif 16" --profile tests/samples/kjv.txt 2>&1 >/dev/null; \
	done; echo; \
	llvm-profdata merge -sparse $${raw} -o $${data}; \
	export DEBUGINFOD_URLS=https://debuginfod.ubuntu.com; \
	llvm-cov show $(OBJDIR)/gleditor -instr-profile=$${data} \
	        -debuginfod \
		-show-line-counts-or-regions -show-mcdc \
		-show-branches=count -show-expansions > gleditor.prof; \
	llvm-cov export $(OBJDIR)/gleditor --format=lcov -debuginfod -instr-profile=$${data} > coverage.lcov; \
	$(RM) -f $${raw} $${data};


run: private .UNVEIL += rx:gleditor
run: $(OBJDIR)/gleditor
	$(OBJDIR)/gleditor tests/samples/quick_brown_fox.txt

doc:
	doxygen

clean: private .UNVEIL += w:gleditor w:gleditor_test w:xudu w:xudu_test
clean:
	@$(RM) -rf gleditor gleditor_test xudu xudu_test build

# -- installation -------------------------------------------------------------

INSTALL ?= install
# DESTDIR is how every packaging tool stages an install into a directory that
# is not the final one. It prefixes the paths and nothing else: what the
# program is told about itself still names the eventual location.
INSTALL_DATADIR := $(DESTDIR)$(appdir)

# GLEDITOR_DATADIR is compiled in here and nowhere else, so the binary that
# gets installed knows where its data went even if it is later moved somewhere
# the executable-relative search cannot follow.
install: GLEDITOR_DATADIR := $(appdir)
install: $(OBJDIR)/gleditor $(OBJDIR)/xudu
ifdef GLEDITOR_ENABLE_VULKAN
install: shaders
endif
install:
	$(INSTALL) -d $(DESTDIR)$(bindir)
	$(INSTALL) -m 755 $(OBJDIR)/gleditor $(DESTDIR)$(bindir)/gleditor
	$(INSTALL) -m 755 $(OBJDIR)/xudu $(DESTDIR)$(bindir)/xudu
	# The real name is what a program records; the linker name is what a later
	# build resolves -lgleditor against, so both have to be installed.
	$(INSTALL) -d $(DESTDIR)$(libdir)
	$(INSTALL) -m 755 $(LIBREAL) $(DESTDIR)$(libdir)/$(notdir $(LIBREAL))
	ln -sf $(LIBNAME).$(SOVERSION) $(DESTDIR)$(libdir)/$(LIBNAME)
	# Public headers, so something outside this tree can be built on the
	# library. Copied wholesale: the split between what a program may include
	# and what it may not is the directory itself.
	$(INSTALL) -d $(DESTDIR)$(includedir)
	cp -R include/gleditor $(DESTDIR)$(includedir)/
	$(INSTALL) -d $(DESTDIR)$(libdir)/pkgconfig
	$(SED) -e 's,@PREFIX@,$(prefix),g' -e 's,@LIBDIR@,$(libdir),g' \
	       -e 's,@INCLUDEDIR@,$(includedir),g' -e 's,@VERSION@,$(VERS),g' \
	       packaging/gleditor.pc.in > $(OBJDIR)/gleditor.pc
	$(INSTALL) -m 644 $(OBJDIR)/gleditor.pc $(DESTDIR)$(libdir)/pkgconfig/gleditor.pc
	$(INSTALL) -d $(INSTALL_DATADIR)/shaders
	$(INSTALL) -m 644 assets/shaders/*.glsl $(INSTALL_DATADIR)/shaders
ifdef GLEDITOR_ENABLE_VULKAN
	$(INSTALL) -d $(INSTALL_DATADIR)/shaders/vulkan
	$(INSTALL) -m 644 $(SPIRV) $(INSTALL_DATADIR)/shaders/vulkan
endif
	$(INSTALL) -m 644 logo.png $(INSTALL_DATADIR)/logo.png
	$(INSTALL) -d $(DESTDIR)$(datadir)/applications
	$(INSTALL) -m 644 packaging/gleditor.desktop $(DESTDIR)$(datadir)/applications/
	$(INSTALL) -d $(DESTDIR)$(datadir)/metainfo
	$(INSTALL) -m 644 packaging/gleditor.metainfo.xml $(DESTDIR)$(datadir)/metainfo/
	$(INSTALL) -d $(DESTDIR)$(datadir)/icons/hicolor/256x256/apps
	$(INSTALL) -m 644 logo.png $(DESTDIR)$(datadir)/icons/hicolor/256x256/apps/gleditor.png
	$(INSTALL) -d $(DESTDIR)$(mandir)/man1
	$(SED) 's,@DATADIR@,$(appdir),g' packaging/gleditor.1 > $(OBJDIR)/gleditor.1
	$(INSTALL) -m 644 $(OBJDIR)/gleditor.1 $(DESTDIR)$(mandir)/man1/gleditor.1
	$(SED) 's,@DATADIR@,$(appdir),g' packaging/xudu.1 > $(OBJDIR)/xudu.1
	$(INSTALL) -m 644 $(OBJDIR)/xudu.1 $(DESTDIR)$(mandir)/man1/xudu.1

uninstall:
	$(RM) -f $(DESTDIR)$(bindir)/gleditor
	$(RM) -f $(DESTDIR)$(bindir)/xudu
	$(RM) -f $(DESTDIR)$(libdir)/$(LIBNAME).$(SOVERSION)
	$(RM) -f $(DESTDIR)$(libdir)/$(LIBNAME)
	$(RM) -f $(DESTDIR)$(libdir)/pkgconfig/gleditor.pc
	$(RM) -rf $(DESTDIR)$(includedir)/gleditor
	$(RM) -rf $(INSTALL_DATADIR)
	$(RM) -f $(DESTDIR)$(datadir)/applications/gleditor.desktop
	$(RM) -f $(DESTDIR)$(datadir)/metainfo/gleditor.metainfo.xml
	$(RM) -f $(DESTDIR)$(datadir)/icons/hicolor/256x256/apps/gleditor.png
	$(RM) -f $(DESTDIR)$(mandir)/man1/gleditor.1
	$(RM) -f $(DESTDIR)$(mandir)/man1/xudu.1

# A release tarball, which is what the distribution packages build from. The
# submodules are vendored dependencies rather than optional extras, so the
# archive has to carry them: `git archive` alone produces a tree that cannot be
# compiled. VERSION is written into the tarball because the unpacked copy has
# no git history to ask.
DIST_NAME := gleditor-$(VERS)
dist:
	@$(RM) -rf $(OBJDIR)/dist/$(DIST_NAME)
	@$(MKDIR) -p $(OBJDIR)/dist/$(DIST_NAME)
	git archive HEAD | tar -x -C $(OBJDIR)/dist/$(DIST_NAME)
	git submodule foreach --quiet \
	  'mkdir -p "$(CURDIR)/$(OBJDIR)/dist/$(DIST_NAME)/$$sm_path" && \
	   git archive HEAD | tar -x -C "$(CURDIR)/$(OBJDIR)/dist/$(DIST_NAME)/$$sm_path"'
	echo $(VERS) > $(OBJDIR)/dist/$(DIST_NAME)/VERSION
	tar -czf $(OBJDIR)/dist/$(DIST_NAME).tar.gz -C $(OBJDIR)/dist $(DIST_NAME)
	@echo "wrote $(OBJDIR)/dist/$(DIST_NAME).tar.gz"

$(OBJDIR)/%.o: %.cpp
	$(COMPILE.cpp) $(OUTPUT_OPTION) $<

# The rewritten target must carry the $(OBJDIR)/ prefix that $(OBJS) uses.
# Emitting a bare "tests/foo.o" attaches every header dependency to a target
# make never builds, so header edits silently produce a stale binary.
# -MP adds phony targets for the headers so that deleting one does not wedge
# the build with "No rule to make target".
$(OBJDIR)/%.dep: %.cpp
	set -e; $(RM) -f $@; \
	$(CXX) -MM -MP $(CXXFLAGS) $< > $@.$$$$; \
	$(SED) 's,^\($(*F)\)\.o[ :]*,$(OBJDIR)/$*.o $(OBJDIR)/$*.j : ,' < $@.$$$$ > $@; \
	$(RM) -f $@.$$$$

$(OBJDIR)/%.j: %.cpp
	$(REAL_CXX) -MJ $@ $(CXXFLAGS) -E $< > /dev/null
		
# clang -MJ emits one trailing-comma-terminated object per file, so the comma on
# the final entry has to go: JSON has no trailing commas and clangd rejects the
# whole database when one is present.
$(OBJDIR)/compile_commands.json: $(JFILES)
	{ echo '['; cat $^ | $(SED) '$$ s/,[[:space:]]*$$//'; echo ']'; } > $@


.PHONY: clean doc run test profile shaders install uninstall dist lib \
	sanitize/address sanitize/address/run sanitize/thread sanitize/thread/run \
	sanitize/memory sanitize/memory/run

# dist and clean compile nothing, and the .dep files are generated by running
# the compiler over every source. Including them for those goals meant `make
# dist` needed every header the build needs -- which is how the Arch package,
# whose only job at that point was to roll a tarball, failed on a missing
# glibmm header.
ifeq (,$(filter clean dist,$(MAKECMDGOALS)))
include $(DEPS)
endif
