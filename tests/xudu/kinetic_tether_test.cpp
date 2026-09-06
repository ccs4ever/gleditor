/**
 * @file kinetic_tether_test.cpp
 * @brief Unit tests for Stage 5 KineticTetherEngine, elastic spring physics, and void spawning.
 */
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "xudu/core/kinetic_tether.hpp"
#include "xudu/core/store.hpp"
#include "xudu/core/user_permascroll.hpp"

using namespace xudu;

class KineticTetherTest : public ::testing::Test {};

TEST_F(KineticTetherTest, DefaultStateIsIdle) {
  KineticTetherEngine tether;
  EXPECT_EQ(tether.state(), TetherState::Idle);
  EXPECT_FALSE(tether.isDragging());
  EXPECT_FALSE(tether.busy());
  EXPECT_FLOAT_EQ(tether.distance(), 0.0F);
}

TEST_F(KineticTetherTest, DetachmentThresholdCancelsUnder120Px) {
  KineticTetherEngine tether;
  TetherPayload payload{
      .span            = PrimediaSpan{0, 0, 14},
      .previewText     = "Project Xanadu",
      .originVersion   = MicroversionId{},
      .originDocIndex  = 0,
      .originCharStart = 0,
      .originCharEnd   = 14,
      .originScreenPos = glm::vec2(100.0F, 100.0F),
  };

  tether.startDrag(payload, 100.0F, 100.0F);
  EXPECT_TRUE(tether.isDragging());
  EXPECT_TRUE(tether.busy());

  // Drag 60px away (under 120px threshold)
  tether.updateDrag(160.0F, 100.0F);

  bool spawned = false;
  tether.setVoidSpawnHandler(
      [&spawned](const TetherPayload &, float, float) { spawned = true; });

  const bool triggered = tether.endDrag(160.0F, 100.0F);
  EXPECT_FALSE(triggered);
  EXPECT_FALSE(spawned);
  EXPECT_EQ(tether.state(), TetherState::SnappingBack);
  EXPECT_TRUE(tether.busy());
}

TEST_F(KineticTetherTest, DetachmentThresholdSucceedsOver120Px) {
  KineticTetherEngine tether;
  TetherPayload payload{
      .span            = PrimediaSpan{0, 0, 14},
      .previewText     = "Project Xanadu",
      .originVersion   = MicroversionId{},
      .originDocIndex  = 0,
      .originCharStart = 0,
      .originCharEnd   = 14,
      .originScreenPos = glm::vec2(100.0F, 100.0F),
  };

  tether.startDrag(payload, 100.0F, 100.0F);
  // Drag 250px away (well over 120px threshold)
  tether.updateDrag(350.0F, 100.0F);

  bool spawned = false;
  float outX   = 0.0F;
  float outY   = 0.0F;
  tether.setVoidSpawnHandler([&](const TetherPayload &p, float sx, float sy) {
    spawned = true;
    outX    = sx;
    outY    = sy;
    EXPECT_EQ(p.previewText, "Project Xanadu");
  });

  const bool triggered = tether.endDrag(350.0F, 100.0F);
  EXPECT_TRUE(triggered);
  EXPECT_TRUE(spawned);
  EXPECT_FLOAT_EQ(outX, 350.0F);
  EXPECT_FLOAT_EQ(outY, 100.0F);
  EXPECT_EQ(tether.state(), TetherState::Idle);
  EXPECT_FALSE(tether.busy());
}

TEST_F(KineticTetherTest, VoidReleaseSpawnsNewDocumentWithTranscludeOp) {
  auto scroll = std::make_shared<UserPermascroll>();
  Store store(scroll);

  // Insert initial text into source doc
  const auto v0 = store.insert(MicroversionId{}, 0,
                               "Project Xanadu Literary Machines");
  const auto initialPermascrollBytes = scroll->size();

  // Create tether payload from span [0, 14) ("Project Xanadu")
  const auto verPieces = store.rebuild(v0);
  const auto spans     = verPieces.spansFor(0, 14);
  ASSERT_FALSE(spans.empty());

  TetherPayload payload{
      .span            = spans.front(),
      .previewText     = "Project Xanadu",
      .originVersion   = v0,
      .originDocIndex  = 0,
      .originCharStart = 0,
      .originCharEnd   = 14,
      .originScreenPos = glm::vec2(200.0F, 400.0F),
  };

  // Perform void transclusion spawn: parent is Genesis (MicroversionId{})
  const auto spawnedVer = store.transclude(
      MicroversionId{}, 0, payload.originVersion, payload.originCharStart,
      payload.originCharEnd - payload.originCharStart);

  ASSERT_FALSE(spawnedVer.isZero());

  // Verify text of spawned document matches span exactly
  const auto spawnedText = store.textOf(spawnedVer);
  EXPECT_EQ(spawnedText, "Project Xanadu");

  // Verify zero raw byte duplication in permascroll
  EXPECT_EQ(scroll->size(), initialPermascrollBytes);

  // Verify that Version::occurrencesOf confirms address identity
  const auto spawnedPieces = store.rebuild(spawnedVer);
  const auto occs          = spawnedPieces.occurrencesOf(payload.span);
  ASSERT_FALSE(occs.empty());
  EXPECT_EQ(occs.front().start, 0U);
  EXPECT_EQ(occs.front().end, 14U);
}
