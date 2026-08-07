
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
CXX := $(shell which clang++ 2>/dev/null || which c++)
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
# Checked for the same reason: without it the build carries on with empty
# include flags and fails much later on a header, naming the header rather than
# the package. Distributions ship several parallel pangomm ABIs under different
# package names, so getting the wrong one is an ordinary packaging mistake.
ifneq ($(shell pkg-config --exists pangomm-2.48 && echo 1),1)
$(error pangomm-2.48 not found by pkg-config; on Arch it is pangomm-2.48, on Fedora pangomm2.48-devel, on Debian libpangomm-2.48-dev)
endif

# The GL/GLES entry points are resolved at runtime through SDL rather than
# linked, so no GL library is needed here; `gl` is still listed because the
# backend includes GL/glcorearb.h for its typedefs and enum values.
PKGS := pangomm-2.48 $(SDL_PKG) gl
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
TEST_PKGS := gmock_main

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
override CXXFLAGS += $(DEBUG_OPTS) $(STD_FLAG) -Ibuild/src -Iinclude -Ithirdparty/Choreograph/src -Ithirdparty/argparse/include -Wall -Wextra $(shell pkg-config $(STATIC) --cflags $(PKGS))
ifdef GLEDITOR_DATADIR
override CXXFLAGS += -DGLEDITOR_DATADIR='"$(GLEDITOR_DATADIR)"'
endif
override CXXFLAGS += -DGLEDITOR_SDL_MAJOR=$(GLEDITOR_SDL)
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
GLSLANG := $(shell which glslangValidator 2>/dev/null)
# The Vulkan backend only compiles when Vulkan is enabled; everything else is
# backend-neutral or belongs to the GL family backend.
VK_SRCS := $(shell find src/render/vulkan -name '*.cpp' 2>/dev/null)
SHARED_SRCS := $(filter-out $(VK_SRCS),$(shell find thirdparty/Choreograph/src/ src/ -name '*.cpp' -a ! -name main.cpp ))
ifdef GLEDITOR_ENABLE_VULKAN
SHARED_SRCS += $(VK_SRCS)
endif
SRCS := $(SHARED_SRCS) src/main.cpp
TEST_SRCS := $(SHARED_SRCS) $(shell find tests/ -name '*.cpp')
OBJDIR := build/
OBJS := $(addprefix $(OBJDIR)/,$(patsubst %.cpp,%.o,$(SRCS)))
TEST_OBJS := $(addprefix $(OBJDIR)/,$(patsubst %.cpp,%.o,$(TEST_SRCS)))
OBJ_DIRS := $(sort $(dir $(OBJS)))
TEST_OBJ_DIRS := $(sort $(dir $(TEST_OBJS)))
ALL_OBJS := $(sort $(OBJS) $(TEST_OBJS))
ALL_OBJ_DIRS := $(sort $(OBJDIR)/ $(OBJDIR)/tmp/ $(OBJ_DIRS) $(TEST_OBJ_DIRS))
DEPS := $(sort $(patsubst %.o,%.dep,$(TEST_OBJS) $(OBJS)))
JFILES := $(sort $(patsubst %.o,%.j,$(TEST_OBJS) $(OBJS)))

ifneq ($(LANDLOCKMAKE_VERSION),)
.STRICT = 1
.UNVEIL = \
	rwcx:$(OBJDIR)/ \
	rwcx:$(OBJDIR)/src/ \
	rwcx:$(OBJDIR)/tmp/ \
	rx:thirdparty/ \
	include/ src/ tests/ \
	rw:/dev/null \
	rx:/usr/bin/ \
	rx:/usr/include/ \
	$(shell pkg-config $(STATIC) --cflags-only-I $(PKGS) $(TEST_PKGS) | $(SED) 's/ *-I\([^ ]*\)/\1\n/g')

.PLEDGE = exec proc prot_exec stdio rpath wpath cpath

endif

.FEATURES = output-sync


SPIRV := assets/shaders/vulkan/glyph.vert.spv assets/shaders/vulkan/glyph.frag.spv

all: gleditor gleditor_test $(OBJDIR)/compile_commands.json
ifdef GLEDITOR_ENABLE_VULKAN
all: shaders
endif

# cannot unveil a nonexistant directory, have to remove the sandbox for
# just the directory creation
$(ALL_OBJ_DIRS): private .UNSANDBOXED = 1
$(ALL_OBJ_DIRS):
	[ -d "$@" ] || $(MKDIR) -p "$@"

$(TEST_OBJS): | $(OBJDIR)/ $(OBJDIR)/tmp/ $(TEST_OBJ_DIRS)
$(OBJS): | $(OBJDIR)/ $(OBJDIR)/tmp/ $(OBJ_DIRS)
$(DEPS) $(JFILES) $(OBJDIR)/src/config.h: | $(OBJDIR)/ $(OBJDIR)/tmp/ $(OBJ_DIRS) $(TEST_OBJ_DIRS)
$(TEST_OBJS): CXXFLAGS += $(shell pkg-config $(STATIC) --cflags $(TEST_PKGS))

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

$(OBJDIR)/src/main.o $(OBJDIR)/src/main.dep: $(OBJDIR)/src/config.h

# The SPIR-V the Vulkan backend loads is produced from the same portable shader
# bodies the GL backends compile at runtime, and through the same preamble
# generator, so the two forms cannot drift apart.
$(OBJDIR)/shader_assemble: tools/shader_assemble.cpp src/render/shader_source.cpp src/render/backend.cpp | $(OBJDIR)/
	$(CXX) $(CXXFLAGS) -o $@ $^

assets/shaders/vulkan/%.spv: assets/shaders/%.glsl $(OBJDIR)/shader_assemble
	@[ -n "$(GLSLANG)" ] || { echo "glslangValidator not found; install glslang-tools" >&2; exit 1; }
	@$(MKDIR) -p assets/shaders/vulkan $(OBJDIR)/shaders
	$(OBJDIR)/shader_assemble vulkan $(word 2,$(subst ., ,$(notdir $<))) $< $(OBJDIR)/shaders/$(notdir $<)
	$(GLSLANG) -V --target-env vulkan1.0 -S $(word 2,$(subst ., ,$(notdir $<))) $(OBJDIR)/shaders/$(notdir $<) -o $@

shaders: $(SPIRV)
.PHONY: shaders

gleditor: $(OBJDIR)/gleditor
# Libraries after the objects that need them. GNU ld resolves left to right, so
# the other order only ever worked because clang's linker is forgiving about
# it; gcc with link-time optimisation, which is what Debian builds with,
# reported every pangomm and glibmm symbol as undefined.
$(OBJDIR)/gleditor: $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)
.PHONY: gleditor

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


.PHONY: gleditor_test
gleditor_test: $(OBJDIR)/gleditor_test
$(OBJDIR)/gleditor_test: $(TEST_OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS) $(shell pkg-config $(STATIC) --libs $(TEST_PKGS))

test: $(OBJDIR)/gleditor_test
	$(OBJDIR)/gleditor_test

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

clean: private .UNVEIL += w:gleditor w:gleditor_test
clean:
	@$(RM) -rf gleditor gleditor_test build

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
install: $(OBJDIR)/gleditor
ifdef GLEDITOR_ENABLE_VULKAN
install: shaders
endif
install:
	$(INSTALL) -d $(DESTDIR)$(bindir)
	$(INSTALL) -m 755 $(OBJDIR)/gleditor $(DESTDIR)$(bindir)/gleditor
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

uninstall:
	$(RM) -f $(DESTDIR)$(bindir)/gleditor
	$(RM) -rf $(INSTALL_DATADIR)
	$(RM) -f $(DESTDIR)$(datadir)/applications/gleditor.desktop
	$(RM) -f $(DESTDIR)$(datadir)/metainfo/gleditor.metainfo.xml
	$(RM) -f $(DESTDIR)$(datadir)/icons/hicolor/256x256/apps/gleditor.png
	$(RM) -f $(DESTDIR)$(mandir)/man1/gleditor.1

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
	$(CXX) -MJ $@ $(CXXFLAGS) -E $< > /dev/null
		
# clang -MJ emits one trailing-comma-terminated object per file, so the comma on
# the final entry has to go: JSON has no trailing commas and clangd rejects the
# whole database when one is present.
$(OBJDIR)/compile_commands.json: $(JFILES)
	{ echo '['; cat $^ | $(SED) '$$ s/,[[:space:]]*$$//'; echo ']'; } > $@


.PHONY: clean doc run test profile shaders install uninstall dist \
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
