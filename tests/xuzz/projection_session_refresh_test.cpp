/**
 * @file projection_session_refresh_test.cpp
 * @brief Tests for the phase-2 session verbs: refresh() union-identity
 * re-projection, the live-run tombstone, staleAttributes, checkpoint/
 * rewind/pin with map repair, WindowedLens discard, and lens-declared roots
 * (design/sql-to-zigzag.md §5.2/§5.5).
 */
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/projection_lens.hpp"
#include "common/xanadu/zigzag/projection_session.hpp"

namespace {

using namespace zigzag;

/// A mutable in-memory lens: its node/edge lists are the "source," mutated
/// between refresh() calls.
class MockLens final : public ProjectionLens, public WindowedLens {
public:
  std::vector<SourceNode> nodes;
  std::vector<SourceEdge> edges;
  std::vector<std::pair<std::string, std::string>> staleAttrs;
  std::vector<ExternalKey> declaredRoots;
  bool useDeclaredRoots{false};
  // WindowedLens observability
  int discardCalls{0};
  ExternalKey lastBoundary;
  int materializeCalls{0};

  [[nodiscard]] std::vector<std::pair<std::string, std::optional<GenericDim>>>
  dimensions() const override {
    return {{"d.contains", GenericDim::Containment},
            {"d.seq", GenericDim::Sequence},
            {"d.role", std::nullopt},
            {"d.state", std::nullopt},
            {"d.membership", std::nullopt}};
  }

  void forEachNode(gleditor::cpp26::function_ref<void(const SourceNode &)> emit)
      const override {
    for (const auto &n : nodes) emit(n);
  }
  void forEachEdge(gleditor::cpp26::function_ref<void(const SourceEdge &)> emit)
      const override {
    for (const auto &e : edges) emit(e);
  }
  [[nodiscard]] std::vector<std::pair<std::string, std::string>>
  staleAttributes(const ExternalKey & /*key*/) const override {
    return staleAttrs;
  }
  bool forEachRoot(gleditor::cpp26::function_ref<void(const ExternalKey &)>
                       emit) const override {
    if (!useDeclaredRoots) return false;
    for (const auto &k : declaredRoots) emit(k);
    return true;
  }

  std::expected<void, std::string>
  materializePage(const ExternalKey & /*k*/) override {
    ++materializeCalls;
    return {};
  }
  void discardMaterializedAfter(const ExternalKey &k) override {
    ++discardCalls;
    lastBoundary = k;
  }
};

struct Fixture {
  ArenaManifold arena;
  vortex::VortexCore core{arena};
  MockLens *lens{nullptr};
  std::unique_ptr<ProjectionSession> session;

  void start() {
    auto l  = std::make_unique<MockLens>();
    lens    = l.get();
    *lens   = initialLens();
    session = std::make_unique<ProjectionSession>(std::move(l), core);
  }

  static MockLens initialLens() {
    MockLens l;
    l.nodes = {
        {.key        = "A",
         .content    = std::string{"alpha"},
         .attributes = {{"d.role", "root"}}},
        {.key        = "B",
         .content    = std::string{"beta"},
         .attributes = {{"d.role", "kid"}}},
        {.key        = "C",
         .content    = std::string{"gamma"},
         .attributes = {{"d.role", "kid"}}},
    };
    l.edges = {
        {.from = "A", .to = "B", .dimension = "d.contains"},
        {.from = "B", .to = "C", .dimension = "d.seq"},
    };
    l.staleAttrs = {{"d.state", "stale"}};
    return l;
  }

  DimRef dim(std::string_view name) { return core.findOrMintDimension(name); }
};

TEST(RefreshTest, SameKeyKeepsCellAndRestatesContent) {
  Fixture f;
  f.start();
  const auto before = f.session->cellFor("B");
  ASSERT_TRUE(before.has_value());
  EXPECT_EQ(f.core.arena().textOf(*before), "beta");

  f.lens->nodes[1].content = std::string{"beta-v2"};
  EXPECT_EQ(f.session->refresh(), 0u); // nothing new minted

  const auto after = f.session->cellFor("B");
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(*after, *before); // union identity
  EXPECT_EQ(f.core.arena().textOf(*after), "beta-v2");
}

TEST(RefreshTest, NewKeyMintsAbsentKeyStales) {
  Fixture f;
  f.start();
  // Drop B, add D: B's cell persists and gets stale attrs; D mints fresh.
  f.lens->nodes.erase(
      std::remove_if(f.lens->nodes.begin(), f.lens->nodes.end(),
                     [](const SourceNode &n) { return n.key == "B"; }),
      f.lens->nodes.end());
  f.lens->edges.erase(std::remove_if(f.lens->edges.begin(), f.lens->edges.end(),
                                     [](const SourceEdge &e) {
                                       return e.from == "B" || e.to == "B";
                                     }),
                      f.lens->edges.end());
  f.lens->nodes.push_back(
      {.key = "D", .content = std::string{"delta"}, .attributes = {}});

  EXPECT_EQ(f.session->refresh(), 1u);
  EXPECT_TRUE(f.session->cellFor("D").has_value());
  const auto b = f.session->cellFor("B");
  ASSERT_TRUE(b.has_value());
  const auto staleDim = f.dim("d.state");
  const auto stale    = f.core.arena().linked(*b, staleDim, Posward);
  ASSERT_TRUE(stale.has_value());
  EXPECT_EQ(f.core.arena().textOf(*stale), "stale");
}

TEST(RefreshTest, TombstoneSeversRemovedEdgesAndEmptiesRank) {
  Fixture f;
  f.start();
  const auto a        = *f.session->cellFor("A");
  const auto c        = *f.session->cellFor("C");
  const auto contains = f.dim("d.contains");
  const auto seq      = f.dim("d.seq");
  ASSERT_TRUE(f.core.arena().linked(a, contains, Posward).has_value());
  ASSERT_TRUE(f.core.arena().linked(c, seq, Posward).has_value() ||
              f.core.arena().linked(c, seq, DimVector::NEG).has_value());

  // Remove every edge: A's firstborn is severed, B's sibling link dies.
  f.lens->edges.clear();
  EXPECT_EQ(f.session->refresh(), 0u);
  EXPECT_FALSE(f.core.arena().linked(a, contains, Posward).has_value());
  EXPECT_FALSE(f.core.arena().linked(c, seq, DimVector::NEG).has_value());
  // Every node now floats -> every node is a structural root.
  EXPECT_EQ(f.session->roots().size(), 3u);
}

TEST(RefreshTest, AttributeRewriteReplacesValue) {
  Fixture f;
  f.start();
  const auto roleDim = f.dim("d.role");
  const auto b       = *f.session->cellFor("B");
  EXPECT_EQ(f.core.arena().textOf(*f.core.arena().linked(b, roleDim, Posward)),
            "kid");
  f.lens->nodes[1].attributes = {{"d.role", "grown"}};
  (void)f.session->refresh();
  EXPECT_EQ(f.core.arena().textOf(*f.core.arena().linked(b, roleDim, Posward)),
            "grown");
}

TEST(RefreshTest, RemovedAttributeIsUnlinked) {
  Fixture f;
  f.start();
  const auto roleDim          = f.dim("d.role");
  const auto b                = *f.session->cellFor("B");
  f.lens->nodes[1].attributes = {};
  (void)f.session->refresh();
  EXPECT_FALSE(f.core.arena().linked(b, roleDim, Posward).has_value());
}

TEST(RewindTest, RewindForgetsKeysAndRevertsAttrAndTrail) {
  Fixture f;
  f.start();
  const auto cp      = f.session->checkpoint();
  const auto roleDim = f.dim("d.role");
  const auto b       = *f.session->cellFor("B");

  // Grow the projection past the boundary.
  f.lens->nodes.push_back(
      {.key = "D", .content = std::string{"delta"}, .attributes = {}});
  f.lens->nodes[1].attributes = {{"d.role", "changed"}};
  (void)f.session->refresh();
  ASSERT_TRUE(f.session->cellFor("D").has_value());
  ASSERT_EQ(f.core.arena().textOf(*f.core.arena().linked(b, roleDim, Posward)),
            "changed");

  ASSERT_TRUE(f.session->rewind(cp).has_value());
  EXPECT_FALSE(f.session->cellFor("D").has_value());
  // The attr rewrite was trailed: it reverts to the pre-checkpoint value.
  const auto cur = f.core.arena().linked(b, roleDim, Posward);
  ASSERT_TRUE(cur.has_value());
  EXPECT_EQ(f.core.arena().textOf(*cur), "kid");
  // roots_ trimmed to the boundary.
  EXPECT_EQ(f.session->roots().size(), 1u);
}

TEST(RewindTest, PinnedKeyRefusesRewind) {
  Fixture f;
  f.start();
  const auto cp = f.session->checkpoint();
  f.lens->nodes.push_back(
      {.key = "D", .content = std::string{"delta"}, .attributes = {}});
  (void)f.session->refresh();
  f.session->pin("D");
  const auto r = f.session->rewind(cp);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().find("D"), std::string::npos);
  // Unpin and the same rewind goes through.
  f.session->unpin("D");
  EXPECT_TRUE(f.session->rewind(cp).has_value());
}

TEST(RewindTest, ForeignMintRefusesRewind) {
  Fixture f;
  f.start();
  const auto cp = f.session->checkpoint();
  (void)f.core.arena().makeCell("a solver's answer cell");
  const auto r = f.session->rewind(cp);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().find("foreign mint"), std::string::npos);
}

TEST(RewindTest, WindowedDiscardDropsBufferAtBoundary) {
  Fixture f;
  f.start();
  const auto cp = f.session->checkpoint();
  f.lens->nodes.push_back(
      {.key = "D", .content = std::string{"delta"}, .attributes = {}});
  (void)f.session->refresh();
  ASSERT_TRUE(f.session->rewind(cp).has_value());
  EXPECT_EQ(f.lens->discardCalls, 1);
  EXPECT_EQ(f.lens->lastBoundary, "D"); // earliest key above the boundary
}

TEST(RewindTest, BoundaryKeyIsEarliestMintedAboveCp) {
  Fixture f;
  f.start();
  const auto cp = f.session->checkpoint();
  f.lens->nodes.push_back(
      {.key = "D", .content = std::string{"delta"}, .attributes = {}});
  f.lens->nodes.push_back(
      {.key = "E", .content = std::string{"epsilon"}, .attributes = {}});
  (void)f.session->refresh();
  ASSERT_TRUE(f.session->rewind(cp).has_value());
  EXPECT_EQ(f.lens->lastBoundary, "D");
}

TEST(RootsTest, StructuralDefaultAndLensDeclared) {
  Fixture f;
  f.start();
  // Structural default: only A (nothing points into it).
  ASSERT_EQ(f.session->roots().size(), 1u);
  const auto a = *f.session->cellFor("A");
  EXPECT_EQ(f.session->roots()[0], a);

  // Lens-declared: election is wholesale -- the lens names B and D.
  f.lens->useDeclaredRoots = true;
  f.lens->declaredRoots    = {"B", "D"};
  f.lens->nodes.push_back(
      {.key = "D", .content = std::string{"delta"}, .attributes = {}});
  (void)f.session->refresh();
  ASSERT_EQ(f.session->roots().size(), 2u);
  EXPECT_EQ(f.session->roots()[0], *f.session->cellFor("B"));
  EXPECT_EQ(f.session->roots()[1], *f.session->cellFor("D"));
}

TEST(RootsTest, DeclaredRootsOnlyEnumeratePresentKeys) {
  Fixture f;
  f.start();
  f.lens->useDeclaredRoots = true;
  f.lens->declaredRoots    = {"B", "ZZ-missing"};
  (void)f.session->refresh();
  // The absent key's root election resolves to nothing -- no ghost root.
  EXPECT_EQ(f.session->roots().size(), 1u);
}

TEST(RefreshTest, StaleCellsNotInRoots) {
  Fixture f;
  f.start();
  f.lens->nodes.clear();
  f.lens->edges.clear();
  (void)f.session->refresh();
  // Everything stale: present set is empty -> no roots.
  EXPECT_TRUE(f.session->roots().empty());
}

} // namespace
