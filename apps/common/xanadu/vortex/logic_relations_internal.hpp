/**
 * @file logic_relations_internal.hpp
 * @brief Internal definitions shared between logic_relations.cpp and
 * logic_relations_arena.cpp (Cards P3 and P4).
 */
#ifndef COMMON_XANADU_VORTEX_LOGIC_RELATIONS_INTERNAL_HPP
#define COMMON_XANADU_VORTEX_LOGIC_RELATIONS_INTERNAL_HPP

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>

#include "common/xanadu/vortex/logic_compile.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_stdlib.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace zigzag::vortex::logic {

/// What a relation accepts for a bound argument. A variable is always
/// accepted: it is the generative mode.
enum class ArgKind : std::uint8_t {
  Any,     ///< no constraint (a value the relation only unifies)
  Atom,    ///< a name or text
  Integer, ///< a cell id, step or slot index
};

/// A bound argument of the wrong type. The relation adapter turns it into the
/// query's error, as the arithmetic builtins do for a non-number.
class RelationError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/// Base class for relation providers that need access to VortexCore.
class CoreBoundProvider : public RelationProvider {
public:
  void bindCore(VortexCore *c) noexcept { core_ = c; }

  /// Fixes the cells an unbound-argument scan may visit for one solve: the
  /// dense arena indices in [@p scanBegin, @p scanEnd). The beginning is where
  /// the engine's own bootstrap cells (dimensions, std modules, stdlib
  /// clauses) stop; the end is the cell count when the solve started, so cells
  /// the query itself mints (terms, compiled ops, frames) are never visited
  /// and a growing arena cannot change what an iteration yields.
  void bindScan(std::size_t scanBegin, std::size_t scanEnd) noexcept {
    scanBegin_ = scanBegin;
    scanEnd_   = scanEnd;
  }

  /// Called once as a solve begins, before its query is lowered: a provider
  /// drops whatever it cached from the store or arena of an earlier solve.
  virtual void beginSolve() {}

  /// Types of the arguments in position order; empty means unchecked.
  [[nodiscard]] virtual std::span<const ArgKind> signature() const noexcept {
    return {};
  }

protected:
  [[nodiscard]] std::size_t scanBegin() const noexcept {
    return std::max<std::size_t>(1, scanBegin_);
  }
  [[nodiscard]] std::size_t scanEnd() const noexcept { return scanEnd_; }

  VortexCore *core_{nullptr};

private:
  std::size_t scanBegin_{0};
  std::size_t scanEnd_{0};
};

/// Raises RelationError for the first bound argument whose type @p kinds
/// refuses. A variable passes (generative mode); a wrong type is an ISO type
/// error naming the relation, the position and the offending term, never a
/// silent failure.
inline void checkArguments(const VortexStdLib &stdlib,
                           const zigzag::ArenaManifold &arena,
                           const PredicateKey &key,
                           std::span<const ArgKind> kinds,
                           std::span<const CellRef> arguments) {
  for (std::size_t i = 0; i < kinds.size() && i < arguments.size(); ++i) {
    if (kinds[i] == ArgKind::Any) continue;
    const auto bound = stdlib.deref(arguments[i]);
    if (!bound || stdlib.isVar(*bound)) continue;
    const bool integer = arena.valueKindOf(*bound) == xanadu::ValueKind::Int64;
    const bool textual = arena.valueKindOf(*bound) == xanadu::ValueKind::None &&
                         stdlib.argumentsOf(*bound).empty();
    if (kinds[i] == ArgKind::Integer ? integer : textual) continue;
    throw RelationError(
        "type error: " + key.display() + " argument " + std::to_string(i + 1) +
        " must be " +
        (kinds[i] == ArgKind::Integer ? "an integer" : "an atom") + ", found " +
        stdlib.renderTerm(*bound).value_or("?"));
  }
}

/// Resolves an argument cell to a valid CellRef in the arena, checking that
/// Int64 scalars are within [0, CellRef::max()] and contained in the arena.
inline std::optional<CellRef> cellRefArgument(zigzag::ArenaManifold &arena,
                                              const CellRef argument) {
  if (const auto value = arena.asInt64(argument)) {
    if (*value < 0 || static_cast<std::uint64_t>(*value) >
                          std::numeric_limits<CellRef>::max()) {
      return std::nullopt;
    }
    const auto cell = static_cast<CellRef>(*value);
    return arena.contains(cell) ? std::optional<CellRef>{cell} : std::nullopt;
  }
  return arena.contains(argument) ? std::optional<CellRef>{argument}
                                  : std::nullopt;
}

} // namespace zigzag::vortex::logic

#endif // COMMON_XANADU_VORTEX_LOGIC_RELATIONS_INTERNAL_HPP
