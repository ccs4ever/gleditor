#include "result_slice.hpp"

#include <algorithm>
#include <ranges>
#include <stdexcept>
#include <utility>

#include "common/xanadu/publication.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace xanadu {

MicroversionId writeResultSlice(Store &store,
                                const std::span<const ResultRow> rows) {
  if (store.homeCell() != zigzag::noCell) {
    throw std::invalid_argument("result output store must be empty");
  }
  // Every quotation checked before anything is written: a refused row leaves
  // the store as it was.
  for (const auto &row : rows) {
    if (row.quote &&
        (row.quote->spans.empty() ||
         !std::ranges::all_of(row.quote->spans, [&](const PrimediaSpan &span) {
           return canCarry(*row.quote->store, store, span);
         }))) {
      throw std::invalid_argument("result row \"" + row.text +
                                  "\" quotes a scroll this store cannot name");
    }
  }

  auto version      = store.sliceGenesis(MicroversionId{});
  const auto result = store.makeDimension(version, "d.result");
  version           = result.version;
  const auto source = store.makeDimension(version, "d.source");
  version           = source.version;

  auto manifold = store.rebuildManifold(version);
  auto previous = store.homeCell();
  for (const auto &row : rows) {
    zigzag::CellRef cell = zigzag::noCell;
    if (row.quote) {
      // The quoted bytes, transcluded: the first span makes the cell and
      // each after it is spliced on, one span per operation (U3).
      const auto &spans = row.quote->spans;
      version           = store.makeCell(version,
                                         *carrySpan(*row.quote->store, store, spans[0]));
      cell              = store.cellRefOf(version);
      manifold.advanceOrRefold(store, version);
      std::uint64_t at = spans[0].length;
      for (const auto &quoted : spans | std::views::drop(1)) {
        version = store.spliceCellSpan(
            version, cell, at, 0, *carrySpan(*row.quote->store, store, quoted));
        manifold.advanceOrRefold(store, version);
        at += quoted.length;
      }
    } else {
      version = store.makeCell(version, row.text);
      cell    = store.cellRefOf(version);
      manifold.advanceOrRefold(store, version);
    }
    version = store.setLink(version, previous, result.dim,
                            zigzag::DimVector::POS, cell);
    manifold.advanceOrRefold(store, version);
    previous = cell;
    if (!row.source.empty()) {
      version               = store.makeCell(version, row.source);
      const auto sourceCell = store.cellRefOf(version);
      manifold.advanceOrRefold(store, version);
      version = store.setLink(version, cell, source.dim, zigzag::DimVector::POS,
                              sourceCell);
      manifold.advanceOrRefold(store, version);
    }
  }
  store.setCurrentVersions({version});
  return version;
}

std::vector<ResultRow> readResultSlice(const Store &store,
                                       const MicroversionId version) {
  const auto manifold = store.rebuildManifold(version);
  const auto result   = manifold.dimensionNamed("d.result", store);
  if (!result) {
    throw std::invalid_argument("store has no d.result rank");
  }
  const auto source = manifold.dimensionNamed("d.source", store);
  std::vector<ResultRow> rows;
  auto cell = manifold.linked(manifold.home(), *result, zigzag::DimVector::POS);
  for (std::size_t remaining = manifold.cellCount();
       cell != zigzag::noCell && remaining-- > 0;
       cell = manifold.linked(cell, *result, zigzag::DimVector::POS)) {
    const auto content = manifold.contentOf(cell);
    ResultRow row{.text   = manifold.textOf(cell, store),
                  .source = {},
                  .quote =
                      QuotedSpans{.store = &store,
                                  .spans = {content.begin(), content.end()}}};
    if (source) {
      if (const auto origin =
              manifold.linked(cell, *source, zigzag::DimVector::POS);
          origin != zigzag::noCell) {
        row.source = manifold.textOf(origin, store);
      }
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

} // namespace xanadu
