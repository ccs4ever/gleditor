#pragma once
// Host-side data access for `find`: the lines of loaded documents that hold a
// needle. A document's prose is no cell, so each hit is answered with a
// quotation cell of its line. This reads stores; it evaluates no query AST.

#include <string_view>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {

/// The relation name a lowered `find` asks RelationNext for. It cannot collide
/// with a logic relation, whose names carry no dot-qualified `vql.` prefix.
inline constexpr std::string_view findRelationName = "vql.find";

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

/// What `find` answers: per store, in store order, first every manifold cell
/// whose text holds @p needle (as the arena's proxy for it), then that store's
/// document-line hits as proseHits() describes them. Mints and charges as
/// proseHits() does.
[[nodiscard]] std::vector<zigzag::CellRef>
findHits(MultiStoreCoordinator &coordinator, std::string_view needle,
         zigzag::ArenaManifold &arena);

/// The provider RelationNext asks for findRelationName: one argument, the
/// needle's cell. The first call of a scan (position 0) computes findHits() and
/// keeps the list; later positions index into it, so a scan costs one search,
/// not one per hit. The list is the provider's own and is dropped with it, so
/// a second scan never sees the first one's hits. Register it for the length of
/// one query run and remove it after.
[[nodiscard]] zigzag::vortex::RelationProvider
makeFindProvider(MultiStoreCoordinator &coordinator);

} // namespace xanadu::vql
