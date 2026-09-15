// The combined frontend reuses Xudu's application composition while enabling
// its Zigzag presentation boundary at compile time.
#define XUZZ_BUILD 1
#define main xuzz_xudu_main
#include "../xudu/main.cpp"
#undef main

int main(const int argc, char **argv) { return xuzz_xudu_main(argc, argv); }
