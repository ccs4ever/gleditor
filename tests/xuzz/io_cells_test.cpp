#include <gtest/gtest.h>

#include <string_view>

#include "common/xanadu/vortex/io_cells.hpp"

namespace {

using xanadu::vortex::appendIoRequest;
using xanadu::vortex::IoDimension;
using xanadu::vortex::makeCapabilityCell;
using xanadu::vortex::makeEndpointCell;
using xanadu::vortex::makeIoCellSchema;
using zigzag::ArenaManifold;
using zigzag::DimVector;

TEST(IoCellsTest,
     sharedEndpointAndCapabilityHavePrivateViewsAndReciprocalQueue) {
  ArenaManifold arena;
  const auto schema = makeIoCellSchema(arena);
  ASSERT_TRUE(schema);
  const auto endpoint   = makeEndpointCell(arena);
  const auto capability = makeCapabilityCell(arena);
  ASSERT_TRUE(endpoint);
  ASSERT_TRUE(capability);

  const auto first = appendIoRequest(arena, *schema, *endpoint, *capability);
  ASSERT_TRUE(first);
  const auto second = appendIoRequest(arena, *schema, *endpoint, *capability);
  ASSERT_TRUE(second);

  const auto endpointDim   = schema->dim(IoDimension::Endpoint);
  const auto capabilityDim = schema->dim(IoDimension::Capability);
  const auto cloneDim      = schema->dim(IoDimension::Clone);
  const auto queueDim      = schema->dim(IoDimension::Queue);

  EXPECT_EQ(arena.linked(first->request.cell, endpointDim),
            first->endpointOccurrence.cell);
  EXPECT_EQ(arena.linked(first->endpointOccurrence.cell, endpointDim),
            first->endpointView.cell);
  EXPECT_EQ(arena.cloneMaster(first->endpointView.cell, cloneDim),
            endpoint->cell);
  EXPECT_EQ(arena.linked(second->request.cell, capabilityDim),
            second->capabilityOccurrence.cell);
  EXPECT_EQ(arena.linked(second->capabilityOccurrence.cell, capabilityDim),
            second->capabilityView.cell);
  EXPECT_EQ(arena.cloneMaster(second->capabilityView.cell, cloneDim),
            capability->cell);

  EXPECT_EQ(arena.linked(schema->queueHead.cell, queueDim),
            first->request.cell);
  EXPECT_EQ(arena.linked(first->request.cell, queueDim), second->request.cell);
  EXPECT_EQ(arena.linked(second->request.cell, queueDim, DimVector::NEG),
            first->request.cell);
  EXPECT_EQ(arena.linked(first->request.cell, queueDim, DimVector::NEG),
            schema->queueHead.cell);
  EXPECT_EQ(arena.linked(endpoint->cell, cloneDim), first->endpointView.cell);
  EXPECT_EQ(arena.linked(first->endpointView.cell, cloneDim),
            second->endpointView.cell);
}

TEST(IoCellsTest, invalidDeclarationLeavesArenaUntouched) {
  ArenaManifold arena;
  const auto schema = makeIoCellSchema(arena);
  ASSERT_TRUE(schema);
  const auto capability = makeCapabilityCell(arena);
  ASSERT_TRUE(capability);
  const auto before = arena.cellCount();

  const auto refused = appendIoRequest(arena, *schema, {}, *capability);
  EXPECT_FALSE(refused);
  EXPECT_EQ(arena.cellCount(), before);
}

TEST(IoCellsTest, staleSameIndexHandleCannotAdmitRequest) {
  ArenaManifold arena;
  const auto schema = makeIoCellSchema(arena);
  ASSERT_TRUE(schema);
  const auto capability = makeCapabilityCell(arena);
  ASSERT_TRUE(capability);

  const auto mark  = arena.mark();
  const auto stale = makeEndpointCell(arena);
  ASSERT_TRUE(stale);
  ASSERT_TRUE(arena.release(mark));

  const auto current = makeEndpointCell(arena);
  ASSERT_TRUE(current);
  ASSERT_EQ(current->cell, stale->cell);
  ASSERT_NE(current->incarnation, stale->incarnation);
  EXPECT_FALSE(appendIoRequest(arena, *schema, *stale, *capability));
  EXPECT_TRUE(appendIoRequest(arena, *schema, *current, *capability));
}

TEST(IoCellsTest, handleFromAnotherArenaCannotAdmitRequest) {
  ArenaManifold owner;
  ArenaManifold other;
  const auto schema = makeIoCellSchema(other);
  ASSERT_TRUE(schema);
  const auto endpoint   = makeEndpointCell(owner);
  const auto capability = makeCapabilityCell(other);
  ASSERT_TRUE(endpoint);
  ASSERT_TRUE(capability);

  EXPECT_FALSE(appendIoRequest(other, *schema, *endpoint, *capability));
}

TEST(IoCellsTest, cellTagsHaveStableClosedSpellings) {
  EXPECT_EQ(xanadu::vortex::toString(xanadu::vortex::IoCellTag::Request),
            std::string_view{"Request"});
  EXPECT_EQ(xanadu::vortex::toString(xanadu::vortex::IoOutcomeTag::Unknown),
            std::string_view{"Unknown"});
}

TEST(IoCellsTest, ordinaryTextCannotForgeRuntimeCellTag) {
  ArenaManifold arena;
  const auto textCell    = arena.makeCell("Endpoint");
  const auto incarnation = arena.incarnationOf(textCell);
  ASSERT_TRUE(incarnation);

  EXPECT_FALSE(xanadu::vortex::ioCellTag(arena, textCell, *incarnation));
  const auto schema     = makeIoCellSchema(arena);
  const auto capability = makeCapabilityCell(arena);
  ASSERT_TRUE(schema);
  ASSERT_TRUE(capability);
  const xanadu::vortex::EndpointCell forged{.cell        = textCell,
                                            .incarnation = *incarnation};
  EXPECT_FALSE(appendIoRequest(arena, *schema, forged, *capability));
}

} // namespace
