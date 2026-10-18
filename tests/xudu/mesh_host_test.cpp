#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "common/xanadu/identity/identity_serialization.hpp"
#include "common/xanadu/resolver.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/mesh_host.hpp"
#include "identity_fixture.hpp"

namespace zigzag::vortex::mesh {
namespace {
using namespace std::chrono_literals;
using Fingerprint = xanadu::identity::Fingerprint;
using Message     = xanadu::identity::ActorMessage;
using MessageType = xanadu::identity::MessageType;
constexpr EvaluationBudget budget{1'000'000, 100'000, 100, 1'000'000};
std::uint64_t secondsNow() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}
std::expected<xanadu::SignedProvenance, std::string>
fixturePrimedia(xanadu::Store &, std::string_view bytes, std::uint64_t start,
                std::string_view scrollKey) {
  xanadu::Provenance record;
  record.author        = {"Mesh fixture", "mesh@test.invalid", "fixture"};
  record.title         = "Primedia fixture";
  record.permascroll   = std::string(scrollKey);
  record.contentLength = bytes.size();
  record.contentDigest = xanadu::sha256Hex(bytes);
  record.extra.emplace_back("content_start", std::to_string(start));
  return xanadu::SignedProvenance{record.toTsv(), "fixture signature"};
}
class LockedSource final : public xanadu::ContentSource {
public:
  void add(std::string_view torrent, std::string directory) {
    std::lock_guard lock(mutex);
    source.add(torrent, std::move(directory));
  }
  gleditor::cpp26::optional<const xanadu::Metainfo &>
  metainfo(const xanadu::InfoHash &h) const override {
    std::lock_guard lock(mutex);
    return source.metainfo(h);
  }
  std::string readStream(const xanadu::InfoHash &h, std::uint64_t o,
                         std::uint64_t n) const override {
    std::lock_guard lock(mutex);
    return source.readStream(h, o, n);
  }

private:
  mutable std::mutex mutex;
  xanadu::DirectoryContentSource source;
};
struct Hardware {
  std::mutex mutex;
  LockedSource source;
  std::map<std::string, std::deque<AuthenticatedActorEvent>> events;
  std::vector<JobAnnouncement> announcements;
  std::vector<Message> frames;
  std::function<void(Message &)> intercept;
};
class HardwareLink final : public MeshHostTransport {
public:
  HardwareLink(std::shared_ptr<Hardware> hardware, Fingerprint identity)
      : hardware(std::move(hardware)), identity(identity),
        checkedSource(*this) {
    std::lock_guard lock(this->hardware->mutex);
    this->hardware->events[identity.toString()];
  }
  const xanadu::ContentSource &source() const override { return checkedSource; }
  void poll() override { check(); }
  bool send(const Fingerprint &peer, const Message &message) override {
    check();
    std::lock_guard lock(hardware->mutex);
    auto wire = xanadu::identity::encodeActorMessage(message);
    if (!wire) return false;
    auto decoded = xanadu::identity::decodeActorMessage(std::span{
        reinterpret_cast<const std::uint8_t *>(wire->data()), wire->size()});
    if (!decoded) return false;
    if (hardware->intercept) hardware->intercept(*decoded);
    hardware->frames.push_back(*decoded);
    const auto found = hardware->events.find(peer.toString());
    if (found == hardware->events.end()) return false;
    found->second.push_back({identity, *decoded});
    return true;
  }
  std::vector<AuthenticatedActorEvent> takeActorMessages() override {
    check();
    std::lock_guard lock(hardware->mutex);
    auto &queue = hardware->events.at(identity.toString());
    std::vector<AuthenticatedActorEvent> result(queue.begin(), queue.end());
    queue.clear();
    return result;
  }
  std::vector<JobAnnouncement> takeAnnouncements() override {
    check();
    std::lock_guard lock(hardware->mutex);
    return hardware->announcements;
  }
  std::expected<void, std::string>
  announce(const ImmutableJob &job, const CapabilityGrant &grant) override {
    check();
    std::lock_guard lock(hardware->mutex);
    hardware->announcements.push_back({job, grant});
    return {};
  }
  std::expected<void, std::string> prepare(const ImmutableJob &job, ValueLimits,
                                           std::size_t maximum) override {
    check();
    ++prepared;
    const auto meta = source().metainfo(job.program);
    if (!meta || meta->totalLength() > maximum)
      return std::unexpected("hardware: missing program");
    return {};
  }
  std::expected<void, std::string> prepareValue(const xanadu::InfoHash &h,
                                                ValueLimits) override {
    check();
    if (!source().metainfo(h))
      return std::unexpected("hardware: missing result");
    return {};
  }
  std::expected<void, std::string> seed(const ValueSeed &value) override {
    check();
    hardware->source.add(
        value.history.torrentFile,
        (value.carrierPath.parent_path() / value.history.hash.hex()).string());
    for (const auto &payload : value.payloads)
      hardware->source.add(payload.torrent.file, payload.path.string());
    hardware->source.add(value.carrier.file, value.carrierPath.string());
    ++seeded;
    return {};
  }
  std::atomic<unsigned> prepared{0}, seeded{0};
  std::atomic<bool> wrongThread{false};
  std::thread::id ui = std::this_thread::get_id();

private:
  void check() {
    std::lock_guard lock(checkMutex);
    const auto now = std::this_thread::get_id();
    if (now == ui) wrongThread = true;
    if (!owner)
      owner = now;
    else if (*owner != now)
      wrongThread = true;
  }
  class CheckedSource final : public xanadu::ContentSource {
  public:
    explicit CheckedSource(HardwareLink &link) : link(link) {}
    gleditor::cpp26::optional<const xanadu::Metainfo &>
    metainfo(const xanadu::InfoHash &hash) const override {
      link.check();
      return link.hardware->source.metainfo(hash);
    }
    std::string readStream(const xanadu::InfoHash &hash, std::uint64_t offset,
                           std::uint64_t length) const override {
      link.check();
      return link.hardware->source.readStream(hash, offset, length);
    }

  private:
    HardwareLink &link;
  };
  std::mutex checkMutex;
  std::optional<std::thread::id> owner;
  std::shared_ptr<Hardware> hardware;
  Fingerprint identity;
  CheckedSource checkedSource;
};
class MeshHostTest : public testing::Test {
protected:
  xanadu::MutableKeys requesterKeys = xanadu::createMutableKeys(),
                      workerKeys    = xanadu::createMutableKeys();
  Fingerprint requesterIdentity =
      *Fingerprint::fromString(std::string(40, 'A'));
  Fingerprint workerIdentity = *Fingerprint::fromString(std::string(40, 'B'));
  NodeId requester{requesterIdentity.toString(), requesterKeys.publicKey.hex(),
                   1};
  NodeId worker{workerIdentity.toString(), workerKeys.publicKey.hex(), 2};
  std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("xanadu-mesh-host-" + requesterKeys.publicKey.hex());
  std::shared_ptr<Hardware> hardware = std::make_shared<Hardware>();
  std::shared_ptr<HardwareLink> requesterLink =
      std::make_shared<HardwareLink>(hardware, requesterIdentity);
  std::shared_ptr<HardwareLink> workerLink =
      std::make_shared<HardwareLink>(hardware, workerIdentity);
  ~MeshHostTest() override { std::filesystem::remove_all(directory); }
  MeshHostOptions options(NodeId node, xanadu::MutableKeys keys,
                          std::shared_ptr<MeshHostTransport> link) {
    MeshHostOptions o;
    o.worker.node               = node;
    o.worker.keys               = keys;
    o.transport                 = std::move(link);
    o.worker.budget             = budget;
    o.worker.hashcashDifficulty = 4;
    o.worker.permascroll        = std::make_shared<xanadu::UserPermascroll>();
    o.worker.seedRoot           = directory / node.authorKey / "seeds";
    o.resultRoot                = directory / node.authorKey / "answers";
    o.worker.provenance         = [](xanadu::Store &store)
        -> std::expected<xanadu::SignedProvenance, std::string> {
      if (store.opCount() == 0) return std::unexpected("unpromoted provenance");
      return xanadu::SignedProvenance{"fixture author", "fixture signature"};
    };
    o.worker.primediaProvenance = fixturePrimedia;
    o.worker.delegation =
        [this](const Fingerprint &fp) -> std::optional<xanadu::PublicKey> {
      if (fp == requesterIdentity) return requesterKeys.publicKey;
      if (fp == workerIdentity) return workerKeys.publicKey;
      return std::nullopt;
    };
    o.worker.authorize = [this](const ImmutableJob &,
                                const CapabilityGrant &g) {
      return g.issuer == requesterKeys.publicKey;
    };
    o.tick  = 2ms;
    o.retry = 20ms;
    return o;
  }
  ImmutableJob job(Frontend frontend        = Frontend::Vpl,
                   std::string_view program = "1 + 2") {
    auto seed = publishProgram(program, directory / "programs");
    if (!seed) throw std::runtime_error(seed.error());
    hardware->source.add(seed->torrent.file, seed->path.string());
    return {.program   = seed->torrent.hash,
            .frontend  = frontend,
            .base      = {},
            .parent    = {},
            .budget    = budget,
            .requester = requester};
  }
  CapabilityGrant grant(const ImmutableJob &job) {
    CapabilityGrant g{.job       = jobHash(job),
                      .requester = requester,
                      .worker    = worker,
                      .expires   = secondsNow() + 60,
                      .budget    = budget,
                      .issuer    = requesterKeys.publicKey};
    g.proof       = {.resource       = g.job,
                     .timestamp      = secondsNow(),
                     .nonce          = 0,
                     .difficultyBits = 4};
    g.proof.nonce = *xanadu::identity::HashcashEngine::mint(
        g.job, g.proof.timestamp, g.proof.difficultyBits);
    g.signature = xanadu::signMutableItem(grantSigningBuffer(g), requesterKeys);
    return g;
  }
  HostFutureSnapshot finish(MeshHost &host, std::string_view hash,
                            std::chrono::seconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto snapshot = host.poll(hash);
      if (snapshot && (snapshot->state == FutureState::Succeeded ||
                       snapshot->state == FutureState::Failed))
        return *snapshot;
      std::this_thread::sleep_for(2ms);
    }
    throw std::runtime_error("host future timed out");
  }
};

TEST_F(
    MeshHostTest,
    TwoOwnersExecuteFetchAndMergeEveryFrontendWithoutCallingHardwareOnUiOrWorker) {
  MeshHost requesting(options(requester, requesterKeys, requesterLink));
  MeshHost computing(options(worker, workerKeys, workerLink));
  const std::pair<Frontend, std::string_view> programs[] = {
      {Frontend::Vpl, "1 + 2"},
      {Frontend::Vlace, "1 + 2"},
      {Frontend::Vquery, "##/d.remote%\"three\""},
      {Frontend::Vprolog, "answer(3). ?- answer(X)."}};
  for (const auto &[frontend, source] : programs) {
    auto j           = job(frontend, source);
    auto g           = grant(j);
    const auto start = std::chrono::steady_clock::now();
    auto dispatched  = requesting.dispatch(j, g, true);
    ASSERT_TRUE(dispatched) << dispatched.error();
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
    auto ready = finish(requesting, *dispatched);
    ASSERT_EQ(ready.state, FutureState::Succeeded) << ready.error;
    EXPECT_EQ(ready.expectedWorkers, 1U);
    EXPECT_EQ(ready.verifiedWorkers, 1U);
    ASSERT_EQ(ready.merged.size(), 1U);
    ASSERT_TRUE(std::filesystem::exists(ready.store / "ops.nodes"));
    xanadu::Store reopened;
    reopened.setContentSource(&hardware->source);
    reopened.load(ready.store.string());
    const auto result =
        reopened.rebuildManifold(ready.merged.front().promoted.version);
    EXPECT_FALSE(result.cells().empty());
    const auto operations = reopened.opCount();
    ASSERT_TRUE(requesting.dispatch(j, g));
    EXPECT_EQ(requesting.poll(*dispatched)->merged.front().promoted.version,
              ready.merged.front().promoted.version);
    reopened.load(ready.store.string());
    EXPECT_EQ(reopened.opCount(), operations);
  }
  EXPECT_EQ(workerLink->seeded.load(), 4U);
  EXPECT_EQ(workerLink->prepared.load(), 4U);
  EXPECT_FALSE(requesterLink->wrongThread);
  EXPECT_FALSE(workerLink->wrongThread);
  std::lock_guard lock(hardware->mutex);
  for (const auto &frame : hardware->frames)
    if (frame.type == MessageType::ActorDeliverWriteSet) {
      EXPECT_TRUE(frame.value.empty());
      EXPECT_EQ(frame.infoHash.size(), 40U);
    }
}

TEST_F(MeshHostTest,
       SubstitutedSignedResultFromTheExpectedWorkerCannotBecomeAnAnswer) {
  ArenaManifold arena;
  auto root = arena.makeCell("unrelated result");
  xanadu::Store store;
  auto unrelated =
      publishValue(store, {}, arena, root, workerKeys, worker,
                   xanadu::SignedProvenance{"fixture", "signature"},
                   directory / "unrelated", {}, "a different job");
  ASSERT_TRUE(unrelated) << unrelated.error();
  hardware->source.add(
      unrelated->history.torrentFile,
      (unrelated->carrierPath.parent_path() / unrelated->history.hash.hex())
          .string());
  for (const auto &payload : unrelated->payloads)
    hardware->source.add(payload.torrent.file, payload.path.string());
  hardware->source.add(unrelated->carrier.file,
                       unrelated->carrierPath.string());
  hardware->intercept = [hash = unrelated->carrier.hash.hex()](Message &m) {
    if (m.type == MessageType::ActorDeliverWriteSet) m.infoHash = hash;
  };
  MeshHost requesting(options(requester, requesterKeys, requesterLink));
  MeshHost computing(options(worker, workerKeys, workerLink));
  auto j    = job();
  auto sent = requesting.dispatch(j, grant(j));
  ASSERT_TRUE(sent) << sent.error();
  auto failed = finish(requesting, *sent);
  EXPECT_EQ(failed.state, FutureState::Failed);
  EXPECT_NE(failed.error.find("substitution"), std::string::npos);
  EXPECT_TRUE(failed.merged.empty());
  EXPECT_TRUE(failed.store.empty());
}

TEST_F(MeshHostTest, RefusingComputePolicyNeverPreparesUntrustedContent) {
  auto client             = options(requester, requesterKeys, requesterLink);
  auto server             = options(worker, workerKeys, workerLink);
  server.worker.authorize = {};
  MeshHost requesting(client), computing(server);
  auto j    = job();
  auto g    = grant(j);
  auto sent = requesting.dispatch(j, g);
  ASSERT_TRUE(sent) << sent.error();
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(workerLink->prepared.load(), 0U);
  EXPECT_EQ(workerLink->seeded.load(), 0U);
  EXPECT_EQ(requesting.poll(*sent)->verifiedWorkers, 0U);
}

TEST_F(MeshHostTest,
       ClosedWorkerSetAndRetentionBoundsAreEnforcedBeforeNetworkDispatch) {
  auto o            = options(requester, requesterKeys, requesterLink);
  o.maximumRequests = 1;
  MeshHost host(o);
  auto j = job();
  auto g = grant(j);
  ASSERT_TRUE(host.dispatch(j, g));
  EXPECT_FALSE(host.dispatch(j, std::vector<CapabilityGrant>{g, g}));
  auto second = job(Frontend::Vlace);
  EXPECT_FALSE(host.dispatch(second, grant(second)));
  auto changed = g;
  changed.worker.arenaEpoch++;
  changed.signature =
      xanadu::signMutableItem(grantSigningBuffer(changed), requesterKeys);
  EXPECT_FALSE(host.dispatch(j, changed));
}
TEST_F(MeshHostTest, WorkerFailureIsExplicitAndNamesTheExactWorker) {
  MeshHost requesting(options(requester, requesterKeys, requesterLink));
  MeshHost computing(options(worker, workerKeys, workerLink));
  auto j          = job(Frontend::Vpl, "(");
  auto dispatched = requesting.dispatch(j, grant(j));
  ASSERT_TRUE(dispatched) << dispatched.error();
  auto failed = finish(requesting, *dispatched);
  EXPECT_EQ(failed.state, FutureState::Failed);
  EXPECT_FALSE(failed.error.empty());
  EXPECT_TRUE(failed.merged.empty());
  std::lock_guard lock(hardware->mutex);
  const auto notification =
      std::ranges::find_if(hardware->frames, [](const auto &frame) {
        return frame.type == MessageType::ActorSpawnAck &&
               !frame.reason.empty();
      });
  ASSERT_NE(notification, hardware->frames.end());
  ASSERT_TRUE(notification->source);
  EXPECT_EQ(notification->source->node, worker.str());
  EXPECT_EQ(notification->source->epoch, worker.arenaEpoch);
}

TEST_F(MeshHostTest, FailureFromAnotherWorkerEpochCannotFailTheExpectedFuture) {
  auto impostor = worker;
  impostor.arenaEpoch++;
  std::atomic<unsigned> failures{0};
  hardware->intercept = [&](Message &message) {
    if (message.type == MessageType::ActorSpawnAck && !message.reason.empty()) {
      message.source = xanadu::identity::ActorPid{
          impostor.str(), message.requestId, impostor.arenaEpoch, 1};
      ++failures;
    }
  };
  MeshHost requesting(options(requester, requesterKeys, requesterLink));
  MeshHost computing(options(worker, workerKeys, workerLink));
  auto j          = job(Frontend::Vpl, "(");
  auto dispatched = requesting.dispatch(j, grant(j));
  ASSERT_TRUE(dispatched) << dispatched.error();
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (failures.load() == 0 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(2ms);
  ASSERT_GT(failures.load(), 0U);
  std::this_thread::sleep_for(20ms);
  EXPECT_EQ(requesting.poll(*dispatched)->state, FutureState::Running);
  EXPECT_TRUE(requesting.poll(*dispatched)->merged.empty());
}

TEST(MeshHostSocketTest,
     AuthenticatedTwoHostFutureFetchesProgramsAndResultsOverRealPeerSockets) {
  namespace identity = xanadu::identity;
  xanadu::MutableKeys keys;
  keys.publicKey =
      xanadu::PublicKey::fromHex(xanadu::testing::kTestDevicePublicKeyHex);
  keys.secretKey =
      xanadu::SecretKey::fromHex(xanadu::testing::kTestDeviceSecretKeyHex);
  const auto fingerprint =
      *Fingerprint::fromString(xanadu::testing::kAuthorFingerprint);
  identity::IdentityNetworkController::Options identityOptions{
      .localFingerprint = fingerprint, .localDeviceKeys = keys};
  auto requesterIdentity =
      std::make_shared<identity::IdentityNetworkController>(identityOptions);
  auto workerIdentity =
      std::make_shared<identity::IdentityNetworkController>(identityOptions);
  const auto delegation = xanadu::testing::fixtureDelegation(keys.publicKey);
  ASSERT_TRUE(requesterIdentity->trustDelegation(
      delegation, xanadu::testing::kAuthorPublicKey));
  ASSERT_TRUE(workerIdentity->trustDelegation(
      delegation, xanadu::testing::kAuthorPublicKey));
  const auto root = std::filesystem::temp_directory_path() /
                    ("xanadu-mesh-host-socket-" +
                     xanadu::createMutableKeys().publicKey.hex());
  struct Cleanup {
    std::filesystem::path root;
    ~Cleanup() { std::filesystem::remove_all(root); }
  } cleanup{root};
  const auto network = [&](std::string name) {
    MeshTransportOptions o;
    o.downloads                            = root / name / "downloads";
    o.swarm.listenInterfaces               = "127.0.0.1:0";
    o.swarm.enableDht                      = false;
    o.swarm.enableLocalDiscovery           = false;
    o.swarm.enableTrackers                 = false;
    o.swarm.allowManyConnectionsPerAddress = true;
    o.swarm.readTimeout                    = 10s;
    o.swarm.metadataTimeout                = 10s;
    o.metadataTimeout                      = 10s;
    return o;
  };
  auto requesterTransport = std::make_shared<MeshSwarmTransport>(
      requesterIdentity, network("requester"));
  auto workerTransport =
      std::make_shared<MeshSwarmTransport>(workerIdentity, network("worker"));
  const auto listening = std::chrono::steady_clock::now() + 5s;
  while ((requesterTransport->source().listenPort() == 0 ||
          workerTransport->source().listenPort() == 0) &&
         std::chrono::steady_clock::now() < listening)
    std::this_thread::sleep_for(10ms);
  ASSERT_NE(requesterTransport->source().listenPort(), 0);
  ASSERT_NE(workerTransport->source().listenPort(), 0);
  ASSERT_TRUE(requesterTransport->addPeerHint(
      "127.0.0.1", workerTransport->source().listenPort()));
  ASSERT_TRUE(workerTransport->addPeerHint(
      "127.0.0.1", requesterTransport->source().listenPort()));
  // Only the requester starts with program metadata or bytes. The shared topic
  // supplies the first authenticated control socket; no program torrent is
  // installed on the worker to manufacture a connection.
  auto program = publishProgram("1 + 2", root / "program");
  ASSERT_TRUE(program) << program.error();
  requesterTransport->source().addTorrent(program->torrent.file,
                                          program->path.string(), true);
  const NodeId requester{fingerprint.toString(), keys.publicKey.hex(), 71};
  const NodeId worker{fingerprint.toString(), keys.publicKey.hex(), 72};
  auto configure =
      [&](NodeId node,
          std::shared_ptr<identity::IdentityNetworkController> identity,
          std::shared_ptr<MeshSwarmTransport> transport, std::string name) {
        MeshHostOptions o;
        o.worker.node        = std::move(node);
        o.worker.keys        = keys;
        o.worker.permascroll = std::make_shared<xanadu::UserPermascroll>();
        o.worker.budget      = budget;
        o.worker.seedRoot    = root / name / "seeds";
        o.worker.hashcashDifficulty = 4;
        o.worker.provenance         = [](xanadu::Store &)
            -> std::expected<xanadu::SignedProvenance, std::string> {
          return xanadu::SignedProvenance{"fixture author",
                                          "fixture signature"};
        };
        o.worker.primediaProvenance = fixturePrimedia;
        o.worker.authorize = [key = keys.publicKey](const ImmutableJob &,
                                                    const CapabilityGrant &g) {
          return g.issuer == key;
        };
        o.transport  = makeSwarmHostTransport(std::move(transport));
        o.identity   = std::move(identity);
        o.resultRoot = root / name / "results";
        o.tick       = 5ms;
        o.retry      = 100ms;
        return o;
      };
  auto requesting = std::make_unique<MeshHost>(
      configure(requester, requesterIdentity, requesterTransport, "requester"));
  auto computing = std::make_unique<MeshHost>(
      configure(worker, workerIdentity, workerTransport, "worker"));
  ImmutableJob job{.program   = program->torrent.hash,
                   .frontend  = Frontend::Vpl,
                   .base      = {},
                   .parent    = {},
                   .budget    = budget,
                   .requester = requester};
  CapabilityGrant grant{.job       = jobHash(job),
                        .requester = requester,
                        .worker    = worker,
                        .expires   = secondsNow() + 120,
                        .budget    = budget,
                        .issuer    = keys.publicKey};
  grant.proof = {.resource       = grant.job,
                 .timestamp      = secondsNow(),
                 .nonce          = 0,
                 .difficultyBits = 4};
  grant.proof.nonce =
      *identity::HashcashEngine::mint(grant.job, grant.proof.timestamp, 4);
  grant.signature  = xanadu::signMutableItem(grantSigningBuffer(grant), keys);
  const auto start = std::chrono::steady_clock::now();
  auto sent        = requesting->dispatch(job, grant, true);
  ASSERT_TRUE(sent) << sent.error();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
  std::optional<HostFutureSnapshot> finished;
  const auto deadline = std::chrono::steady_clock::now() + 90s;
  while (std::chrono::steady_clock::now() < deadline) {
    finished = requesting->poll(*sent);
    if (finished && (finished->state == FutureState::Succeeded ||
                     finished->state == FutureState::Failed))
      break;
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(finished);
  ASSERT_EQ(finished->state, FutureState::Succeeded) << finished->error;
  ASSERT_EQ(finished->verifiedWorkers, 1U);
  ASSERT_EQ(finished->merged.size(), 1U);
  requesting.reset();
  computing.reset();
  xanadu::Store result;
  result.setContentSource(&requesterTransport->source());
  result.load(finished->store.string());
  const auto graph =
      result.rebuildManifold(finished->merged.front().promoted.version);
  const auto rootCell = finished->merged.front().promoted.cells.front();
  const auto along    = [&](CellRef from, std::string_view name) {
    for (const auto &edge : graph.dimensionsOf(from))
      if (graph.textOf(edge.dim, result) == name) return edge.pos;
    return zigzag::noCell;
  };
  const auto record = along(rootCell, "d.mesh.items");
  ASSERT_NE(record, zigzag::noCell);
  const auto value = along(record, "d.mesh.value");
  ASSERT_NE(value, zigzag::noCell);
  EXPECT_EQ(graph.asInt64(value), 3);
  EXPECT_EQ(finished->verifiedWorkers, 1U);
}

} // namespace
} // namespace zigzag::vortex::mesh
