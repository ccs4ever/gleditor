/**
 * @file android_bootstrap.cpp
 * @brief Implementation of gleditor::androidBootstrap.
 */
#include <gleditor/android_bootstrap.hpp> // IWYU pragma: associated

#ifdef __ANDROID__

#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <system_error>

#include <glib-object.h>
#include <jni.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_system.h>

namespace gleditor {

namespace {

// dlopen() only runs a library's constructors after every constructor of the
// shared objects it depends on has already run -- that is what normally
// guarantees glib's own GType bootstrap finishes before glibmm's or
// pangomm's constructors touch it. That guarantee does not hold here: glib,
// glibmm and pangomm are all statically linked into this one libmain.so
// (see /CMakeLists.txt), so every translation unit's constructors, from all
// three, are merged into a single .init_array with no ordering between them.
// glibmm carries a legacy constructor of its own that calls the deprecated
// g_type_init(), which -- on the glib version vcpkg builds -- asserts
// rather than initializing anything if the type system was not already
// bootstrapped by the time it runs, crashing the app before main() is ever
// reached. Forcing that bootstrap here first, via g_type_ensure() (the
// public, non-deprecated replacement) at constructor priority 101 -- the
// highest available to user code, since 0-100 is reserved for the
// implementation -- is what keeps glibmm's constructor from being the one
// that loses that race.
__attribute__((constructor(101))) void primeGTypeSystem() {
  g_type_ensure(G_TYPE_OBJECT);
}

// The full set of files the GL/GLES and Vulkan backends open by path, kept in
// step with assets/shaders/*.glsl and the SPIRV list in the Makefile. Small
// and fixed, so copying the lot on every launch is simpler than tracking
// whether it changed -- a few kilobytes of text and SPIR-V, not an asset
// pack.
constexpr std::array<const char *, 8> kShaderAssets = {
    "shaders/glyph.vert.glsl",       "shaders/glyph.frag.glsl",
    "shaders/beam.vert.glsl",        "shaders/beam.frag.glsl",
    "shaders/vulkan/glyph.vert.spv", "shaders/vulkan/glyph.frag.spv",
    "shaders/vulkan/beam.vert.spv",  "shaders/vulkan/beam.frag.spv",
};

/// Copies one asset out of the APK. SDL_LoadFile resolves a relative path
/// through the Android asset manager, which is what makes the source side of
/// this a normal-looking read; the write side is a plain file because
/// GLEDITOR_ASSET_DIR must name somewhere std::ifstream can later open.
void copyAsset(const std::filesystem::path &destRoot, const char *relative) {
  std::size_t size = 0;
  void *data = SDL_LoadFile(relative, &size);
  if (nullptr == data) {
    // Left for whatever opens it later to report by name, same as a missing
    // asset on any other platform.
    return;
  }
  const auto destPath = destRoot / relative;
  std::error_code err;
  std::filesystem::create_directories(destPath.parent_path(), err);
  std::ofstream out(destPath, std::ios::binary | std::ios::trunc);
  out.write(static_cast<const char *>(data),
            static_cast<std::streamsize>(size));
  SDL_free(data);
}

void extractShaderAssets(const std::filesystem::path &internal) {
  const std::filesystem::path assetRoot = internal / "assets";
  for (const char *asset : kShaderAssets) {
    copyAsset(assetRoot, asset);
  }
  setenv("GLEDITOR_ASSET_DIR", assetRoot.string().c_str(), 1);
}

/// Android has no fontconfig of its own; without one, Pango finds no fonts at
/// all. This writes out the bundled fonts.conf (assets/fontconfig/fonts.conf,
/// which points at /system/fonts and restates the generic family aliases a
/// desktop fontconfig install would otherwise supply) with its cache
/// directory placeholder filled in, and points FONTCONFIG_FILE at the result.
void extractFontConfig(const std::filesystem::path &internal) {
  std::size_t size = 0;
  void *data = SDL_LoadFile("fontconfig/fonts.conf", &size);
  if (nullptr == data) {
    return;
  }
  std::string conf(static_cast<const char *>(data), size);
  SDL_free(data);

  const auto cacheDir = internal / "fontconfig-cache";
  std::error_code err;
  std::filesystem::create_directories(cacheDir, err);

  const std::string placeholder = "@@CACHE_DIR@@";
  if (const auto pos = conf.find(placeholder); std::string::npos != pos) {
    conf.replace(pos, placeholder.size(), cacheDir.string());
  }

  const auto destPath = internal / "fontconfig" / "fonts.conf";
  std::filesystem::create_directories(destPath.parent_path(), err);
  std::ofstream out(destPath, std::ios::binary | std::ios::trunc);
  out.write(conf.data(), static_cast<std::streamsize>(conf.size()));

  setenv("FONTCONFIG_FILE", destPath.string().c_str(), 1);
}

/// Reads a "backend" extra off the launch intent, when the app was started
/// with one, and applies it the same way GLEDITOR_BACKEND already works
/// everywhere else. SDL's stock Java Activity is used unmodified, so this --
/// JNI calls against the Activity and Intent SDL itself hands back -- is the
/// only way in that does not mean writing a custom one.
void applyBackendOverrideFromIntent() {
  auto *env      = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
  auto activity  = static_cast<jobject>(SDL_GetAndroidActivity());
  if (nullptr == env || nullptr == activity) {
    return;
  }

  const jclass activityClass = env->GetObjectClass(activity);
  const jmethodID getIntent  = env->GetMethodID(
      activityClass, "getIntent", "()Landroid/content/Intent;");
  const jobject intent =
      nullptr != getIntent ? env->CallObjectMethod(activity, getIntent)
                            : nullptr;
  if (nullptr == intent) {
    return;
  }

  const jclass intentClass = env->GetObjectClass(intent);
  const jmethodID getStringExtra =
      env->GetMethodID(intentClass, "getStringExtra",
                        "(Ljava/lang/String;)Ljava/lang/String;");
  if (nullptr == getStringExtra) {
    return;
  }
  const jstring key = env->NewStringUTF("backend");
  const auto value  = static_cast<jstring>(
      env->CallObjectMethod(intent, getStringExtra, key));
  env->DeleteLocalRef(key);
  if (nullptr == value) {
    return;
  }
  const char *chars = env->GetStringUTFChars(value, nullptr);
  setenv("GLEDITOR_BACKEND", chars, 1);
  env->ReleaseStringUTFChars(value, chars);
  env->DeleteLocalRef(value);
}

} // namespace

void androidBootstrap() {
  const char *internal = SDL_GetAndroidInternalStoragePath();
  if (nullptr != internal && '\0' != internal[0]) {
    extractShaderAssets(internal);
    extractFontConfig(internal);
  }
  applyBackendOverrideFromIntent();
}

} // namespace gleditor

#endif // __ANDROID__
// vi: set sw=2 sts=2 ts=2 et:
