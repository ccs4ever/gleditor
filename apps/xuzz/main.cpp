/**
 * @file main.cpp
 * @brief Sovereign entry point for Xuzz.
 */
#include "xuzz_app.hpp"
#include <gleditor/android_bootstrap.hpp>
#include <gleditor/app.hpp>
#include <iostream>

#ifdef __ANDROID__
#include <SDL3/SDL_main.h>
#endif

int main(const int argc, char **argv) {
#ifdef __ANDROID__
  gleditor::androidBootstrap();
#endif
  gleditor::initLocale();

  try {
    xuzz::XuzzApp app;
    return app.run(argc, argv);
  } catch (const std::exception &err) {
    std::cerr << "Error: " << err.what() << "\n";
    return 1;
  }
}
