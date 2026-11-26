#pragma once
// Host-side relations behind `join(rows, separator)`: the rows' spans, with
// one separator between each pair, assembled into a single cell. A row that
// quotes text keeps quoting it, so the result stays a transclusion of its
// sources rather than retyped text; only a number, which has no span, and a
// separator that is no cell's text are new bytes, and those sit in the arena's
// scratch buffer, which `promote()` is the one road out of.

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "common/xanadu/spool.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {

/// One row: `join_step(row, separator)`. The scan position is the number of
/// rows already taken (0 starts a join) and the bound is the most bytes a join
/// may hold; a step past the bound is "no tuple", never a truncation.
inline constexpr std::string_view joinStepRelation = "join_step";
/// The end of the walk: `join_end(separator)` answers the joined cell (the one
/// row itself when there was one, an empty text when there was none).
inline constexpr std::string_view joinEndRelation = "join_end";

/// What a walk has taken so far. One per execution: the rows of a join are all
/// computed before its first step, so two walks never interleave.
struct JoinWalk {
  std::vector<PrimediaSpan> runs;
  std::optional<zigzag::CellRef> only;
  std::uint64_t rows{0};
  std::uint64_t bytes{0};
};

[[nodiscard]] zigzag::vortex::RelationProvider
joinStepProvider(zigzag::vortex::VortexVM &vm, std::shared_ptr<JoinWalk> walk);
[[nodiscard]] zigzag::vortex::RelationProvider
joinEndProvider(std::shared_ptr<JoinWalk> walk);

} // namespace xanadu::vql
