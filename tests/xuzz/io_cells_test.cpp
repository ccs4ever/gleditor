#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/vortex/io_cells.hpp"
#include "common/xanadu/vortex/payload_resolver.hpp"

namespace {

using namespace xanadu::vortex;
using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimVector;

struct Topology {
  ArenaManifold arena;
  IoCellSchema schema;
  EndpointCell endpoint;
  CapabilityCell capability;

  Topology() {
    schema     = makeIoCellSchema(arena).value();
    endpoint   = makeEndpointCell(arena, schema).value();
    capability = makeCapabilityCell(arena, schema, endpoint).value();
  }

  [[nodiscard]] zigzag::DimRef dim(const IoDimension which) const {
    return schema.dim(which);
  }
  [[nodiscard]] std::optional<CellRef> pos(const CellRef from,
                                           const IoDimension which) const {
    return arena.linked(from, dim(which), DimVector::POS);
  }
  [[nodiscard]] std::optional<CellRef> neg(const CellRef from,
                                           const IoDimension which) const {
    return arena.linked(from, dim(which), DimVector::NEG);
  }
};

TEST(IoCellsTest, schemaDimensionsAreMintedOnceThroughTheRegistry) {
  ArenaManifold arena;
  const auto first = makeIoCellSchema(arena);
  ASSERT_TRUE(first);
  const auto cells  = arena.cellCount();
  const auto second = makeIoCellSchema(arena);
  ASSERT_TRUE(second);
  EXPECT_EQ(arena.cellCount(), cells);
  for (std::size_t i = 0; i < ioDimensionCount; ++i) {
    const auto which = static_cast<IoDimension>(i);
    EXPECT_EQ(first->dim(which), second->dim(which));
    EXPECT_EQ(arena.dimensionNamed(dimensionName(which)), first->dim(which));
    for (std::size_t j = 0; j < i; ++j) {
      EXPECT_NE(first->dim(which), first->dim(static_cast<IoDimension>(j)));
    }
  }
  EXPECT_EQ(dimensionName(IoDimension::Queue), "d.io.queue");
  EXPECT_EQ(dimensionName(IoDimension::Lease), "d.io.lease");
}

TEST(IoCellsTest,
     twoRequestsShareEndpointCapabilityAndQueueAndEveryLinkTraverses) {
  Topology t;
  const auto first =
      appendIoRequest(t.arena, t.schema, t.endpoint, t.capability);
  ASSERT_TRUE(first);
  const auto firstSnapshot = *first;
  const auto second =
      appendIoRequest(t.arena, t.schema, t.endpoint, t.capability);
  ASSERT_TRUE(second);

  // Queue: a reciprocal rank of request cells behind the endpoint's queue.
  const auto queue = endpointQueue(t.arena, t.schema, t.endpoint);
  ASSERT_TRUE(queue);
  EXPECT_EQ(t.neg(queue->cell, IoDimension::Occurrence), t.endpoint.cell);
  EXPECT_EQ(t.pos(queue->cell, IoDimension::Queue), first->request.cell);
  EXPECT_EQ(t.pos(first->request.cell, IoDimension::Queue),
            second->request.cell);
  EXPECT_FALSE(t.pos(second->request.cell, IoDimension::Queue));
  EXPECT_EQ(t.neg(second->request.cell, IoDimension::Queue),
            first->request.cell);
  EXPECT_EQ(t.neg(first->request.cell, IoDimension::Queue), queue->cell);
  const auto queued = queuedRequests(t.arena, t.schema, t.endpoint);
  ASSERT_TRUE(queued);
  EXPECT_EQ(*queued,
            (std::vector<RequestCell>{first->request, second->request}));

  // Each request field: request -> private occurrence -> clone view, both
  // ways, and the view's clone master is the shared canonical cell.
  for (const auto &graph : {*first, *second}) {
    EXPECT_EQ(t.pos(graph.request.cell, IoDimension::Endpoint),
              graph.endpointOccurrence.cell);
    EXPECT_EQ(t.pos(graph.endpointOccurrence.cell, IoDimension::Endpoint),
              graph.endpointView.cell);
    EXPECT_EQ(t.neg(graph.endpointView.cell, IoDimension::Endpoint),
              graph.endpointOccurrence.cell);
    EXPECT_EQ(t.neg(graph.endpointOccurrence.cell, IoDimension::Endpoint),
              graph.request.cell);
    EXPECT_EQ(t.pos(graph.request.cell, IoDimension::Capability),
              graph.capabilityOccurrence.cell);
    EXPECT_EQ(t.pos(graph.capabilityOccurrence.cell, IoDimension::Capability),
              graph.capabilityView.cell);
    EXPECT_EQ(t.neg(graph.capabilityView.cell, IoDimension::Capability),
              graph.capabilityOccurrence.cell);
    EXPECT_EQ(t.neg(graph.capabilityOccurrence.cell, IoDimension::Capability),
              graph.request.cell);
    EXPECT_EQ(
        t.arena.cloneMaster(graph.endpointView.cell, t.dim(IoDimension::Clone)),
        t.endpoint.cell);
    EXPECT_EQ(t.arena.cloneMaster(graph.capabilityView.cell,
                                  t.dim(IoDimension::Clone)),
              t.capability.cell);
    EXPECT_EQ(requestEndpoint(t.arena, t.schema, graph.request), t.endpoint);
    EXPECT_EQ(requestCapability(t.arena, t.schema, graph.request),
              t.capability);
  }

  // Clone ranks: canonical first, then each view in admission order, both
  // ways. The capability's own endpoint field is the first endpoint view.
  const auto capabilityEndpointOccurrence =
      t.pos(t.capability.cell, IoDimension::Endpoint);
  ASSERT_TRUE(capabilityEndpointOccurrence);
  const auto capabilityEndpointView =
      t.pos(*capabilityEndpointOccurrence, IoDimension::Endpoint);
  ASSERT_TRUE(capabilityEndpointView);
  const std::array endpointRank{t.endpoint.cell, *capabilityEndpointView,
                                first->endpointView.cell,
                                second->endpointView.cell};
  const std::array capabilityRank{t.capability.cell, first->capabilityView.cell,
                                  second->capabilityView.cell};
  const auto walk = [&](const auto &rank) {
    for (std::size_t i = 0; i + 1 < rank.size(); ++i) {
      EXPECT_EQ(t.pos(rank[i], IoDimension::Clone), rank[i + 1]);
      EXPECT_EQ(t.neg(rank[i + 1], IoDimension::Clone), rank[i]);
    }
    EXPECT_FALSE(t.neg(rank.front(), IoDimension::Clone));
    EXPECT_FALSE(t.pos(rank.back(), IoDimension::Clone));
  };
  walk(endpointRank);
  walk(capabilityRank);

  // No fan-in: the canonical cells are reached only through clone views.
  EXPECT_FALSE(t.neg(t.endpoint.cell, IoDimension::Endpoint));
  EXPECT_FALSE(t.neg(t.capability.cell, IoDimension::Capability));
  EXPECT_FALSE(t.neg(t.endpoint.cell, IoDimension::Queue));

  // Appending the second request overwrote nothing of the first.
  EXPECT_EQ(first->request, firstSnapshot.request);
  EXPECT_EQ(t.pos(firstSnapshot.request.cell, IoDimension::Endpoint),
            firstSnapshot.endpointOccurrence.cell);
  EXPECT_EQ(
      t.pos(firstSnapshot.capabilityOccurrence.cell, IoDimension::Capability),
      firstSnapshot.capabilityView.cell);
}

TEST(IoCellsTest, capabilityOfAnotherEndpointIsRefusedUntouched) {
  Topology t;
  const auto other = makeEndpointCell(t.arena, t.schema);
  ASSERT_TRUE(other);
  const auto before  = t.arena.cellCount();
  const auto refused = appendIoRequest(t.arena, t.schema, *other, t.capability);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), IoCellError::InvalidCapability);
  EXPECT_EQ(t.arena.cellCount(), before);
  EXPECT_TRUE(queuedRequests(t.arena, t.schema, *other)->empty());
}

TEST(IoCellsTest, invalidHandleLeavesArenaUntouched) {
  Topology t;
  const auto before  = t.arena.cellCount();
  const auto refused = appendIoRequest(t.arena, t.schema, {}, t.capability);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), IoCellError::StaleCell);
  EXPECT_EQ(t.arena.cellCount(), before);
}

TEST(IoCellsTest, staleSameIndexHandleCannotAdmitRequest) {
  Topology t;
  const auto mark  = t.arena.mark();
  const auto stale = makeEndpointCell(t.arena, t.schema);
  ASSERT_TRUE(stale);
  ASSERT_TRUE(t.arena.release(mark));

  const auto current = makeEndpointCell(t.arena, t.schema);
  ASSERT_TRUE(current);
  ASSERT_EQ(current->cell, stale->cell);
  ASSERT_NE(current->incarnation, stale->incarnation);
  const auto capability = makeCapabilityCell(t.arena, t.schema, *current);
  ASSERT_TRUE(capability);
  const auto refused = appendIoRequest(t.arena, t.schema, *stale, *capability);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), IoCellError::StaleCell);
  EXPECT_TRUE(appendIoRequest(t.arena, t.schema, *current, *capability));
}

TEST(IoCellsTest, handleFromAnotherArenaCannotAdmitRequest) {
  Topology owner;
  Topology other;
  EXPECT_FALSE(appendIoRequest(other.arena, other.schema, owner.endpoint,
                               other.capability));
}

TEST(IoCellsTest, cellTagsHaveStableClosedSpellings) {
  EXPECT_EQ(toString(IoCellTag::Request), std::string_view{"Request"});
  EXPECT_EQ(toString(IoCellTag::Waiter), std::string_view{"Waiter"});
  EXPECT_EQ(toString(IoOutcomeTag::Rejected), std::string_view{"Rejected"});
  EXPECT_EQ(toString(IoOutcomeTag::Unknown), std::string_view{"Unknown"});
}

TEST(IoCellsTest, ordinaryTextCannotForgeRuntimeCellTag) {
  Topology t;
  const auto textCell    = t.arena.makeCell("Endpoint");
  const auto incarnation = t.arena.incarnationOf(textCell);
  ASSERT_TRUE(incarnation);
  EXPECT_EQ(ioCellTag(t.arena, textCell, *incarnation).error(),
            IoCellError::WrongTag);
  const EndpointCell forged{.cell = textCell, .incarnation = *incarnation};
  EXPECT_EQ(makeCapabilityCell(t.arena, t.schema, forged).error(),
            IoCellError::WrongTag);
  // A capability handle cannot pass as an endpoint either.
  const EndpointCell misTyped{.cell        = t.capability.cell,
                              .incarnation = t.capability.incarnation};
  EXPECT_EQ(makeCapabilityCell(t.arena, t.schema, misTyped).error(),
            IoCellError::WrongTag);
}

TEST(IoCellsTest, outcomeTagIsTypedContentAndTerminalOnce) {
  Topology t;
  const auto request =
      appendIoRequest(t.arena, t.schema, t.endpoint, t.capability);
  ASSERT_TRUE(request);
  EXPECT_EQ(
      ioCellTag(t.arena, request->request.cell, request->request.incarnation),
      IoCellTag::Request);

  const auto complete = makeOutcomeCell(t.arena, t.schema, request->request,
                                        IoOutcomeTag::Complete);
  ASSERT_TRUE(complete);
  EXPECT_EQ(ioOutcomeTag(t.arena, *complete), IoOutcomeTag::Complete);
  EXPECT_EQ(t.pos(request->request.cell, IoDimension::Outcome), complete->cell);
  EXPECT_EQ(
      makeOutcomeCell(t.arena, t.schema, request->request, IoOutcomeTag::Failed)
          .error(),
      IoCellError::AlreadyTerminal);

  const auto second =
      appendIoRequest(t.arena, t.schema, t.endpoint, t.capability);
  ASSERT_TRUE(second);
  EXPECT_EQ(makeOutcomeCell(t.arena, t.schema, second->request,
                            IoOutcomeTag::Rejected)
                .error(),
            IoCellError::InvalidOutcome);
  EXPECT_EQ(
      makeOutcomeCell(t.arena, t.schema, std::nullopt, IoOutcomeTag::Complete)
          .error(),
      IoCellError::InvalidOutcome);
  const auto rejected =
      makeOutcomeCell(t.arena, t.schema, std::nullopt, IoOutcomeTag::Rejected);
  ASSERT_TRUE(rejected);
  EXPECT_EQ(ioOutcomeTag(t.arena, *rejected), IoOutcomeTag::Rejected);

  // A request tag carries no outcome; asking for one is a typed refusal.
  const OutcomeCell notAnOutcome{.cell        = request->request.cell,
                                 .incarnation = request->request.incarnation};
  EXPECT_EQ(ioOutcomeTag(t.arena, notAnOutcome).error(), IoCellError::WrongTag);
}

TEST(IoCellsTest, transientIoGraphRefusesSerializationByName) {
  xanadu::Store store;
  const auto parent = store.sliceGenesis(xanadu::MicroversionId{});
  Topology t;
  const auto mark = t.arena.mark();
  const auto request =
      appendIoRequest(t.arena, t.schema, t.endpoint, t.capability);
  ASSERT_TRUE(request);
  const auto outcome = makeOutcomeCell(t.arena, t.schema, request->request,
                                       IoOutcomeTag::Complete);
  ASSERT_TRUE(outcome);
  const auto ops = store.opCount();

  for (const CellRef root : {request->request.cell, t.endpoint.cell,
                             t.capability.cell, outcome->cell}) {
    const auto refused = zigzag::promoteOrRefuse(store, parent, t.arena, root);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error(), zigzag::PromotionRefusal::RuntimeOnlyState);
  }
  // An ordinary cell that merely links to I/O state is refused the same way:
  // its reachable subgraph would carry the request into Structure.
  const auto note = t.arena.makeCell("note");
  ASSERT_TRUE(t.arena.link(note, t.arena.ensureDimension("d.note"),
                           DimVector::POS, request->request.cell));
  EXPECT_EQ(zigzag::promoteOrRefuse(store, parent, t.arena, note).error(),
            zigzag::PromotionRefusal::RuntimeOnlyState);
  EXPECT_EQ(store.opCount(), ops);
  EXPECT_EQ(t.arena.exportPrefix(mark).error(),
            zigzag::ArenaRefusal::RuntimeOnlyState);
}

TEST(IoCellsTest, exceptionMidRequestLeavesNoPartialGraph) {
  Topology t;
  const auto before = t.arena.cellCount();
  const auto limit  = t.arena.allocationLimit();
  // Five cells make a request; the third allocation throws.
  t.arena.setAllocationLimitForTesting(static_cast<std::uint32_t>(before + 2));
  EXPECT_THROW(static_cast<void>(appendIoRequest(t.arena, t.schema, t.endpoint,
                                                 t.capability)),
               std::runtime_error);
  t.arena.setAllocationLimitForTesting(limit);

  EXPECT_EQ(t.arena.cellCount(), before);
  EXPECT_EQ(t.arena.outstandingMarks(), 0U);
  EXPECT_TRUE(queuedRequests(t.arena, t.schema, t.endpoint)->empty());
  EXPECT_FALSE(t.pos(t.capability.cell, IoDimension::Capability));

  const auto request =
      appendIoRequest(t.arena, t.schema, t.endpoint, t.capability);
  ASSERT_TRUE(request);
  EXPECT_EQ(*queuedRequests(t.arena, t.schema, t.endpoint),
            std::vector<RequestCell>{request->request});
}

TEST(IoCellsTest, markReleaseDropsRequestAndReusedRefIsStale) {
  Topology t;
  const auto kept =
      appendIoRequest(t.arena, t.schema, t.endpoint, t.capability);
  ASSERT_TRUE(kept);

  const auto mark = t.arena.mark();
  const auto dropped =
      appendIoRequest(t.arena, t.schema, t.endpoint, t.capability);
  ASSERT_TRUE(dropped);
  ASSERT_TRUE(t.arena.release(mark));

  EXPECT_EQ(*queuedRequests(t.arena, t.schema, t.endpoint),
            std::vector<RequestCell>{kept->request});
  EXPECT_FALSE(t.pos(kept->endpointView.cell, IoDimension::Clone));
  EXPECT_EQ(requestEndpoint(t.arena, t.schema, dropped->request).error(),
            IoCellError::StaleCell);

  const auto reused =
      appendIoRequest(t.arena, t.schema, t.endpoint, t.capability);
  ASSERT_TRUE(reused);
  ASSERT_EQ(reused->request.cell, dropped->request.cell);
  EXPECT_NE(reused->request.incarnation, dropped->request.incarnation);
  EXPECT_EQ(makeOutcomeCell(t.arena, t.schema, dropped->request,
                            IoOutcomeTag::Complete)
                .error(),
            IoCellError::StaleCell);
  EXPECT_EQ(requestEndpoint(t.arena, t.schema, reused->request), t.endpoint);
}

TEST(IoCellsTest, rolledBackSchemaIsRefusedAsStale) {
  ArenaManifold arena;
  const auto mark   = arena.mark();
  const auto schema = makeIoCellSchema(arena);
  ASSERT_TRUE(schema);
  ASSERT_TRUE(arena.release(mark));
  EXPECT_EQ(makeEndpointCell(arena, *schema).error(), IoCellError::StaleSchema);
}

struct PayloadFixture {
  PayloadResolver resolver;
  PayloadRef origin{.kind       = PayloadSourceKind::Transient,
                    .source     = 7,
                    .generation = 1,
                    .occurrence = 3};
  PayloadLease lease;
  /// A second lease held for the fixture's whole life: transient sources
  /// recycle once their last lease or owner token releases (plan §8.1),
  /// and these tests exercise payload-CELL lease lifecycle, which needs
  /// the source to stay resolvable. Every liveLeases() expectation below
  /// counts this pin.
  PayloadLease pin;

  PayloadFixture() {
    const std::array bytes{std::byte{0x00}, std::byte{0xfe}, std::byte{0x41}};
    resolver.registerSource(origin, bytes).value();
    lease = resolver.resolveTransient({origin, 0, bytes.size()}).value();
    pin   = resolver.resolveTransient({origin, 0, bytes.size()}).value();
  }
};

TEST(IoCellsTest, payloadCellLeaseFollowsMarkReleaseOverwriteAndArena) {
  PayloadFixture p;
  Topology t;
  ASSERT_EQ(p.resolver.liveLeases(), 2U); // lease + pin

  const auto mark    = t.arena.mark();
  const auto payload = makePayloadCell(t.arena, p.resolver, p.lease);
  ASSERT_TRUE(payload);
  EXPECT_EQ(p.resolver.liveLeases(), 3U);
  EXPECT_EQ(payloadRangeOf(t.arena, *payload), p.lease.range);
  p.resolver.release(p.lease.lease);
  EXPECT_EQ(p.resolver.liveLeases(), 2U);
  ASSERT_TRUE(t.arena.release(mark));
  EXPECT_EQ(p.resolver.liveLeases(), 1U); // the pin alone remains
  EXPECT_FALSE(payloadRangeOf(t.arena, *payload));

  // Overwriting the cell's value releases the lease that followed it.
  const auto relet = p.resolver.resolveTransient(p.lease.range);
  ASSERT_TRUE(relet);
  const auto overwritten = makePayloadCell(t.arena, p.resolver, *relet);
  ASSERT_TRUE(overwritten);
  p.resolver.release(relet->lease);
  EXPECT_EQ(p.resolver.liveLeases(), 2U);
  const auto text = t.arena.intern("replaced");
  ASSERT_TRUE(t.arena.setContent(
      overwritten->cell, std::span<const xanadu::PrimediaSpan>{&text, 1}));
  EXPECT_EQ(p.resolver.liveLeases(), 1U);

  // Arena destruction releases the rest.
  {
    Topology scoped;
    const auto again = p.resolver.resolveTransient(p.lease.range);
    ASSERT_TRUE(again);
    ASSERT_TRUE(makePayloadCell(scoped.arena, p.resolver, *again));
    p.resolver.release(again->lease);
    EXPECT_EQ(p.resolver.liveLeases(), 2U);
  }
  EXPECT_EQ(p.resolver.liveLeases(), 1U);
}

TEST(IoCellsTest, stalePayloadLeaseIsRefusedAndArenaMayOutliveResolver) {
  Topology t;
  {
    PayloadFixture p;
    p.resolver.release(p.lease.lease);
    EXPECT_EQ(makePayloadCell(t.arena, p.resolver, p.lease).error(),
              IoCellError::PayloadRefused);
    const auto fresh = p.resolver.resolveTransient(p.lease.range);
    ASSERT_TRUE(fresh);
    ASSERT_TRUE(makePayloadCell(t.arena, p.resolver, *fresh));
  }
  // The cell's token still pins its bytes; destroying it later is harmless.
  SUCCEED();
}

} // namespace
