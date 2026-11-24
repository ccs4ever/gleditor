#include <gtest/gtest.h>

#include <cstdint>
#include <expected>
#include <memory>

#include "common/xanadu/vortex/arena_io_cell_store.hpp"

namespace {

using namespace xanadu::vortex;

constexpr AdapterKey openKey{3, 4};

struct OpenArena {
  zigzag::ArenaManifold arena;
  IoCellSchema schema{makeIoCellSchema(arena).value()};
  EndpointCell source{makeEndpointCell(arena, schema).value()};
  CapabilityCell authority{makeCapabilityCell(arena, schema, source).value()};
  PayloadResolver payloads;

  auto decoder() const -> ArenaIoCellStore::RequestDecoder {
    return [](CapabilityCell,
              zigzag::CellRef) -> std::expected<CellRequest, IoErrorInfo> {
      return std::unexpected{IoErrorInfo{IoErrorCode::InvalidRequest, 0}};
    };
  }
};

auto requestedOpen(const CapabilityCell authority) -> OpenSpec {
  return OpenSpec{.adapterKey      = openKey,
                  .locator         = {std::byte{0x61}},
                  .authCapability  = authority,
                  .requestedRights = 0x03,
                  .consistency     = 0x04};
}

TEST(IoOpenAuthorityTest, MissingPolicyFailsClosed) {
  OpenArena fixture;
  ArenaIoCellStore cells(fixture.arena, fixture.schema, fixture.payloads,
                         fixture.decoder());

  const auto authorized = cells.authorizeOpen(requestedOpen(fixture.authority));
  ASSERT_FALSE(authorized);
  EXPECT_EQ(authorized.error().code, IoErrorCode::InvalidCapability);
  const auto opened =
      cells.mintOpenedEndpoint(requestedOpen(fixture.authority));
  ASSERT_FALSE(opened);
  EXPECT_EQ(opened.error().code, IoErrorCode::InvalidCapability);
}

TEST(IoOpenAuthorityTest, PolicyMustCoverRequestedRightsAndConsistency) {
  OpenArena fixture;
  ArenaIoCellStore cells(
      fixture.arena, fixture.schema, fixture.payloads, fixture.decoder(), {},
      [authority = fixture.authority](const OpenSpec &spec)
          -> std::expected<ArenaIoCellStore::OpenGrant, IoErrorInfo> {
        if (spec.authCapability != authority) {
          return std::unexpected{
              IoErrorInfo{IoErrorCode::InvalidCapability, 7}};
        }
        return ArenaIoCellStore::OpenGrant{.rights = 0x01, .consistency = 0x04};
      });

  const auto authorized = cells.authorizeOpen(requestedOpen(fixture.authority));
  ASSERT_FALSE(authorized);
  EXPECT_EQ(authorized.error().stableDetail, 2U);
}

TEST(IoOpenAuthorityTest,
     MintsRestrictedCapabilityBoundToAuthorityAndEndpoint) {
  OpenArena fixture;
  std::uint32_t calls{};
  ArenaIoCellStore cells(
      fixture.arena, fixture.schema, fixture.payloads, fixture.decoder(), {},
      [&calls, authority = fixture.authority](const OpenSpec &spec)
          -> std::expected<ArenaIoCellStore::OpenGrant, IoErrorInfo> {
        ++calls;
        if (spec.authCapability != authority) {
          return std::unexpected{
              IoErrorInfo{IoErrorCode::InvalidCapability, 7}};
        }
        return ArenaIoCellStore::OpenGrant{.rights = 0x07, .consistency = 0x0c};
      });

  const auto spec = requestedOpen(fixture.authority);
  ASSERT_TRUE(cells.authorizeOpen(spec));
  const auto opened = cells.mintOpenedEndpoint(spec);
  ASSERT_TRUE(opened);
  EXPECT_EQ(calls, 2U);
  EXPECT_EQ(ioCellTag(fixture.arena, opened->endpoint.cell,
                      opened->endpoint.incarnation),
            IoCellTag::Endpoint);
  EXPECT_EQ(ioCellTag(fixture.arena, opened->initialCapability.cell,
                      opened->initialCapability.incarnation),
            IoCellTag::Capability);
  EXPECT_EQ(capabilityEndpoint(fixture.arena, fixture.schema,
                               opened->initialCapability),
            opened->endpoint);
  const auto authOccurrence = fixture.arena.linked(
      opened->initialCapability.cell,
      fixture.schema.dim(IoDimension::Capability), zigzag::DimVector::POS);
  ASSERT_TRUE(authOccurrence);
  const auto cloneView = fixture.arena.linked(
      *authOccurrence, fixture.schema.dim(IoDimension::Capability),
      zigzag::DimVector::POS);
  ASSERT_TRUE(cloneView);
  EXPECT_EQ(fixture.arena.linked(*cloneView,
                                 fixture.schema.dim(IoDimension::Clone),
                                 zigzag::DimVector::NEG),
            fixture.authority.cell);
}

TEST(IoOpenAuthorityTest, StaleAuthorityIsRejectedBeforePolicyCall) {
  OpenArena fixture;
  std::uint32_t calls{};
  ArenaIoCellStore cells(
      fixture.arena, fixture.schema, fixture.payloads, fixture.decoder(), {},
      [&calls](const OpenSpec &)
          -> std::expected<ArenaIoCellStore::OpenGrant, IoErrorInfo> {
        ++calls;
        return ArenaIoCellStore::OpenGrant{.rights      = ~std::uint64_t{},
                                           .consistency = ~std::uint64_t{}};
      });
  auto spec = requestedOpen(fixture.authority);
  ++spec.authCapability->incarnation;

  const auto authorized = cells.authorizeOpen(spec);
  ASSERT_FALSE(authorized);
  EXPECT_EQ(authorized.error().code, IoErrorCode::InvalidCapability);
  EXPECT_EQ(calls, 0U);
}

} // namespace
