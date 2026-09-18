// The combined frontend reuses Xudu's application composition while enabling
// its Zigzag presentation boundary at compile time.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage) -- gates #ifdef branches in
// the included file, which a constexpr constant cannot do.
#define XUZZ_BUILD 1
#define main xuzz_xudu_main
// NOLINTNEXTLINE(bugprone-suspicious-include) -- deliberate reuse of xudu's
// entire main.cpp under a renamed entry point, not an accidental .cpp include.
#include "../xudu/main.cpp"
#undef main

int main(const int argc, char **argv) { return xuzz_xudu_main(argc, argv); }
