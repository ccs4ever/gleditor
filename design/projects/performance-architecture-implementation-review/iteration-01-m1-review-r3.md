# M1 LLVM profile merge review — iteration 01, pass 3

## Verdict: ready

The narrow fix is correct. Both LLVM merge sites now use the default `llvm-profdata merge` behavior:
the compiler capability probe and the actual training-profile merge omit `-sparse`. GCC's profile
path is unchanged. The docs explain why the zero-count function records matter and link to LLVM's
option documentation.

The regression test compiles and runs a Clang-instrumented translation unit containing one uncalled
function, then compares default and sparse merged profiles with `llvm-profdata show`. It confirms
the default profile retains the function with count zero while the sparse profile drops it. The
mocked full pipeline also checks that the actual merge command contains no `-sparse`.

## Verification

- `python3 -m unittest tests/performance/test_release_profile.py -v`: 15 tests passed, including the
  real Clang/llvm-profdata cold-function check and mocked lifecycle check.
- No project build or runtime workload was run.
