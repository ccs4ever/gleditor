/**
 * @file swarm.cpp
 * @brief Fetching quoted content from another machine.
 *
 * These run only when a peer has been set up for them to talk to, which
 * `tools/swarm-netns-test.sh` does: it puts the peer in its own network
 * namespace, this process in another, and joins them with a veth pair. So the
 * two have separate network stacks, separate addresses and separate loopbacks,
 * and the only way between them is a network device -- a transfer that
 * succeeds here really did go over the wire.
 *
 * Without that setup they skip rather than fail, because a machine with no
 * peer to talk to has nothing to say about whether talking to peers works.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <xudu/core/origin.hpp>
#include <xudu/core/resolver.hpp>
#include <xudu/core/store.hpp>
#include <xudu/core/swarm.hpp>
#include <xudu/core/torrent.hpp>

namespace {

using namespace std::chrono_literals;

using xudu::InfoHash;
using xudu::MicroversionId;
using xudu::Origin;
using xudu::PrimediaSpan;
using xudu::Resolver;
using xudu::Store;
using xudu::SwarmContentSource;

/// Where the other peer is, as the harness reports it.
struct PeerUnderTest {
  std::string host;
  std::uint16_t port{};
  std::string torrentPath;
  std::string text;
};

[[nodiscard]] const char *env(const char *name) {
  const auto *value = std::getenv(name);
  return nullptr == value ? "" : value;
}

std::string readWholeFile(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

class SwarmTest : public testing::Test {
protected:
  PeerUnderTest peer;
  std::filesystem::path downloads;

  void SetUp() override {
    if (!xudu::swarmSupported()) {
      GTEST_SKIP() << "built without libtorrent";
    }
    const std::string host = env("XUDU_PEER_HOST");
    const std::string port = env("XUDU_PEER_PORT");
    if (host.empty() || port.empty()) {
      GTEST_SKIP() << "no peer to talk to; run tools/swarm-netns-test.sh";
    }
    peer.host        = host;
    peer.port        = static_cast<std::uint16_t>(std::stoul(port));
    peer.torrentPath = env("XUDU_PEER_TORRENT");
    peer.text        = readWholeFile(env("XUDU_PEER_TEXT"));
    ASSERT_FALSE(peer.text.empty()) << "the harness did not say what to expect";

    downloads = std::filesystem::temp_directory_path() /
                ("xudu-swarm-" +
                 std::string(testing::UnitTest::GetInstance()
                                 ->current_test_info()
                                 ->name()));
    std::filesystem::remove_all(downloads);
    std::filesystem::create_directories(downloads);
  }

  void TearDown() override {
    if (!downloads.empty()) {
      std::filesystem::remove_all(downloads);
    }
  }

  /// A source joined to the peer's swarm and nothing else.
  [[nodiscard]] SwarmContentSource::Options options() const {
    SwarmContentSource::Options opts;
    opts.enableDht            = false;
    opts.enableLocalDiscovery = false;
    opts.enableTrackers       = false;
    opts.listenInterfaces     = "0.0.0.0:0";
    opts.readTimeout          = 30s;
    opts.metadataTimeout      = 30s;
    return opts;
  }
};

TEST_F(SwarmTest, contentArrivesFromTheOtherMachine) {
  SwarmContentSource swarm(options());
  const auto hash = swarm.addTorrent(readWholeFile(peer.torrentPath),
                                     downloads.string(), false);
  swarm.connectPeer(hash, peer.host, peer.port);

  const auto *meta = swarm.metainfo(hash);
  ASSERT_NE(meta, nullptr);

  // A range in the middle, so this is not accidentally reading a file that
  // happened to already exist.
  const auto &file = meta->files().front();
  const Origin origin{hash, 0, file.path, file.offset, file.length};

  const Resolver resolver(&swarm);
  const auto got = resolver.read(origin, PrimediaSpan{1, 4, 5});
  EXPECT_EQ(got, peer.text.substr(4, 5));
}

TEST_F(SwarmTest, aRangeSpanningPiecesArrives) {
  SwarmContentSource swarm(options());
  const auto hash = swarm.addTorrent(readWholeFile(peer.torrentPath),
                                     downloads.string(), false);
  swarm.connectPeer(hash, peer.host, peer.port);
  const auto *meta = swarm.metainfo(hash);
  ASSERT_NE(meta, nullptr);
  ASSERT_GT(meta->pieceCount(), 1U) << "the sample needs several pieces";

  const auto &file = meta->files().front();
  const Origin origin{hash, 0, file.path, file.offset, file.length};
  const Resolver resolver(&swarm);

  // Straddling the first piece boundary, so two pieces have to be fetched and
  // both verified before anything is returned.
  const auto at     = meta->pieceLength() - 3;
  const auto got    = resolver.read(origin, PrimediaSpan{1, at, 8});
  EXPECT_EQ(got, peer.text.substr(static_cast<std::size_t>(at), 8));
}

TEST_F(SwarmTest, theWholeFileArrives) {
  SwarmContentSource swarm(options());
  const auto hash = swarm.addTorrent(readWholeFile(peer.torrentPath),
                                     downloads.string(), false);
  swarm.connectPeer(hash, peer.host, peer.port);
  const auto *meta = swarm.metainfo(hash);
  ASSERT_NE(meta, nullptr);

  const auto &file = meta->files().front();
  const Origin origin{hash, 0, file.path, file.offset, file.length};
  const Resolver resolver(&swarm);
  EXPECT_EQ(resolver.read(origin, PrimediaSpan{1, 0, file.length}), peer.text);
}

TEST_F(SwarmTest, aMagnetGetsItsMetadataFromAPeer) {
  // The thing a magnet cannot do alone. The link names the content; the piece
  // hashes come from the other machine, and libtorrent accepts them only if
  // they hash back to the name the link carried.
  const auto meta = xudu::Metainfo::parse(readWholeFile(peer.torrentPath));

  SwarmContentSource swarm(options());
  const auto hash = swarm.addMagnet(meta.magnet(), downloads.string());
  EXPECT_EQ(hash, meta.hash());

  // Nothing can be described yet: that is what makes it a magnet.
  EXPECT_EQ(swarm.metainfo(hash), nullptr);

  swarm.connectPeer(hash, peer.host, peer.port);
  ASSERT_TRUE(swarm.waitForMetadata(hash, 30s))
      << "no metadata arrived from the peer";

  const auto *fetched = swarm.metainfo(hash);
  ASSERT_NE(fetched, nullptr);
  // Read back through this codebase's own parser, and it agrees about the
  // identity -- so the content really is what the magnet named.
  EXPECT_EQ(fetched->hash(), meta.hash());
  EXPECT_EQ(fetched->pieceCount(), meta.pieceCount());
  EXPECT_EQ(fetched->totalLength(), meta.totalLength());
}

TEST_F(SwarmTest, aDocumentQuotesContentThisMachineNeverHad) {
  // The whole argument, end to end: a document holding no content of its own,
  // whose text comes from a machine it has never met, verified on arrival.
  SwarmContentSource swarm(options());
  const auto hash = swarm.addTorrent(readWholeFile(peer.torrentPath),
                                     downloads.string(), false);
  swarm.connectPeer(hash, peer.host, peer.port);
  const auto *meta = swarm.metainfo(hash);
  ASSERT_NE(meta, nullptr);

  Store store;
  store.setContentSource(&swarm);
  const auto &file = meta->files().front();
  const Origin origin{hash, 0, file.path, file.offset, file.length};

  const auto quoted =
      store.transcludeExternal(MicroversionId{}, 0, origin, 4, 5);

  EXPECT_EQ(store.textOf(quoted), peer.text.substr(4, 5));
  // Not one byte of it was typed here.
  EXPECT_EQ(store.primedia().size(), 0U);
}

TEST_F(SwarmTest, contentNobodyIsSeedingReadsAsNothing) {
  // No peer is introduced, so there is nowhere for the bytes to come from. The
  // read has to give up rather than block forever, and report nothing rather
  // than something plausible.
  auto opts        = options();
  opts.readTimeout = 2s;
  SwarmContentSource swarm(opts);
  const auto hash = swarm.addTorrent(readWholeFile(peer.torrentPath),
                                     downloads.string(), false);

  const auto *meta = swarm.metainfo(hash);
  ASSERT_NE(meta, nullptr);
  const auto &file = meta->files().front();
  const Origin origin{hash, 0, file.path, file.offset, file.length};

  const Resolver resolver(&swarm);
  const auto started = std::chrono::steady_clock::now();
  EXPECT_EQ(resolver.read(origin, PrimediaSpan{1, 0, 5}), "");
  EXPECT_LT(std::chrono::steady_clock::now() - started, 20s)
      << "the read did not give up";
}

TEST_F(SwarmTest, theContentReallyCameOverTheNetwork) {
  // Guards against the transfer tests passing because the bytes happened to be
  // on disk already.
  //
  // Counting connected peers does not work: a peer with nothing left to give
  // disconnects, so by the time a completed read can be asked about, the
  // connection it used is gone. Bytes received do not go back down -- but they
  // are accumulated on libtorrent's own tick rather than the moment a piece
  // lands, so this waits for the counter rather than reading it once.
  SwarmContentSource swarm(options());
  const auto hash = swarm.addTorrent(readWholeFile(peer.torrentPath),
                                     downloads.string(), false);
  EXPECT_EQ(swarm.bytesFromPeers(hash), 0) << "nothing has been fetched yet";
  ASSERT_TRUE(std::filesystem::is_empty(downloads))
      << "the download directory has to start empty for this to prove anything";

  swarm.connectPeer(hash, peer.host, peer.port);
  const auto *meta = swarm.metainfo(hash);
  ASSERT_NE(meta, nullptr);
  const auto &file = meta->files().front();
  const Origin origin{hash, 0, file.path, file.offset, file.length};

  const Resolver resolver(&swarm);
  ASSERT_EQ(resolver.read(origin, PrimediaSpan{1, 0, file.length}), peer.text);

  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (swarm.bytesFromPeers(hash) <= 0 &&
         std::chrono::steady_clock::now() < deadline) {
    static_cast<void>(swarm.metainfo(hash)); // pumps the session
    std::this_thread::sleep_for(100ms);
  }
  EXPECT_GE(swarm.bytesFromPeers(hash),
            static_cast<std::int64_t>(peer.text.size()))
      << "the content did not come from a peer";

  // And it landed on this machine, in a directory that had nothing in it.
  // Nothing but the network could have put it there.
  EXPECT_FALSE(std::filesystem::is_empty(downloads));
}

} // namespace

// vi: set sw=2 sts=2 ts=2 et:
