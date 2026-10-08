/**
 * @file view_manifold_random_test.cpp
 * @brief Random sequences of view-space calls, each checked against a model
 *        and against verifyViewSpace() (design/view-system.md §6.4, §16.2).
 *
 * The refs each call is handed are drawn from everything minted so far, so a
 * sequence mixes current cells with tossed ones, cells of the other arena that
 * share a number, and real cells dressed as view cells. The model predicts
 * the outcome of every call, refusal reason included, from the spec's rules
 * alone; the space must agree and must stay sound after each call.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <tuple>
#include <vector>

#include "common/xanadu/view/view_manifold.hpp"
#include "common/xanadu/zigzag/manifold.hpp"
#include "view_space_fixture.hpp"

namespace {

using xanadu::view::bindingEpoch;
using xanadu::view::Layer;
using xanadu::view::ViewCellRef;
using xanadu::view::ViewEpoch;
using xanadu::view::ViewError;
using xanadu::view::ViewManifold;
using xuzz_test::RealSlice;
using xuzz_test::violationsOf;
using zigzag::CellRef;
using zigzag::DimVector;

constexpr std::size_t layerIndex(const Layer layer) {
  return Layer::Binding == layer ? 0 : 1;
}

/// What the spec says the space holds, kept independently of it.
class Model {
public:
  using Key = std::tuple<CellRef, CellRef, DimVector>;

  [[nodiscard]] ViewEpoch epochOf(const Layer layer) const {
    return Layer::Binding == layer ? bindingEpoch : epoch_;
  }

  [[nodiscard]] ViewCellRef wrap(const Layer layer, const CellRef ref) const {
    return ViewCellRef{.ref = ref, .epoch = epochOf(layer), .layer = layer};
  }

  /// §6.6's order: staleness first, then which arena the ref belongs to.
  [[nodiscard]] std::optional<ViewError> refusal(const Layer layer,
                                                 const ViewCellRef cell) const {
    if (cell.epoch != epochOf(cell.layer)) {
      return ViewError::StaleEpoch;
    }
    if (cell.layer != layer || !live(layer).contains(cell.ref)) {
      return ViewError::RealCellInViewLink;
    }
    return std::nullopt;
  }

  void minted(const Layer layer, const CellRef ref) {
    liveSet(layer).insert(ref);
  }
  void targets(const Layer layer, const CellRef ref, const CellRef target) {
    targets_[layerIndex(layer)][ref] = target;
  }

  [[nodiscard]] std::optional<CellRef> neighbour(const Layer layer,
                                                 const Key &key) const {
    const auto &links = links_[layerIndex(layer)];
    const auto found  = links.find(key);
    return found == links.end() ? std::nullopt
                                : std::optional<CellRef>{found->second};
  }

  /// What link() must answer, applying it when it succeeds.
  [[nodiscard]] std::optional<ViewError>
  link(const Layer layer, const ViewCellRef from, const ViewCellRef dim,
       const DimVector dir, const ViewCellRef to) {
    for (const auto &end : {from, dim, to}) {
      if (const auto why = refusal(layer, end)) {
        return why;
      }
    }
    const auto mine   = neighbour(layer, {from.ref, dim.ref, dir});
    const auto theirs = neighbour(layer, {to.ref, dim.ref, -dir});
    if (mine == to.ref) {
      return std::nullopt;
    }
    if (mine.has_value() || (theirs.has_value() && *theirs != from.ref)) {
      return ViewError::OccupiedDirection;
    }
    auto &links                     = links_[layerIndex(layer)];
    links[{from.ref, dim.ref, dir}] = to.ref;
    links[{to.ref, dim.ref, -dir}]  = from.ref;
    return std::nullopt;
  }

  [[nodiscard]] std::optional<ViewError> unlink(const Layer layer,
                                                const ViewCellRef from,
                                                const ViewCellRef dim,
                                                const DimVector dir) {
    for (const auto &end : {from, dim}) {
      if (const auto why = refusal(layer, end)) {
        return why;
      }
    }
    if (const auto far = neighbour(layer, {from.ref, dim.ref, dir})) {
      auto &links = links_[layerIndex(layer)];
      links.erase({from.ref, dim.ref, dir});
      links.erase({*far, dim.ref, -dir});
    }
    return std::nullopt;
  }

  void toss() {
    ++epoch_;
    liveSet(Layer::Derived).clear();
    links_[layerIndex(Layer::Derived)].clear();
    targets_[layerIndex(Layer::Derived)].clear();
  }

  [[nodiscard]] const std::set<CellRef> &live(const Layer layer) const {
    return live_[layerIndex(layer)];
  }
  [[nodiscard]] const std::map<Key, CellRef> &links(const Layer layer) const {
    return links_[layerIndex(layer)];
  }
  [[nodiscard]] std::optional<CellRef> target(const Layer layer,
                                              const CellRef ref) const {
    const auto &targets = targets_[layerIndex(layer)];
    const auto found    = targets.find(ref);
    return found == targets.end() ? std::nullopt
                                  : std::optional<CellRef>{found->second};
  }

private:
  [[nodiscard]] std::set<CellRef> &liveSet(const Layer layer) {
    return live_[layerIndex(layer)];
  }

  ViewEpoch epoch_{bindingEpoch + 1};
  std::array<std::set<CellRef>, 2> live_;
  std::array<std::map<Key, CellRef>, 2> links_;
  std::array<std::map<CellRef, CellRef>, 2> targets_;
};

/// The whole of each arena agrees with the model: every modelled link is
/// there, nothing else is, and every occurrence holds its modelled target.
void expectAgrees(ViewManifold &space, const Model &model) {
  for (const auto layer : {Layer::Binding, Layer::Derived}) {
    for (const auto &[key, far] : model.links(layer)) {
      const auto &[from, dim, dir] = key;
      EXPECT_EQ(
          space.linked(model.wrap(layer, from), model.wrap(layer, dim), dir),
          model.wrap(layer, far));
    }
    std::size_t sides = 0;
    auto &arena       = space.arenaForTesting(layer);
    for (const auto &slot : arena.cells()) {
      for (const auto &edge : arena.dimensionsOf(slot.birthOp)) {
        sides += (zigzag::noCell != edge.pos ? 1U : 0U) +
                 (zigzag::noCell != edge.neg ? 1U : 0U);
      }
    }
    EXPECT_EQ(sides, model.links(layer).size());
    for (const auto ref : model.live(layer)) {
      EXPECT_EQ(space.target(model.wrap(layer, ref)), model.target(layer, ref));
    }
  }
}

class ViewManifoldRandomTest : public ::testing::TestWithParam<std::uint32_t> {
};

TEST_P(ViewManifoldRandomTest, everyCallMatchesTheModelAndLeavesTheSpaceSound) {
  constexpr int steps      = 2'000;
  constexpr int checkEvery = 250;

  RealSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  Model model;
  std::mt19937 rng{GetParam()};
  const auto roll = [&](const int sides) {
    return std::uniform_int_distribution<int>{0, sides - 1}(rng);
  };

  // Everything ever minted, stale or not, plus real cells dressed as view
  // cells of each layer, so that refusals are exercised as often as writes.
  std::vector<ViewCellRef> pool;
  // How often link() succeeded, and was refused for each reason, so a run that
  // only ever exercised one path does not pass as a test of all of them.
  std::map<std::optional<ViewError>, int> outcomes;
  const auto anyLayer = [&] {
    return 0 == roll(2) ? Layer::Binding : Layer::Derived;
  };
  // Mostly a live cell of the layer the call names, so that writes succeed
  // and collide; otherwise anything at all. A dimension is drawn from the
  // layer's first few cells, so that links meet on a shared key.
  constexpr int fewDims = 4;
  const auto draw = [&](const Layer layer, const bool asDim) -> ViewCellRef {
    const auto &live = model.live(layer);
    if (roll(10) < 8 && !live.empty()) {
      const auto size = static_cast<int>(live.size());
      auto it         = live.begin();
      std::advance(it, roll(asDim ? std::min(size, fewDims) : size));
      return model.wrap(layer, *it);
    }
    const auto other = anyLayer();
    if (roll(4) == 0 || pool.empty()) {
      return model.wrap(other, slice.cells[static_cast<std::size_t>(roll(
                                   static_cast<int>(slice.cells.size())))]);
    }
    return pool[static_cast<std::size_t>(roll(static_cast<int>(pool.size())))];
  };
  const auto record = [&](const ViewCellRef cell) {
    model.minted(cell.layer, cell.ref);
    pool.push_back(cell);
  };

  for (int step = 0; step < steps; ++step) {
    const auto op = roll(100);
    if (op < 15) {
      const auto layer = anyLayer();
      const auto cell  = space.mint(layer);
      ASSERT_TRUE(cell.has_value());
      ASSERT_EQ(cell->layer, layer);
      ASSERT_EQ(cell->epoch, model.epochOf(layer));
      record(*cell);
    } else if (op < 25) {
      const auto layer = anyLayer();
      const auto real  = slice.cells[static_cast<std::size_t>(
          roll(static_cast<int>(slice.cells.size())))];
      const auto cell  = space.mintOccurrence(layer, real);
      ASSERT_TRUE(cell.has_value());
      record(*cell);
      model.targets(layer, cell->ref, real);
    } else if (op < 28) {
      // Minted on first use and then the same cell for the epoch. Not drawn
      // from: a random rank on d.packing can loop, which the verifier rightly
      // reports, and packs have their own tests.
      const auto pack    = space.packDim();
      const auto packing = space.packingDim();
      ASSERT_TRUE(space.isCurrent(pack));
      ASSERT_NE(pack, packing);
      ASSERT_EQ(space.packDim(), pack);
      ASSERT_EQ(space.packingDim(), packing);
    } else if (op < 75) {
      const auto layer    = anyLayer();
      const auto from     = draw(layer, false);
      const auto dim      = draw(layer, true);
      const auto to       = draw(layer, false);
      const auto dir      = 0 == roll(2) ? DimVector::POS : DimVector::NEG;
      const auto expected = model.link(layer, from, dim, dir, to);
      const auto result   = space.link(layer, from, dim, dir, to);
      ++outcomes[expected];
      ASSERT_EQ(violationsOf(space), "") << "step " << step;
      if (expected.has_value()) {
        ASSERT_FALSE(result.has_value()) << "step " << step;
        ASSERT_EQ(result.error(), *expected) << "step " << step;
      } else {
        ASSERT_TRUE(result.has_value()) << "step " << step;
        ASSERT_EQ(space.linked(from, dim, dir), to) << "step " << step;
      }
    } else if (op < 92) {
      const auto layer    = anyLayer();
      const auto from     = draw(layer, false);
      const auto dim      = draw(layer, true);
      const auto dir      = 0 == roll(2) ? DimVector::POS : DimVector::NEG;
      const auto expected = model.unlink(layer, from, dim, dir);
      const auto result   = space.unlink(layer, from, dim, dir);
      ASSERT_EQ(violationsOf(space), "") << "step " << step;
      if (expected.has_value()) {
        ASSERT_FALSE(result.has_value()) << "step " << step;
        ASSERT_EQ(result.error(), *expected) << "step " << step;
      } else {
        ASSERT_TRUE(result.has_value()) << "step " << step;
        ASSERT_EQ(space.linked(from, dim, dir), std::nullopt)
            << "step " << step;
      }
    } else if (op < 95) {
      space.toss();
      model.toss();
      ASSERT_EQ(space.derivedCellCount(), 0U);
    } else {
      // Reads with any ref: a stale or foreign one is answered with nothing.
      const auto cell = draw(anyLayer(), false);
      if (model.refusal(cell.layer, cell).has_value()) {
        ASSERT_EQ(space.target(cell), std::nullopt) << "step " << step;
        ASSERT_FALSE(space.resolveReal(cell).has_value()) << "step " << step;
      } else if (const auto real = model.target(cell.layer, cell.ref)) {
        ASSERT_EQ(space.resolveReal(cell), *real) << "step " << step;
      }
    }
    ASSERT_EQ(violationsOf(space), "") << "step " << step;
    ASSERT_EQ(space.shadowCount(Layer::Binding), 0U);
    ASSERT_EQ(space.shadowCount(Layer::Derived), 0U);
    ASSERT_EQ(space.trailSize(Layer::Derived), 0U);
    if (0 == step % checkEvery) {
      expectAgrees(space, model);
    }
  }
  expectAgrees(space, model);
  for (const auto outcome :
       {std::optional<ViewError>{}, std::optional{ViewError::StaleEpoch},
        std::optional{ViewError::RealCellInViewLink},
        std::optional{ViewError::OccupiedDirection}}) {
    EXPECT_GT(outcomes[outcome], 0);
  }
}

INSTANTIATE_TEST_SUITE_P(Seeds, ViewManifoldRandomTest,
                         ::testing::Range(1U, 9U));

} // namespace
