/**
 * @file coalesce.hpp
 * @brief Where the pages of an active link go: the strategy interface, and
 *        the built-in strategy over the tension engine.
 *
 * design/view-system.md §10.3.2. A link's participants are bodies, one of
 * them pinned where the reader is; each tie asks for two passages level and a
 * given gap between their pages. A strategy is a pure function of its input,
 * so it can run inside a view's layout() and two layouts of one input agree.
 *
 * The built-in strategy is the tension engine LinkBeams steps today, with the
 * changes spike S2 found necessary (plan §3.1). Today's alignment is 25 steps
 * from wherever the documents happen to be heading, one pair at a time, and
 * stops mid-flight; this one starts every solve from the home row, solves
 * every tie of a link at once, steps until the bodies settle or a cap is
 * reached, takes heights from the anchors rather than from the springs, and
 * ends with a pass that leaves no two pages overlapping.
 */
#ifndef COMMON_XANADU_VIEW_PAGE_COALESCE_HPP
#define COMMON_XANADU_VIEW_PAGE_COALESCE_HPP

#include <cstdint>
#include <span>

#include <glm/vec3.hpp>

#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/tension_layout.hpp"
#include "common/xanadu/view/page_view.hpp"

namespace xanadu::view {

/// A page taking part. Placement-local units: x right, y up, centres.
struct CoalesceBody {
  PageRef page;
  glm::vec3 home{};     // where it stands in the row
  glm::vec3 position{}; // out: where the strategy puts it
  float width{}, height{};
  bool pinned{}; // the anchor page: it does not move
};

/// Two passages to bring level, a gap apart.
struct CoalesceTie {
  std::uint32_t from{}, to{};     // bodies
  float fromHeight{}, toHeight{}; // passage centres, from each page's top
  float gap{};                    // wanted between the two pages
};

class CoalesceStrategy {
public:
  CoalesceStrategy()                                    = default;
  CoalesceStrategy(const CoalesceStrategy &)            = delete;
  CoalesceStrategy &operator=(const CoalesceStrategy &) = delete;
  CoalesceStrategy(CoalesceStrategy &&)                 = delete;
  CoalesceStrategy &operator=(CoalesceStrategy &&)      = delete;
  virtual ~CoalesceStrategy()                           = default;

  /// Pure: the same bodies and ties give the same positions. Reads each
  /// body's home and never its position, so a solve does not depend on
  /// where an earlier one, or an animation, left anything. A tie naming a
  /// body that is not there, or one body twice, is ignored.
  virtual void solve(std::span<CoalesceBody> bodies,
                     std::span<const CoalesceTie> ties) const noexcept = 0;
};

/// The tension engine, cold-started from the home row on every solve.
///
/// Each tie draws its far page to the side of the near one that its home is
/// on, so pages are not dragged across the row. Heights are not left to the
/// springs: every passage is set level with the one it is tied to, outward
/// from the pinned pages. The engine then settles the horizontal positions,
/// and a last pass pushes each page outward from the anchor, in the order
/// the row had them, until it is coalesceGap clear of every page already
/// placed that it shares any height with.
class TensionCoalesce final : public CoalesceStrategy {
public:
  /// @p physics in the engine's own units, which PageBaseConfig::physicsUnitPx
  /// converts to and from.
  TensionCoalesce(TensionParams physics, PageBaseConfig page) noexcept;

  void solve(std::span<CoalesceBody> bodies,
             std::span<const CoalesceTie> ties) const noexcept override;

private:
  TensionParams physics_;
  PageBaseConfig page_;
};

} // namespace xanadu::view

#endif // COMMON_XANADU_VIEW_PAGE_COALESCE_HPP
