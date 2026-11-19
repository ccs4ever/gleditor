#include "common/xanadu/vql/prose_hits.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>

namespace xanadu::vql {
using zigzag::CellRef;

namespace {

// One store's document lines. The hit order inside a version is offset order;
// the caller owns the order between stores.
void appendLineHits(MultiStoreCoordinator &coordinator, const StoreInfo &info,
                    const std::string_view needle, zigzag::ArenaManifold &arena,
                    std::vector<CellRef> &hits) {
  const auto sourceDim = coordinator.resolveDimension("d.source");
  const auto identity =
      info.path.empty() ? "store:" + info.store->documentId().str() : info.path;
  for (const auto &version : info.store->currentVersions()) {
    arena.consumeWork();
    const auto document = info.store->rebuild(version);
    const auto text     = info.store->textOf(version);
    for (auto at = text.find(needle); std::string::npos != at;) {
      arena.consumeWork();
      const auto newline =
          at == 0 ? std::string::npos : text.rfind('\n', at - 1);
      const auto begin = std::string::npos == newline ? 0 : newline + 1;
      const auto end   = std::min(text.find('\n', at), text.size());
      const auto spans =
          document.spansFor(static_cast<std::uint32_t>(begin),
                            static_cast<std::uint32_t>(end - begin));
      const auto hit    = arena.makeQuote(info.spaceId, spans);
      const auto source = arena.makeCell(
          identity + "#version=" + version.str() + "&at=" + std::to_string(at));
      zigzag::expectWritten(
          arena.link(hit, sourceDim, zigzag::DimVector::POS, source));
      hits.push_back(hit);
      // The next hit must be on a later line: resume past this line's end.
      at = end < text.size() ? text.find(needle, end + 1) : std::string::npos;
    }
  }
}

// Every manifold cell of one store whose text holds the needle, answered by
// the arena's proxy for it: the cell itself is the store's, not the arena's.
void appendCellHits(const StoreInfo &info, const std::string_view needle,
                    zigzag::ArenaManifold &arena, std::vector<CellRef> &hits) {
  for (const auto &slot : info.manifold->cells()) {
    arena.consumeWork();
    if (info.manifold->textOf(slot.birthOp, *info.store).find(needle) !=
        std::string::npos)
      hits.push_back(arena.proxyFor(info.spaceId, slot.birthOp));
  }
}

} // namespace

std::vector<CellRef> proseHits(MultiStoreCoordinator &coordinator,
                               const std::string_view needle,
                               zigzag::ArenaManifold &arena) {
  std::vector<CellRef> hits;
  for (const auto &info : coordinator.stores()) {
    arena.consumeWork();
    if (!info.manifold || !info.store) continue;
    appendLineHits(coordinator, info, needle, arena, hits);
  }
  return hits;
}

std::vector<CellRef> findHits(MultiStoreCoordinator &coordinator,
                              const std::string_view needle,
                              zigzag::ArenaManifold &arena) {
  std::vector<CellRef> hits;
  for (const auto &info : coordinator.stores()) {
    arena.consumeWork();
    if (!info.manifold || !info.store) continue;
    appendCellHits(info, needle, arena, hits);
    appendLineHits(coordinator, info, needle, arena, hits);
  }
  return hits;
}

zigzag::vortex::RelationProvider
makeFindProvider(MultiStoreCoordinator &coordinator) {
  using zigzag::vortex::RelationAnswer;
  using zigzag::vortex::RelationQuery;
  using zigzag::vortex::RelationResult;
  // Shared so the copy RelationNext takes of the provider keeps one list.
  auto hits = std::make_shared<std::vector<CellRef>>();
  return [&coordinator, hits](const RelationQuery &query) -> RelationResult {
    if (query.arguments.size() != 1)
      return std::unexpected("find expects one argument, the needle");
    if (query.position == 0) {
      *hits = findHits(coordinator, query.arena.textOf(query.arguments.front()),
                       query.arena);
    }
    const auto count = static_cast<std::int64_t>(hits->size());
    if (query.position < 0 || query.position > count)
      return std::unexpected("find was resumed at position " +
                             std::to_string(query.position) + " of " +
                             std::to_string(count) + " hits");
    if (query.position == count) return std::optional<RelationAnswer>{};
    return std::optional<RelationAnswer>{RelationAnswer{
        .nextPosition = query.position + 1,
        .limit        = count,
        .answers      = {(*hits)[static_cast<std::size_t>(query.position)]}}};
  };
}

} // namespace xanadu::vql
