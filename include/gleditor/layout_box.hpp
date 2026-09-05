/**
 * @file layout_box.hpp
 * @brief The vocabulary a text layout shares with whatever embeds media in
 *        it: alignment, paragraph style, page geometry, and reserved boxes.
 *
 * Kept separate from glyphcache/types.hpp (which DecoratedRange lives in)
 * rather than added to it: that header is about atlas geometry and is
 * included by half the tree, and none of what is here needs any of that.
 * Nothing in this header knows what a caller draws in a LayoutBox -- an
 * image, a video card, a table -- only how big it is and how text should
 * behave around it, which is what lets the layout engine stay reusable
 * across gleditor, xudu and zigzag.
 */
#ifndef GLEDITOR_LAYOUT_BOX_HPP
#define GLEDITOR_LAYOUT_BOX_HPP

#include <cstdint>
#include <optional>
#include <vector>

namespace gleditor {

/// Where a line's ink sits within the horizontal space available to it.
/// Applies to text lines directly, and, through the BlockStyleRange
/// covering its anchor, to a Block box as well -- a centred figure and a
/// centred paragraph are the same question asked of different content.
enum class TextAlign : std::uint8_t { Left, Right, Centre, Justify };

/// How a box relates to the text around it. The distinction the whole
/// point of a layout box turns on: Block interrupts the flow (the lines
/// above and below it are full width), Float is stepped around by it (text
/// beside it narrows), Inline is part of it (the box sits on a line like an
/// oversized glyph).
enum class BoxPlacement : std::uint8_t { Block, FloatLeft, FloatRight, Inline };

/**
 * @brief A rectangle of reserved space anchored to one character.
 *
 * The generic thing an application attaches to a byte offset in the text it
 * supplies -- conventionally the OBJECT REPLACEMENT CHARACTER (U+FFFC), so
 * the box has exactly one character's worth of the document to sit at
 * rather than a media item's own pixel height deciding how many bytes it
 * occupies. The library never learns what is drawn in the box, only its
 * size and how the flow should treat it.
 */
struct LayoutBox {
  /// Byte offset of the anchor character. Same offset convention as
  /// DecoratedRange: relative to whatever text it is handed alongside.
  std::uint32_t anchor{};
  float widthPx{};
  float heightPx{};
  /// Space the flow leaves below (Block) or beside (Float) the box, so the
  /// gap is reserved by the layout rather than applied as a fudge by
  /// whatever draws into the box afterwards.
  float marginPx{};
  BoxPlacement placement{BoxPlacement::Block};
  /// Distance the box's bottom sits below the baseline, for Inline only.
  /// Zero -- sitting on the baseline -- for every other placement.
  float baselineOffsetPx{};
  /// Opaque to the library, echoed back in a page's placed-box list so an
  /// application can match a placement to whatever it meant to draw there
  /// without re-deriving it from the anchor byte offset alone.
  std::uint32_t id{};

  [[nodiscard]] bool operator==(const LayoutBox &) const = default;
};

/**
 * @brief Paragraph-level style over a byte range.
 *
 * The block counterpart to DecoratedRange, which is per-glyph. A line takes
 * the style of the range covering its *first* byte, so a range need only
 * cover a paragraph's start to style all of it. Same offset convention as
 * DecoratedRange and LayoutBox: relative to whatever text it is handed
 * alongside, translated by the caller between a whole document's offsets
 * and a single page's slice.
 */
struct BlockStyleRange {
  std::uint32_t start{};
  std::uint32_t end{}; ///< Half-open: covers [start, end).
  TextAlign align{TextAlign::Left};
  float indentFirstPx{}; ///< Extra left inset on a paragraph's first line.
  float indentLeftPx{};
  float indentRightPx{};
  /// Overrides the placement of any LayoutBox anchored in this range.
  /// nullopt -- the common case -- leaves the box's own placement alone.
  /// Exists so a style range can say "float this figure right" without the
  /// box specification itself carrying that decision.
  std::optional<BoxPlacement> placement{};

  [[nodiscard]] bool operator==(const BlockStyleRange &) const = default;
};

/// Whether a page's own quad is a fixed, author-chosen size or grows to fit
/// whatever landed on it. FitContent is first (and so is what a
/// default-constructed PageSize means) because it is the one every page had
/// before page size became a document property at all -- a PageSize nobody
/// set is "no opinion", the same meaning an empty DecoratedRange list has
/// always carried elsewhere in this header's neighbourhood.
enum class PageSizing : std::uint8_t { FitContent, Fixed };

/// A document's page geometry, in layout pixels.
struct PageSize {
  PageSizing mode{PageSizing::FitContent};
  /// For Fixed, the page's own size. For FitContent, unused -- the page's
  /// size comes from its content instead, the same as every page did before
  /// this existed.
  float widthPx{};
  float heightPx{};
  float marginPx{};

  [[nodiscard]] float textWidthPx() const { return widthPx - 2.0F * marginPx; }
  [[nodiscard]] float textHeightPx() const {
    return heightPx - 2.0F * marginPx;
  }

  [[nodiscard]] bool operator==(const PageSize &) const = default;
};

/// 8.5x11in "Letter" at 139.7 pixels per inch and a 24px margin -- the
/// geometry every gleditor/xudu page had before page size became a
/// per-document property, kept as the default so a TextSource that does not
/// override pageSize() changes nothing for it.
inline constexpr float letterDpi = 139.70F;
inline constexpr PageSize letterPage{
    .mode     = PageSizing::Fixed,
    .widthPx  = (letterDpi * 8.5F) + 48.0F,
    .heightPx = (letterDpi * 11.0F) + 48.0F,
    .marginPx = 24.0F,
};

} // namespace gleditor

#endif // GLEDITOR_LAYOUT_BOX_HPP
