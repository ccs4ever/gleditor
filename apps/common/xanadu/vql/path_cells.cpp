#include "common/xanadu/vql/path_cells.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <format>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <variant>

#include "common/xanadu/scalar.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/query_scalar.hpp"

namespace xanadu::vql {
using zigzag::CellRef;
using zigzag::DimVector;

namespace {

std::unexpected<PathRefusal> refuse(const PathRefusalKind kind,
                                    std::string message) {
  return std::unexpected{
      PathRefusal{.kind = kind, .message = std::move(message)}};
}

std::unexpected<PathRefusal> beyondTier(const std::string_view what) {
  return refuse(PathRefusalKind::BeyondFunctorTier,
                std::format("VQL a rootless path value holds only signed "
                            "dimension steps and literal clamps: {} is beyond "
                            "that",
                            what));
}

std::unexpected<PathRefusal> freeVariable(const std::string_view name) {
  return refuse(PathRefusalKind::FreeVariable,
                std::format("VQL a rootless path value cannot name {}: a path "
                            "with a free variable is a closure, which is not "
                            "a path value",
                            name));
}

std::optional<PathRefusal> refusalForStep(const PathStep &step) {
  const auto beyond = [](const std::string_view what) {
    return beyondTier(what).error();
  };
  if (step.capturedFunctor) return beyond("a captured functor");
  const auto *dimension = std::get_if<SignedDimensionStep>(&step.selector);
  if (!dimension) return beyond("a step that is not a signed dimension");
  if (dimension->dimName.starts_with('$'))
    return freeVariable(dimension->dimName).error();
  if (dimension->placement != Placement::Default)
    return beyond("a placement (::head, ::tail, ::rank)");
  if (!dimension->creates.empty()) return beyond("a creating step");
  if (step.derefMaster) return beyond("a master dereference");
  if (!step.predicates.empty() || !step.shapePredicates.empty())
    return beyond("a predicate (which may name a variable)");
  if (step.projectionAfter) return beyond("a projection");
  if (step.versionPin) return beyond("a version pin");
  if (step.yieldMode != YieldMode::Default) return beyond("a yield mode");
  return std::nullopt;
}

/// Index of an element within a clamp over @p count items, as the compiled
/// clamp (compile_clamp.cpp) normalises it: negatives count from the end and
/// an index off either end is clamped into the stream, so `[5]` over three
/// cells is the last one.
std::size_t clampIndex(const std::int64_t index, const std::size_t count) {
  const auto last = static_cast<std::int64_t>(count) - 1;
  const auto raw =
      index < 0 ? static_cast<std::int64_t>(count) + index : index - 1;
  return static_cast<std::size_t>(std::clamp<std::int64_t>(raw, 0, last));
}

void applyClamp(std::vector<CellRef> &cells, const RangeClamp &clamp) {
  if (cells.empty()) return;
  const auto from = clampIndex(clamp.start, cells.size());
  const auto to   = clampIndex(clamp.end.value_or(clamp.start), cells.size());
  if (from > to) {
    cells.clear();
    return;
  }
  cells =
      std::vector<CellRef>(cells.begin() + static_cast<std::ptrdiff_t>(from),
                           cells.begin() + static_cast<std::ptrdiff_t>(to) + 1);
}

} // namespace

std::optional<PathDimensions>
findPathDimensions(const zigzag::vortex::VortexCore &core) {
  PathDimensions dims;
  const std::array<std::pair<zigzag::DimRef *, std::string_view>, 6> names{{
      {&dims.marker, pathMarkerDimension},
      {&dims.step, pathStepDimension},
      {&dims.dir, viewDirDimension},
      {&dims.bidir, viewBidirDimension},
      {&dims.clamp, pathClampDimension},
      {&dims.epoch, viewEpochDimension},
  }};
  for (const auto &[slot, name] : names) {
    const auto found = core.findDimension(name);
    if (!found) return std::nullopt;
    *slot = *found;
  }
  return dims;
}

PathDimensions ensurePathDimensions(zigzag::vortex::VortexCore &core) {
  return PathDimensions{
      .marker = core.findOrMintDimension(pathMarkerDimension),
      .step   = core.findOrMintDimension(pathStepDimension),
      .dir    = core.findOrMintDimension(viewDirDimension),
      .bidir  = core.findOrMintDimension(viewBidirDimension),
      .clamp  = core.findOrMintDimension(pathClampDimension),
      .epoch  = core.findOrMintDimension(viewEpochDimension),
  };
}

std::expected<std::vector<PathStepSpec>, PathRefusal>
classifyFunctorSteps(const std::span<const PathStep> steps) {
  std::vector<PathStepSpec> out;
  out.reserve(steps.size());
  for (const PathStep &step : steps) {
    if (auto refusal = refusalForStep(step))
      return std::unexpected{std::move(*refusal)};
    const auto &dimension = std::get<SignedDimensionStep>(step.selector);
    out.push_back({.dimension     = dimension.dimName,
                   .direction     = dimension.direction,
                   .bidirectional = dimension.isBidir,
                   .clamp         = step.rangeClamp});
  }
  return out;
}

std::expected<std::vector<PathStepSpec>, PathRefusal>
classifyFunctorPath(const PathExpression &expression) {
  if (!expression.isFunctor)
    return beyondTier("a path with an anchor (a rootless path is written "
                      "/+d.x, with nothing before the slash)");
  if (!expression.anchorPredicates.empty() || expression.anchorProjection ||
      expression.shapeProjection || expression.cloneTail)
    return beyondTier("an anchor predicate, projection or clone tail");
  return classifyFunctorSteps(expression.steps);
}

std::expected<CellRef, PathRefusal>
mintPath(zigzag::ArenaManifold &arena, const PathDimensions &dims,
         const std::span<const PathStepSpec> steps,
         const std::size_t maxSteps) {
  if (steps.empty())
    return refuse(PathRefusalKind::NoSteps, "VQL a rootless path has no steps");
  // Every refusal comes before the first cell, so a refused path leaves the
  // arena as it was.
  if (steps.size() > maxSteps)
    return refuse(PathRefusalKind::TooManySteps,
                  std::format("VQL a rootless path of {} steps exceeds the "
                              "maximum of {} (vql.path.max_steps)",
                              steps.size(), maxSteps));
  for (const PathStepSpec &step : steps) {
    if (step.dimension.starts_with('$')) return freeVariable(step.dimension);
    if (step.dimension.empty())
      return beyondTier("a step that names no dimension");
  }

  const CellRef path = arena.makeCell();
  zigzag::expectWritten(arena.link(path, dims.marker, DimVector::POS, path));
  if (const auto born = arena.epochOfCell(path))
    zigzag::expectWritten(
        arena.link(path, dims.epoch, false,
                   arena.makeScalarCell(static_cast<std::int64_t>(*born))));
  CellRef previous = path;
  for (const PathStepSpec &spec : steps) {
    const CellRef step = arena.makeCell(spec.dimension);
    zigzag::expectWritten(arena.link(previous, dims.step, false, step));
    zigzag::expectWritten(
        arena.link(step, dims.dir, false,
                   arena.makeScalarCell(static_cast<std::int64_t>(
                       zigzag::toSign(spec.direction)))));
    zigzag::expectWritten(arena.link(step, dims.bidir, false,
                                     arena.makeScalarCell(spec.bidirectional)));
    if (spec.clamp) {
      const CellRef start = arena.makeScalarCell(spec.clamp->start);
      zigzag::expectWritten(arena.link(step, dims.clamp, false, start));
      if (spec.clamp->end)
        zigzag::expectWritten(arena.link(
            start, dims.clamp, false, arena.makeScalarCell(*spec.clamp->end)));
    }
    previous = step;
  }
  return path;
}

bool isPathCell(const zigzag::ArenaManifold &arena, const PathDimensions &dims,
                const CellRef cell) {
  return arena.contains(cell) &&
         arena.linked(cell, dims.marker, DimVector::POS) == cell;
}

std::optional<std::string> stalePathReason(const zigzag::ArenaManifold &arena,
                                           const PathDimensions &dims,
                                           const CellRef path) {
  const auto witness  = arena.linked(path, dims.epoch, DimVector::POS);
  const auto now      = arena.epochOfCell(path);
  const auto recorded = witness ? arena.asInt64(*witness) : std::nullopt;
  if (!recorded || !now)
    return "VQL a path handle's epoch witness is missing or not a number: it "
           "was not made by a query of this session";
  if (static_cast<std::int64_t>(*now) != *recorded)
    return "VQL a path handle was released and its number reused: it no "
           "longer names the cell it was made with";
  return std::nullopt;
}

std::expected<std::vector<PathStepSpec>, PathRefusal>
readPath(const zigzag::ArenaManifold &arena, const PathDimensions &dims,
         const CellRef path, const std::size_t maxSteps) {
  if (!isPathCell(arena, dims, path))
    return refuse(PathRefusalKind::NotAPath,
                  "VQL the cell is not a rootless path value");
  if (auto stale = stalePathReason(arena, dims, path))
    return refuse(PathRefusalKind::Stale, std::move(*stale));

  const auto damaged = [](const std::string_view what) {
    return refuse(
        PathRefusalKind::NotAPath,
        std::format("VQL a rootless path value is damaged: {}", what));
  };
  std::vector<PathStepSpec> steps;
  for (const CellRef step : zigzag::rankAfter(arena, path, dims.step)) {
    if (steps.size() == maxSteps)
      return refuse(PathRefusalKind::TooManySteps,
                    std::format("VQL a rootless path of more than {} steps "
                                "exceeds the maximum (vql.path.max_steps)",
                                maxSteps));
    const auto sign =
        arena.linked(step, dims.dir, DimVector::POS)
            .and_then([&](const CellRef c) { return arena.asInt64(c); });
    const auto bidir =
        arena.linked(step, dims.bidir, DimVector::POS)
            .and_then([&](const CellRef c) { return arena.asBool(c); });
    if (!sign || (*sign != 1 && *sign != -1) || !bidir)
      return damaged("a step has no direction or bidirectionality");
    PathStepSpec spec{.dimension = arena.textOf(step),
                      .direction = *sign > 0 ? DimVector::POS : DimVector::NEG,
                      .bidirectional = *bidir,
                      .clamp         = std::nullopt};
    if (spec.dimension.empty()) return damaged("a step names no dimension");
    if (const auto startCell = arena.linked(step, dims.clamp, DimVector::POS)) {
      const auto start = arena.asInt64(*startCell);
      if (!start) return damaged("a clamp's start is not an integer");
      spec.clamp = RangeClamp{.start = *start};
      if (const auto endCell =
              arena.linked(*startCell, dims.clamp, DimVector::POS)) {
        const auto end = arena.asInt64(*endCell);
        if (!end) return damaged("a clamp's end is not an integer");
        spec.clamp->end = *end;
      }
    }
    steps.push_back(std::move(spec));
  }
  if (steps.empty())
    return refuse(PathRefusalKind::NoSteps, "VQL a rootless path has no steps");
  return steps;
}

std::expected<PathWalk, PathRefusal> walkPath(zigzag::vortex::VortexCore &core,
                                              const PathDimensions &dims,
                                              const CellRef subject,
                                              const CellRef path,
                                              const PathWalkBudget &budget) {
  auto &arena = core.arena();
  if (!arena.contains(subject))
    return refuse(PathRefusalKind::NotAPath,
                  "VQL a path walk was given a subject that is not a cell");
  const auto steps = readPath(arena, dims, path, budget.maxSteps);
  if (!steps) return std::unexpected{steps.error()};

  const auto oneStepReach     = arena.traversalBound() + 1;
  const std::size_t maxVisits = budget.maxVisits.value_or(
      budget.maxSteps > std::numeric_limits<std::size_t>::max() / oneStepReach
          ? std::numeric_limits<std::size_t>::max()
          : budget.maxSteps * oneStepReach);
  PathWalk walk;
  walk.cells = {subject};

  // One link read: the budget and the arena's fuel are charged before the read,
  // as RankBegin's iterator charges them, so a refused read costs what it
  // would have cost.
  const auto charge = [&]() -> std::optional<PathRefusal> {
    if (walk.visits >= maxVisits)
      return PathRefusal{PathRefusalKind::VisitBudget,
                         std::format("VQL a rootless path walk read more than "
                                     "{} cells and was stopped",
                                     maxVisits)};
    ++walk.visits;
    try {
      arena.consumeWork();
    } catch (const std::runtime_error &exhausted) {
      return PathRefusal{PathRefusalKind::FuelExhausted, exhausted.what()};
    }
    return std::nullopt;
  };

  for (const PathStepSpec &step : *steps) {
    const auto dim = core.findDimension(step.dimension);
    if (!dim) return PathWalk{.cells = {}, .visits = walk.visits};
    // Both walks of a bidirectional step, positive first, as the compiled step
    // yields them.
    const std::array<DimVector, 2> both{DimVector::POS, DimVector::NEG};
    const std::span<const DimVector> directions =
        step.bidirectional ? std::span<const DimVector>{both}
                           : std::span<const DimVector>{&step.direction, 1};
    std::vector<CellRef> reached;
    for (const CellRef anchor : walk.cells) {
      for (const DimVector direction : directions) {
        // The anchor is in the seen set, so a rank that rings back to it ends
        // there, and a lasso ends where it first repeats.
        std::unordered_set<CellRef> seen{anchor};
        CellRef current = anchor;
        for (;;) {
          if (auto stopped = charge())
            return std::unexpected{std::move(*stopped)};
          const auto next = arena.linkedMinting(current, *dim, direction);
          if (!next || !seen.insert(*next).second) break;
          reached.push_back(*next);
          current = *next;
        }
      }
    }
    if (step.clamp) applyClamp(reached, *step.clamp);
    walk.cells = std::move(reached);
    if (walk.cells.empty()) break;
  }
  return walk;
}

namespace {

using Comparable = std::variant<std::int64_t, double, bool, std::string>;

Comparable classify(zigzag::QueryScalar value) {
  return std::visit(
      [](auto &&held) -> Comparable {
        using T = std::remove_cvref_t<decltype(held)>;
        if constexpr (std::same_as<T, std::string>) {
          const auto inferred = xanadu::inferTextValue(held);
          switch (inferred.kind) {
          case xanadu::ValueKind::Int64:
            return std::bit_cast<std::int64_t>(inferred.bits);
          case xanadu::ValueKind::Double:
            return std::bit_cast<double>(inferred.bits);
          case xanadu::ValueKind::Bool:
            return inferred.bits != 0;
          default:
            return std::move(held);
          }
        } else {
          return held;
        }
      },
      std::move(value));
}

std::string canonicalText(const Comparable &value) {
  return std::visit(
      [](const auto &held) -> std::string {
        if constexpr (std::same_as<std::remove_cvref_t<decltype(held)>,
                                   std::string>)
          return held;
        else
          return xanadu::scalarValue(held).text;
      },
      value);
}

/// The Equal case of the VM's VqlCompare (vortex_comparison.cpp), which keeps
/// its helpers private. Mirrored here and not widened into a public VM header
/// because this card changes no core file; the opcode card should replace this
/// body with a call to one shared function so there is a single definition of
/// "equal by value".
bool equalByValue(zigzag::QueryScalar left, zigzag::QueryScalar right) {
  // Two plain text cells compare as text: a string is never unequal to itself
  // and "1" differs from "1.0". Only a text against a number infers a number.
  if (std::holds_alternative<std::string>(left) &&
      std::holds_alternative<std::string>(right))
    return std::get<std::string>(left) == std::get<std::string>(right);
  const auto l = classify(std::move(left));
  const auto r = classify(std::move(right));
  if (l.index() == r.index()) return l == r;
  if (const auto *i = std::get_if<std::int64_t>(&l))
    if (const auto *d = std::get_if<double>(&r))
      return !std::isnan(*d) && std::trunc(*d) == *d && std::abs(*d) < 0x1p63 &&
             static_cast<std::int64_t>(*d) == *i;
  if (const auto *d = std::get_if<double>(&l))
    if (const auto *i = std::get_if<std::int64_t>(&r))
      return !std::isnan(*d) && std::trunc(*d) == *d && std::abs(*d) < 0x1p63 &&
             static_cast<std::int64_t>(*d) == *i;
  return canonicalText(l) == canonicalText(r);
}

} // namespace

std::expected<bool, PathRefusal>
pathTargetMatches(zigzag::vortex::VortexCore &core,
                  const std::span<const CellRef> reached,
                  const std::optional<CellRef> neighbour) {
  if (reached.empty() || !neighbour) return false;
  if (reached.size() > 1)
    return refuse(PathRefusalKind::SeveralResults,
                  std::format("VQL a path target reached {} cells; a shape "
                              "match needs exactly one",
                              reached.size()));
  const auto read = [&](const CellRef cell) {
    return zigzag::readQueryScalar(core.arena(), cell, core.dims().clone,
                                   core.dims().runtimeObjectClaim.value());
  };
  auto left  = read(reached.front());
  auto right = read(*neighbour);
  // A cell with no value is not equal to anything, as in the VM's comparison.
  const auto missing = [](const auto &scalar) {
    return !scalar && scalar.error() == zigzag::QueryScalarReadError::Missing;
  };
  if (missing(left) || missing(right)) return false;
  if (!left || !right)
    return refuse(PathRefusalKind::Unreadable,
                  "VQL a path target's value cannot be compared: a foreign, "
                  "structural or unreadable cell");
  return equalByValue(std::move(*left), std::move(*right));
}

} // namespace xanadu::vql
