/**
 * @file view_marks.hpp
 * @brief The chrome every view draws the same way: ghosts, view-only marks,
 *        the focus mark, and how faded text steps down (plan §5.6, D1, D2;
 *        design/view-system.md §9.1.5, §15).
 *
 * Three views built one after another must not each invent their own ghost,
 * so the marks are made here, once, and a view only says where (plan §5.6).
 * A mark is an ordinary layout record: it is placed, faded and culled by the
 * same rules as what it marks, and the presenter tells the kinds apart by
 * their flags, never by the view that emitted them.
 *
 * Two rules from the visual review are kept here so no view can break them:
 *
 * - A placeholder never outshines what it stands for (plan §5.1 principle 6,
 *   D1). A ghost's opacity is a share of the fade floor of the view that
 *   emits it, so it is dimmer than the dimmest real thing that view draws
 *   whatever the floor is set to.
 * - Fading a box and keeping its text readable are separate (D2). A box may
 *   fade to its floor, but text is drawn only at an opacity where it still
 *   meets contrast: below one threshold it gives way to its shorter form, and
 *   below a second to bars, which have no contrast to lose (§15).
 */
#ifndef COMMON_XANADU_VIEW_VIEW_MARKS_HPP
#define COMMON_XANADU_VIEW_VIEW_MARKS_HPP

#include <cstdint>

#include "common/xanadu/view/view_records.hpp"

namespace xanadu::view {

/// What a Marker record stands for. It is the marker's SubjectId slot, and
/// its value is the value of the subject it marks, so a mark keeps its
/// identity for as long as what it marks does. Later views add kinds here:
/// a pack's seams (E7), edge heat (the stretch view's second sub-view).
enum class MarkKind : std::uint32_t {
  Ghost, ///< an empty outline for something the pane would cut
};

/// The SubjectId of the @p kind mark of @p subject, keeping its epoch so a
/// mark of a tossed view cell is never confused with one of its successor.
[[nodiscard]] SubjectId markId(MarkKind kind,
                               const SubjectId &subject) noexcept;

/// A ghost's opacity: @p share of the emitting view's fade floor (D1).
/// Both are clamped to [0, 1], so a ghost is never brighter than the floor.
[[nodiscard]] float ghostOpacity(float fadeFloor, float share) noexcept;

/**
 * @brief The ghost that stands for @p item where the pane would cut it.
 *
 * The same plane and box, no content, view-only chrome: a ghost has soft
 * corners and is never announced as a cell (V-R45, plan §5.1 principle 1).
 * It does not inherit the item's focus.
 */
[[nodiscard]] PlacedItem ghostOf(const PlacedItem &item,
                                 float opacity) noexcept;

/// Mark @p item as view-only at @p opacity: drawn with soft corners and never
/// given the role of a cell or a page (V-R45).
PlacedItem *markViewOnly(PlacedItem &item, float opacity) noexcept;

/// The focus mark. The accursed cell is never faded, hidden or stepped down
/// (§9.1.5), so this also restores full opacity and full content.
PlacedItem *markFocus(PlacedItem &item) noexcept;

/// When faded text steps down (D2). Opacities are fractions; the line size
/// is in pixels on screen.
struct Legibility {
  /// Below this opacity Full content gives way to Abbreviated.
  float abbreviateBelow{};
  /// Below this opacity text gives way to Coarse bars. It is the least
  /// opacity at which the theme's text still meets contrast (§15).
  float coarseBelow{};
  /// Below this projected line height text gives way to Coarse bars:
  /// PaneFrame::minReadableLinePx (plan §5.5 check 6). Zero: no limit.
  float minReadableLinePx{};
};

/// The content an item drawn at @p opacity, whose lines project
/// @p linePx tall, may show. Steps, not a slide: text never fades or shrinks
/// past being read (plan §5.1 principle 7).
[[nodiscard]] ContentMode legibleContent(float opacity, float linePx,
                                         const Legibility &steps) noexcept;

} // namespace xanadu::view

#endif // COMMON_XANADU_VIEW_VIEW_MARKS_HPP
