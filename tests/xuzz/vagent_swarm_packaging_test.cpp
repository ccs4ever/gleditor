/**
 * @file vagent_swarm_packaging_test.cpp
 * @brief Unit and integration tests for Track 3: Swarm-Transport Package
 * Distribution.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

#include "common/xanadu/bencode.hpp"
#include "common/xanadu/merkle_ledger.hpp"
#include "common/xanadu/resolver.hpp"
#include "common/xanadu/swarm.hpp"
#include "common/xanadu/torrent.hpp"
#include "common/xanadu/vagent/cluster/package_cas.hpp"
#include "common/xanadu/vagent/cluster/package_distribution.hpp"
#include "common/xanadu/vagent/cluster/package_format.hpp"
#include "common/xanadu/vagent/cluster/pending_fetch.hpp"
#include "common/xanadu/vortex/mesh_actor.hpp"

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace xanadu;
using namespace xanadu::vagent::cluster;
using namespace zigzag::vortex::mesh;
using xanadu::identity::ActorMessage;
using xanadu::identity::Fingerprint;
using xanadu::identity::MessageType;

Fingerprint makeTestFingerprint(char c) {
  const std::string s(40, c);
  return *Fingerprint::fromString(s);
}

fs::path createTempDir(std::string_view prefix) {
  static std::atomic<std::uint64_t> counter{0};
  const auto p =
      fs::temp_directory_path() /
      (std::string(prefix) + "_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()) +
       "_" + std::to_string(counter.fetch_add(1)));
  std::error_code ec;
  fs::create_directories(p, ec);
  return p;
}

struct TestCleanup {
  std::vector<fs::path> paths;
  ~TestCleanup() {
    std::error_code ec;
    for (const auto &p : paths) {
      fs::remove_all(p, ec);
    }
  }
};

SwarmContentSource::Options loopbackSwarmOptions() {
  SwarmContentSource::Options opts;
  opts.listenInterfaces               = "127.0.0.1:0";
  opts.enableDht                      = false;
  opts.enableLocalDiscovery           = false;
  opts.enableTrackers                 = false;
  opts.allowManyConnectionsPerAddress = true;
  opts.readTimeout                    = 3s;
  opts.metadataTimeout                = 3s;
  return opts;
}

// ============================================================================
// Suite 1: Package Publication Unit Tests
// ============================================================================

TEST(VagentSwarmPackagingTest,
     PublishBlobGeneratesDeterministicTorrentAndInfoHash) {
  const fs::path dir = createTempDir("vagent_pkg_pub_blob");
  TestCleanup cleanup{{dir}};

  PackageCas cas(CasOptions{.casDir = dir});
  PendingFetchQueue queue;
  SwarmPackageDistribution distributor(cas, queue);

  const std::string payload =
      "Hello Swarm Package Blob Distribution Content 12345";
  const std::string sha256 = toHex32(sha256Digest(payload));

  auto storeRes = cas.storeBlob(sha256, payload);
  ASSERT_TRUE(storeRes.has_value());

  auto pubRes = distributor.publishBlob(sha256, "widget.so");
  ASSERT_TRUE(pubRes.has_value());

  EXPECT_FALSE(pubRes->infoHash.isZero());
  EXPECT_EQ(pubRes->sha256, sha256);
  EXPECT_EQ(pubRes->size, payload.size());
  EXPECT_TRUE(pubRes->magnetUri.starts_with("magnet:?xt=urn:btih:"));
  EXPECT_TRUE(pubRes->magnetUri.find(pubRes->infoHash.hex()) !=
              std::string::npos);

  // Parse generated torrent file and verify structure
  const Metainfo meta = Metainfo::parse(pubRes->torrentFile);
  EXPECT_EQ(meta.name(), sha256); // Canonical CAS single-file torrent naming
  EXPECT_TRUE(pubRes->magnetUri.find("&dn=widget.so") != std::string::npos);
  EXPECT_EQ(meta.pieceLength(), 256ULL * 1024ULL); // K6 canonical 256 KiB
  EXPECT_EQ(meta.hash(), pubRes->infoHash);
  EXPECT_EQ(meta.totalLength(), payload.size());

  // Check CAS InfoHash mapping
  EXPECT_EQ(cas.sha256ForInfoHash(pubRes->infoHash), sha256);
  EXPECT_EQ(cas.infoHashForSha256(sha256), pubRes->infoHash);

  // Determinism check: publishing again produces bit-for-bit identical torrent
  // & InfoHash
  auto pubRes2 = distributor.publishBlob(sha256, "widget.so");
  ASSERT_TRUE(pubRes2.has_value());
  EXPECT_EQ(pubRes2->infoHash, pubRes->infoHash);
  EXPECT_EQ(pubRes2->torrentFile, pubRes->torrentFile);
}

TEST(VagentSwarmPackagingTest, PublishPackageManifestPureAndStandalone) {
  const fs::path dir = createTempDir("vagent_pkg_pub_manifest");
  TestCleanup cleanup{{dir}};

  PackageCas cas(CasOptions{.casDir = dir});
  PendingFetchQueue queue;
  SwarmPackageDistribution distributor(cas, queue);

  PackageManifest manifest;
  manifest.apiVersion     = 1;
  manifest.format         = "vagent-package-1";
  manifest.kind           = "pure";
  manifest.compilerFormat = "vagent-program-4";
  manifest.programBody    = "(agent greeter (on start (say \"hello\")))";

  const std::string expectedProgRef =
      computePackageProgramRef(manifest.kind, manifest.programBody,
                               computeTargetsDigest(manifest.targets));

  // 1. Publish as swarm seeding manifest (standalone = false)
  auto pubSeeding = distributor.publishPackage(manifest, false);
  ASSERT_TRUE(pubSeeding.has_value());
  EXPECT_EQ(pubSeeding->programRef, expectedProgRef);
  EXPECT_FALSE(pubSeeding->infoHash.isZero());
  EXPECT_TRUE(cas.hasBlob(pubSeeding->sha256));

  // Verify decoded manifest from CAS has no payloads
  auto readSeeding = cas.readBlob(pubSeeding->sha256);
  ASSERT_TRUE(readSeeding.has_value());
  auto decSeeding = decodePackageManifest(*readSeeding);
  ASSERT_TRUE(decSeeding.has_value());
  EXPECT_TRUE(decSeeding->payloads.empty());

  // 2. Publish with standalone = true and payload
  manifest.payloads["payload1_sha"] = "dummy payload bytes";
  auto pubStandalone = distributor.publishPackage(manifest, true);
  ASSERT_TRUE(pubStandalone.has_value());
  EXPECT_TRUE(cas.hasBlob(pubStandalone->sha256));

  auto readStandalone = cas.readBlob(pubStandalone->sha256);
  ASSERT_TRUE(readStandalone.has_value());
  auto decStandalone = decodePackageManifest(*readStandalone);
  ASSERT_TRUE(decStandalone.has_value());
  EXPECT_EQ(decStandalone->payloads.size(), 1u);
  EXPECT_EQ(decStandalone->payloads["payload1_sha"], "dummy payload bytes");
}

TEST(VagentSwarmPackagingTest, PublishPackageFileLoadsAndIngests) {
  const fs::path dir = createTempDir("vagent_pkg_pub_file");
  TestCleanup cleanup{{dir}};

  PackageCas cas(CasOptions{.casDir = dir});
  PendingFetchQueue queue;
  SwarmPackageDistribution distributor(cas, queue);

  PackageManifest manifest;
  manifest.apiVersion     = 1;
  manifest.format         = "vagent-package-1";
  manifest.kind           = "native";
  manifest.compilerFormat = "vagent-program-4";
  manifest.programBody    = "oplog-binary-bytes";

  const std::string modData = "mock module ELF binary data";
  const std::string modSha  = toHex32(sha256Digest(modData));

  PackageTarget target;
  target.module.filename = "module.so";
  target.module.sha256   = modSha;
  target.module.size     = static_cast<std::int64_t>(modData.size());
  target.module.infoHash = makeTorrent(modData, modSha, 256 * 1024).hash.hex();
  manifest.targets["x86_64-linux-gnu"] = target;

  manifest.payloads[modSha] = modData;

  const std::string encodedPkg = encodePackageManifest(manifest, true);
  const fs::path pkgPath       = dir / "test_app.vnpkg";
  {
    std::ofstream out(pkgPath, std::ios::binary);
    out.write(encodedPkg.data(),
              static_cast<std::streamsize>(encodedPkg.size()));
  }

  auto pubRes = distributor.publishPackageFile(pkgPath);
  ASSERT_TRUE(pubRes.has_value());

  // Check that both package file and embedded module payload were ingested into
  // CAS
  EXPECT_TRUE(cas.hasBlob(modSha));
  EXPECT_EQ(cas.readBlob(modSha).value(), modData);
  EXPECT_FALSE(pubRes->infoHash.isZero());
}

// ============================================================================
// Suite 2: Topic Rendezvous and DHT Announcement Tests
// ============================================================================

TEST(VagentSwarmPackagingTest, CanonicalTopicAndTargetRendezvous) {
  const std::string progRef =
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  const std::string topic = SwarmPackageDistribution::packageTopic(progRef);

  EXPECT_EQ(topic, "vagent:pkg:" + progRef);
  EXPECT_LE(topic.size(),
            128u); // BitTorrent expert invariant: topic <= 128 bytes
  EXPECT_EQ(topic.find('\0'), std::string::npos);

  const InfoHash target = SwarmPackageDistribution::packageDhtTarget(progRef);
  EXPECT_FALSE(target.isZero());

  // Confirm deterministic SHA-1 of "xudu:topic:" prefix
  const auto expectedTarget = sha1("xudu:topic:" + topic);
  EXPECT_EQ(target.bytes, expectedTarget);
}

// ============================================================================
// Suite 3: PendingProgramFetch Wiring & Bounded Queue Tests
// ============================================================================

TEST(VagentSwarmPackagingTest, PendingProgramFetchQueueWiringAndLimits) {
  const fs::path dir = createTempDir("vagent_pkg_fetch_queue");
  TestCleanup cleanup{{dir}};

  PackageCas cas(CasOptions{.casDir = dir});
  PendingFetchConfig cfg;
  cfg.maxPendingFetches        = 4;
  cfg.maxPendingFetchesPerPeer = 2;
  cfg.fetchTimeout             = 1000ms;
  PendingFetchQueue queue(cfg);

  SwarmPackageDistribution distributor(cas, queue);

  const auto originA = makeTestFingerprint('A');
  const auto originB = makeTestFingerprint('B');
  const InfoHash hashA{{1}};
  const InfoHash hashB{{2}};
  const InfoHash hashC{{3}};

  // 1. Enqueue under peer A
  auto res1 = distributor.initiateFetch("prog_1", originA, hashA, "sha_1");
  EXPECT_TRUE(res1.has_value());
  EXPECT_TRUE(queue.isPending("prog_1"));

  auto res2 = distributor.initiateFetch("prog_2", originA, hashB, "sha_2");
  EXPECT_TRUE(res2.has_value());

  // 3rd fetch for peer A exceeds maxPendingFetchesPerPeer (2)
  auto res3 = distributor.initiateFetch("prog_3", originA, hashC, "sha_3");
  ASSERT_FALSE(res3.has_value());
  EXPECT_EQ(res3.error(), ActorError::ResourceLimit);

  // Peer B can still enqueue up to global limit (4)
  auto res4 = distributor.initiateFetch("prog_4", originB, hashA, "sha_4");
  EXPECT_TRUE(res4.has_value());

  auto res5 = distributor.initiateFetch("prog_5", originB, hashB, "sha_5");
  EXPECT_TRUE(res5.has_value());

  // Global limit (4) is now reached
  auto res6 = distributor.initiateFetch("prog_6", originB, hashC, "sha_6");
  ASSERT_FALSE(res6.has_value());
  EXPECT_EQ(res6.error(), ActorError::ResourceLimit);
}

// ============================================================================
// Suite 4: Integrity Verification and Tampering Rejection
// ============================================================================

TEST(VagentSwarmPackagingTest, CorruptPieceOrPayloadRejectedBeforeCasCommit) {
  const fs::path dir = createTempDir("vagent_pkg_corrupt");
  TestCleanup cleanup{{dir}};

  PackageCas cas(CasOptions{.casDir = dir});
  PendingFetchQueue queue;
  SwarmPackageDistribution distributor(cas, queue);

  const std::string validData =
      "Uncorrupted original package binary payload content";
  const std::string validSha = toHex32(sha256Digest(validData));

  // Create torrent for valid data
  const MadeTorrent made = makeTorrent(validData, validSha, 256 * 1024);

  // Create a DirectoryContentSource that serves corrupted bytes
  const fs::path corruptDir = dir / "corrupt_store";
  fs::create_directories(corruptDir);
  const fs::path corruptFile = corruptDir / validSha;
  {
    std::string badData = validData;
    badData[0] ^= 0xFF; // Tamper/corrupt one byte!
    std::ofstream out(corruptFile, std::ios::binary);
    out.write(badData.data(), static_cast<std::streamsize>(badData.size()));
  }

  DirectoryContentSource source;
  source.add(made.file, corruptDir.string());

  // Direct fetch must fail verification!
  auto fetchRes = distributor.fetchBlobDirect(source, made.hash, validSha);
  ASSERT_FALSE(fetchRes.has_value());
  EXPECT_TRUE(fetchRes.error().find("verification failed") !=
              std::string::npos);

  // CAS MUST NOT contain corrupted blob! (BitTorrent invariant)
  EXPECT_FALSE(cas.hasBlob(validSha));
}

TEST(VagentSwarmPackagingTest, ProgramRefMismatchRejectedBeforeCasCommit) {
  const fs::path dir = createTempDir("vagent_pkg_progref_mismatch");
  TestCleanup cleanup{{dir}};

  PackageCas cas(CasOptions{.casDir = dir});
  PendingFetchQueue queue;
  SwarmPackageDistribution distributor(cas, queue);

  PackageManifest manifest;
  manifest.apiVersion     = 1;
  manifest.format         = "vagent-package-1";
  manifest.kind           = "pure";
  manifest.compilerFormat = "vagent-program-4";
  manifest.programBody    = "legitimate body";

  const std::string encoded = encodePackageManifest(manifest, true);
  const std::string sha256  = toHex32(sha256Digest(encoded));
  const MadeTorrent made    = makeTorrent(encoded, sha256, 256 * 1024);

  const fs::path sourceDir = dir / "src";
  fs::create_directories(sourceDir);
  {
    std::ofstream out(sourceDir / sha256, std::ios::binary);
    out.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
  }

  DirectoryContentSource source;
  source.add(made.file, sourceDir.string());

  // Expecting a completely different programRef
  const std::string forgedProgRef =
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

  auto fetchRes =
      distributor.fetchBlobDirect(source, made.hash, sha256, forgedProgRef);
  ASSERT_FALSE(fetchRes.has_value());
  EXPECT_TRUE(fetchRes.error().find("programRef mismatch") !=
              std::string::npos);

  // CAS must remain untouched
  EXPECT_FALSE(cas.hasBlob(sha256));
}

// ============================================================================
// Suite 5: Mock & Loopback Swarm Transfer and CAS Staging (Integration)
// ============================================================================

TEST(VagentSwarmPackagingTest, DirectContentSourceFetchAndCasCommitAndStaging) {
  const fs::path dir = createTempDir("vagent_pkg_direct_stage");
  TestCleanup cleanup{{dir}};

  PackageCas cas(CasOptions{.casDir = dir / "cas"});
  PendingFetchQueue queue;
  SwarmPackageDistribution distributor(cas, queue);

  const std::string binaryPayload =
      "Mock ELF dynamic library payload bytes for staging test";
  const std::string sha256 = toHex32(sha256Digest(binaryPayload));

  const MadeTorrent made = makeTorrent(binaryPayload, sha256, 256 * 1024);

  const fs::path sourceDir = dir / "peer_data";
  fs::create_directories(sourceDir);
  {
    std::ofstream out(sourceDir / sha256, std::ios::binary);
    out.write(binaryPayload.data(),
              static_cast<std::streamsize>(binaryPayload.size()));
  }

  DirectoryContentSource source;
  source.add(made.file, sourceDir.string());

  // Fetch into CAS
  auto fetchRes = distributor.fetchBlobDirect(source, made.hash, sha256);
  ASSERT_TRUE(fetchRes.has_value()) << fetchRes.error();

  EXPECT_TRUE(cas.hasBlob(sha256));
  EXPECT_TRUE(fs::exists(*fetchRes));

  // Check CAS file permissions: strictly 0400 (read-only)
  struct stat stCas{};
  ASSERT_EQ(::stat(fetchRes->c_str(), &stCas), 0);
  EXPECT_EQ(stCas.st_mode & 0777, 0400);

  // Stage blob into destination (mode 0500 per K8 / K9)
  const fs::path stageDir = dir / "staging";
  fs::create_directories(stageDir);
  const fs::path stagedBinary = stageDir / "libwidget.so.1";

  auto stageRes = cas.stageBlob(sha256, stagedBinary);
  ASSERT_TRUE(stageRes.has_value());
  EXPECT_TRUE(fs::exists(stagedBinary));

  struct stat stStage{};
  ASSERT_EQ(::stat(stagedBinary.c_str(), &stStage), 0);
  EXPECT_EQ(stStage.st_mode & 0777,
            0500); // 0500: read & execute only, no write!
}

TEST(VagentSwarmPackagingTest, LoopbackSwarmTransferAndPendingFetchCompletion) {
  const fs::path testRoot = createTempDir("vagent_loopback_swarm");
  TestCleanup cleanup{{testRoot}};

  const fs::path casDirA = testRoot / "nodeA_cas";
  const fs::path casDirB = testRoot / "nodeB_cas";
  const fs::path dlDirB  = testRoot / "nodeB_dl";

  PackageCas casA(CasOptions{.casDir = casDirA});
  PackageCas casB(CasOptions{.casDir = casDirB});
  PendingFetchQueue queueA;
  PendingFetchQueue queueB;

  auto swarmA = std::make_shared<SwarmContentSource>(loopbackSwarmOptions());
  auto swarmB = std::make_shared<SwarmContentSource>(loopbackSwarmOptions());

  SwarmPackageDistribution distA(casA, queueA, {.downloadDir = casDirA},
                                 swarmA);
  SwarmPackageDistribution distB(casB, queueB, {.downloadDir = dlDirB}, swarmB);

  // Wait for both sessions to bind listening ports
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  std::optional<std::uint16_t> portA;
  std::optional<std::uint16_t> portB;
  while ((!portA || !portB) && std::chrono::steady_clock::now() < deadline) {
    swarmA->poll();
    swarmB->poll();
    portA = swarmA->listenPort();
    portB = swarmB->listenPort();
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(portA.has_value());
  ASSERT_TRUE(portB.has_value());

  // Node A creates and publishes package
  const std::string payload =
      "Loopback Swarm Transfer Verified Package Content 42";
  const std::string sha256 = toHex32(sha256Digest(payload));

  ASSERT_TRUE(casA.storeBlob(sha256, payload).has_value());
  auto pubRes = distA.publishBlob(sha256, sha256);
  ASSERT_TRUE(pubRes.has_value());
  const InfoHash pkgHash = pubRes->infoHash;

  // Node B initiates fetch
  const auto originA = makeTestFingerprint('A');
  const ActorPid pendingPid{"nodeB", "agentX", 1, 100};

  AuthenticatedActorEvent initialEvent;
  initialEvent.sender            = originA;
  initialEvent.message.type      = MessageType::ActorSend;
  initialEvent.message.requestId = "req_init";
  initialEvent.message.target    = pendingPid;
  initialEvent.message.value     = "buffered turn message";

  auto fetchStart = distB.initiateFetch("prog_loopback", originA, pkgHash,
                                        sha256, pendingPid, initialEvent);
  ASSERT_TRUE(fetchStart.has_value());
  EXPECT_TRUE(queueB.isPending("prog_loopback"));
  EXPECT_TRUE(queueB.isPidPending(pendingPid));

  // Buffer another message targeting the pending actor
  AuthenticatedActorEvent msg2;
  msg2.sender            = originA;
  msg2.message.type      = MessageType::ActorSend;
  msg2.message.requestId = "req_msg2";
  msg2.message.target    = pendingPid;
  msg2.message.value     = "second message";
  EXPECT_TRUE(queueB.bufferMessage(pendingPid, msg2, 65536).has_value());

  // Connect Node B directly to Node A on loopback port
  distB.connectPeer(pkgHash, "127.0.0.1", *portA);

  // Non-blocking tick loop driving transfers
  std::vector<std::pair<PendingProgramFetch, fs::path>> completed;
  const auto transferDeadline = std::chrono::steady_clock::now() + 10s;

  while (completed.empty() &&
         std::chrono::steady_clock::now() < transferDeadline) {
    distA.tick();
    auto done = distB.tick();
    if (!done.empty()) {
      completed = std::move(done);
      break;
    }
    std::this_thread::sleep_for(20ms);
  }

  ASSERT_FALSE(completed.empty())
      << "Swarm transfer did not complete within deadline";
  const auto &[completedFetch, stagedPath] = completed.front();

  EXPECT_EQ(completedFetch.programRef, "prog_loopback");
  EXPECT_FALSE(queueB.isPending("prog_loopback"));
  EXPECT_FALSE(queueB.isPidPending(pendingPid));

  // Waiting buffered messages are preserved for admission
  EXPECT_EQ(completedFetch.waitingEvents.size(), 2u);
  EXPECT_EQ(completedFetch.waitingEvents[0].message.requestId, "req_init");
  EXPECT_EQ(completedFetch.waitingEvents[1].message.requestId, "req_msg2");

  // Node B CAS now has the verified blob
  EXPECT_TRUE(casB.hasBlob(sha256));
  EXPECT_EQ(casB.readBlob(sha256).value(), payload);

  // Stage into isolated directory with mode 0500
  const fs::path stageFile = testRoot / "staged" / "app.so";
  ASSERT_TRUE(casB.stageBlob(sha256, stageFile).has_value());
  EXPECT_TRUE(fs::exists(stageFile));

  struct stat st{};
  ASSERT_EQ(::stat(stageFile.c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0500);
}

TEST(VagentSwarmPackagingTest, FetchTimeoutAbortsPendingFetchAndCleansUp) {
  const fs::path testRoot = createTempDir("vagent_fetch_timeout");
  TestCleanup cleanup{{testRoot}};

  PackageCas cas(CasOptions{.casDir = testRoot / "cas"});
  PendingFetchConfig cfg;
  cfg.fetchTimeout = 50ms; // Very short timeout for testing
  PendingFetchQueue queue(cfg);

  SwarmPackageDistribution distributor(cas, queue, {.fetchTimeout = 50ms});

  const auto origin = makeTestFingerprint('T');
  const InfoHash unreachableHash{{9, 9, 9}};

  const auto startTime = std::chrono::steady_clock::now();
  auto initRes         = distributor.initiateFetch(
      "prog_timeout", origin, unreachableHash, "timeout_sha", std::nullopt,
      std::nullopt, startTime);
  ASSERT_TRUE(initRes.has_value());
  EXPECT_TRUE(queue.isPending("prog_timeout"));

  // Advance clock past timeout
  const auto pastTimeout = startTime + 100ms;
  auto completed         = distributor.tick(pastTimeout);

  EXPECT_TRUE(completed.empty());
  EXPECT_FALSE(queue.isPending("prog_timeout"));
  EXPECT_FALSE(distributor.fetchState("prog_timeout").has_value());
}

TEST(VagentSwarmPackagingTest, ExceedingMaxPackageBytesAbortsDownload) {
  const fs::path testRoot = createTempDir("vagent_max_bytes");
  TestCleanup cleanup{{testRoot}};

  PackageCas cas(CasOptions{.casDir = testRoot / "cas"});
  PendingFetchConfig cfg;
  cfg.maxPackageBytes = 100; // 100 bytes limit
  PendingFetchQueue queue(cfg);

  SwarmPackageDistribution distributor(cas, queue, {.maxPackageBytes = 100});

  const auto origin = makeTestFingerprint('X');
  const InfoHash hash{{7}};

  ASSERT_TRUE(
      distributor.initiateFetch("prog_overflow", origin, hash).has_value());

  // Record bytes exceeding maxPackageBytes (150 > 100)
  auto recRes = queue.recordBytesFetched("prog_overflow", 150);
  ASSERT_FALSE(recRes.has_value());
  EXPECT_EQ(recRes.error(), ActorError::TransportUnavailable);

  // Fetch must be aborted
  EXPECT_FALSE(queue.isPending("prog_overflow"));
}

} // namespace
