/**
 * @file swarm_lens_test.cpp
 * @brief SwarmLens: the Merkle identity ledger as a projection
 *        (design/lens-candidates.md section 3.6).
 */
#include <filesystem>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "common/xanadu/managed_torrent.hpp"
#include "common/xanadu/merkle_ledger.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/projection_session.hpp"
#include "common/xanadu/zigzag/swarm_lens.hpp"

namespace {

namespace fs = std::filesystem;
using xanadu::IdentityKeyLink;
using xanadu::MerkleLedger;
using zigzag::CellRef;
using zigzag::OptionalCell;
using zigzag::Posward;
using zigzag::ProjectionSession;
using zigzag::SwarmLens;
using zigzag::vortex::VortexCore;

xanadu::IdentityKeyLink entry(std::string who, std::string fp,
                              std::uint64_t ts) {
  IdentityKeyLink e;
  e.fingerprint = std::move(fp);
  e.identity    = std::move(who);
  e.email       = "x@x";
  e.timestamp   = ts;
  return e;
}

struct ProjectedSwarm {
  MerkleLedger ledger;
  zigzag::ArenaManifold arena;
  VortexCore core{arena};
  std::unique_ptr<ProjectionSession> session;

  void build() {
    session = std::make_unique<ProjectionSession>(
        std::make_unique<SwarmLens>("test", ledger), core);
  }
  CellRef dim(std::string_view n) { return core.findOrMintDimension(n); }
  std::string attr(CellRef c, std::string_view rank) {
    const auto a = arena.linked(c, dim(rank), Posward);
    return a.has_value() ? arena.textOf(*a) : "";
  }
};

TEST(SwarmLensTest, LedgerEntriesProjectInSequenceOrder) {
  ProjectedSwarm p;
  (void)p.ledger.appendKey(entry("alice", "1111aaaa", 1000));
  (void)p.ledger.appendKey(entry("bob", "2222bbbb", 2000));
  p.build();
  const auto root = p.session->roots().front();
  EXPECT_EQ(p.attr(root, "d.role"), "sw.ledger");
  EXPECT_EQ(p.attr(root, "d.roothash"), p.ledger.rootHex());
  const auto e0 = p.arena.linked(root, p.dim("d.contains"), Posward);
  ASSERT_TRUE(e0.has_value());
  const auto e1 = p.arena.linked(*e0, p.dim("d.seq"), Posward);
  ASSERT_TRUE(e1.has_value());
  EXPECT_EQ(p.arena.textOf(*e0), "alice");
  EXPECT_EQ(p.arena.textOf(*e1), "bob");
  EXPECT_EQ(p.attr(*e0, "d.fingerprint"), "1111AAAA"); // normalized
  EXPECT_EQ(p.attr(*e0, "d.keystab"), "pk");
  EXPECT_EQ(*p.session->keyFor(*e0), "swarm:test:e:0");
  EXPECT_EQ(*p.session->keyFor(*e1), "swarm:test:e:1");
  // The leaf hash rides on the entry -- the cryptographic identity the
  // ledger itself would prove.
  EXPECT_EQ(p.attr(*e0, "d.hash"), p.ledger.entries().front().leafHashHex());
}

TEST(SwarmLensTest, AppendsAppearOnRefreshWithStableKeys) {
  ProjectedSwarm p;
  (void)p.ledger.appendKey(entry("alice", "1111aaaa", 1000));
  p.build();
  const auto e0Key = "swarm:test:e:0";
  const auto e0    = p.session->cellFor(e0Key);
  ASSERT_TRUE(e0.has_value());
  // Append-only growth: e:0 keeps its cell, e:1 mints fresh.
  (void)p.ledger.appendKey(entry("bob", "2222bbbb", 2000));
  (void)p.session->refresh();
  EXPECT_EQ(*p.session->cellFor(e0Key), *e0);
  const auto e1 = p.session->cellFor("swarm:test:e:1");
  ASSERT_TRUE(e1.has_value());
  EXPECT_EQ(p.arena.textOf(*e1), "bob");
}

TEST(SwarmLensTest, ManagedTorrentsProjectTheLiveHalf) {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("swarm-lens-" + std::to_string(::getpid()));
  // A closed swarm: no DHT/LSD/trackers -- the manager registers and
  // seeds on this host only.
  xanadu::SystemTorrentManager::Options opts;
  opts.cacheRoot                      = dir.string();
  opts.enableDht                      = false;
  opts.enableLsd                      = false;
  opts.enableTrackers                 = false;
  opts.restrictDhtToDistinctNetworks  = false;
  opts.allowManyConnectionsPerAddress = true;
  xanadu::SystemTorrentManager manager(opts);

  ProjectedSwarm p;
  (void)p.ledger.appendKey(entry("alice", "1111aaaa", 1000));
  std::string err;
  const auto hash =
      manager.registerLedger(p.ledger, std::nullopt, "identity", &err);
  ASSERT_FALSE(hash.isZero()) << err;

  auto l          = std::make_unique<SwarmLens>("test", p.ledger, &manager);
  p.session       = std::make_unique<ProjectionSession>(std::move(l), p.core);
  const auto tkey = "swarm:test:t:" + hash.hex();
  const auto t    = p.session->cellFor(tkey);
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(p.attr(*t, "d.role"), "sw.torrent");
  EXPECT_EQ(p.attr(*t, "d.name"), "identity");
  EXPECT_EQ(p.attr(*t, "d.state"), "seeding");
  EXPECT_EQ(p.attr(*t, "d.keystab"), "pk");
  EXPECT_EQ(*p.session->keyFor(*t), tkey);
  fs::remove_all(dir);
}

} // namespace
