/**
 * @file caret_motion.hpp
 * @brief Where the caret goes for a key: by character, word, line and page.
 */
#ifndef GLEDITOR_CARET_MOTION_HPP
#define GLEDITOR_CARET_MOTION_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <gleditor/doc.hpp>

namespace gleditor {

/// A caret movement a key asks for.
enum class CaretMotion : std::uint8_t {
  Left,
  Right,
  WordLeft,
  WordRight,
  Up,
  Down,
  LineStart,
  LineEnd,
  DocumentStart,
  DocumentEnd,
};

/**
 * @brief The next character boundary from @p at in UTF-8 @p text.
 *
 * By code point: a caret never lands inside a character's bytes, which is
 * where an edit would split it.
 */
[[nodiscard]] std::uint32_t stepCharacter(std::string_view text,
                                          std::uint32_t at, bool forward);

/// The start of the next word from @p at, or of this one going back: runs of
/// space are skipped first, as editors do.
[[nodiscard]] std::uint32_t stepWord(std::string_view text, std::uint32_t at,
                                     bool forward);

/// Which line of @p shaping holds page-relative byte @p at, and how far along
/// it the caret stands, in the shaping's own pixels.
struct LinePlace {
  std::size_t line{};
  float x{};
};
[[nodiscard]] LinePlace placeOnLines(const PageShaping &shaping,
                                     std::uint32_t at);

/**
 * @brief The page-relative byte on line @p line of @p shaping nearest @p x.
 *
 * Past the last glyph is the line's end, before any newline that ends it, so
 * moving down onto a short line lands at its end rather than on the next one.
 */
[[nodiscard]] std::uint32_t offsetOnLine(const PageShaping &shaping,
                                         std::string_view pageText,
                                         std::size_t line, float x);

/**
 * @brief Where the caret at @p offset in @p doc lands for @p motion.
 *
 * Moves across pages. Line and vertical motion needs the page built; on one
 * that is not, the caret stays where it is rather than guessing.
 */
[[nodiscard]] std::uint32_t caretTarget(const Doc &doc, std::uint32_t offset,
                                        CaretMotion motion);

} // namespace gleditor

#endif // GLEDITOR_CARET_MOTION_HPP
