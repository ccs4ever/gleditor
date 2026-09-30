#include "result_slice.hpp"

#include <stdexcept>
#include <utility>

#include "common/xanadu/zigzag/manifold.hpp"

namespace xanadu {

MicroversionId writeResultSlice(Store &store,
                                const std::span<const ResultRow> rows) {
  if (store.homeCell() != zigzag::noCell) {
    throw std::invalid_argument("result output store must be empty");
  }
  auto version      = store.sliceGenesis(MicroversionId{});
  const auto result = store.makeDimension(version, "d.result");
  version           = result.version;
  const auto source = store.makeDimension(version, "d.source");
  version           = source.version;

  auto manifold = store.rebuildManifold(version);
  auto previous = store.homeCell();
  for (const auto &row : rows) {
    version         = store.makeCell(version, row.text);
    const auto cell = store.cellRefOf(version);
    manifold.advanceOrRefold(store, version);
    version = store.setLink(version, previous, result.dim,
                            zigzag::DimVector::POS, cell, &manifold);
    manifold.advanceOrRefold(store, version);
    previous = cell;
    if (!row.source.empty()) {
      version               = store.makeCell(version, row.source);
      const auto sourceCell = store.cellRefOf(version);
      manifold.advanceOrRefold(store, version);
      version = store.setLink(version, cell, source.dim, zigzag::DimVector::POS,
                              sourceCell, &manifold);
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
    ResultRow row{.text = manifold.textOf(cell, store), .source = {}};
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
