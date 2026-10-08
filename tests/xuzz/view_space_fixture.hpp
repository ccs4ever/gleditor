/**
 * @file view_space_fixture.hpp
 * @brief A small real slice for the view-space tests to read through.
 */
#ifndef TESTS_XUZZ_VIEW_SPACE_FIXTURE_HPP
#define TESTS_XUZZ_VIEW_SPACE_FIXTURE_HPP

#include <gtest/gtest.h>

#include <string>
#include <tuple>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/view/view_manifold.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace xuzz_test {

/// A store with one dimension and a rank of three cells on it. Manifold has no
/// public way to mint cells, so a slice is built as operations and folded.
struct RealSlice {
  xanadu::Store store;
  xanadu::MicroversionId at;
  zigzag::DimRef dim{zigzag::noCell};
  std::vector<zigzag::CellRef> cells;

  RealSlice() {
    at                = store.sliceGenesis(xanadu::MicroversionId{});
    const auto minted = store.makeDimension(at, "d.step");
    at                = minted.version;
    dim               = minted.dim;
    for (const auto *const text : {"first", "second", "third"}) {
      at = store.makeCell(at, text);
      cells.push_back(store.cellRefOf(at));
    }
    at = store.setLink(at, cells[0], dim, zigzag::DimVector::POS, cells[1]);
    at = store.setLink(at, cells[1], dim, zigzag::DimVector::POS, cells[2]);
  }

  [[nodiscard]] zigzag::Manifold manifold() const {
    return store.rebuildManifold(at);
  }
};

/// Every violation verifyViewSpace() reports, for a failure message that
/// names them.
[[nodiscard]] inline std::string
violationsOf(const xanadu::view::ViewManifold &space) {
  std::string found;
  std::ignore = xanadu::view::verifyViewSpace(
      space, [&](const xanadu::view::ViewSpaceViolation &violation) {
        found += std::string{violation.rule};
        if (violation.cell.has_value()) {
          found += "@" + std::to_string(violation.cell->ref);
        }
        found += " ";
      });
  return found;
}

/// The check every test that mutates a view space makes before any other
/// (design/view-system.md §6.6), and I3's on both arenas.
inline void expectSound(const xanadu::view::ViewManifold &space) {
  EXPECT_EQ(violationsOf(space), "");
  EXPECT_EQ(space.shadowCount(xanadu::view::Layer::Binding), 0U);
  EXPECT_EQ(space.shadowCount(xanadu::view::Layer::Derived), 0U);
}

} // namespace xuzz_test

#endif // TESTS_XUZZ_VIEW_SPACE_FIXTURE_HPP
