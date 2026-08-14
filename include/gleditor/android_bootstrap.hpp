/**
 * @file android_bootstrap.hpp
 * @brief The one Android-specific entry point the rest of the library never
 *        needs to know exists.
 */
#ifndef GLEDITOR_ANDROID_BOOTSTRAP_H
#define GLEDITOR_ANDROID_BOOTSTRAP_H

#ifdef __ANDROID__

namespace gleditor {

/**
 * @brief Prepares the process for everything else main() does, on Android
 *        alone.
 *
 * Two things, both worked out here so that no other file has to know it is
 * running on Android:
 *
 *  - The shaders are bundled as APK assets, which std::ifstream cannot open --
 *    they live inside a zip, not on a filesystem. This copies the small fixed
 *    set the renderer reads out to real storage and points GLEDITOR_ASSET_DIR
 *    at them, which paths.cpp already honours verbatim.
 *  - There is no command line to put --backend on. The compiled-in default is
 *    "opengles", chosen for reaching every device; this reads a "backend"
 *    extra off the launch intent, when one was given, so
 *    `adb shell am start -n <pkg>/org.libsdl.app.SDLActivity --es backend
 *    vulkan` can exercise the Vulkan backend without a rebuild.
 *
 * Must run before gleditor::addCommonArguments builds the argument parser,
 * since that is where the backend default is read.
 */
void androidBootstrap();

} // namespace gleditor

#endif // __ANDROID__

#endif // GLEDITOR_ANDROID_BOOTSTRAP_H
