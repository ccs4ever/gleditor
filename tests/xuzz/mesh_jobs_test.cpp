#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include "common/xanadu/resolver.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/mesh_jobs.hpp"

namespace zigzag::vortex::mesh {
namespace {
constexpr EvaluationBudget limits{1'000'000, 100'000, 100, 1'000'000};
constexpr std::uint64_t now = 1'800'000'000;

class MeshJobsTest : public testing::Test {
protected:
  xanadu::MutableKeys requesterKeys = xanadu::createMutableKeys();
  xanadu::MutableKeys workerKeys    = xanadu::createMutableKeys();
  xanadu::identity::Fingerprint peer =
      *xanadu::identity::Fingerprint::fromString(std::string(40, 'A'));
  NodeId requester{peer.toString(), requesterKeys.publicKey.hex(), 1};
  NodeId worker{std::string(40, 'B'), workerKeys.publicKey.hex(), 3};
  std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("xanadu-mesh-jobs-" + workerKeys.publicKey.hex());
  std::shared_ptr<xanadu::DirectoryContentSource> source =
      std::make_shared<xanadu::DirectoryContentSource>();
  std::atomic<unsigned> seeds{0};
  ~MeshJobsTest() override { std::filesystem::remove_all(directory); }

  ImmutableJob job(Frontend frontend        = Frontend::Vpl,
                   std::string_view program = "1 + 2") {
    auto seed = publishProgram(program, directory);
    if (!seed) throw std::runtime_error(seed.error());
    source->add(seed->torrent.file, seed->path.string());
    return {.program   = seed->torrent.hash,
            .frontend  = frontend,
            .base      = {},
            .parent    = {},
            .budget    = limits,
            .requester = requester};
  }
  DelegatedKeyVerifier delegation() {
    return [this](const xanadu::identity::Fingerprint &fp)
               -> std::optional<xanadu::PublicKey> {
      if (fp == peer) return requesterKeys.publicKey;
      return std::nullopt;
    };
  }
  JobAuthorizer authorizer() {
    return [this](const ImmutableJob &, const CapabilityGrant &grant) {
      return grant.issuer == requesterKeys.publicKey;
    };
  }
  CapabilityGrant grant(const ImmutableJob &j) {
    CapabilityGrant g{.job       = jobHash(j),
                      .requester = requester,
                      .worker    = worker,
                      .expires   = now + 60,
                      .budget    = limits,
                      .issuer    = requesterKeys.publicKey};
    g.proof     = {.resource  = g.job,
                   .timestamp = now,
                   .nonce = *xanadu::identity::HashcashEngine::mint(g.job, now, 4),
                   .difficultyBits = 4};
    g.signature = xanadu::signMutableItem(grantSigningBuffer(g), requesterKeys);
    return g;
  }
  JobWorkerOptions options() {
    JobWorkerOptions o;
    o.node               = worker;
    o.keys               = workerKeys;
    o.source             = source;
    o.seedRoot           = directory;
    o.budget             = limits;
    o.permascroll        = std::make_shared<xanadu::UserPermascroll>();
    o.hashcashDifficulty = 4;
    o.delegation         = delegation();
    o.authorize          = authorizer();
    o.provenance         = [](xanadu::Store &store)
        -> std::expected<xanadu::SignedProvenance, std::string> {
      if (store.opCount() == 0)
        return std::unexpected("provenance requested before promotion");
      return xanadu::SignedProvenance{"fixture authorship",
                                      "fixture signature"};
    };
    o.primediaProvenance = [](xanadu::Store &, std::string_view bytes,
                              std::uint64_t, std::string_view scroll)
        -> std::expected<xanadu::SignedProvenance, std::string> {
      xanadu::Provenance record;
      record.author        = {"Mesh fixture", "mesh@test.invalid", {}};
      record.permascroll   = std::string(scroll);
      record.contentLength = bytes.size();
      record.contentDigest = xanadu::sha256Hex(bytes);
      return xanadu::SignedProvenance{record.toTsv(), "fixture signature"};
    };
    o.seed = [this](const ValueSeed &s) -> std::expected<void, std::string> {
      for (const auto &payload : s.payloads)
        source->add(payload.torrent.file, payload.path.string());
      source->add(
          s.history.torrentFile,
          (s.carrierPath.parent_path() / s.history.hash.hex()).string());
      source->add(s.carrier.file, s.carrierPath.string());
      ++seeds;
      return {};
    };
    return o;
  }
  FutureSnapshot finish(FutureScheduler &scheduler, const std::string &hash) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
      const auto state = scheduler.poll(hash);
      if (state && (state->state == FutureState::Succeeded ||
                    state->state == FutureState::Failed))
        return *state;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    throw std::runtime_error("future did not settle within test deadline");
  }
};

TEST_F(MeshJobsTest, QueuedCapabilitiesExpireBeforeExecution) {
  auto first    = job();
  auto second   = job(Frontend::Vpl, "2 + 3");
  auto settings = options();
  std::atomic<std::int64_t> elapsed{0};
  settings.monotonicNow = [&] {
    return std::chrono::steady_clock::time_point(
        std::chrono::seconds(elapsed.load()));
  };
  std::promise<void> seeding;
  std::promise<void> release;
  auto released     = release.get_future().share();
  auto originalSeed = settings.seed;
  settings.seed =
      [&](const ValueSeed &value) -> std::expected<void, std::string> {
    seeding.set_value();
    released.wait();
    return originalSeed(value);
  };
  FutureScheduler scheduler(std::move(settings));
  auto running = scheduler.submit(first, grant(first), peer, now);
  ASSERT_TRUE(running);
  if (seeding.get_future().wait_for(std::chrono::seconds(10)) !=
      std::future_status::ready) {
    release.set_value();
    FAIL() << "first job did not reach seeding";
  }
  auto queued = scheduler.submit(second, grant(second), peer, now);
  elapsed.store(60);
  release.set_value();
  ASSERT_TRUE(queued);
  EXPECT_EQ(finish(scheduler, *running).state, FutureState::Succeeded);
  const auto expired = finish(scheduler, *queued);
  EXPECT_EQ(expired.state, FutureState::Failed);
  EXPECT_FALSE(expired.result);
  EXPECT_EQ(seeds.load(), 1);
}

TEST_F(MeshJobsTest,
       CanonicalJobIdentityBindsProgramBaseParentFrontendBudgetAndRequester) {
  auto original = job();
  auto decoded  = decodeJob(encodeJob(original));
  ASSERT_TRUE(decoded) << decoded.error();
  EXPECT_EQ(jobHash(*decoded), jobHash(original));
  auto changed     = original;
  changed.frontend = Frontend::Vlace;
  EXPECT_NE(jobHash(changed), jobHash(original));
  changed = original;
  changed.budget.maxCycles--;
  EXPECT_NE(jobHash(changed), jobHash(original));
  changed = original;
  changed.requester.arenaEpoch++;
  EXPECT_NE(jobHash(changed), jobHash(original));
  changed        = original;
  changed.base   = original.program;
  changed.parent = xanadu::MicroversionId::parse("1");
  EXPECT_NE(jobHash(changed), jobHash(original));
  EXPECT_FALSE(decodeJob(encodeJob(original) + "junk"));
  changed        = original;
  changed.parent = xanadu::MicroversionId::parse("1");
  EXPECT_FALSE(decodeJob(encodeJob(changed)));
  changed                 = original;
  changed.budget.maxCells = 0;
  EXPECT_FALSE(decodeJob(encodeJob(changed)));
}

TEST_F(
    MeshJobsTest,
    CapabilityRequiresExactJobRealSignatureDelegationIssuerTrustExpiryAndBudget) {
  auto j = job();
  auto g = grant(j);
  ASSERT_TRUE(decodeGrant(encodeGrant(g)));
  EXPECT_TRUE(verifyGrant(j, g, worker, peer, delegation(), authorizer(), now));
  EXPECT_FALSE(verifyGrant(j, g, worker, peer, {}, authorizer(), now));
  EXPECT_FALSE(verifyGrant(j, g, worker, peer, delegation(), {}, now));
  EXPECT_FALSE(
      verifyGrant(j, g, worker, peer, delegation(), authorizer(), g.expires));
  auto changed = g;
  changed.signature.bytes[0] ^= 1;
  EXPECT_FALSE(
      verifyGrant(j, changed, worker, peer, delegation(), authorizer(), now));
  changed        = g;
  changed.job[0] = changed.job[0] == 'a' ? 'b' : 'a';
  EXPECT_FALSE(
      verifyGrant(j, changed, worker, peer, delegation(), authorizer(), now));
  changed                 = g;
  changed.budget.maxCells = 1;
  changed.signature =
      xanadu::signMutableItem(grantSigningBuffer(changed), requesterKeys);
  EXPECT_FALSE(
      verifyGrant(j, changed, worker, peer, delegation(), authorizer(), now));
  auto wrongWorker = worker;
  wrongWorker.arenaEpoch++;
  EXPECT_FALSE(
      verifyGrant(j, g, wrongWorker, peer, delegation(), authorizer(), now));
  EXPECT_FALSE(verifyGrant(
      j, g, worker, peer,
      [this](auto) -> std::optional<xanadu::PublicKey> {
        return workerKeys.publicKey;
      },
      authorizer(), now));
  changed        = g;
  changed.issuer = workerKeys.publicKey;
  changed.signature =
      xanadu::signMutableItem(grantSigningBuffer(changed), workerKeys);
  EXPECT_FALSE(
      verifyGrant(j, changed, worker, peer, delegation(), authorizer(), now));
}

TEST_F(MeshJobsTest,
       EveryFrontendFetchesImmutableSourceAndSeedsAnIdempotentJobBoundResult) {
  const std::pair<Frontend, std::string_view> programs[] = {
      {Frontend::Vpl, "1 + 2"},
      {Frontend::Vlace, "1 + 2"},
      {Frontend::Vquery, "##/d.remote%\"three\""},
      {Frontend::Vprolog, "answer(3). ?- answer(X)."}};
  for (const auto &[frontend, program] : programs) {
    SCOPED_TRACE(static_cast<unsigned>(frontend));
    auto j = job(frontend, program);
    auto g = grant(j);
    FutureScheduler scheduler(options());
    auto accepted = scheduler.submit(j, g, peer, now);
    ASSERT_TRUE(accepted) << accepted.error();
    EXPECT_EQ(*scheduler.submit(j, g, peer, now), *accepted);
    auto settled = finish(scheduler, *accepted);
    ASSERT_EQ(settled.state, FutureState::Succeeded) << settled.error;
    ASSERT_TRUE(settled.result);
    ASSERT_TRUE(settled.value);
    EXPECT_EQ(settled.value->job, *accepted);
    auto received = fetchValue(*settled.result, *source,
                               std::make_shared<xanadu::UserPermascroll>());
    ASSERT_TRUE(received) << received.error();
    EXPECT_EQ(received->value.job, *accepted);
    const auto before = seeds.load();
    EXPECT_EQ(*scheduler.submit(j, g, peer, now), *accepted);
    EXPECT_EQ(seeds.load(), before);
    EXPECT_EQ(scheduler.poll(*accepted)->result, settled.result);
  }
  EXPECT_EQ(seeds.load(), 4U);
}

TEST_F(
    MeshJobsTest,
    UnknownProgramsFailRatherThanPublishingEmptyAnswersAndDedupRemainsBounded) {
  auto j = job();
  j.program.bytes[0] ^= 1;
  auto g        = grant(j);
  auto o        = options();
  o.maxRetained = 1;
  FutureScheduler scheduler(std::move(o));
  auto accepted = scheduler.submit(j, g, peer, now);
  ASSERT_TRUE(accepted) << accepted.error();
  auto settled = finish(scheduler, *accepted);
  EXPECT_EQ(settled.state, FutureState::Failed);
  EXPECT_FALSE(settled.error.empty());
  EXPECT_EQ(seeds.load(), 0U);
  auto next = j;
  next.requester.arenaEpoch++;
  auto nextGrant      = grant(next);
  nextGrant.requester = next.requester;
  nextGrant.signature =
      xanadu::signMutableItem(grantSigningBuffer(nextGrant), requesterKeys);
  EXPECT_FALSE(scheduler.submit(next, nextGrant, peer, now));
  EXPECT_EQ(*scheduler.submit(j, g, peer, now), *accepted);
  EXPECT_TRUE(scheduler.forget(*accepted));
  EXPECT_FALSE(scheduler.poll(*accepted));
}

TEST_F(MeshJobsTest,
       AdmissionRefusesMissingPublicationPolicyAndInvalidProofBeforeWork) {
  auto j      = job();
  auto g      = grant(j);
  auto o      = options();
  o.authorize = {};
  FutureScheduler refused(std::move(o));
  EXPECT_FALSE(refused.submit(j, g, peer, now));
  o      = options();
  o.seed = {};
  FutureScheduler unseeded(std::move(o));
  EXPECT_FALSE(unseeded.submit(j, g, peer, now));
  o                    = options();
  o.hashcashDifficulty = 255;
  FutureScheduler proof(std::move(o));
  EXPECT_FALSE(proof.submit(j, g, peer, now));
  EXPECT_EQ(seeds.load(), 0U);
}
TEST_F(MeshJobsTest,
       FetchRunsInBackgroundWhilePollingAndQueueAdmissionStayResponsive) {
  class GateSource final : public xanadu::ContentSource {
  public:
    std::shared_ptr<xanadu::DirectoryContentSource> backing;
    mutable std::promise<void> entered;
    std::shared_future<void> release;
    mutable std::atomic<bool> signalled{false};
    gleditor::cpp26::optional<const xanadu::Metainfo &>
    metainfo(const xanadu::InfoHash &hash) const override {
      if (!signalled.exchange(true)) entered.set_value();
      release.wait_for(std::chrono::seconds(3));
      return backing->metainfo(hash);
    }
    std::string readStream(const xanadu::InfoHash &hash, std::uint64_t offset,
                           std::uint64_t length) const override {
      return backing->readStream(hash, offset, length);
    }
  };
  auto j        = job();
  auto g        = grant(j);
  auto gate     = std::make_shared<GateSource>();
  gate->backing = source;
  std::promise<void> release;
  gate->release = release.get_future().share();
  auto entered  = gate->entered.get_future();
  auto o        = options();
  o.source      = gate;
  o.maxQueued   = 0;
  {
    FutureScheduler full(o);
    EXPECT_FALSE(full.submit(j, g, peer, now));
  }
  o.maxQueued = 1;
  FutureScheduler scheduler(o);
  // Release on every assertion exit before joining the scheduler thread.
  struct Release {
    std::promise<void> &promise;
    ~Release() { promise.set_value(); }
  } releaseGuard{release};
  const auto started = std::chrono::steady_clock::now();
  auto submitted     = scheduler.submit(j, g, peer, now);
  ASSERT_TRUE(submitted) << submitted.error();
  EXPECT_LT(std::chrono::steady_clock::now() - started,
            std::chrono::seconds(1));
  ASSERT_EQ(entered.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
  const auto current = scheduler.poll(*submitted);
  ASSERT_TRUE(current);
  EXPECT_EQ(current->state, FutureState::Running);
  EXPECT_EQ(*scheduler.submit(j, g, peer, now), *submitted);
  auto second = j;
  second.requester.arenaEpoch++;
  auto secondGrant      = grant(second);
  secondGrant.requester = second.requester;
  secondGrant.signature =
      xanadu::signMutableItem(grantSigningBuffer(secondGrant), requesterKeys);
  ASSERT_TRUE(scheduler.submit(second, secondGrant, peer, now));
  auto third = second;
  third.requester.arenaEpoch++;
  auto thirdGrant      = grant(third);
  thirdGrant.requester = third.requester;
  thirdGrant.signature =
      xanadu::signMutableItem(grantSigningBuffer(thirdGrant), requesterKeys);
  EXPECT_FALSE(scheduler.submit(third, thirdGrant, peer, now));
  EXPECT_FALSE(scheduler.forget(*submitted));
}

TEST_F(MeshJobsTest, BaseSnapshotRequiresTheExplicitImmutableParent) {
  auto j = job(Frontend::Vquery, "##/d.remote%\"derived\"");
  xanadu::Store base;
  ArenaManifold arena;
  auto root = arena.makeCell("base snapshot");
  auto published =
      publishValue(base, {}, arena, root, workerKeys, worker,
                   xanadu::SignedProvenance{"fixture", "signature"}, directory);
  ASSERT_TRUE(published) << published.error();
  for (const auto &payload : published->payloads)
    source->add(payload.torrent.file, payload.path.string());
  source->add(published->history.torrentFile,
              (directory / published->history.hash.hex()).string());
  source->add(published->carrier.file, published->carrierPath.string());
  j.base   = published->carrier.hash;
  j.parent = published->value.publication.version;
  FutureScheduler scheduler(options());
  auto accepted = scheduler.submit(j, grant(j), peer, now);
  ASSERT_TRUE(accepted) << accepted.error();
  auto completed = finish(scheduler, *accepted);
  ASSERT_EQ(completed.state, FutureState::Succeeded) << completed.error;
  EXPECT_EQ(completed.value->parent, j.parent);
  auto mismatch   = j;
  mismatch.parent = {};
  auto refused    = scheduler.submit(mismatch, grant(mismatch), peer, now);
  ASSERT_TRUE(refused) << refused.error();
  auto failed = finish(scheduler, *refused);
  EXPECT_EQ(failed.state, FutureState::Failed);
  EXPECT_NE(failed.error.find("parent mismatch"), std::string::npos);
}

} // namespace
} // namespace zigzag::vortex::mesh
