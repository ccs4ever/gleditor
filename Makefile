CXX = clang++
PKGS := pangomm-2.48 spdlog sdl2 SDL2_image gl glu glew
TEST_PKGS := gmock_main
#ifdef DEBUG
DEBUG_OPTS := -g -gembed-source -fdebug-macro -O0 -fprofile-instr-generate -fcoverage-mapping
#endif
CXXFLAGS = $(DEBUG_OPTS) -std=c++23 -Wall -Wextra $(shell pkg-config --cflags $(PKGS))
LDFLAGS = $(DEBUG_OPTS)
LIBS := $(shell pkg-config --libs $(PKGS))
SHARED_SRCS := $(shell find src/ -name '*.cpp' -a ! -name main.cpp )
SRCS := $(SHARED_SRCS) src/main.cpp
TEST_SRCS := $(SHARED_SRCS) $(shell find tests/ -name '*.cpp')
OBJS := $(patsubst %.cpp,%.o,$(SRCS))
TEST_OBJS := $(patsubst %.cpp,%.o,$(TEST_SRCS))
ALL_OBJS := $(sort $(OBJS) $(TEST_OBJS))
DEPS := $(sort $(patsubst %.cpp,%.dep,$(TEST_SRCS) $(SRCS)))
JFILES := $(sort $(patsubst %.cpp,%.j,$(TEST_SRCS) $(SRCS)))

all: gleditor gleditor_test compile_commands.json

$(TEST_OBJS): CXXFLAGS += $(shell pkg-config --cflags $(TEST_PKGS))

gleditor: $(OBJS)
	$(CXX) $(LIBS) $(LDFLAGS) $(OBJS) -o gleditor

gleditor_test: $(TEST_OBJS)
	$(CXX) $(LIBS) $(LDFLAGS) $(shell pkg-config --libs $(TEST_PKGS)) $(TEST_OBJS) -o gleditor_test

test: gleditor_test
	./gleditor_test

# produces gleditor_test.prof (a human-readable code coverage report) and
# coverage.lcov (a coverage report in lcov format) suitable for feeding into other tools like NeoVim
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
	rm -rf gleditor gleditor_test $(shell find src tests -name '*.[oj]' -o -name '*.dep' -o -name '*.dep.[0-9]*')

.PHONY: clean doc run test profile

%.dep: %.cpp
	set -e; rm -f $@; \
	$(CXX) -MM $(CXXFLAGS) $< > $@.$$$$; \
	sed 's,\($(*F)\)\.o[ :]*,$*.o $*.j $@ : ,g' < $@.$$$$ > $@; \
	rm -f $@.$$$$

%.j: %.cpp
	$(CXX) -MJ $@ $(CXXFLAGS) -E $< > /dev/null
		
compile_commands.json: $(JFILES)
	{ echo '['; find . -name '*.j' -exec cat '{}' +; echo ']'; } > compile_commands.json

ifeq (,$(filter clean,$(MAKECMDGOALS)))
include $(DEPS)
endif
