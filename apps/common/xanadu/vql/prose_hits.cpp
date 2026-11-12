#include "common/xanadu/vql/prose_hits.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

namespace xanadu::vql {
using zigzag::CellRef;

std::vector<CellRef> proseHits(MultiStoreCoordinator &coordinator,
                               const std::string_view needle,
                               zigzag::ArenaManifold &arena) {
  std::vector<CellRef> hits;
  const auto sourceDim = coordinator.resolveDimension("d.source");
  for (const auto &info : coordinator.stores()) {
    arena.consumeWork();
    if (!info.manifold || !info.store) continue;
    const auto identity = info.path.empty()
                              ? "store:" + info.store->documentId().str()
                              : info.path;
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
        const auto hit = arena.makeQuote(info.spaceId, spans);
        const auto source =
            arena.makeCell(identity + "#version=" + version.str() +
                           "&at=" + std::to_string(at));
        zigzag::expectWritten(
            arena.link(hit, sourceDim, zigzag::DimVector::POS, source));
        hits.push_back(hit);
        // The next hit must be on a later line: resume past this line's end.
        at = end < text.size() ? text.find(needle, end + 1) : std::string::npos;
      }
    }
  }
  return hits;
}

} // namespace xanadu::vql
