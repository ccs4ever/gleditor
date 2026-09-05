#pragma once

#include <cstdint>
#include <gleditor/doc.hpp>
#include <gleditor/glyphcache/types.hpp>
#include <gleditor/text/font.hpp>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace gleditor::text {

/**
 * @brief Options controlling multi-line text layout and page slicing.
 */
struct LayoutOptions {
  float maxWidthPx{
      1024.0F}; ///< Maximum width before line wrapping (0 for unbounded)
  float maxHeightPx{
      1024.0F}; ///< Maximum height before page slicing (0 for unbounded)
  bool singleParagraph{false}; ///< True to treat newlines as spaces (for
                               ///< single-line titles/toasts)
  bool ellipsize{
      false}; ///< True to truncate with "..." if exceeding maxWidthPx
  /// Which decorations apply where in this text slice, checked per glyph
  /// cluster by its byte offset. Empty -- the default, and every caller
  /// before this existed -- means no glyph in the page carries any.
  std::vector<DecoratedRange> decoratedRanges{};
  /// Ranges that must not be split across a page boundary -- see
  /// AtomicRange's own comment. When starting a range whose full height
  /// would not fit in what remains of maxHeightPx, layoutPage() ends the
  /// page there instead of partway through the range, so the whole thing
  /// begins fresh on the next page's call instead of splitting across both.
  /// Empty by default: plain text has no atomic ranges, the same "no
  /// opinion" meaning decoratedRanges' own empty default carries.
  std::vector<AtomicRange> atomicRanges{};
};

/**
 * @brief Multi-line, Unicode-compliant text layout and shaping engine using
 *        HarfBuzz, libunibreak, FriBidi, and FreeType.
 */
class TextLayout {
public:
  /**
   * @brief Perform multi-line layout and pagination on a UTF-8 text slice.
   *
   * @param text The UTF-8 text string to shape and paginate.
   * @param font The loaded FontFace to use for metrics and HarfBuzz shaping.
   * @param options Layout constraints including maximum width, height, and
   * wrapping.
   * @return PageShaping Plain data ready for vertex generation and atlas cache
   * queries.
   */
  static PageShaping layoutPage(std::string_view text, const FontFacePtr &font,
                                const LayoutOptions &options);

  /**
   * @brief Quick single-line measurement (for toasts, canvas titles).
   */
  static PageShaping layoutSingleLine(std::string_view text,
                                      const FontFacePtr &font,
                                      float maxWidthPx = 0.0F,
                                      bool ellipsize   = false);
  static PageShaping layoutSingleLine(std::string_view text,
                                      const FontFacePtr &font,
                                      const LayoutOptions &options);
};

} // namespace gleditor::text
