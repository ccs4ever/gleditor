/**
 * @file path_cells.hpp
 * @brief A rootless path is data: cells and dimensions, walked by a bounded
 * reader (RP1's step rank and P1b's walker, design/projects/
 * vql-shape-dispatch-design.md §8.9).
 *
 * A functor-tier path is a `PartialPathFunctor` (`/+d.fwd[1]`) cut down to
 * what needs neither code nor an environment: signed dimension steps and
 * literal clamps. It is minted as a path cell P that carries
 *
 *  - a self-link on d.vql.path, the marker that tells a path from a shape or a
 *    view handle by an edge and not by sniffing a type;
 *  - the release epoch it was minted in on d.vql.epoch, the witness every
 *    reader checks so a number reused by a later cell is refused by name;
 *  - its steps in order on d.vql.step, the rank the head P starts. A step
 *    cell's text is the dimension it names (as a rule cell's is), its sign is a
 *    cell on d.vql.dir and its bidirectionality a cell on d.vql.bidir, the
 *    dimensions a shape rule uses. A literal clamp is a cell on d.vql.clamp
 *    holding the start index, which carries a second d.vql.clamp edge to the
 *    end index when there is one: an absent end is an absent edge, never a
 *    value, so `[1]` and `[1, 1]` differ only by that edge as they do in the
 *    grammar.
 *
 * A step has no negation (a path is read, not tested for absence), so
 * d.vql.neg is not minted and the dimension needs no d.vql.to edge: the text
 * of the step cell is read by the same code as a rule's.
 *
 * Nothing here appends an operation (R8): the cells are the session's arena
 * and a path is immutable once minted, which is what lets a reader share it.
 * Nothing here is wired into a shape or a functor value yet; the walker is a
 * function the next card's opcodes call.
 */
#ifndef COMMON_XANADU_VQL_PATH_CELLS_HPP
#define COMMON_XANADU_VQL_PATH_CELLS_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vql/ast.hpp"
#include "common/xanadu/vql/shape_limits.hpp"
#include "common/xanadu/vql/view_cells.hpp"

namespace xanadu::vql {

/// The path handle's marker: a cell carrying a self-link here is a path.
inline constexpr std::string_view pathMarkerDimension = "d.vql.path";
/// The rank a path's steps are on, headed by the path cell.
inline constexpr std::string_view pathStepDimension = "d.vql.step";
/// A step's literal clamp: step -> start index -> optional end index.
inline constexpr std::string_view pathClampDimension = "d.vql.clamp";

struct PathDimensions {
  zigzag::DimRef marker{};
  zigzag::DimRef step{};
  zigzag::DimRef dir{};
  zigzag::DimRef bidir{};
  zigzag::DimRef clamp{};
  zigzag::DimRef epoch{};
};

/// The path dimensions when the core names them all, minting nothing.
[[nodiscard]] std::optional<PathDimensions>
findPathDimensions(const zigzag::vortex::VortexCore &core);

/// Names every path dimension, minting those not yet there. The engine does
/// this where it names the shape dimensions, so the first path does not count
/// as minting a dimension.
PathDimensions ensurePathDimensions(zigzag::vortex::VortexCore &core);

/// One step of a functor-tier path, as the grammar writes it.
struct PathStepSpec {
  std::string dimension;
  zigzag::DimVector direction{zigzag::DimVector::POS};
  bool bidirectional{false};
  std::optional<RangeClamp> clamp;

  /// RangeClamp has no equality of its own, and the AST is not this card's.
  bool operator==(const PathStepSpec &other) const {
    return dimension == other.dimension && direction == other.direction &&
           bidirectional == other.bidirectional &&
           clamp.has_value() == other.clamp.has_value() &&
           (!clamp || (clamp->start == other.clamp->start &&
                       clamp->end == other.clamp->end));
  }
};

enum class PathRefusalKind : std::uint8_t {
  FreeVariable,      ///< a `$name`: a closure, which is V6's
  BeyondFunctorTier, ///< anything but signed dimension steps and clamps
  NoSteps,           ///< nothing to walk
  TooManySteps,      ///< over the vql.path.max_steps ceiling
  NotAPath,          ///< the cell carries no marker, or its cells are damaged
  Stale,             ///< the witness is missing or names another epoch
  SeveralResults,    ///< a match needs exactly one cell (Q10)
  Unreadable,        ///< a value the comparison cannot read (Q11)
  FuelExhausted,     ///< the arena's work budget ran out mid-walk
  VisitBudget,       ///< the walk visited more cells than it was allowed
};

/// Why a path was refused, by name: the kind for a caller to branch on and the
/// text a query's author reads.
struct PathRefusal {
  PathRefusalKind kind{PathRefusalKind::NotAPath};
  std::string message;
};

/// Reads a rootless path expression (`/+d.fwd[1]`, as the parser makes it) as
/// steps. Refuses by name a `$name` (a free variable makes the path a closure),
/// a path anchored on `.` or anywhere else, and every step that is not a plain
/// signed dimension with at most a literal clamp. This is the one compile-time
/// entry point; the compiler does not call it yet.
[[nodiscard]] std::expected<std::vector<PathStepSpec>, PathRefusal>
classifyFunctorPath(const PathExpression &expression);

/// The same over bare steps, for a path bound by `let` and kept as steps.
[[nodiscard]] std::expected<std::vector<PathStepSpec>, PathRefusal>
classifyFunctorSteps(std::span<const PathStep> steps);

/// Mints @p steps as a path cell, or refuses before minting anything: a `$name`
/// in a dimension, no steps, or more than @p maxSteps. The ceiling is the
/// caller's (ShapeLimits::maxPathSteps, the vql.path.max_steps setting), so a
/// host that lowers it is obeyed by the mint and by every later read.
[[nodiscard]] std::expected<zigzag::CellRef, PathRefusal>
mintPath(zigzag::ArenaManifold &arena, const PathDimensions &dims,
         std::span<const PathStepSpec> steps, std::size_t maxSteps);

/// True when @p cell carries the path marker, by the dimension's number.
[[nodiscard]] bool isPathCell(const zigzag::ArenaManifold &arena,
                              const PathDimensions &dims, zigzag::CellRef cell);

/// Why @p path can no longer be trusted: no witness, or one naming an epoch
/// other than the one the cell is in now (the number was released and reused).
[[nodiscard]] std::optional<std::string>
stalePathReason(const zigzag::ArenaManifold &arena, const PathDimensions &dims,
                zigzag::CellRef path);

/// Decodes @p path back to its steps. The rank is walked with the standard
/// rank protection (it ends at its end or when the head is reached again), so
/// a rank damaged into a ring still ends. Refuses a cell that is not a live
/// path and a path of more than @p maxSteps steps.
[[nodiscard]] std::expected<std::vector<PathStepSpec>, PathRefusal>
readPath(const zigzag::ArenaManifold &arena, const PathDimensions &dims,
         zigzag::CellRef path, std::size_t maxSteps);

/// What a walk is held to. The step ceiling is the setting's; the visit
/// ceiling bounds the fan-out of duplicates a rank walk can multiply from step
/// to step, which fuel alone does not when the arena has no work budget.
struct PathWalkBudget {
  std::size_t maxSteps{defaultPathMaxSteps};
  /// Cell reads the whole walk may make; absent means maxSteps times the cells
  /// one step could reach, which no honest path exceeds.
  std::optional<std::size_t> maxVisits;
};

/// The cells a walk reached, in walk order with duplicates kept (a stream, as
/// the language's steps yield), and the link reads it made. The reads were
/// already charged to the arena's work budget, one unit each, as the VM's rank
/// iterator charges them; the count is for a caller that also keeps its own.
struct PathWalk {
  std::vector<zigzag::CellRef> cells;
  std::size_t visits{0};
};

/// Walks @p path from @p subject. Each step walks the rank along its dimension
/// from every cell the last step reached, as `/+d.x` does in a query: the
/// neighbours, not the start, a rank's cycle ending the walk where it began,
/// and a bidirectional step the positive walk then the negative one. A
/// step's clamp then windows that step's whole output, 1-based with negative
/// indices from the end and out-of-range indices clamped into it. A dimension
/// the core does not name has no links, so the step reaches nothing.
///
/// Zero, one or many cells are all answers: the caller decides what the count
/// means (exactly one for an assignment, pathTargetMatches for a test).
///
/// This is C++ and not Vortex because it is the body of opcodes (the shape
/// test and the path application) that run once per candidate cell in a
/// query's hot loop: a Vortex loop would pay an instruction dispatch and a
/// stream append for every link read, and the walk needs the arena's linked
/// read, its work budget and its proxy minting, which Vortex reaches only
/// through ProbeLink one cell at a time. It reads exactly what ProbeLink and
/// RankBegin read, and writes nothing.
[[nodiscard]] std::expected<PathWalk, PathRefusal>
walkPath(zigzag::vortex::VortexCore &core, const PathDimensions &dims,
         zigzag::CellRef subject, zigzag::CellRef path,
         const PathWalkBudget &budget = {});

/// The match reading of a walk's result against the cell a candidate holds on
/// the rule's dimension. None reached: never matches (false). One: compares by
/// value with @p neighbour (Q11), as a scalar target does, so a clone linked by
/// an assignment still matches and a same-cell identity test is not offered.
/// An absent neighbour matches nothing.
///
/// POTENTIAL EXPANSION POINT (Q10): several cells reached refuse by name. The
/// refusal is where a "many" mode (a list as a value, an existential or a
/// sequence reading) would go; keeping it a refusal costs nothing and keeps a
/// filter's truth independent of cardinality.
[[nodiscard]] std::expected<bool, PathRefusal>
pathTargetMatches(zigzag::vortex::VortexCore &core,
                  std::span<const zigzag::CellRef> reached,
                  std::optional<zigzag::CellRef> neighbour);

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_PATH_CELLS_HPP
