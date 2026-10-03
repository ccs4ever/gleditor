/**
 * @file script.hpp
 * @brief How superscript and subscript text is sized and placed on its line.
 *
 * Shared by the glyph cache, which rasterises a raised or lowered glyph
 * smaller, and the layout, which moves it and advances past it: the two must
 * agree, or a script glyph would be drawn at one size and spaced for another.
 */
#ifndef GLEDITOR_TEXT_SCRIPT_HPP
#define GLEDITOR_TEXT_SCRIPT_HPP

namespace gleditor::text {

/// A script glyph's size relative to its line's text: the usual 0.6 to 0.7
/// of typesetting, at the small end so that the shifts below -- which are
/// bounded by what the smaller size leaves free -- are large enough to read
/// as raised and lowered.
inline constexpr float kScriptScale = 0.6F;

/**
 * @brief How far above the baseline a superscript sits.
 *
 * As far as keeps the scaled glyph's ascent within the line's own ascent, so
 * a superscript never reaches into the line above.
 */
[[nodiscard]] constexpr float superscriptRise(const float ascent) noexcept {
  return (1.0F - kScriptScale) * ascent;
}

/**
 * @brief How far below the baseline a subscript sits.
 *
 * As far as keeps the scaled glyph's descent within what the line has below
 * its baseline -- descent and line gap together -- so a subscript never
 * reaches into the line below.
 */
[[nodiscard]] constexpr float subscriptDrop(const float ascent,
                                            const float lineHeight) noexcept {
  return (1.0F - kScriptScale) * (lineHeight - ascent);
}

} // namespace gleditor::text

#endif // GLEDITOR_TEXT_SCRIPT_HPP
