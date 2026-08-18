/**
 * @file paths.hpp
 * @brief Where the program's data files are, wherever it happens to be run
 *        from.
 *
 * The shaders and the window icon are read at run time, so something has to
 * say where they are. Until this existed the answer was "./assets", which is
 * only true when the program is started from the source tree -- an installed
 * /usr/bin/gleditor found nothing, and the README had to tell people to cd to
 * the repository first. That is the one thing a package cannot ask of a user.
 */
#ifndef GLEDITOR_PATHS_H
#define GLEDITOR_PATHS_H

#include <string>
#include <string_view>

namespace gleditor {

/**
 * @brief Directory holding the installed data files.
 *
 * The first of these that exists wins, and the answer is worked out once:
 *
 *  1. `$GLEDITOR_ASSET_DIR`, taken as given without an existence check, so a
 *     wrong value reports itself as a missing shader rather than silently
 *     falling through to a directory the caller did not mean.
 *  2. `<directory of the executable>/assets` -- the portable layout, which is
 *     what the Windows zip unpacks to.
 *  3. `<directory of the executable>/../share/gleditor` -- the installed Unix
 *     layout. Derived from the executable rather than from the configured
 *     prefix so that moving or relocating a tree keeps working.
 *  4. `GLEDITOR_DATADIR`, if the build defined one. The install target does;
 *     a plain `make` deliberately does not, because a developer's build tree
 *     must not quietly pick up the assets of an older installed copy.
 *  5. `./assets`, which is the source tree, and is what makes `make run` and
 *     every existing tool keep working unchanged.
 *
 * Step 2 and 3 need the executable's own path, which comes from SDL and is
 * therefore only available once SDL has been initialised. When it is not --
 * in the unit tests, which never open a window -- those steps are skipped
 * rather than treated as an error.
 */
[[nodiscard]] std::string assetDir();

/// Path to @p relative within assetDir(), with the platform's separator.
[[nodiscard]] std::string assetPath(std::string_view relative);

/**
 * @brief Forget the cached answer, so the next call works it out again.
 *
 * Only for tests, which set `$GLEDITOR_ASSET_DIR` to different values in one
 * process and would otherwise see whichever was set first.
 */
void resetAssetDirForTesting();

} // namespace gleditor

#endif // GLEDITOR_PATHS_H
