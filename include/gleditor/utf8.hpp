/**
 * @file utf8.hpp
 * @brief Moving a byte offset to a character boundary.
 *
 * Documents are addressed in bytes: the caret, a selection, a picking result
 * and an edit all name a byte offset, because that is what survives text being
 * inserted before it. UTF-8 characters are one to four bytes, so a caller
 * doing arithmetic on those offsets can land inside one, and an edit that acts
 * on such a range leaves a fragment of a character behind -- which Pango then
 * refuses to shape, a long way from whatever produced it.
 *
 * Both directions are here because a range needs both. Snapping the start
 * backwards and the end forwards covers whole characters; snapping both the
 * same way collapses a range naming part of one character to nothing, or
 * silently drops a character from the end.
 */
#ifndef GLEDITOR_UTF8_H
#define GLEDITOR_UTF8_H

#include <cstdint>
#include <string_view>

namespace gleditor {

/**
 * @brief Move @p offset back to the start of the character it lands in.
 *
 * Continuation bytes are 10xxxxxx; every other byte begins a character. An
 * offset at or past the end, or already on a boundary, is returned unchanged.
 */
[[nodiscard]] std::uint32_t alignToCharacterStart(std::string_view text,
                                                  std::uint32_t offset);

/**
 * @brief Move @p offset forward to the next character boundary.
 *
 * The end of a range, where alignToCharacterStart() is its start. An offset
 * inside a character advances past the whole of it; one already on a boundary,
 * or at the end, is returned unchanged.
 */
[[nodiscard]] std::uint32_t alignToCharacterEnd(std::string_view text,
                                                std::uint32_t offset);

/**
 * @brief Check if @p text is valid UTF-8.
 *
 * If invalid, returns false and writes the byte index of the first malformed
 * sequence to @p badOffsetOut.
 */
[[nodiscard]] bool validateUtf8(std::string_view text,
                                std::size_t &badOffsetOut);

/**
 * @brief Replace malformed UTF-8 sequences with the Unicode replacement
 *        character (U+FFFD).
 */
[[nodiscard]] std::string makeValidUtf8(std::string_view text);

} // namespace gleditor

#endif // GLEDITOR_UTF8_H
