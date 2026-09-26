/**
 * @file reading_place.hpp
 * @brief Where a reader was when they stopped, kept as cells in the
 *        reader's activity store so a session resumes there.
 */
#ifndef XANADU_READING_PLACE_HPP
#define XANADU_READING_PLACE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "microversion.hpp"

namespace xanadu {

class Store;

/// One open document: which store, which version, and the caret in it.
struct DocumentPlace {
  std::string storePath;
  std::string version;
  std::uint32_t caret{};
  /// The selection's other end; equal to @ref caret for none.
  std::uint32_t anchor{};

  bool operator==(const DocumentPlace &) const = default;
};

/**
 * @brief Everything a session needs to come back to where it was.
 *
 * The views, not the text: the text is in its stores, saved as it is typed,
 * and this names what was open onto it.
 */
struct ReadingPlace {
  /// In tab order.
  std::vector<DocumentPlace> documents;
  /// Index into @ref documents of the one with the caret.
  std::optional<std::size_t> active;
  /// Camera position and field of view.
  std::optional<std::array<double, 4>> camera;
  /// The store the ZigZag presentation showed, its focused cell, and whether
  /// it had the keyboard; empty store path for none.
  std::string zigzagStore;
  /// The version the slice was shown at: a store's text and its structure
  /// can be on different branches, so its latest is not necessarily it.
  std::string zigzagVersion;
  std::int64_t zigzagFocus{};
  bool zigzagHasKeyboard{};

  bool operator==(const ReadingPlace &) const = default;
};

/**
 * @brief Append @p place to @p store as its newest place.
 *
 * Append-only, as the activity store is (R8): a place is a new cell at the
 * end of the home cell's d.places rank, with its documents along
 * d.documents, each document's version, caret and anchor along d.version,
 * d.caret and d.anchor, the camera along d.camera, the ZigZag store,
 * version, focus and keyboard along d.zigzag and the active document along
 * d.active. Earlier places stay,
 * which is what a history of visits needs later. Starts the slice if the
 * store has none.
 */
MicroversionId recordPlace(Store &store, const ReadingPlace &place);

/// The newest place in @p store, or nothing for a store with none.
[[nodiscard]] std::optional<ReadingPlace> latestPlace(const Store &store);

/**
 * @brief Where the reader's activity store lives:
 *        `$XDG_DATA_HOME/xudu/activity`.
 *
 * Data rather than configuration, so beside the xanadocs rather than with
 * the system xanadocs; system://activity is its name.
 */
[[nodiscard]] std::filesystem::path activityDirectory();

} // namespace xanadu

#endif // XANADU_READING_PLACE_HPP
