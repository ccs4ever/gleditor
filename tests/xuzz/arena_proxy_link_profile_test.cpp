/**
 * @file arena_proxy_link_profile_test.cpp
 * @brief Opt-in raw profile for local and foreign-proxy link mutations.
 *
 * Run explicitly with --gtest_also_run_disabled_tests
 * --gtest_filter='*DISABLED_ProxyLinkProfile'. Each printed case uses a fresh
 * store and arena; fixture construction is outside the timed interval. The
 * measurements make no speedup claim.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace zigzag {
namespace {

using Clock = std::chrono::steady_clock;
using xanadu::Store;
using xanadu::UserPermascroll;

enum class Mutation { Link, Unlink };
enum class NeighborProxy { Existing, Missing };

std::string_view mutationName(const Mutation mutation) {
  return mutation == Mutation::Link ? "link" : "unlink";
}

std::string_view proxyName(const NeighborProxy proxy) {
  return proxy == NeighborProxy::Existing ? "existing" : "missing";
}

struct SourceEdge {
  CellRef from;
  CellRef to;
};

struct Fixture {
  std::shared_ptr<Store> store{
      std::make_shared<Store>(std::make_shared<UserPermascroll>())};
  std::shared_ptr<Manifold> source;
  ArenaManifold arena;
  DimRef sourceDimension{noCell};
  DimRef arenaDimension{noCell};
  std::uint32_t space{0};
  std::vector<SourceEdge> edges;

  explicit Fixture(const std::size_t count) {
    auto version    = store->sliceGenesis({});
    const auto axis = store->makeDimension(version, "d.profile-edge");
    version         = axis.version;
    sourceDimension = axis.dim;
    edges.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
      version            = store->makeCell(version, "source from");
      const CellRef from = store->cellRefOf(version).value();
      version            = store->makeCell(version, "source to");
      const CellRef to   = store->cellRefOf(version).value();
      version =
          store->setLink(version, from, sourceDimension, DimVector::POS, to);
      edges.push_back({.from = from, .to = to});
    }

    source = std::make_shared<Manifold>(store->rebuildManifold(version));
    space  = arena.attach(Space{.manifold      = source.get(),
                                .ownedManifold = source,
                                .store         = store.get(),
                                .reader        = store.get(),
                                .label         = "proxy-link-profile"});
    arenaDimension = arena.ensureDimension("d.profile-arena-edge");
    arena.bindDimension(arenaDimension, space, sourceDimension,
                        DimensionBindingMode::Explicit);
  }
};

void runProfileCase(const std::size_t count, const bool local,
                    const Mutation mutation, const NeighborProxy neighborMode) {
  Fixture fixture(count);
  auto &arena            = fixture.arena;
  const DimRef dimension = fixture.arenaDimension;
  std::vector<CellRef> owners;
  std::vector<std::optional<CellRef>> originalNeighbors;
  std::vector<CellRef> replacementNeighbors;
  owners.reserve(count);
  originalNeighbors.reserve(count);
  replacementNeighbors.reserve(count);

  for (const auto &edge : fixture.edges) {
    if (local) {
      const CellRef owner    = arena.makeCell("local from");
      const CellRef neighbor = arena.makeCell("local to");
      zigzag::expectWritten(
          arena.link(owner, dimension, DimVector::POS, neighbor));
      owners.push_back(owner);
      originalNeighbors.push_back(neighbor);
    } else {
      const CellRef owner = arena.proxyFor(fixture.space, edge.from);
      owners.push_back(owner);
      if (neighborMode == NeighborProxy::Existing) {
        originalNeighbors.push_back(arena.proxyFor(fixture.space, edge.to));
      } else {
        originalNeighbors.push_back(std::nullopt);
      }
    }

    if (mutation == Mutation::Link) {
      replacementNeighbors.push_back(arena.makeCell("replacement target"));
    }
  }

  // Capture the authored topology before timing; profile setup and validation
  // are deliberately outside the measured interval.
  for (const auto &edge : fixture.edges) {
    ASSERT_EQ(fixture.source->linked(edge.from, fixture.sourceDimension,
                                     DimVector::POS),
              edge.to);
  }
  const std::size_t cellsBefore = arena.cellCount();
  ASSERT_EQ(arena.outstandingMarks(), 0U);

  const auto start = Clock::now();
  for (std::size_t index = 0; index < count; ++index) {
    if (mutation == Mutation::Link) {
      zigzag::expectWritten(arena.link(owners[index], dimension, DimVector::POS,
                                       replacementNeighbors[index]));
    } else {
      zigzag::expectWritten(
          arena.unlink(owners[index], dimension, DimVector::POS));
    }
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      Clock::now() - start);
  const std::size_t cellsAfter = arena.cellCount();

  for (std::size_t index = 0; index < count; ++index) {
    if (mutation == Mutation::Link) {
      const CellRef target = replacementNeighbors[index];
      EXPECT_EQ(arena.linked(owners[index], dimension, DimVector::POS), target);
      EXPECT_EQ(arena.linked(target, dimension, DimVector::NEG), owners[index]);
      if (local) {
        ASSERT_TRUE(originalNeighbors[index]);
        EXPECT_FALSE(
            arena.linked(*originalNeighbors[index], dimension, DimVector::NEG));
      }
    } else {
      EXPECT_FALSE(arena.linked(owners[index], dimension, DimVector::POS));
    }

    if (!local) {
      const auto &edge = fixture.edges[index];
      EXPECT_EQ(fixture.source->linked(edge.from, fixture.sourceDimension,
                                       DimVector::POS),
                edge.to);
      EXPECT_EQ(fixture.source->linked(edge.to, fixture.sourceDimension,
                                       DimVector::NEG),
                edge.from);
      const auto neighbor =
          !originalNeighbors[index]
              ? arena.findExistingProxy(fixture.space, edge.to)
              : originalNeighbors[index];
      ASSERT_TRUE(neighbor);
      const auto origin = arena.foreignOf(*neighbor);
      ASSERT_TRUE(origin);
      EXPECT_EQ(origin->space, fixture.space);
      EXPECT_EQ(origin->index, edge.to);
      EXPECT_FALSE(arena.linked(*neighbor, dimension, DimVector::NEG));
    } else if (mutation == Mutation::Unlink) {
      ASSERT_TRUE(originalNeighbors[index]);
      EXPECT_FALSE(
          arena.linked(*originalNeighbors[index], dimension, DimVector::NEG));
    }
  }
  EXPECT_EQ(arena.outstandingMarks(), 0U);

  std::cout << "ARENA_PROXY_LINK_PROFILE count=" << count
            << " kind=" << (local ? "local_fast" : "foreign_proxy")
            << " mutation=" << mutationName(mutation) << " neighbor_proxy="
            << (local ? "not_applicable" : proxyName(neighborMode))
            << " run_us=" << elapsed.count()
            << " arena_cell_delta=" << cellsAfter - cellsBefore << '\n';
}

TEST(ArenaProxyLinkProfileTest, DISABLED_ProxyLinkProfile) {
  for (const std::size_t count : {32U, 128U}) {
    for (const Mutation mutation : {Mutation::Link, Mutation::Unlink}) {
      runProfileCase(count, true, mutation, NeighborProxy::Missing);
      for (const NeighborProxy proxy :
           {NeighborProxy::Existing, NeighborProxy::Missing}) {
        runProfileCase(count, false, mutation, proxy);
      }
    }
  }
}

} // namespace
} // namespace zigzag
