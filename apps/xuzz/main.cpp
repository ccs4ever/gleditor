// The combined frontend reuses Xudu's application composition while enabling
// its Zigzag presentation boundary at compile time.
//
// XUZZ_BUILD gates #ifdef branches in the included file, which a constexpr
// constant cannot do.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define XUZZ_BUILD 1
#define main xuzz_xudu_main
// Deliberate reuse of xudu's entire main.cpp under a renamed entry point,
// not an accidental .cpp include.
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../xudu/main.cpp"
#undef main

// Forwards to xudu's own main, whose catch(std::exception) is the
// deliberate boundary documented there.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(const int argc, char **argv) { return xuzz_xudu_main(argc, argv); }
