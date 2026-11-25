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
#include <unordered_map>
#include <unordered_set>
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

  // Write-path support for the dirty_/rewind tests: canApply admits every
  // key; applyContentChange records the write and restates the node's
  // content so commit()'s trailing refresh() sees what was sent.
  bool writable{true};
  std::vector<std::pair<ExternalKey, std::string>> applied;
  // applyContentChange may report the item under a new key (a source
  // relocation): the node -- and every edge naming it -- is renamed so
  // the trailing refresh() emits it under the new name, exactly like a
  // real lens's unproject half would.
  std::unordered_map<ExternalKey, ExternalKey> renameOnApply;
  // Keys whose apply refuses: commit() must leave them -- and the
  // un-attempted tail behind the refusal -- dirty.
  std::unordered_set<ExternalKey> failApply;
  [[nodiscard]] bool canApply(const ExternalKey &) const override {
    return writable;
  }
  std::expected<ExternalKey, std::string>
  applyContentChange(const ExternalKey &key, std::string_view text) override {
    if (failApply.contains(key)) {
      return std::unexpected("lens refused " + key);
    }
    applied.emplace_back(key, std::string{text});
    for (auto &n : nodes) {
      if (n.key == key) {
        n.content = NodeContent{std::string{text}};
        if (const auto rename = renameOnApply.find(key);
            rename != renameOnApply.end()) {
          for (auto &e : edges) {
            if (e.from == key) e.from = rename->second;
            if (e.to == key) e.to = rename->second;
          }
          n.key = rename->second;
        }
      }
    }
    if (const auto rename = renameOnApply.find(key);
        rename != renameOnApply.end()) {
      return rename->second;
    }
    return key;
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
  EXPECT_EQ(f.session->refresh().value(), 0u); // nothing new minted

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

  EXPECT_EQ(f.session->refresh().value(), 1u);
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
  EXPECT_EQ(f.session->refresh().value(), 0u);
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

TEST(RewindTest, RewindRestoresDirtySetToCheckpointSnapshot) {
  Fixture f;
  f.start();
  // An edit pending at the boundary stays pending across the rewind.
  ASSERT_TRUE(f.session->editContent("B", "edited-at-cp"));
  const auto cp = f.session->checkpoint();
  EXPECT_EQ(f.session->dirtyCount(), 1U);

  // Edits after the boundary -- on a surviving cell (B, minted before cp)
  // and on a cell the toss removes (D) -- must leave dirty_ with the
  // rewind: otherwise a later commit() would push text the rewind already
  // undid back into the source.
  ASSERT_TRUE(f.session->editContent("B", "edited-after-cp"));
  ASSERT_TRUE(f.session->editContent("C", "gamma-edit"));
  f.lens->nodes.push_back(
      {.key = "D", .content = std::string{"delta"}, .attributes = {}});
  (void)f.session->refresh();
  ASSERT_TRUE(f.session->cellFor("D").has_value());
  ASSERT_TRUE(f.session->editContent("D", "delta-edit"));
  EXPECT_EQ(f.session->dirtyCount(), 3U);

  ASSERT_TRUE(f.session->rewind(cp).has_value());
  EXPECT_EQ(f.session->dirtyCount(), 1U);
  EXPECT_FALSE(f.session->cellFor("D").has_value());
  // The arena content reverted to the checkpoint-era edit on B, and to
  // the source text on C.
  EXPECT_EQ(f.core.arena().textOf(*f.session->cellFor("B")), "edited-at-cp");
  EXPECT_EQ(f.core.arena().textOf(*f.session->cellFor("C")), "gamma");

  // commit() sends exactly the checkpoint-era pending edit.
  const auto committed = f.session->commit();
  ASSERT_TRUE(committed.has_value()) << committed.error();
  EXPECT_EQ(*committed, 1);
  ASSERT_EQ(f.lens->applied.size(), 1U);
  EXPECT_EQ(f.lens->applied.front().first, "B");
  EXPECT_EQ(f.lens->applied.front().second, "edited-at-cp");
  EXPECT_EQ(f.session->dirtyCount(), 0U);
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

// -- commit() ordering, relocation and refusal -------------------------------

TEST(CommitTest, CommitAppliesDirtyKeysInSortedOrder) {
  Fixture f;
  f.start();
  for (const char *k : {"D", "E", "F", "G", "H"}) {
    f.lens->nodes.push_back(
        {.key = k, .content = std::string{k}, .attributes = {}});
  }
  (void)f.session->refresh();

  // Dirty the set in scrambled order; commit() must apply the edits in
  // sorted key order -- the lens stops at the first refusal, so hash
  // order would make which edits land nondeterministic.
  for (const char *k : {"H", "C", "A", "E", "B", "D", "G", "F"}) {
    ASSERT_TRUE(f.session->editContent(k, std::string{k} + "-edit"));
  }
  const auto committed = f.session->commit();
  ASSERT_TRUE(committed.has_value()) << committed.error();
  EXPECT_EQ(*committed, 8);
  ASSERT_EQ(f.lens->applied.size(), 8U);
  std::vector<std::string> appliedOrder;
  appliedOrder.reserve(f.lens->applied.size());
  for (const auto &[key, text] : f.lens->applied) {
    static_cast<void>(text);
    appliedOrder.push_back(key);
  }
  EXPECT_EQ(appliedOrder,
            (std::vector<std::string>{"A", "B", "C", "D", "E", "F", "G", "H"}));
}

TEST(CommitTest, RelocatedKeyRebindsCellAndKeepsMintOrderForRoots) {
  Fixture f;
  f.start();
  const auto cellA = *f.session->cellFor("A");
  ASSERT_EQ(f.session->roots().size(), 1U);
  EXPECT_EQ(f.session->roots()[0], cellA);

  // The source renames A -> A2 during the commit's unproject half: the
  // session must rebind the SAME cell to the new name AND rewrite the
  // mint-order log, or recomputeRoots() iterates a stale name that
  // presentKeys_ no longer contains and drops the root entirely.
  f.lens->renameOnApply["A"] = "A2";
  ASSERT_TRUE(f.session->editContent("A", "a-edit"));
  const auto committed = f.session->commit();
  ASSERT_TRUE(committed.has_value()) << committed.error();
  EXPECT_EQ(*committed, 1);

  EXPECT_FALSE(f.session->cellFor("A").has_value());
  ASSERT_TRUE(f.session->cellFor("A2").has_value());
  EXPECT_EQ(*f.session->cellFor("A2"), cellA);
  ASSERT_TRUE(f.session->keyFor(cellA).has_value());
  EXPECT_EQ(*f.session->keyFor(cellA), "A2");

  // The relocated node is still the structural root: mintOrder_ was
  // rewritten in place, so recomputeRoots sees "A2" where "A" was.
  ASSERT_EQ(f.session->roots().size(), 1U);
  EXPECT_EQ(f.session->roots()[0], cellA);
}

TEST(CommitTest, RelocatedKeyCollidingWithBoundKeyKeepsBothBindings) {
  Fixture f;
  f.start();
  // An edge-free node so the renamed A->D edge (the old A->B contains
  // edge) cannot self-loop during the trailing refresh().
  f.lens->nodes.push_back(
      {.key = "D", .content = std::string{"delta"}, .attributes = {}});
  (void)f.session->refresh();
  const auto cellA = *f.session->cellFor("A");
  const auto cellD = *f.session->cellFor("D");

  // The source renames A onto D's already-bound name: a collision the
  // rebind must refuse. Unguarded, keyToCell_.emplace would keep D->cellD
  // while cellToKey_ and mintOrder_ claimed D for cellA too -- the maps
  // and recomputeRoots() would then disagree (and elect cellD twice).
  f.lens->renameOnApply["A"] = "D";
  ASSERT_TRUE(f.session->editContent("A", "a-edit"));
  const auto committed = f.session->commit();
  ASSERT_TRUE(committed.has_value()) << committed.error();
  EXPECT_EQ(*committed, 1);

  // The rebind was skipped: A's cell stays bound to A (the source no
  // longer emits it, so it goes stale), and D's binding is untouched.
  ASSERT_TRUE(f.session->cellFor("A").has_value());
  EXPECT_EQ(*f.session->cellFor("A"), cellA);
  ASSERT_TRUE(f.session->keyFor(cellA).has_value());
  EXPECT_EQ(*f.session->keyFor(cellA), "A");
  ASSERT_TRUE(f.session->cellFor("D").has_value());
  EXPECT_EQ(*f.session->cellFor("D"), cellD);
  EXPECT_EQ(*f.session->keyFor(cellD), "D");

  // mintOrder_ kept its single "D" entry: the root election names
  // cellD once, not twice.
  ASSERT_EQ(f.session->roots().size(), 1U);
  EXPECT_EQ(f.session->roots()[0], cellD);
}

TEST(CommitTest, CommitRefusalLeavesFailedAndUnattemptedKeysDirty) {
  Fixture f;
  f.start();
  ASSERT_TRUE(f.session->editContent("A", "a-edit"));
  ASSERT_TRUE(f.session->editContent("B", "b-edit"));
  ASSERT_TRUE(f.session->editContent("C", "c-edit"));
  f.lens->failApply.insert("B");

  const auto committed = f.session->commit();
  ASSERT_FALSE(committed.has_value());
  EXPECT_NE(committed.error().find("B"), std::string::npos);
  // Sorted order A,B,C: A applied and left dirty_, B refused, C was
  // never attempted -- the guard reinserts exactly {B,C}.
  EXPECT_EQ(f.session->dirtyCount(), 2U);
  ASSERT_EQ(f.lens->applied.size(), 1U);
  EXPECT_EQ(f.lens->applied.front().first, "A");

  // A retry with the refusal lifted applies the two survivors -- proof
  // the failed key and the un-attempted tail really stayed pending.
  f.lens->failApply.clear();
  const auto retried = f.session->commit();
  ASSERT_TRUE(retried.has_value()) << retried.error();
  EXPECT_EQ(*retried, 2);
  EXPECT_EQ(f.session->dirtyCount(), 0U);
}

} // namespace
