/**
 * @file view_space_fixture.hpp
 * @brief A small real slice for the view-space tests to read through.
 */
#ifndef TESTS_XUZZ_VIEW_SPACE_FIXTURE_HPP
#define TESTS_XUZZ_VIEW_SPACE_FIXTURE_HPP

#include <gtest/gtest.h>

#include <functional>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
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

/**
 * The worked example of design/view-system.md §9.3.4. Person c has e-mails
 * e1, e2, one phone p1 and addresses a1, a2, a3; a name n1; and a contact
 * that leads to e1 as well. d.spare is a dimension no cell links on.
 */
struct ContactSlice {
  xanadu::Store store;
  xanadu::MicroversionId at;
  std::map<std::string, zigzag::DimRef, std::less<>> dims;
  std::map<std::string, zigzag::CellRef, std::less<>> cells;

  ContactSlice() {
    at = store.sliceGenesis(xanadu::MicroversionId{});
    for (const auto *const name : {"d.email", "d.phone", "d.address", "d.name",
                                   "d.contact", "d.spare"}) {
      const auto minted = store.makeDimension(at, name);
      at                = minted.version;
      dims[name]        = minted.dim;
    }
    for (const auto *const text :
         {"c", "e1", "e2", "p1", "a1", "a2", "a3", "n1"}) {
      at          = store.makeCell(at, text);
      cells[text] = store.cellRefOf(at);
    }
    rank("d.email", {"c", "e1", "e2"});
    rank("d.phone", {"c", "p1"});
    rank("d.address", {"c", "a1", "a2", "a3"});
    rank("d.name", {"c", "n1"});
    rank("d.contact", {"c", "e1"});
  }

  [[nodiscard]] zigzag::DimRef dim(const std::string_view name) const {
    return dims.find(name)->second;
  }
  [[nodiscard]] zigzag::CellRef cell(const std::string_view name) const {
    return cells.find(name)->second;
  }
  [[nodiscard]] zigzag::Manifold manifold() const {
    return store.rebuildManifold(at);
  }

private:
  void rank(const std::string_view dimension,
            std::initializer_list<const char *> along) {
    const char *previous = nullptr;
    for (const auto *const name : along) {
      if (previous != nullptr) {
        at = store.setLink(at, cell(previous), dim(dimension),
                           zigzag::DimVector::POS, cell(name));
      }
      previous = name;
    }
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
