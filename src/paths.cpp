/**
 * @file paths.cpp
 * @brief Implementation of the data file search described in paths.hpp.
 */
#include <gleditor/paths.hpp> // IWYU pragma: associated

#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include <gleditor/sdl_compat.hpp>

namespace {

/**
 * @brief Directory the running executable sits in, or nullopt.
 *
 * SDL knows this on every platform it supports, which is the reason to ask it
 * rather than to reach for /proc/self/exe and a Windows equivalent. It answers
 * only once SDL has been initialised, so the unit tests -- which never open a
 * window -- get nullopt here and fall through to the later candidates.
 */
std::optional<std::filesystem::path> executableDir() {
  const auto *base = sdl::basePath();
  if (nullptr == base || '\0' == base[0]) {
    return std::nullopt;
  }
  return std::filesystem::path(base);
}

/// True when @p dir names an existing directory, with filesystem errors
/// treated as "no": a candidate that cannot be stat'd cannot be used.
bool isDirectory(const std::filesystem::path &dir) {
  std::error_code err;
  return std::filesystem::is_directory(dir, err);
}

std::string findAssetDir() {
  // An explicit request is honoured as given. Checking it for existence and
  // silently moving on would turn "you pointed me at the wrong directory" into
  // "your shaders are missing", which is a far harder thing to work out.
  if (const auto *requested = std::getenv("GLEDITOR_ASSET_DIR");
      nullptr != requested && '\0' != requested[0]) {
    return requested;
  }

  if (const auto exeDir = executableDir()) {
    // Portable layout first: a tree that carries its own assets means them.
    if (const auto portable = *exeDir / "assets"; isDirectory(portable)) {
      return portable.string();
    }
    // Installed layout: bin/gleditor -> share/gleditor. SDL hands back the
    // directory with a trailing separator, which filesystem::path reads as an
    // empty filename, so the first parent_path() only strips that.
    if (const auto installed =
            exeDir->parent_path().parent_path() / "share" / "gleditor";
        isDirectory(installed)) {
      return installed.string();
    }
  }

#ifdef GLEDITOR_DATADIR
  if (const std::filesystem::path configured(GLEDITOR_DATADIR);
      isDirectory(configured)) {
    return configured.string();
  }
#endif

  // The source tree, and the last word: returned whether or not it exists, so
  // that a failure names the directory the program actually looked in.
  return "assets";
}

std::mutex cacheLock;
std::optional<std::string> cachedDir;

} // namespace

namespace gleditor {

std::string assetDir() {
  // Worked out once. The search stats a handful of directories and the answer
  // is wanted from more than one place; caching it also stops the answer
  // changing under a program that has already acted on it.
  const std::lock_guard guard(cacheLock);
  if (!cachedDir.has_value()) {
    cachedDir = findAssetDir();
  }
  return *cachedDir;
}

std::string assetPath(const std::string_view relative) {
  return (std::filesystem::path(assetDir()) / relative).string();
}

void resetAssetDirForTesting() {
  const std::lock_guard guard(cacheLock);
  cachedDir.reset();
}

} // namespace gleditor
// vi: set sw=2 sts=2 ts=2 et:
