
#SHELL = thirdparty/cosmocc/bin/cocmd
SHELL = dash

#STATIC = --static
STATIC =

CXX = $(shell which clang++)
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
PKGS := pangomm-2.48 sdl3 sdl3-image gl glu glew
ifdef GLEDITOR_ENABLE_VULKAN
PKGS += vulkan
endif
TEST_PKGS := gmock_main
VERS := $(shell git describe --tags --always --match "v[0-9]*.[0-9]*.[0-9]*" HEAD | tr -d v)
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
override CXXFLAGS += $(DEBUG_OPTS) -std=c++2c -Ibuild/src -Iinclude -Ithirdparty/Choreograph/src -Ithirdparty/argparse/include -Wall -Wextra $(shell pkg-config $(STATIC) --cflags $(PKGS))
ifdef GLEDITOR_ENABLE_VULKAN
override CXXFLAGS += -DGLEDITOR_ENABLE_VULKAN=1
endif
override LDFLAGS += $(DEBUG_OPTS) $(findstring $(STATIC),-static) -rtlib=compiler-rt 
# XXX: work on this in a separate branch, get tests working again for now
#CXXFLAGS += -stdlib=libc++ -fexperimental-library 
#LDFLAGS += -v -stdlib=libc++ -fexperimental-library 
LIBS := $(shell pkg-config $(STATIC) --libs $(PKGS))
SHARED_SRCS := $(shell find thirdparty/Choreograph/src/ src/ -name '*.cpp' -a ! -name main.cpp )
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


all: gleditor gleditor_test $(OBJDIR)/compile_commands.json

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

$(OBJDIR)/src/main.o $(OBJDIR)/src/main.dep: $(OBJDIR)/src/config.h

gleditor: $(OBJDIR)/gleditor
$(OBJDIR)/gleditor: $(OBJS)
	$(CXX) $(LIBS) $(LDFLAGS) -o $@ $^
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
	$(CXX) $(LIBS) $(LDFLAGS) $(shell pkg-config $(STATIC) --libs $(TEST_PKGS)) -o $@ $^

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
		echo "*\c"; LLVM_PROFILE_FILE=$${raw} $(OBJDIR)/gleditor --font "Serif 16" --profile --file kjv.txt 2>&1 >/dev/null; \
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
	$(OBJDIR)/gleditor test.txt

doc:
	doxygen

clean: private .UNVEIL += w:gleditor w:gleditor_test
clean:
	@$(RM) -rf gleditor gleditor_test build

$(OBJDIR)/%.o: %.cpp
	$(COMPILE.cpp) $(OUTPUT_OPTION) $<

$(OBJDIR)/%.dep: %.cpp
	set -e; $(RM) -f $@; \
	$(CXX) -MM $(CXXFLAGS) $< > $@.$$$$; \
	$(SED) 's,\($(*F)\)\.o[ :]*,$*.o $*.j : ,g' < $@.$$$$ > $@; \
	$(RM) -f $@.$$$$

$(OBJDIR)/%.j: %.cpp
	$(CXX) -MJ $@ $(CXXFLAGS) -E $< > /dev/null
		
$(OBJDIR)/compile_commands.json: $(JFILES)
	{ echo '['; cat $^; echo ']'; } > $@


.PHONY: clean doc run test profile sanitize/address sanitize/address/run sanitize/thread sanitize/thread/run \
	sanitize/memory sanitize/memory/run

ifeq (,$(filter clean,$(MAKECMDGOALS)))
include $(DEPS)
endif
