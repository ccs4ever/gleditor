CXX = clang++
# removed spdlog
PKGS := pangomm-2.48 sdl2 SDL2_image gl glu glew
TEST_PKGS := gmock_main
VERS := $(shell git describe --tags --always --match "v[0-9]*.[0-9]*.[0-9]*" HEAD | tr -d v)
#ifdef DEBUG
SANITIZE_ADDR_OPTS := -fsanitize=address,undefined,integer -fno-omit-frame-pointer -fsanitize-address-use-after-return=runtime \
	         -fsanitize-address-use-after-scope 
SANITIZE_THR_OPTS := -fsanitize=thread,undefined,integer -fno-omit-frame-pointer 
SANITIZE_MEM_OPTS := -fsanitize=memory,undefined,integer -fPIE -pie -fno-omit-frame-pointer \
		     -fsanitize-memory-track-origins
DEBUG_OPTS := -g -gembed-source -fdebug-macro -O0
PROFILE_OPTS := -fprofile-instr-generate -fcoverage-mapping 
#endif
CXXFLAGS = $(DEBUG_OPTS) -std=c++23 -Ithirdparty/Choreograph/src -Ithirdparty/argparse/include -Wall -Wextra $(shell pkg-config --cflags $(PKGS))
LDFLAGS = $(DEBUG_OPTS) -rtlib=compiler-rt 
# XXX: work on this in a separate branch, get tests working again for now
#CXXFLAGS += -stdlib=libc++ -fexperimental-library 
#LDFLAGS += -v -stdlib=libc++ -fexperimental-library 
LIBS := $(shell pkg-config --libs $(PKGS))
SHARED_SRCS := $(shell find thirdparty/Choreograph/src/ src/ -name '*.cpp' -a ! -name main.cpp )
SRCS := $(SHARED_SRCS) src/main.cpp
TEST_SRCS := $(SHARED_SRCS) $(shell find tests/ -name '*.cpp')
OBJS := $(patsubst %.cpp,%.o,$(SRCS))
TEST_OBJS := $(patsubst %.cpp,%.o,$(TEST_SRCS))
ALL_OBJS := $(sort $(OBJS) $(TEST_OBJS))
DEPS := $(sort $(patsubst %.cpp,%.dep,$(TEST_SRCS) $(SRCS)))
JFILES := $(sort $(patsubst %.cpp,%.j,$(TEST_SRCS) $(SRCS)))

all: gleditor gleditor_test compile_commands.json

$(TEST_OBJS): CXXFLAGS += $(shell pkg-config --cflags $(TEST_PKGS))

ifeq (,$(filter clean,$(MAKECMDGOALS)))
MKCFG = sed 's/\@\@VERS\@\@/$(VERS)/'
src/config.h: src/config.h.in $(VERS)
	@[ "`$(MKCFG) $< | cksum`" = "`cat $@ 2>/dev/null | cksum`" ] || \
	$(MKCFG) $< > $@
.PHONY: $(VERS)
endif

src/main.o src/main.dep: src/config.h

gleditor: $(OBJS)
	$(CXX) $(LIBS) $(LDFLAGS) $(OBJS) -o gleditor

sanitize/address: CXXFLAGS += $(SANITIZE_ADDR_OPTS)
sanitize/address: LDFLAGS += $(SANITIZE_ADDR_OPTS)
sanitize/address: gleditor

sanitize/address/run: sanitize/address
	ASAN_OPTIONS=check_initialization_order=1:detect_leaks=1:strict_string_checks=1 ./gleditor

sanitize/thread: CXXFLAGS += $(SANITIZE_THR_OPTS)
sanitize/thread: LDFLAGS += $(SANITIZE_THR_OPTS)
sanitize/thread: gleditor

sanitize/thread/run: sanitize/thread
	TSAN_OPTIONS=second_deadlock_stack=1:detect_leaks=1:strict_string_checks=1 ./gleditor

# Only use if your entire library chain has been compiled with MSAN
# otherwise it will generate a neverending wave of false positives from SDL/stdlib
sanitize/memory: CXXFLAGS += $(SANITIZE_MEM_OPTS)
sanitize/memory: LDFLAGS += $(SANITIZE_MEM_OPTS)
sanitize/memory: gleditor

sanitize/memory/run: sanitize/memory
	MSAN_OPTIONS=check_initialization_order=1:detect_leaks=1:strict_string_checks=1 ./gleditor


gleditor_test: $(TEST_OBJS)
	$(CXX) $(LIBS) $(LDFLAGS) $(shell pkg-config --libs $(TEST_PKGS)) $(TEST_OBJS) -o gleditor_test

test: gleditor_test
	./gleditor_test

# produces gleditor_test.prof (a human-readable code coverage report) and
# coverage.lcov (a coverage report in lcov format) suitable for feeding into other tools like NeoVim
profile: CXXFLAGS += $(PROFILE_OPTS)
profile: LDFLAGS += $(PROFILE_OPTS)
profile: gleditor_test
	set -e; \
	raw=gleditor_test.profraw; data=gleditor_test.profdata; \
	trap "rm -f $${raw} $${data}" EXIT HUP KILL TERM; \
	seq 1 100 | while read f; do \
		echo "*\c"; LLVM_PROFILE_FILE=$${raw} ./gleditor_test 2>&1 >/dev/null; \
	done; echo; \
	llvm-profdata merge -sparse $${raw} -o $${data}; \
	llvm-cov show ./gleditor_test -instr-profile=$${data} \
		-show-line-counts-or-regions -show-branches=count -show-expansions > gleditor_test.prof; \
	llvm-cov export ./gleditor_test --format=lcov --instr-profile=$${data} > coverage.lcov; \
	rm -f $${raw} $${data};


run: gleditor
	./gleditor

doc:
	doxygen

clean:
	@rm -rf gleditor gleditor_test $(shell find src tests -name '*.[oj]' -o -name '*.dep' -o -name '*.dep.[0-9]*')


%.dep: %.cpp
	set -e; rm -f $@; \
	$(CXX) -MM $(CXXFLAGS) $< > $@.$$$$; \
	sed 's,\($(*F)\)\.o[ :]*,$*.o $*.j : ,g' < $@.$$$$ > $@; \
	rm -f $@.$$$$

%.j: %.cpp
	$(CXX) -MJ $@ $(CXXFLAGS) -E $< > /dev/null
		
compile_commands.json: $(JFILES)
	{ echo '['; find . -name '*.j' -exec cat '{}' +; echo ']'; } > compile_commands.json


.PHONY: clean doc run test profile sanitize/address sanitize/address/run sanitize/thread sanitize/thread/run \
	sanitize/memory sanitize/memory/run

ifeq (,$(filter clean,$(MAKECMDGOALS)))
include $(DEPS)
endif
