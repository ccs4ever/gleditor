/**
 * @file result_store.cpp
 * @brief A query's answer written as a slice of its own, to open in xuzz.
 */
#include "result_store.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "multi_store.hpp"

namespace xanadu::vql {

namespace {

/// A result's content: the spans to quote when every one of them lives in
/// the shared permascroll, else the text to write.
struct Content {
  std::vector<PrimediaSpan> spans;
  std::string text;
};

Content contentOf(const MultiStoreCoordinator &coordinator,
                  const zigzag::CellRef cell) {
  const auto &arena = coordinator.arena();
  // Scroll 0 is the author's permascroll, which every loaded store shares:
  // an address there means the same bytes in the answer as in the source.
  const auto quotable = [](const std::span<const PrimediaSpan> spans) {
    return !spans.empty() && std::ranges::all_of(spans, [](const auto &span) {
      return 0 == span.scroll && span.length > 0;
    });
  };
  if (const auto foreign = arena.resolveForeign(cell)) {
    const auto &[store, local] = *foreign;
    for (const auto &info : coordinator.stores()) {
      if (info.store.get() == store && info.manifold) {
        const auto spans = info.manifold->contentOf(local);
        if (quotable(spans)) {
          return {.spans = {spans.begin(), spans.end()}, .text = {}};
        }
        return {.spans = {}, .text = info.manifold->textOf(local, *store)};
      }
    }
  }
  if (const auto spans = arena.contentOf(cell); quotable(spans)) {
    return {.spans = {spans.begin(), spans.end()}, .text = {}};
  }
  const auto value = coordinator.core().render(cell);
  return {.spans = {},
          .text  = std::visit(
              [](const auto &held) -> std::string {
                using T = std::decay_t<decltype(held)>;
                if constexpr (std::is_same_v<T, std::string>) {
                  return held;
                } else if constexpr (std::is_same_v<T, bool>) {
                  return held ? "true" : "false";
                } else {
                  return std::format("{}", held);
                }
              },
              value)};
}

} // namespace

ResultStoreStats
writeResultStore(const MultiStoreCoordinator &coordinator,
                 const std::string_view query,
                 const std::span<const zigzag::CellRef> results, Store &out) {
  if (0 != out.opCount()) {
    throw std::invalid_argument(
        "a result store is written fresh, and this one already has operations");
  }
  ResultStoreStats stats;
  auto at         = out.sliceGenesis(MicroversionId{});
  const auto home = out.homeCell();
  // The default ZigZag view shows d.1 across and d.2 down: the results on
  // d.1, as a sequence is, and the query on d.2, so a result store opens in
  // xuzz with its answer on screen.
  const auto queryDim = out.makeDimension(at, "d.2");
  at                  = queryDim.version;
  const auto rankDim  = out.makeDimension(at, "d.1");
  at                  = rankDim.version;

  at               = out.makeCell(at, query);
  const auto asked = out.cellRefOf(at);
  at = out.setLink(at, home, queryDim.dim, zigzag::DimVector::POS, asked);

  auto previous = home;
  for (const auto result : results) {
    const auto content = contentOf(coordinator, result);
    zigzag::CellRef cell{};
    if (!content.spans.empty()) {
      at                   = out.makeCell(at, content.spans.front());
      cell                 = out.cellRefOf(at);
      std::uint64_t length = content.spans.front().length;
      for (std::size_t i = 1; i < content.spans.size(); ++i) {
        at = out.spliceCellSpan(at, cell, length, 0, content.spans[i]);
        length += content.spans[i].length;
      }
      ++stats.quoted;
    } else {
      at   = out.makeCell(at, content.text);
      cell = out.cellRefOf(at);
      ++stats.copied;
    }
    at = out.setLink(at, previous, rankDim.dim, zigzag::DimVector::POS, cell);
    previous = cell;
    ++stats.cells;
  }
  return stats;
}

} // namespace xanadu::vql
