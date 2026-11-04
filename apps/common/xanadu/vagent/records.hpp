#pragma once

// Shared record/rank helpers for the `+d.vars` / `+d.patch` / `+d.recipe`
// cell shapes. meta.cpp (the runtime's mutation surface) and genesis.cpp
// (the replay ceremony) read and write the same records — keyed `d.vars`
// records, patch fields, `+d.step` chains and rank tails — so the cell
// manipulation lives once here rather than being re-implemented per TU.

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

#include <bit>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xanadu::vagent {

/// The `+d.step`-linked chain that follows a single lead edge — the
/// `d.refs`/`d.recipe`/`d.patch-head` shape.
[[nodiscard]] inline std::vector<CellRef>
stepChain(zigzag::ArenaManifold &arena, CellRef origin, DimRef leadDim,
          DimRef step) {
  std::vector<CellRef> out;
  for (auto cell = arena.linked(origin, leadDim, Posward);
       cell && arena.contains(*cell);
       cell = arena.linked(*cell, step, Posward))
    out.push_back(*cell);
  return out;
}

/// Unlink @p cell off a same-dim rank with predecessor→successor repair
/// (the `prune`/`unlearn` unlink idiom). The forward edge drops first so
/// the successor's backlink is free before the predecessor repair rewires
/// it.
inline void unlinkRank(zigzag::ArenaManifold &arena, CellRef cell,
                       DimRef dim) {
  const auto pred = arena.linked(cell, dim, Negward);
  const auto succ = arena.linked(cell, dim, Posward);
  if (succ) zigzag::expectWritten(arena.unlink(cell, dim, Posward));
  if (pred) {
    if (succ)
      zigzag::expectWritten(arena.link(*pred, dim, Posward, *succ));
    else
      zigzag::expectWritten(arena.unlink(*pred, dim, Posward));
  }
}

/// Parse a record field as int64. `std::stoll` throws on a malformed
/// record — a crash mid-ceremony — so fields read through this stay a
/// `std::nullopt` the caller can fall back or fault on.
[[nodiscard]] inline std::optional<std::int64_t>
toInt64(std::string_view text) noexcept {
  std::int64_t value = 0;
  const auto *first  = text.data();
  const auto *last   = first + text.size();
  const auto [ptr, ec] = std::from_chars(first, last, value);
  if (ec != std::errc{} || ptr != last) return std::nullopt;
  return value;
}

/// Mint or find the `+d.vars` record keyed @p key on @p owner.
[[nodiscard]] inline std::optional<CellRef>
varsRecord(zigzag::ArenaManifold &arena, VortexCore &core, CellRef owner,
           std::string_view key, bool mint) {
  const auto vars = core.dims().vars;
  for (const auto record : zigzag::rankAfter(arena, owner, vars))
    if (arena.textOf(record) == key) return record;
  if (!mint) return std::nullopt;
  const auto record = arena.makeCell(key);
  zigzag::expectWritten(
      arena.link(zigzag::rankTail(arena, owner, vars), vars, Posward, record));
  return record;
}

/// A patch record's field text/int: `descriptor` selects the `+d.values`
/// descriptor's `d.vars` chain (target/verb/parent), false selects the
/// patch cell's own (ordinal/alias/priority).
[[nodiscard]] inline std::optional<std::string>
patchField(zigzag::ArenaManifold &arena, VortexCore &core, CellRef patch,
           std::string_view key, bool descriptor) {
  const auto owner = descriptor
                         ? arena.linked(patch, core.dims().values, Posward)
                         : std::optional{patch};
  if (!owner || !arena.contains(*owner)) return std::nullopt;
  for (const auto record : zigzag::rankAfter(arena, *owner, core.dims().vars)) {
    if (arena.textOf(record) == key) {
      const auto value = arena.linked(record, core.dims().values, Posward);
      if (!value) return std::nullopt;
      if (const auto number = arena.asInt64(*value))
        return std::to_string(*number);
      return arena.textOf(*value);
    }
  }
  return std::nullopt;
}

/// The `+d.patch-head` chain's recorded head names, in order.
[[nodiscard]] inline std::vector<std::string>
patchHeadNames(zigzag::ArenaManifold &arena, VortexCore &core, CellRef patch,
               DimRef patchHead) {
  std::vector<std::string> names;
  for (const auto record :
       stepChain(arena, patch, patchHead, core.dims().step)) {
    const auto name = arena.linked(record, core.dims().values, Posward);
    names.emplace_back(name ? arena.textOf(*name) : std::string{});
  }
  return names;
}

/// An int64 value write leaving content alone. Arena cells carry no
/// clone-master indirection — setValueBits takes the cell itself.
inline void writeInt(zigzag::ArenaManifold &arena, CellRef cell,
                     std::int64_t value) {
  zigzag::expectWritten(arena.setValueBits(
      cell, xanadu::ValueKind::Int64, std::bit_cast<std::uint64_t>(value)));
}

} // namespace xanadu::vagent
