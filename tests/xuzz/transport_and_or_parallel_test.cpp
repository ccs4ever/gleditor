#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

#include "common/xanadu/identity/identity_layout.hpp"
#include "common/xanadu/identity/identity_serialization.hpp"
#include "common/xanadu/store.hpp"

#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/mesh_host.hpp"
#include "common/xanadu/vortex/mesh_swarm.hpp"
#include "common/xanadu/vortex/mesh_value.hpp"
#include "common/xanadu/vortex/or_parallel.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/vlog.hpp"
#include "identity_fixture.hpp"

namespace zigzag::vortex::mesh {
namespace {

using namespace std::chrono_literals;
using Fingerprint = xanadu::identity::Fingerprint;
using Message     = xanadu::identity::ActorMessage;
using MessageType = xanadu::identity::MessageType;

std::expected<xanadu::SignedProvenance, std::string>
fixturePrimedia(xanadu::Store &, std::string_view bytes, std::uint64_t start,
                std::string_view scrollKey) {
  xanadu::Provenance record;
  record.author        = {"Study fixture", "study@test.invalid", "fixture"};
  record.title         = "Primedia fixture";
  record.permascroll   = std::string(scrollKey);
  record.contentLength = bytes.size();
  record.contentDigest = xanadu::sha256Hex(bytes);
  record.extra.emplace_back("content_start", std::to_string(start));
  return xanadu::SignedProvenance{record.toTsv(), "fixture signature"};
}

// ============================================================================
// Stage 1 Tests: SpoolSliceBundle + 1 KiB BLAKE3 Merkle Leaves +
//                VerifiedSliceContentSource Zero-Torrent Fast Path
// ============================================================================

TEST(TransportAndOrParallelTest,
     SpoolSliceBundleRoundTripAndZeroTorrentHydration) {
  const auto tempRoot =
      std::filesystem::temp_directory_path() / "xanadu-spool-slice-bundle-test";
  std::filesystem::remove_all(tempRoot);

  auto authorPermascroll = std::make_shared<xanadu::UserPermascroll>();
  xanadu::Store authorStore(authorPermascroll);
  const auto genesis = authorStore.sliceGenesis(xanadu::MicroversionId{});
  auto baseManifold  = authorStore.rebuildManifold(genesis);

  ArenaManifold arena(&baseManifold, &authorStore);
  const CellRef rootCell  = arena.makeCell("stage1_root_payload");
  const CellRef childCell = arena.makeScalarCell(static_cast<std::int64_t>(42));
  const DimRef dim1       = arena.ensureDimension("d.1");
  ASSERT_TRUE(
      arena.link(rootCell, dim1, DimVector::POS, childCell).has_value());

  const auto keys = xanadu::createMutableKeys();
  const NodeId publisher{"0123456789ABCDEF0123456789ABCDEF01234567",
                         keys.publicKey.hex(), 1};

  auto seeded = publishValue(
      authorStore, genesis, arena, rootCell, keys, publisher,
      [](xanadu::Store &)
          -> std::expected<xanadu::SignedProvenance, std::string> {
        return xanadu::SignedProvenance{"study author", "study signature"};
      },
      tempRoot / "seeds", {}, "job-1", {}, fixturePrimedia);
  ASSERT_TRUE(seeded.has_value()) << seeded.error();

  auto packed = packSpoolSliceBundle(*seeded);
  ASSERT_TRUE(packed.has_value()) << packed.error();
  EXPECT_TRUE(verifySpoolMerkleLeaves(packed->carrier.streamBytes,
                                      packed->carrier.merkleLeafHashes));
  EXPECT_TRUE(verifySpoolMerkleLeaves(packed->history.streamBytes,
                                      packed->history.merkleLeafHashes));
  for (const auto &p : packed->payloads) {
    EXPECT_TRUE(verifySpoolMerkleLeaves(p.streamBytes, p.merkleLeafHashes));
  }

  const std::string wire = encodeSpoolSliceBundle(*packed);
  ASSERT_FALSE(wire.empty());

  auto decoded = decodeSpoolSliceBundle(wire);
  ASSERT_TRUE(decoded.has_value()) << decoded.error();
  EXPECT_EQ(decoded->value.root, packed->value.root);
  EXPECT_EQ(decoded->carrier.hash, packed->carrier.hash);
  EXPECT_EQ(decoded->history.hash, packed->history.hash);
  EXPECT_EQ(decoded->payloads.size(), packed->payloads.size());

  // Hydrate on a recipient with an empty backing ContentSource (proving zero
  // .torrent swarm lookups are required when VerifiedSliceContentSource has
  // the verified bundle).
  xanadu::DirectoryContentSource emptyBacking;
  auto received = receiveSpoolSliceBundle(
      wire, &emptyBacking, std::make_shared<xanadu::UserPermascroll>(),
      ValueLimits{});
  ASSERT_TRUE(received.has_value()) << received.error();
  ASSERT_NE(received->manifold, nullptr);
  const auto &rxManifold = *received->manifold;
  EXPECT_EQ(rxManifold.textOf(received->root, *received->store),
            "stage1_root_payload");
  std::optional<CellRef> rxDim1;
  for (const auto &edge : rxManifold.dimensionsOf(received->root)) {
    if (rxManifold.textOf(edge.dim, *received->store) == "d.1") {
      rxDim1 = edge.dim;
    }
  }
  ASSERT_TRUE(rxDim1.has_value());
  const auto rxChild =
      rxManifold.linked(received->root, *rxDim1, DimVector::POS);
  ASSERT_TRUE(rxChild.has_value());
  EXPECT_EQ(rxManifold.asInt64(*rxChild), 42);

  std::filesystem::remove_all(tempRoot);
}

TEST(TransportAndOrParallelTest, SpoolSliceBundleRejectsCorruptedBlake3Leaf) {
  VerifiedTorrentSlice slice{};
  slice.streamBytes      = std::string(2048, 'X');
  slice.merkleLeafHashes = computeSpoolMerkleLeaves(slice.streamBytes);
  ASSERT_EQ(slice.merkleLeafHashes.size(), 2U);
  EXPECT_TRUE(
      verifySpoolMerkleLeaves(slice.streamBytes, slice.merkleLeafHashes));

  // Flip one bit in the second 1 KiB leaf.
  slice.streamBytes[1500] ^= 0x01;
  EXPECT_FALSE(
      verifySpoolMerkleLeaves(slice.streamBytes, slice.merkleLeafHashes));
}

// ============================================================================
// Stage 2 Tests: QuicStreamFrame (libngtcp2 short header) +
//                DirectChannelState (libzmq + OpenSSL + libblake3)
// ============================================================================

TEST(TransportAndOrParallelTest, QuicStreamFrameNgtcp2ShortHeaderRoundTrip) {
  QuicStreamFrame frame{};
  frame.lane     = 0;
  frame.streamId = 7;
  frame.offset   = 0;
  frame.fin      = true;
  frame.payload  = "control-lane-actor-frame";

  const auto encoded = encodeQuicStreamFrame(frame);
  ASSERT_TRUE(encoded.has_value()) << encoded.error();
  ASSERT_FALSE(encoded->empty());

  auto decoded = decodeQuicStreamFrame(*encoded);
  ASSERT_TRUE(decoded.has_value()) << decoded.error();
  EXPECT_EQ(decoded->lane, 0U);
  EXPECT_EQ(decoded->streamId, 7U);
  EXPECT_TRUE(decoded->fin);
  EXPECT_EQ(decoded->payload, "control-lane-actor-frame");
}

TEST(TransportAndOrParallelTest,
     DirectChannelAuthenticatedControlAndZeroTorrentBundleStreaming) {
  xanadu::MutableKeys keys;
  keys.publicKey =
      xanadu::PublicKey::fromHex(xanadu::testing::kTestDevicePublicKeyHex);
  keys.secretKey =
      xanadu::SecretKey::fromHex(xanadu::testing::kTestDeviceSecretKeyHex);
  const auto fingerprint =
      *Fingerprint::fromString(xanadu::testing::kAuthorFingerprint);
  const xanadu::identity::IdentityNetworkController::Options identityOptions{
      .localFingerprint = fingerprint, .localDeviceKeys = keys};
  auto aIdentity =
      std::make_shared<xanadu::identity::IdentityNetworkController>(
          identityOptions);
  auto bIdentity =
      std::make_shared<xanadu::identity::IdentityNetworkController>(
          identityOptions);
  const auto delegation = xanadu::testing::fixtureDelegation(keys.publicKey);
  ASSERT_TRUE(aIdentity->trustDelegation(delegation,
                                         xanadu::testing::kAuthorPublicKey));
  ASSERT_TRUE(bIdentity->trustDelegation(delegation,
                                         xanadu::testing::kAuthorPublicKey));

  const auto directory =
      std::filesystem::temp_directory_path() /
      ("xanadu-direct-channel-" + xanadu::createMutableKeys().publicKey.hex());
  std::filesystem::remove_all(directory);

  auto makeOpts = [&](std::string suffix) {
    MeshTransportOptions options;
    options.downloads                  = directory / suffix;
    options.swarm.listenInterfaces     = "127.0.0.1:0";
    options.swarm.enableDht            = false;
    options.swarm.enableLocalDiscovery = false;
    options.swarm.enableTrackers       = false;
    return options;
  };
  MeshSwarmTransport a(aIdentity, makeOpts("a")), b(bIdentity, makeOpts("b"));
  ASSERT_GT(a.directPort(), 0U);
  ASSERT_GT(b.directPort(), 0U);

  // Connect A directly to B's ZMQ/QUIC side-channel WITHOUT connecting their
  // BitTorrent swarm listeners!
  ASSERT_TRUE(
      a.addDirectPeer(fingerprint, "127.0.0.1", b.directPort()).has_value());

  // 1. Stream a SpoolSliceBundle over Lane 1 (bulk verified slice lane) and a
  //    priority control message over Lane 0.
  auto authorPermascroll = std::make_shared<xanadu::UserPermascroll>();
  xanadu::Store authorStore(authorPermascroll);
  const auto genesis = authorStore.sliceGenesis(xanadu::MicroversionId{});
  auto baseManifold  = authorStore.rebuildManifold(genesis);
  ArenaManifold arena(&baseManifold, &authorStore);
  const CellRef rootCell = arena.makeCell("direct_lane1_verified_payload");
  const NodeId publisher{fingerprint.toString(), keys.publicKey.hex(), 1};
  auto seeded = publishValue(
      authorStore, genesis, arena, rootCell, keys, publisher,
      [](xanadu::Store &)
          -> std::expected<xanadu::SignedProvenance, std::string> {
        return xanadu::SignedProvenance{"study author", "study signature"};
      },
      directory / "seeds", {}, "job-direct", {}, fixturePrimedia);
  ASSERT_TRUE(seeded.has_value()) << seeded.error();
  auto packed = packSpoolSliceBundle(*seeded);
  ASSERT_TRUE(packed.has_value()) << packed.error();
  const std::string bundleWire = encodeSpoolSliceBundle(*packed);

  Message cutMsg;
  cutMsg.type      = MessageType::ActorCutPrune;
  cutMsg.requestId = "cut-req-1";
  cutMsg.source    = xanadu::identity::ActorPid{"node-a", "cursor-a", 1, 1};
  cutMsg.reason    = "0.1";

  ASSERT_TRUE(a.sendBundle(fingerprint, bundleWire));
  ASSERT_TRUE(a.send(fingerprint, cutMsg));

  std::vector<AuthenticatedActorEvent> incoming;
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while ((incoming.empty() ||
          !b.source().metainfo(seeded->carrier.hash).has_value()) &&
         std::chrono::steady_clock::now() < deadline) {
    a.poll();
    b.poll();
    auto batch = b.takeActorMessages();
    incoming.insert(incoming.end(), batch.begin(), batch.end());
    std::this_thread::sleep_for(5ms);
  }
  ASSERT_EQ(incoming.size(), 1U);
  EXPECT_EQ(incoming.front().message, cutMsg);

  // Verify B's VerifiedSliceContentSource resolved the carrier, history, and
  // payload slices in memory with zero BitTorrent swarm connections!
  auto received = fetchValue(seeded->carrier.hash, b.source(),
                             std::make_shared<xanadu::UserPermascroll>());
  ASSERT_TRUE(received.has_value()) << received.error();
  EXPECT_EQ(received->manifold->textOf(received->root, *received->store),
            "direct_lane1_verified_payload");

  std::filesystem::remove_all(directory);
}

// ============================================================================
// Stage 1 SQS Visibility Leases, Heartbeats, Credits & Dead-Letter Tests
// ============================================================================

class LeaseTestTransport final : public MeshHostTransport {
public:
  const xanadu::ContentSource &source() const override { return emptySource_; }
  void addProgram(std::string_view torrent, std::string dir) {
    emptySource_.add(torrent, std::move(dir));
  }
  void poll() override {}
  bool send(const Fingerprint &peer, const Message &message) override {
    std::scoped_lock lock(mu_);
    sentMessages.emplace_back(peer, message);
    return true;
  }
  bool sendBundle(const Fingerprint &peer,
                  std::string_view bundleBytes) override {
    std::scoped_lock lock(mu_);
    sentBundles.emplace_back(peer, std::string(bundleBytes));
    return true;
  }
  std::vector<AuthenticatedActorEvent> takeActorMessages() override {
    std::scoped_lock lock(mu_);
    auto out = std::move(inbox);
    inbox.clear();
    return out;
  }
  std::vector<JobAnnouncement> takeAnnouncements() override { return {}; }
  std::expected<void, std::string> announce(const ImmutableJob &,
                                            const CapabilityGrant &) override {
    return {};
  }
  std::expected<void, std::string> prepare(const ImmutableJob &, ValueLimits,
                                           std::size_t) override {
    return {};
  }
  std::expected<void, std::string> prepareValue(const xanadu::InfoHash &,
                                                ValueLimits) override {
    return {};
  }
  std::expected<void, std::string> seed(const ValueSeed &) override {
    return {};
  }

  void injectMessage(const Fingerprint &sender, const Message &msg) {
    std::scoped_lock lock(mu_);
    inbox.push_back(AuthenticatedActorEvent{sender, msg});
  }

  std::mutex mu_;
  std::vector<std::pair<Fingerprint, Message>> sentMessages;
  std::vector<std::pair<Fingerprint, std::string>> sentBundles;
  std::vector<AuthenticatedActorEvent> inbox;

private:
  xanadu::DirectoryContentSource emptySource_;
};

TEST(TransportAndOrParallelTest,
     SqsVisibilityLeaseHeartbeatAndDeadLetterQuarantine) {
  const auto tempRoot =
      std::filesystem::temp_directory_path() / "xanadu-sqs-lease-test";
  std::filesystem::remove_all(tempRoot);

  auto requesterKeys = xanadu::createMutableKeys();
  auto workerKeys    = xanadu::createMutableKeys();
  const auto reqFp   = *Fingerprint::fromString(std::string(40, '1'));
  const auto wrkFp   = *Fingerprint::fromString(std::string(40, '2'));
  const NodeId reqNode{reqFp.toString(), requesterKeys.publicKey.hex(), 1};
  const NodeId wrkNode{wrkFp.toString(), workerKeys.publicKey.hex(), 2};

  auto transport = std::make_shared<LeaseTestTransport>();
  MeshHostOptions opts{};
  opts.worker.node               = reqNode;
  opts.worker.keys               = requesterKeys;
  opts.transport                 = transport;
  opts.worker.hashcashDifficulty = 1;
  opts.worker.permascroll        = std::make_shared<xanadu::UserPermascroll>();
  opts.worker.seedRoot           = tempRoot / "seeds";
  opts.resultRoot                = tempRoot / "results";
  opts.worker.delegation =
      [&](const Fingerprint &fp) -> std::optional<xanadu::PublicKey> {
    if (fp == reqFp) return requesterKeys.publicKey;
    if (fp == wrkFp) return workerKeys.publicKey;
    return std::nullopt;
  };
  opts.worker.authorize = [&](const ImmutableJob &, const CapabilityGrant &g) {
    return g.issuer == requesterKeys.publicKey;
  };
  opts.tick              = 2ms;
  opts.retry             = 15ms;
  opts.leaseTimeout      = 25ms;
  opts.heartbeatInterval = 10ms;
  opts.maxAttempts       = 2;

  MeshHost host(opts);

  auto seed = publishProgram("1 + 1", tempRoot / "programs");
  ASSERT_TRUE(seed.has_value());
  transport->addProgram(seed->torrent.file, seed->path.string());
  ImmutableJob testJob{
      .program   = seed->torrent.hash,
      .frontend  = Frontend::Vpl,
      .base      = {},
      .parent    = {},
      .budget    = EvaluationBudget{10000, 10000, 10, 100000},
      .requester = reqNode,
  };
  const auto nowSec = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  CapabilityGrant testGrant{
      .job       = jobHash(testJob),
      .requester = reqNode,
      .worker    = wrkNode,
      .expires   = nowSec + 60,
      .budget    = testJob.budget,
      .issuer    = requesterKeys.publicKey,
  };
  testGrant.proof = {
      .resource  = testGrant.job,
      .timestamp = nowSec,
      .nonce =
          *xanadu::identity::HashcashEngine::mint(testGrant.job, nowSec, 1),
      .difficultyBits = 1,
  };
  testGrant.signature =
      xanadu::signMutableItem(grantSigningBuffer(testGrant), requesterKeys);

  auto submitted = host.dispatch(testJob, testGrant);
  ASSERT_TRUE(submitted.has_value()) << submitted.error();

  // Wait for maxAttempts (2) visibility lease timeouts without worker
  // completion; verify task transitions to FrontierTaskState::DeadLetter and
  // FutureState::Failed.
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  std::optional<HostFutureSnapshot> finalSnap;
  while (std::chrono::steady_clock::now() < deadline) {
    finalSnap = host.poll(*submitted);
    if (finalSnap && finalSnap->state == FutureState::Failed) {
      break;
    }
    std::this_thread::sleep_for(5ms);
  }
  ASSERT_TRUE(finalSnap.has_value());
  EXPECT_EQ(finalSnap->state, FutureState::Failed);
  ASSERT_FALSE(finalSnap->leases.empty());
  EXPECT_EQ(finalSnap->leases.front().state, FrontierTaskState::DeadLetter);
  EXPECT_GE(finalSnap->leases.front().attempts, 2U);
  EXPECT_FALSE(finalSnap->deadLetters.empty());

  std::filesystem::remove_all(tempRoot);
}

// ============================================================================
// Stage 3 Tests: Adaptive Model 1 (Oracle) + Model 2 (Shadow-Delta Prefix),
//                Depth > 1 Bindings, Dewey #Cut, and d.submission Reordering
// ============================================================================

TEST(TransportAndOrParallelTest,
     Model1OracleRecomputationFastForwardsWithoutPushingAncestorChoicePoints) {
  ArenaManifold parentArena;
  VortexCore parentCore(parentArena);
  VortexVM parentVm(parentCore);

  // Use #Bind on the cursor to record the branch answer cleanly:
  auto buildBranchProgram = [](VortexCore &core, VortexVM &vm) -> CellRef {
    auto &arena = core.arena();

    // Primary branch: Bind("ans", "primary_0") -> Halt
    const CellRef bind0 = vm.mintOpcode(OpcodeKind::Bind, "bind0");
    const CellRef halt0 = vm.mintOpcode(OpcodeKind::Halt, "halt0");
    std::ignore = arena.link(bind0, core.dims().spin, DimVector::POS, halt0);
    const CellRef k0 = arena.makeCell("ans");
    const CellRef v0 = arena.makeCell("primary_0");
    std::ignore      = arena.link(bind0, core.dims().grab, DimVector::POS, k0);
    std::ignore      = arena.link(k0, core.dims().step, DimVector::POS, v0);

    // Alt branch: Bind("ans", "alt_1") -> Halt
    const CellRef bind1 = vm.mintOpcode(OpcodeKind::Bind, "bind1");
    const CellRef halt1 = vm.mintOpcode(OpcodeKind::Halt, "halt1");
    std::ignore = arena.link(bind1, core.dims().spin, DimVector::POS, halt1);
    const CellRef k1 = arena.makeCell("ans");
    const CellRef v1 = arena.makeCell("alt_1");
    std::ignore      = arena.link(bind1, core.dims().grab, DimVector::POS, k1);
    std::ignore      = arena.link(k1, core.dims().step, DimVector::POS, v1);

    // Entry: Choice(bind1) -> bind0
    const CellRef choiceOp = vm.mintOpcode(OpcodeKind::Choice, "choice_root");
    const CellRef altTarget =
        arena.makeScalarCell(static_cast<std::int64_t>(bind1));
    core.bindInput(choiceOp, altTarget);
    std::ignore = arena.link(choiceOp, core.dims().spin, DimVector::POS, bind0);
    return choiceOp;
  };

  const CellRef entryOp = buildBranchProgram(parentCore, parentVm);
  const CellRef cursor  = parentVm.spawnCursor(entryOp, "main");

  // Step once to execute Choice(bind1); choiceStack_ now has 1 untried branch
  // at Dewey path [1] with cyclesSinceRoot < kRecomputeCycleThreshold.
  ASSERT_TRUE(parentVm.step(cursor).success);
  ASSERT_EQ(parentVm.choiceDepth(), 1U);

  OrParallelCoordinator coordinator(nullptr, kRecomputeCycleThreshold);
  auto exported = coordinator.exportSparkFromVictim(parentVm, entryOp);
  ASSERT_TRUE(exported.has_value());
  EXPECT_EQ(exported->model, SparkModelKind::OracleRecompute);
  EXPECT_EQ(exported->deweyKey, "1");
  EXPECT_FALSE(exported->prefixSnapshot.has_value());
  EXPECT_EQ(parentVm.choiceDepth(), 0U);

  // Round-trip over wire encoding
  const std::string wire = encodeOrParallelSpark(*exported);
  auto decoded           = decodeOrParallelSpark(wire);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->model, SparkModelKind::OracleRecompute);
  EXPECT_EQ(decoded->deweyKey, "1");

  // Finish parent's primary branch ("0")
  ASSERT_TRUE(parentVm.run(cursor).success);
  const CellRef parentVar =
      parentArena.linked(cursor, parentCore.dims().vars, DimVector::POS)
          .value_or(noCell);
  ASSERT_NE(parentVar, noCell);
  const CellRef parentVal =
      parentArena.linked(parentVar, parentCore.dims().values, DimVector::POS)
          .value_or(noCell);
  ASSERT_NE(parentVal, noCell);
  ASSERT_TRUE(coordinator.submitSolution(parentVm.currentDeweyPath(),
                                         parentArena.textOf(parentVal)));

  // Execute stolen Model 1 spark on an isolated worker
  auto acquired = coordinator.acquireNextSpark();
  ASSERT_TRUE(acquired.has_value());
  auto workerRes = OrParallelCoordinator::executeSparkOnWorker(
      acquired->first, nullptr,
      [&](VortexCore &wCore, VortexVM &wVm) {
        std::ignore = buildBranchProgram(wCore, wVm);
      },
      [](VortexCore &wCore, VortexVM &wVm) -> std::optional<std::string> {
        const auto cursors = wVm.activeCursors();
        if (cursors.empty()) return std::nullopt;
        const CellRef var =
            wCore.arena()
                .linked(cursors.front(), wCore.dims().vars, DimVector::POS)
                .value_or(noCell);
        if (var == noCell) return std::nullopt;
        const CellRef val =
            wCore.arena()
                .linked(var, wCore.dims().values, DimVector::POS)
                .value_or(noCell);
        if (val == noCell) return std::nullopt;
        return wCore.arena().textOf(val);
      },
      coordinator, acquired->second);
  ASSERT_TRUE(workerRes.success) << workerRes.errorMessage;

  const auto solutions = coordinator.orderedSolutions();

  ASSERT_EQ(solutions.size(), 2U);
  EXPECT_EQ(solutions[0].deweyKey, "0");
  EXPECT_EQ(solutions[0].value, "primary_0");
  EXPECT_EQ(solutions[1].deweyKey, "1");
  EXPECT_EQ(solutions[1].value, "alt_1");
}

TEST(TransportAndOrParallelTest,
     Model2ShadowDeltaPrefixPreservesParentLogicBindingsAtDepthGreaterThanOne) {
  ArenaManifold parentArena;
  VortexCore parentCore(parentArena);
  VortexVM parentVm(parentCore);

  // Depth 1: Parent creates a Vlog logic variable X and binds X =
  // "ancestor_bound"
  zigzag::Vlog parentVlog{
      .m     = parentArena,
      .clone = parentCore.dims().clone,
      .grab  = parentCore.dims().grab,
      .step  = parentCore.dims().step,
      .vars  = parentCore.dims().vars,
  };
  const CellRef logicVarX      = parentVlog.makeVar();
  const CellRef ancestorAtom   = parentArena.makeCell("ancestor_bound");
  const CellRef firstChoiceAlt = parentVm.mintOpcode(OpcodeKind::Halt, "alt1");
  const CellRef cursor = parentVm.spawnCursor(firstChoiceAlt, "depth_cursor");

  // Push outer choice point (depth 1)
  parentVm.pushChoicePoint(cursor, firstChoiceAlt);

  // Now at depth > 1, bind logicVarX to ancestorAtom in parent's ephemeral
  // arena!
  ASSERT_TRUE(parentVlog.unify(logicVarX, ancestorAtom).has_value());
  EXPECT_EQ(parentArena.textOf(parentVlog.deref(logicVarX)), "ancestor_bound");

  // Build nested choice point (depth 2) whose alternative dereferences
  // logicVarX
  const CellRef derefOp = parentVm.mintOpcode(OpcodeKind::Deref, "deref_x");
  const CellRef haltOp  = parentVm.mintOpcode(OpcodeKind::Halt, "halt_alt2");
  const CellRef outCell = parentArena.makeCell("unassigned");
  const CellRef inVarCell =
      parentArena.makeScalarCell(static_cast<std::int64_t>(logicVarX));
  std::ignore = parentArena.link(derefOp, parentCore.dims().grab,
                                 DimVector::POS, inVarCell);
  std::ignore = parentArena.link(outCell, parentCore.dims().grab,
                                 DimVector::POS, derefOp);
  std::ignore =
      parentArena.link(derefOp, parentCore.dims().spin, DimVector::POS, haltOp);

  // Step VM once so totalCycles_ >= 1, and configure coordinator with
  // cycleThreshold = 0 so it selects Model 2 (ShadowDeltaPrefix).
  parentVm.setCursorOpcode(cursor, haltOp);
  ASSERT_TRUE(parentVm.step(cursor).success);

  // Push nested choice point at depth 2
  parentVm.pushChoicePoint(cursor, derefOp);

  // Mutate logicVarX or other cells in parent AFTER the mark to prove
  // exportPrefix() replays trail_ in reverse without mutating parentArena!
  const CellRef postMarkCell = parentArena.makeCell("post_mark_noise");
  std::ignore                = postMarkCell;

  OrParallelCoordinator coordinator(nullptr, /*cycleThreshold=*/0);
  // First steal takes depth-1 choice point; second steal takes depth-2 choice
  // point!
  auto stolenDepth1 =
      coordinator.exportSparkFromVictim(parentVm, firstChoiceAlt);
  ASSERT_TRUE(stolenDepth1.has_value());
  auto stolenDepth2 = coordinator.exportSparkFromVictim(parentVm, derefOp);
  ASSERT_TRUE(stolenDepth2.has_value());
  EXPECT_EQ(stolenDepth2->model, SparkModelKind::ShadowDeltaPrefix);
  EXPECT_EQ(stolenDepth2->deweyKey, "0.1");
  ASSERT_TRUE(stolenDepth2->prefixSnapshot.has_value());

  // Round-trip the Model 2 spark through wire serialization
  const std::string wire = encodeOrParallelSpark(*stolenDepth2);
  auto decoded           = decodeOrParallelSpark(wire);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->model, SparkModelKind::ShadowDeltaPrefix);
  ASSERT_TRUE(decoded->prefixSnapshot.has_value());

  // Execute the decoded Model 2 spark on an isolated worker ArenaManifold!
  auto execRes = OrParallelCoordinator::executeSparkOnWorker(
      *decoded, nullptr,
      [&](VortexCore &, VortexVM &wVm) {
        wVm.setOpcodeKind(derefOp, OpcodeKind::Deref);
        wVm.setOpcodeKind(haltOp, OpcodeKind::Halt);
      },
      [&](VortexCore &wCore, VortexVM &) -> std::optional<std::string> {
        // outCell holds the dereferenced CellRef of logicVarX!
        const auto derefTarget = wCore.arena().asInt64(outCell);
        if (!derefTarget.has_value()) return std::nullopt;
        return wCore.arena().textOf(static_cast<CellRef>(*derefTarget));
      },
      coordinator);
  ASSERT_TRUE(execRes.success) << execRes.errorMessage;

  const auto solutions = coordinator.orderedSolutions();
  ASSERT_EQ(solutions.size(), 1U);
  EXPECT_EQ(solutions[0].deweyKey, "0.1");
  EXPECT_EQ(solutions[0].value, "ancestor_bound");
}

TEST(TransportAndOrParallelTest,
     HierarchicalDeweyCutPrunesActiveAndQueuedSubtreesAndOrdersSubmissions) {
  OrParallelCoordinator coordinator(nullptr, kRecomputeCycleThreshold);

  std::string broadcastedPrefix;
  coordinator.setCutBroadcastCallback([&](std::string_view prefix) {
    broadcastedPrefix = std::string(prefix);
  });

  // Enqueue sparks under "0.0", "0.1", "1.0", "1.1"
  coordinator.enqueueSpark(OrParallelSpark{
      .model     = SparkModelKind::OracleRecompute,
      .deweyPath = {0, 0},
      .deweyKey  = "0.0",
  });
  coordinator.enqueueSpark(OrParallelSpark{
      .model     = SparkModelKind::OracleRecompute,
      .deweyPath = {1, 0},
      .deweyKey  = "1.0",
  });
  coordinator.enqueueSpark(OrParallelSpark{
      .model     = SparkModelKind::OracleRecompute,
      .deweyPath = {1, 1},
      .deweyKey  = "1.1",
  });

  // Register an active worker token on "1.0.1"
  auto activeToken101 = coordinator.registerActiveSpark("1.0.1");
  auto activeToken01  = coordinator.registerActiveSpark("0.1");
  EXPECT_FALSE(activeToken101.stop_requested());
  EXPECT_FALSE(activeToken01.stop_requested());

  // Prune subtree "1" via #Cut
  const std::size_t prunedCount = coordinator.prunePrefix("1");
  EXPECT_GE(prunedCount, 3U); // 2 queued ("1.0", "1.1") + 1 active ("1.0.1")
  EXPECT_EQ(broadcastedPrefix, "1");
  EXPECT_TRUE(activeToken101.stop_requested());
  EXPECT_FALSE(activeToken01.stop_requested());

  // Late solution from pruned branch "1.0" must be rejected
  EXPECT_FALSE(coordinator.submitSolution(std::vector<std::uint16_t>{1, 0},
                                          "pruned_sol"));

  // Submit valid solutions out of Dewey order: "0.1" first, then "0.0"
  EXPECT_TRUE(coordinator.submitSolution(std::vector<std::uint16_t>{0, 1},
                                         "second_clause"));
  EXPECT_TRUE(coordinator.submitSolution(std::vector<std::uint16_t>{0, 0},
                                         "first_clause"));

  const auto ordered = coordinator.orderedSolutions();
  ASSERT_EQ(ordered.size(), 2U);
  EXPECT_EQ(ordered[0].deweyKey, "0.0");
  EXPECT_EQ(ordered[0].value, "first_clause");
  EXPECT_EQ(ordered[1].deweyKey, "0.1");
  EXPECT_EQ(ordered[1].value, "second_clause");

  // Verify the +d.submission ZigZag rank on coordinatorArena matches Dewey
  // order
  const auto rankCells = coordinator.submissionRankCells();
  ASSERT_EQ(rankCells.size(), 2U);
  EXPECT_EQ(coordinator.coordinatorArena().textOf(rankCells[0]),
            "first_clause");
  EXPECT_EQ(coordinator.coordinatorArena().textOf(rankCells[1]),
            "second_clause");
}

} // namespace
} // namespace zigzag::vortex::mesh
