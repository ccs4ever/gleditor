#pragma once
// Host-side data access for `find`: the lines of loaded documents that hold a
// needle. A document's prose is no cell, so each hit is answered with a
// quotation cell of its line. This reads stores; it evaluates no query AST.

#include <string_view>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {

/// The quote cell of every line of every loaded store's current documents that
/// holds @p needle, in the order stores, then `currentVersions()`, then offset
/// (one hit per line). An empty needle matches every line; a newline needle
/// matches the line it ends. Each quote spans the line's own primedia, so the
/// bytes stay in the scroll they were typed into, and is linked along
/// `d.source` to a cell naming the document, version and offset of the hit.
/// Mints those cells in @p arena and charges its work budget.
[[nodiscard]] std::vector<zigzag::CellRef>
proseHits(MultiStoreCoordinator &coordinator, std::string_view needle,
          zigzag::ArenaManifold &arena);

} // namespace xanadu::vql
