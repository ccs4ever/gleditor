#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/spool.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/vortex/payload_resolver.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {
using namespace xanadu::vortex;

constexpr auto u64max = std::numeric_limits<std::uint64_t>::max();

/// A permascroll stand-in: one scroll of bytes, short reads past its end.
class FixedScroll final : public xanadu::SpanReader {
public:
  explicit FixedScroll(std::string bytes) : bytes_(std::move(bytes)) {}
  [[nodiscard]] std::string
  read(const xanadu::PrimediaSpan &span) const override {
    if (span.scroll != scroll || span.start > bytes_.size()) return {};
    return bytes_.substr(span.start, span.length);
  }
  static constexpr xanadu::ScrollId scroll = 4;

private:
  std::string bytes_;
};

std::vector<std::byte> bytesOf(const std::string_view text) {
  std::vector<std::byte> out;
  for (const char c : text) out.push_back(static_cast<std::byte>(c));
  return out;
}

PayloadRef transientRef(const std::uint64_t source) {
  return PayloadRef{.kind       = PayloadSourceKind::Transient,
                    .source     = source,
                    .generation = 1,
                    .occurrence = 1};
}

TEST(PayloadResolverTest, SlicesBinaryBytesWithCheckedUint64Offsets) {
  PayloadResolver resolver;
  const std::array bytes{std::byte{0x41}, std::byte{0x00}, std::byte{0xff},
                         std::byte{0x42}, std::byte{0x00}};
  const auto origin = transientRef(1);
  ASSERT_TRUE(resolver.registerSource(origin, bytes));
  const auto lease = resolver.resolveTransient({origin, 0, bytes.size()});
  ASSERT_TRUE(lease);
  const auto part = resolver.slice(*lease, 1, 3);
  ASSERT_TRUE(part);
  EXPECT_EQ(part->range, (PayloadRange{origin, 1, 3}));
  const auto inner = resolver.slice(*part, 1, 2);
  ASSERT_TRUE(inner);
  EXPECT_EQ(inner->range, (PayloadRange{origin, 2, 2}));
  EXPECT_EQ(resolver.read(*inner, 0, 2),
            (std::vector<std::byte>{std::byte{0xff}, std::byte{0x42}}));
  EXPECT_EQ(resolver.read(*lease, 4, 1),
            (std::vector<std::byte>{std::byte{0x00}}));
  EXPECT_TRUE(resolver.read(*lease, 5, 0)->empty());

  for (const auto [offset, length] :
       {std::pair{std::uint64_t{1}, u64max},
        std::pair{u64max, std::uint64_t{1}}, std::pair{u64max, u64max},
        std::pair{std::uint64_t{4}, std::uint64_t{2}}}) {
    EXPECT_EQ(resolver.slice(*lease, offset, length).error().code,
              IoErrorCode::InvalidRequest);
  }
  EXPECT_EQ(resolver.read(*lease, u64max, 1).error().code,
            IoErrorCode::InvalidRequest);
  EXPECT_EQ(resolver.read(*part, 0, 4).error().code,
            IoErrorCode::InvalidRequest);
  EXPECT_EQ(resolver.retain({origin, u64max, 2}).error().code,
            IoErrorCode::InvalidRequest);
}

TEST(PayloadResolverTest, DurableScratchTransientAndProxyDispatchByTag) {
  PayloadResolver resolver;
  const FixedScroll scroll(std::string("pre\0fix-durable", 15));

  const xanadu::PrimediaSpan span{
      .scroll = FixedScroll::scroll, .start = 2, .length = 6};
  const auto durable = resolver.resolveDurable(scroll, span);
  ASSERT_TRUE(durable);
  EXPECT_EQ(durable->range.origin.kind, PayloadSourceKind::Durable);
  EXPECT_EQ(resolver.read(*durable, 0, 6),
            bytesOf(std::string_view("e\0fix-", 6)));
  EXPECT_EQ(resolver.requirePersistable(*durable), span);
  const auto durableSlice = resolver.slice(*durable, 1, 3);
  ASSERT_TRUE(durableSlice);
  EXPECT_EQ(resolver.requirePersistable(*durableSlice),
            (xanadu::PrimediaSpan{
                .scroll = FixedScroll::scroll, .start = 3, .length = 3}));
  EXPECT_EQ(resolver
                .resolveDurable(
                    scroll,
                    {.scroll = FixedScroll::scroll, .start = 10, .length = 50})
                .error()
                .code,
            IoErrorCode::Unresolved);
  EXPECT_EQ(resolver
                .resolveDurable(scroll, {.scroll = FixedScroll::scroll,
                                         .start  = u64max,
                                         .length = 2})
                .error()
                .code,
            IoErrorCode::InvalidRequest);

  zigzag::ArenaManifold arena;
  const auto scratchSpan = arena.intern(std::string_view("s\0b", 3));
  const auto scratch     = resolver.resolveScratch(arena, scratchSpan);
  ASSERT_TRUE(scratch);
  EXPECT_EQ(scratch->range.origin.kind, PayloadSourceKind::Scratch);
  EXPECT_EQ(resolver.read(*scratch, 0, 3),
            bytesOf(std::string_view("s\0b", 3)));
  EXPECT_EQ(resolver.requirePersistable(*scratch).error().code,
            IoErrorCode::TransientNotPersistable);
  // Scratch is never a durable or transient address.
  EXPECT_EQ(resolver.resolveDurable(scroll, scratchSpan).error().code,
            IoErrorCode::InvalidRequest);
  EXPECT_EQ(resolver.resolveScratch(arena, span).error().code,
            IoErrorCode::InvalidRequest);

  const auto transient = transientRef(9);
  ASSERT_TRUE(resolver.registerSource(transient, bytesOf("t")));
  const auto transientLease = resolver.resolveTransient({transient, 0, 1});
  ASSERT_TRUE(transientLease);
  EXPECT_EQ(resolver.requirePersistable(*transientLease).error().code,
            IoErrorCode::TransientNotPersistable);
  EXPECT_EQ(resolver.resolveProxy({.range = {transient, 0, 1}}).error().code,
            IoErrorCode::InvalidRequest);

  const PayloadRef proxy{.kind       = PayloadSourceKind::Proxy,
                         .source     = 9,
                         .generation = 1,
                         .occurrence = 1};
  // Same numbers as the transient origin: the tag keeps them apart.
  EXPECT_EQ(resolver.resolveProxy({.range = {proxy, 0, 1}}).error().code,
            IoErrorCode::Unresolved);
  ASSERT_TRUE(resolver.registerSource(proxy, bytesOf("p")));
  const auto proxyLease = resolver.resolveProxy({.range = {proxy, 0, 1}});
  ASSERT_TRUE(proxyLease);
  EXPECT_EQ(resolver.read(*proxyLease, 0, 1), bytesOf("p"));
  EXPECT_EQ(resolver.read(*transientLease, 0, 1), bytesOf("t"));
  EXPECT_EQ(resolver.requirePersistable(*proxyLease).error().code,
            IoErrorCode::TransientNotPersistable);
  EXPECT_EQ(resolver.retain({proxy, 0, 1})->range.origin, proxy);
}

TEST(PayloadResolverTest, RolledBackScratchIsUnresolved) {
  PayloadResolver resolver;
  zigzag::ArenaManifold arena;
  const auto mark = arena.mark();
  const auto span = arena.intern("gone");
  ASSERT_TRUE(arena.release(mark));
  EXPECT_EQ(resolver.resolveScratch(arena, span).error().code,
            IoErrorCode::Unresolved);
}

TEST(PayloadResolverTest, ScratchChunkLivesOnlyThroughLeasesAndOwners) {
  PayloadResolver resolver;
  zigzag::ArenaManifold arena;
  const auto span  = arena.intern("leased scratch");
  const auto lease = resolver.resolveScratch(arena, span);
  ASSERT_TRUE(lease);
  const auto origin = lease->range.origin;
  ASSERT_EQ(resolver.liveLeases(), 1U);

  auto owner = resolver.retainOwner(*lease);
  ASSERT_TRUE(owner);
  auto ownerToken = std::move(*owner);
  ASSERT_EQ(resolver.liveLeases(), 2U);
  resolver.release(lease->lease);

  EXPECT_EQ(resolver.liveLeases(), 1U);
  EXPECT_EQ(resolver.read(*lease, 0, 1).error().code, IoErrorCode::Stale);
  EXPECT_EQ(resolver.retain({origin, 0, span.length}).error().code,
            IoErrorCode::Unresolved);
  EXPECT_EQ(PayloadResolver::read(*ownerToken, 0, span.length),
            bytesOf("leased scratch"));

  ownerToken.reset();
  EXPECT_EQ(resolver.liveLeases(), 0U);
  EXPECT_EQ(resolver.retain({origin, 0, span.length}).error().code,
            IoErrorCode::Unresolved);
}

TEST(PayloadResolverTest, RegistrationIsTypedAndOriginsAreNotReused) {
  PayloadResolver resolver;
  const auto origin = transientRef(2);
  ASSERT_TRUE(resolver.registerSource(origin, bytesOf("a")));
  EXPECT_EQ(resolver.registerSource(origin, bytesOf("b")).error().code,
            IoErrorCode::Conflict);
  const PayloadRef durable{.kind = PayloadSourceKind::Durable, .source = 4};
  EXPECT_EQ(resolver.registerSource(durable, bytesOf("a")).error().code,
            IoErrorCode::InvalidRequest);
  EXPECT_EQ(resolver.resolveTransient({transientRef(3), 0, 0}).error().code,
            IoErrorCode::Unresolved);
}

TEST(PayloadResolverTest, LeasesPinChunksAcrossSourceRetirement) {
  PayloadResolver resolver;
  const auto origin = transientRef(5);
  ASSERT_TRUE(
      resolver.registerSource(origin, bytesOf(std::string_view("\0x", 2))));
  const auto lease = resolver.resolveTransient({origin, 0, 2});
  ASSERT_TRUE(lease);
  const auto slice = resolver.slice(*lease, 1, 1);
  ASSERT_TRUE(slice);
  ASSERT_EQ(resolver.retireSource(origin), &resolver);

  EXPECT_EQ(resolver.read(*lease, 0, 2), bytesOf(std::string_view("\0x", 2)));
  EXPECT_EQ(resolver.read(*slice, 0, 1), bytesOf("x"));
  EXPECT_EQ(resolver.resolveTransient({origin, 0, 2}).error().code,
            IoErrorCode::Unresolved);
  EXPECT_TRUE(resolver.slice(*lease, 0, 1));

  resolver.release(lease->lease);
  EXPECT_EQ(resolver.read(*lease, 0, 1).error().code, IoErrorCode::Stale);
  EXPECT_EQ(resolver.slice(*lease, 0, 1).error().code, IoErrorCode::Stale);
  // A forged lease id with a different range is stale too.
  const PayloadLease forged{.range = {origin, 0, 1}, .lease = slice->lease};
  EXPECT_EQ(resolver.read(forged, 0, 1).error().code, IoErrorCode::Stale);
  EXPECT_EQ(resolver.read(*slice, 0, 1), bytesOf("x"));
}

TEST(PayloadResolverTest, OwnerTokenHoldsItsOwnLeaseAndOutlivesResolver) {
  std::shared_ptr<const PayloadOwnerToken> owner;
  {
    PayloadResolver resolver;
    const auto origin = transientRef(6);
    ASSERT_TRUE(resolver.registerSource(origin, bytesOf("\x7f")));
    const auto lease = resolver.resolveTransient({origin, 0, 1});
    ASSERT_TRUE(lease);
    auto retained = resolver.retainOwner(*lease);
    ASSERT_TRUE(retained);
    owner = *retained;
    EXPECT_EQ(resolver.liveLeases(), 2U);
    resolver.release(lease->lease);
    EXPECT_EQ(resolver.liveLeases(), 1U);
    EXPECT_EQ(PayloadResolver::read(*owner, 0, 1), bytesOf("\x7f"));
    EXPECT_EQ(PayloadResolver::read(*owner, 1, 1).error().code,
              IoErrorCode::InvalidRequest);
    EXPECT_EQ(resolver.retainOwner(*lease).error().code, IoErrorCode::Stale);
  }
  EXPECT_EQ(PayloadResolver::read(*owner, 0, 1), bytesOf("\x7f"));
  owner.reset();
}

TEST(PayloadResolverTest, ArenaOwnerRefusesPromotionBeforeStoreMutation) {
  PayloadResolver resolver;
  const auto origin = transientRef(8);
  ASSERT_TRUE(
      resolver.registerSource(origin, bytesOf(std::string_view("\0\xff", 2))));
  const auto lease = resolver.resolveTransient({origin, 0, 2});
  ASSERT_TRUE(lease);
  const auto owner = resolver.retainOwner(*lease);
  ASSERT_TRUE(owner);

  xanadu::Store store;
  const auto parent = store.sliceGenesis(xanadu::MicroversionId{});
  zigzag::ArenaManifold arena;
  const auto cell        = arena.makeCell("transient payload");
  const auto incarnation = arena.incarnationOf(cell);
  ASSERT_TRUE(incarnation);
  ASSERT_TRUE(arena.setOwnerLease(
      cell, *incarnation, std::const_pointer_cast<PayloadOwnerToken>(*owner)));
  const auto opCount = store.opCount();

  resolver.release(lease->lease);
  EXPECT_EQ(PayloadResolver::read(**owner, 0, 2),
            bytesOf(std::string_view("\0\xff", 2)));
  const auto refused = zigzag::promoteOrRefuse(store, parent, arena, cell);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), zigzag::PromotionRefusal::RuntimeOnlyState);
  EXPECT_EQ(store.opCount(), opCount);
}

} // namespace
