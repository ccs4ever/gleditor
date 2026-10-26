/**
 * @file vql_perspective_test.cpp
 * @brief Unit tests for VQL Perspectives and Transparent Fallback Rule.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using namespace xanadu::vql;
using zigzag::DimVector;

TEST(VQLPerspectiveTest, ConcurrentAnnotationsAndValencePreservation) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  zigzag::CellRef paragraph = arena.makeCell("Canonical paragraph text");
  engine.setVariable("p", paragraph);

  zigzag::DimRef dComment = engine.resolveDimension("d.comment");

  // 1. Author Alice's annotation under @alice:
  auto aliceSprouts =
      engine.execute("weave { @alice: $p/+d.comment => [ \"Alice note\" ] }");
  ASSERT_EQ(aliceSprouts.size(), 1u);
  EXPECT_EQ(arena.textOf(aliceSprouts[0]), "Alice note");

  // 2. Author Bob's annotation under @bob:
  auto bobSprouts =
      engine.execute("weave { @bob: $p/+d.comment => [ \"Bob note\" ] }");
  ASSERT_EQ(bobSprouts.size(), 1u);
  EXPECT_EQ(arena.textOf(bobSprouts[0]), "Bob note");

  // 3. Verify canonical paragraph's own +d.comment degree <= 1 (specifically 0
  // here)
  EXPECT_FALSE(
      arena.linkedMinting(paragraph, dComment, DimVector::POS).has_value());

  // 4. Verify reading through $p/@alice:+d.comment returns Alice's note
  auto readAlice = engine.execute("$p/@alice:+d.comment");
  ASSERT_EQ(readAlice.size(), 1u);
  EXPECT_EQ(arena.textOf(readAlice[0]), "Alice note");

  // 5. Verify reading through $p/@bob:+d.comment returns Bob's note
  auto readBob = engine.execute("$p/@bob:+d.comment");
  ASSERT_EQ(readBob.size(), 1u);
  EXPECT_EQ(arena.textOf(readBob[0]), "Bob note");

  // 6. Test with leading slash: /$p/@alice:+d.comment
  auto readAliceSlash = engine.execute("/$p/@alice:+d.comment");
  ASSERT_EQ(readAliceSlash.size(), 1u);
  EXPECT_EQ(arena.textOf(readAliceSlash[0]), "Alice note");
}

TEST(VQLPerspectiveTest, TransparentFallbackRule) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  zigzag::CellRef unannotated  = arena.makeCell("Unannotated master paragraph");
  zigzag::CellRef canonicalTag = arena.makeCell("general");
  zigzag::DimRef dTag          = engine.resolveDimension("d.tag");

  EXPECT_TRUE(arena.link(unannotated, dTag, DimVector::POS, canonicalTag));
  engine.setVariable("p", unannotated);

  // Reading through @alice:+d.tag when Alice has not modified d.tag
  // should transparently fall back to canonical master's d.tag link.
  auto fallbackRes = engine.execute("$p/@alice:+d.tag");
  ASSERT_EQ(fallbackRes.size(), 1u);
  EXPECT_EQ(fallbackRes[0], canonicalTag);
  EXPECT_EQ(arena.textOf(fallbackRes[0]), "general");
}

TEST(VQLPerspectiveTest, ScopedPerspectiveBlock) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  zigzag::CellRef paragraph = arena.makeCell("Target paragraph");
  engine.setVariable("p", paragraph);

  // Author Alice note via scoped perspective block
  engine.execute("with perspective(\"alice\") {\n"
                 "  $p/+d.comment => [ \"Scoped Alice note\" ]\n"
                 "}");

  // Outside perspective block, canonical has no comment
  auto readCanonical = engine.execute("$p/+d.comment");
  EXPECT_TRUE(readCanonical.empty());

  // Inside perspective block, $p/+d.comment resolves to Alice's note
  auto readScoped = engine.execute("with perspective(\"alice\") {\n"
                                   "  $p/+d.comment\n"
                                   "}");
  ASSERT_EQ(readScoped.size(), 1u);
  EXPECT_EQ(arena.textOf(readScoped[0]), "Scoped Alice note");
}

} // namespace
