/**
 * @file android_bootstrap.hpp
 * @brief The Android-specific entry points the rest of the library otherwise
 *        never needs to know exist.
 */
#ifndef GLEDITOR_ANDROID_BOOTSTRAP_H
#define GLEDITOR_ANDROID_BOOTSTRAP_H

#ifdef __ANDROID__

#include <string>

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

/**
 * @brief Writes @p content back through the Uri @p documentPath was opened
 *        from, if openDocumentFromIntent() opened it from a content:// one.
 *
 * @p documentPath is a Doc's name() -- for a document opened this way, that
 * is the internal-storage copy openDocumentFromIntent() made of it, not a
 * path the content:// Uri itself would answer to; this is what turns a write
 * there back into a write through the original Uri instead, using whatever
 * grant came with the intent that opened it. Returns false for anything else
 * -- a document opened from a file:// Uri (already a real, directly writable
 * path, needing none of this), one created fresh, or one opened before this
 * process last restarted -- which is Renderer::saveDoc's cue to write
 * @p documentPath as a plain file instead.
 */
bool androidSaveDocument(const std::string &documentPath,
                         const std::string &content);

} // namespace gleditor

#endif // __ANDROID__

#endif // GLEDITOR_ANDROID_BOOTSTRAP_H
