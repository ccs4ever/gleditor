/**
 * @file epistemic_translate_clock.cpp
 * @brief The clock lineage as a transfer reads it (P19 I3-3b2): what a
 *        well-formed lineage record is, so a transfer carries it and refuses
 *        anything else by name.
 *
 * Both roads carry the lineage whole (`i3-3-design.md` 8). A resume carries it
 * and the new executor's first control turn appends that instance's segment,
 * which is the only edit; the generic copier moves the cells.
 */
#include <algorithm>
#include <array>
#include <cmath>
#include <ranges>

#include "common/xanadu/vagent/runtime/epistemic_abi_clock.hpp"
#include "common/xanadu/vagent/runtime/epistemic_limits.hpp"
#include "common/xanadu/vagent/runtime/epistemic_translate.hpp"
#include "common/xanadu/vagent/runtime/runtime_abi.hpp"

namespace xanadu::vagent::runtime {

namespace {

using zigzag::CellRef;
using zigzag::DimRef;

/// The instance the executor names itself with: 16 lowercase hex digits.
constexpr std::size_t kInstanceDigits = 16;

bool isInstance(std::string_view text) {
  return text.size() == kInstanceDigits &&
         std::ranges::all_of(text, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

/// A stored number: a checkpoint types a whole-valued double as an Int from
/// its text, and the routine's comparisons read either as the same number.
std::optional<double> real(const zigzag::ArenaManifold &arena, CellRef cell) {
  const auto kind = arena.valueKindOf(cell);
  if (kind != xanadu::ValueKind::Double && kind != xanadu::ValueKind::Int64)
    return std::nullopt;
  const auto value = arena.asDouble(cell);
  if (!value || !std::isfinite(*value)) return std::nullopt;
  return value;
}

/// One segment: `instance`, `start_steady` and `anchor_steady`, then both wall
/// fields or neither, in the order the routine writes them.
bool segmentWellFormed(const zigzag::ArenaManifold &arena, const ValueDims &vd,
                       CellRef segment) {
  if (arena.textOf(segment) != kLineageSegmentName) return false;
  const auto fields = fieldsOf(arena, vd, segment);
  if (fields.size() != 3 && fields.size() != 5) return false;
  constexpr std::array<std::string_view, 5> names{
      kSegInstance, kSegStartSteady, kSegAnchorSteady, kSegAnchorWall,
      kSegAnchorSync};
  for (std::size_t i = 0; i < fields.size(); ++i)
    if (arena.textOf(fields[i].first) != names[i] || !fields[i].second)
      return false;
  if (!isInstance(arena.textOf(*fields[0].second))) return false;
  const auto start  = real(arena, *fields[1].second);
  const auto anchor = real(arena, *fields[2].second);
  if (!start || !anchor || *start > *anchor) return false;
  if (fields.size() == 3) return true;
  const auto wall = real(arena, *fields[3].second);
  const auto sync = real(arena, *fields[4].second);
  return wall && sync && *sync >= 0.0;
}

} // namespace

bool clockLineageWellFormed(const zigzag::ArenaManifold &arena,
                            const OwnedGraphDims &dims, const CellRef record) {
  if (!dims.vars || !dims.values) return false;
  // A dimension a checkpoint never used is absent from its table, and a list
  // of one element never uses `d.step`: absent means no link, not `d.vars`.
  const ValueDims vd{.vars   = *dims.vars,
                     .values = *dims.values,
                     .grab   = dims.grab.value_or(*dims.vars),
                     .step   = dims.step.value_or(*dims.vars)};
  const auto fields = fieldsOf(arena, vd, record);
  if (fields.size() != 1 ||
      arena.textOf(fields.front().first) != kLineageSegments ||
      !fields.front().second)
    return false;
  const auto list = *fields.front().second;
  if (arena.textOf(list) != kListContent) return false;
  std::size_t count = 0;
  const auto next   = [&](CellRef from, const std::optional<DimRef> &dim) {
    return dim ? arena.linked(from, *dim) : std::nullopt;
  };
  for (auto at = next(list, dims.grab); at; at = next(*at, dims.step)) {
    // A forged chain must not be walked without end: past the largest bound
    // the setting admits, the lineage is not one this build wrote.
    if (++count > static_cast<std::size_t>(kMaxClockLineage)) return false;
    if (!segmentWellFormed(arena, vd, *at)) return false;
  }
  return true;
}

} // namespace xanadu::vagent::runtime
