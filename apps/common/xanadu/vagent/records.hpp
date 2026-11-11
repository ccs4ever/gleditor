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

/// Mint a scalar cell carrying the position of @p ref with the
/// `d.mesh.relocation → "cell"` mark — the manifest's "this Int64 is a
/// live position" signal (fragment.cpp's refsRecord uses the same mark
/// for recipe refs). Promote cannot see an Int64-carried referent, so
/// the mark is what harvests it as a root and what the publish's
/// restatement rewrites the slot at the referent's twin — a bare
/// Int64 position goes verbatim through both and silently stale.
/// Write member refs, record refs and term refs through this; flags
/// and integer values keep `makeScalarCell`.
[[nodiscard]] inline CellRef
refCell(zigzag::ArenaManifold &arena, zigzag::vortex::VortexCore &core,
        CellRef ref) {
  const auto cell =
      arena.makeScalarCell(static_cast<std::int64_t>(ref));
  zigzag::expectWritten(
      arena.link(cell,
                 core.findOrMintDimension("d.mesh.relocation"),
                 zigzag::DimVector::POS, arena.makeCell("cell")));
  return cell;
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
///
/// +d.vars is a shared rank — module symbols and their records ride
/// the same chain, so a member's text alone cannot scope a key to its
/// owner (a symbol missing 'holdsTest' would find the next symbol's).
/// The record's +d.owner arm is the scope, as *scalar content* the way
/// +d.holder carries the agent ref: N records pointing at one owner
/// cannot share a live edge — the owner's -d.owner slot holds one.
/// Members keyed for another owner are skipped, wherever they sit.
[[nodiscard]] inline std::optional<CellRef>
varsRecord(zigzag::ArenaManifold &arena, VortexCore &core, CellRef owner,
           std::string_view key, bool mint) {
  const auto vars = core.dims().vars;
  const auto of   = core.findOrMintDimension("d.owner");
  const auto owns = [&](CellRef record) {
    const auto v = arena.linked(record, of, Posward);
    // A record minted without +d.owner (test fixtures, pre-scope writes)
    // is unscoped and matches any owner; a scoped record must name this
    // one — that is what keeps sibling symbols' keys apart.
    return !v || arena.asInt64(*v).value_or(-1) ==
                     static_cast<std::int64_t>(owner);
  };
  for (const auto record : zigzag::rankAfter(arena, owner, vars))
    if (arena.textOf(record) == key && owns(record)) return record;
  if (!mint) return std::nullopt;
  const auto record = arena.makeCell(key);
  zigzag::expectWritten(
      arena.link(zigzag::rankTail(arena, owner, vars), vars, Posward, record));
  zigzag::expectWritten(
      arena.link(record, of, Posward,
                 arena.makeScalarCell(static_cast<std::int64_t>(owner))));
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
