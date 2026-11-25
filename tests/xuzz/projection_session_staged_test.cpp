/**
 * @file projection_session_staged_test.cpp
 * @brief Staged ProjectionSession publication over the lens IoAdapters
 *        (design/vortex-io-implementation-plan.md §6, §13 M4 row):
 *        prepareRefresh/prepareCommit stage a bounded op on the owner,
 *        the adapter's worker runs it off-owner, and publishRefresh/
 *        publishCommit consume the terminal OutcomeCell on the owner --
 *        with the in-flight slot, dirty-edit retention and rebind rules
 *        commit()/refresh() already keep.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#include <unistd.h>

#include "common/xanadu/vortex/arena_io_cell_store.hpp"
#include "common/xanadu/vortex/io_scheduler.hpp"
#include "common/xanadu/vortex/io_service.hpp"
#include "common/xanadu/vortex/lens_adapters.hpp"
#include "common/xanadu/zigzag/config_lens.hpp"
#include "common/xanadu/zigzag/projection_session.hpp"

namespace {
using namespace xanadu::vortex;
namespace fs = std::filesystem;
using zigzag::ProjectionSession;

constexpr AdapterKey lensKey{20, 1};

std::uint32_t configFileSeq{};

fs::path writeConfigFile(std::string_view content) {
  const auto path = fs::temp_directory_path() /
                    fs::path("io_session_staged_" + std::to_string(::getpid()) +
                             "_" + std::to_string(configFileSeq++) + ".json");
  std::ofstream out(path, std::ios::trunc);
  out << content;
  out.close();
  return path;
}

std::string readFile(const fs::path &path) {
  std::ifstream in(path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

/// The arena the session projects into is also the arena the I/O cells
/// live in: outcome cells minted by the service (or by mintTaggedOutcome
/// below) are read back through ioOutcomeTag on this same manifold.
struct StagedFixture {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  IoCellSchema schema{makeIoCellSchema(arena).value()};
  EndpointCell endpoint{makeEndpointCell(arena, schema).value()};
  CapabilityCell capability{
      makeCapabilityCell(arena, schema, endpoint).value()};
  PayloadResolver payloads;
  ArenaIoCellStore cells{arena, schema, payloads,
                         [](CapabilityCell cap, OwnerCell spec)
                             -> std::expected<CellRequest, IoErrorInfo> {
                           return CellRequest{.key                  = lensKey,
                                              .capability           = cap,
                                              .completionBoundBytes = 4096,
                                              .payloadBoundBytes    = 4096,
                                              .specification        = spec};
                         }};
};

/// A file-backed ConfigLens session + its staged adapter + a live
/// IoService, all sharing one arena. The adapter wraps the session's
/// lens by reference, so it must be emplaced only after the session
/// owns the lens.
struct SessionStack {
  StagedFixture fixture;
  fs::path path;
  std::unique_ptr<zigzag::ConfigLens> lens;
  zigzag::ConfigLens *lensRaw{nullptr};
  std::unique_ptr<ProjectionSession> session;
  std::optional<ConfigLensSourceAdapter> adapter;
  std::unique_ptr<BoundedIoScheduler> scheduler;
  std::unique_ptr<IoService> service;

  explicit SessionStack(std::string_view json) {
    path    = writeConfigFile(json);
    lens    = std::make_unique<zigzag::ConfigLens>(path.string());
    lensRaw = lens.get();
    session =
        std::make_unique<ProjectionSession>(std::move(lens), fixture.core);
    adapter.emplace(*lensRaw);
    scheduler = std::make_unique<BoundedIoScheduler>(
        BoundedIoSchedulerOptions{2, 8, 1 << 20, 1 << 20}, fixture.payloads);
    service = std::make_unique<IoService>(
        IoBudgetSnapshot{8, 1 << 20, 1 << 20, 1 << 20, 8}, std::move(scheduler),
        fixture.payloads, fixture.cells);
    service->registerAdapter(lensKey, *adapter);
  }
  ~SessionStack() {
    if (service) {
      static_cast<void>(service->requestShutdown(ShutdownMode::CancelPending));
    }
    fs::remove(path);
    fs::remove(path.string() + ".tmp");
  }
  SessionStack(const SessionStack &)                     = delete;
  auto operator=(const SessionStack &) -> SessionStack & = delete;
};

/// Pump the service until the request's terminal outcome materializes.
std::optional<OutcomeCell> awaitOutcome(IoService &service,
                                        const RequestCell request) {
  for (int i = 0; i < 400; ++i) {
    if (const auto poll = service.poll(request); poll.outcome.has_value()) {
      return *poll.outcome;
    }
    static_cast<void>(service.pump(8));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return std::nullopt;
}

/// Submit @p stagingKey through the service and return the terminal cell.
/// A pre-admission refusal still mints an OutcomeCell (Rejected) that the
/// session's publish* consumes exactly like a terminal one.
std::optional<OutcomeCell> submitAndAwait(IoService &service,
                                          const CapabilityCell capability,
                                          const std::uint32_t stagingKey) {
  const auto submitted = service.submit(capability, stagingKey);
  if (!std::holds_alternative<Admitted>(submitted.value)) {
    return std::get<Refused>(submitted.value).outcome;
  }
  return awaitOutcome(service, std::get<Admitted>(submitted.value).request);
}

/// Mint a tagged outcome cell directly, for publish* paths that do not
/// need the worker round-trip (refusals, guards, kind mismatches). A mint
/// failure yields an empty handle the publish* then refuses as "not a
/// live outcome" -- the EXPECT still flags the real cause.
OutcomeCell mintTaggedOutcome(StagedFixture &fixture, IoOutcomeTag tag) {
  std::optional<RequestCell> request;
  if (tag != IoOutcomeTag::Rejected) {
    auto minted = makeOpenRequestCell(fixture.arena);
    EXPECT_TRUE(minted.has_value());
    request = minted.value_or(RequestCell{});
  }
  auto cell = makeOutcomeCell(fixture.arena, fixture.schema, request, tag);
  EXPECT_TRUE(cell.has_value());
  return cell.value_or(OutcomeCell{});
}

// -- the full §6 path: stage -> submit -> worker -> publish ---------------

TEST(ProjectionSessionStagedTest,
     StagedRefreshPublishesTheOwnerSideProjection) {
  SessionStack stack("{\"name\":\"old\"}\n");
  auto &session      = *stack.session;
  const auto nameKey = "cfg:" + stack.path.string() + ":/name";
  ASSERT_TRUE(session.cellFor(nameKey).has_value());

  const auto staged = session.prepareRefresh(*stack.adapter);
  ASSERT_TRUE(staged.has_value()) << staged.error();

  const auto outcome =
      submitAndAwait(*stack.service, stack.fixture.capability, *staged);
  ASSERT_TRUE(outcome.has_value());

  // An external writer lands between admission and owner publication:
  // publishRefresh's refresh() is what re-reads the file.
  {
    std::ofstream out(stack.path, std::ios::trunc);
    out << "{\"name\":\"old\",\"extra\":true}\n";
  }

  const auto published = session.publishRefresh(*outcome);
  ASSERT_TRUE(published.has_value()) << published.error();
  const auto extraKey = "cfg:" + stack.path.string() + ":/extra";
  EXPECT_TRUE(session.cellFor(extraKey).has_value());
}

TEST(ProjectionSessionStagedTest,
     StagedCommitAppliesEditsOffOwnerAndRestatesOnPublish) {
  SessionStack stack("{\"name\":\"old\",\"count\":1}\n");
  auto &session       = *stack.session;
  const auto nameKey  = "cfg:" + stack.path.string() + ":/name";
  const auto countKey = "cfg:" + stack.path.string() + ":/count";
  ASSERT_TRUE(session.cellFor(nameKey).has_value());
  ASSERT_TRUE(session.cellFor(countKey).has_value());

  ASSERT_TRUE(session.editContent(nameKey, "newname"));
  ASSERT_TRUE(session.editContent(countKey, "41"));
  EXPECT_EQ(session.dirtyCount(), 2U);

  const auto staged = session.prepareCommit(*stack.adapter);
  ASSERT_TRUE(staged.has_value()) << staged.error();

  const auto outcome =
      submitAndAwait(*stack.service, stack.fixture.capability, *staged);
  ASSERT_TRUE(outcome.has_value());

  const auto published = session.publishCommit(*outcome);
  ASSERT_TRUE(published.has_value()) << published.error();
  EXPECT_EQ(*published, 2);
  EXPECT_EQ(session.dirtyCount(), 0U);

  // The worker's applyContentChange rewrote the file; publishCommit's
  // refresh() restated the cells from it.
  const std::string text = readFile(stack.path);
  EXPECT_NE(text.find("\"newname\""), std::string::npos);
  EXPECT_NE(text.find("\"count\": 41"), std::string::npos);
  const auto nameCell = session.cellFor(nameKey);
  ASSERT_TRUE(nameCell.has_value());
  EXPECT_EQ(stack.fixture.arena.textOf(*nameCell), "newname");
}

TEST(ProjectionSessionStagedTest,
     RefusedCommitEditStaysDirtyAndTheTailWaitsForRetry) {
  SessionStack stack("{\"name\":\"old\",\"count\":1}\n");
  auto &session       = *stack.session;
  const auto nameKey  = "cfg:" + stack.path.string() + ":/name";
  const auto countKey = "cfg:" + stack.path.string() + ":/count";

  // "notanint" passes canApply (a cfg.val member) but fails the lens's
  // JSON-integer coercion inside applyContentChange -- the worker-side
  // refusal commit()'s per-cell rule produces.
  ASSERT_TRUE(session.editContent(nameKey, "first"));
  ASSERT_TRUE(session.editContent(countKey, "notanint"));

  const auto staged = session.prepareCommit(*stack.adapter);
  ASSERT_TRUE(staged.has_value()) << staged.error();
  const auto outcome =
      submitAndAwait(*stack.service, stack.fixture.capability, *staged);
  ASSERT_TRUE(outcome.has_value());

  const auto published = session.publishCommit(*outcome);
  EXPECT_FALSE(published.has_value());
  // Every edit stays dirty: the batch stops at the first refusal and
  // the un-attempted tail was never applied.
  EXPECT_EQ(session.dirtyCount(), 2U);

  // The file was not written by the refused batch.
  EXPECT_EQ(readFile(stack.path).find("first"), std::string::npos);

  // Fix the bad edit and commit again: the retry carries both keys.
  ASSERT_TRUE(session.editContent(countKey, "7"));
  const auto restaged = session.prepareCommit(*stack.adapter);
  ASSERT_TRUE(restaged.has_value()) << restaged.error();
  const auto outcome2 =
      submitAndAwait(*stack.service, stack.fixture.capability, *restaged);
  ASSERT_TRUE(outcome2.has_value());
  const auto published2 = session.publishCommit(*outcome2);
  ASSERT_TRUE(published2.has_value()) << published2.error();
  EXPECT_EQ(*published2, 2);
  EXPECT_EQ(session.dirtyCount(), 0U);
  EXPECT_NE(readFile(stack.path).find("\"count\": 7"), std::string::npos);
}

// -- owner-side rules that need no worker ---------------------------------

TEST(ProjectionSessionStagedTest, OneStagedOpInFlightAtATime) {
  SessionStack stack("{\"name\":\"old\"}\n");
  auto &session      = *stack.session;
  const auto nameKey = "cfg:" + stack.path.string() + ":/name";
  ASSERT_TRUE(session.editContent(nameKey, "x"));

  const auto refresh = session.prepareRefresh(*stack.adapter);
  ASSERT_TRUE(refresh.has_value());
  // A second prepare* refuses while the first is still in flight.
  EXPECT_FALSE(session.prepareCommit(*stack.adapter).has_value());
  EXPECT_FALSE(session.prepareRefresh(*stack.adapter).has_value());

  // A commit outcome cannot publish against the refresh slot.
  const auto commitCell =
      mintTaggedOutcome(stack.fixture, IoOutcomeTag::Complete);
  EXPECT_FALSE(session.publishCommit(commitCell).has_value());
  // ...and the refresh slot is still held for its own outcome.
  const auto refreshCell =
      mintTaggedOutcome(stack.fixture, IoOutcomeTag::Complete);
  const auto published = session.publishRefresh(refreshCell);
  ASSERT_TRUE(published.has_value()) << published.error();
}

TEST(ProjectionSessionStagedTest, NonCompleteOutcomesChangeNothing) {
  SessionStack stack("{\"name\":\"old\",\"count\":1}\n");
  auto &session       = *stack.session;
  const auto countKey = "cfg:" + stack.path.string() + ":/count";
  ASSERT_TRUE(session.editContent(countKey, "9"));

  // A Stale-tagged outcome on a staged commit: typed refusal, edits stay.
  const auto staged = session.prepareCommit(*stack.adapter);
  ASSERT_TRUE(staged.has_value());
  const auto stale = mintTaggedOutcome(stack.fixture, IoOutcomeTag::Stale);
  EXPECT_FALSE(session.publishCommit(stale).has_value());
  EXPECT_EQ(session.dirtyCount(), 1U);

  // The slot freed: a fresh commit can stage and publish. This Complete
  // tag stands in for an outcome whose worker never ran the batch, so
  // every edit reports unapplied -- publishCommit counts zero applied
  // yet still frees the slot and refreshes.
  const auto restaged = session.prepareCommit(*stack.adapter);
  ASSERT_TRUE(restaged.has_value());
  const auto complete =
      mintTaggedOutcome(stack.fixture, IoOutcomeTag::Complete);
  const auto published = session.publishCommit(complete);
  ASSERT_TRUE(published.has_value()) << published.error();
  EXPECT_EQ(*published, 0);
  EXPECT_EQ(session.dirtyCount(), 1U);
}

TEST(ProjectionSessionStagedTest, PublishWithoutStagedOpRefuses) {
  SessionStack stack("{\"name\":\"old\"}\n");
  const auto complete =
      mintTaggedOutcome(stack.fixture, IoOutcomeTag::Complete);
  EXPECT_FALSE(stack.session->publishRefresh(complete).has_value());
  EXPECT_FALSE(stack.session->publishCommit(complete).has_value());
}

TEST(ProjectionSessionStagedTest, PrepareCommitWithNoDirtyEditsRefuses) {
  SessionStack stack("{\"name\":\"old\"}\n");
  EXPECT_FALSE(stack.session->prepareCommit(*stack.adapter).has_value());
}

TEST(ProjectionSessionStagedTest,
     FailedCommitOutcomeStillConsumesTheAppliedPrefix) {
  // The worker's performCommit stops at the first refusal and reports a
  // Failed outcome -- but edits sorted before the refusal DID reach the
  // source. publishCommit must consume their applied flags on the Failed
  // tag exactly as it does on Complete, or the next commit() re-sends
  // text the source already holds.
  SessionStack stack("{\"aaa\":1,\"zzz\":1}\n");
  auto &session     = *stack.session;
  const auto aaaKey = "cfg:" + stack.path.string() + ":/aaa";
  const auto zzzKey = "cfg:" + stack.path.string() + ":/zzz";
  ASSERT_TRUE(session.cellFor(aaaKey).has_value());
  ASSERT_TRUE(session.cellFor(zzzKey).has_value());

  // Sorted order puts /aaa first: it applies, then /zzz's non-numeric
  // text fails the lens's integer coercion.
  ASSERT_TRUE(session.editContent(aaaKey, "10"));
  ASSERT_TRUE(session.editContent(zzzKey, "junk"));

  const auto staged = session.prepareCommit(*stack.adapter);
  ASSERT_TRUE(staged.has_value()) << staged.error();
  const auto outcome =
      submitAndAwait(*stack.service, stack.fixture.capability, *staged);
  ASSERT_TRUE(outcome.has_value());

  const auto published = session.publishCommit(*outcome);
  EXPECT_FALSE(published.has_value()); // Failed outcome: typed refusal
  // /aaa reached the source and left the dirty set; only /zzz stays.
  EXPECT_EQ(session.dirtyCount(), 1U);
  EXPECT_NE(readFile(stack.path).find("\"aaa\": 10"), std::string::npos);
}

TEST(ProjectionSessionStagedTest,
     DeadOutcomeCellDoesNotConsumeTheStagedCommit) {
  SessionStack stack("{\"name\":\"old\"}\n");
  auto &session      = *stack.session;
  const auto nameKey = "cfg:" + stack.path.string() + ":/name";
  ASSERT_TRUE(session.cellFor(nameKey).has_value());
  ASSERT_TRUE(session.editContent(nameKey, "newname"));

  const auto staged = session.prepareCommit(*stack.adapter);
  ASSERT_TRUE(staged.has_value());

  // A live cell that is not an outcome is a caller error: publishCommit
  // must refuse WITHOUT consuming the staged batch -- dropping it here
  // would lose the worker's per-edit applied results with no way to
  // publish against the real outcome later.
  const auto plain            = stack.fixture.arena.makeCell("not an outcome");
  const auto plainIncarnation = stack.fixture.arena.incarnationOf(plain);
  ASSERT_TRUE(plainIncarnation.has_value());
  const auto wrongTag =
      OutcomeCell{.cell = plain, .incarnation = *plainIncarnation};
  EXPECT_FALSE(session.publishCommit(wrongTag).has_value());
  EXPECT_EQ(session.dirtyCount(), 1U);

  // The staged slot survived: the real terminal outcome still publishes
  // and applies the batch.
  const auto outcome =
      submitAndAwait(*stack.service, stack.fixture.capability, *staged);
  ASSERT_TRUE(outcome.has_value());
  const auto published = session.publishCommit(*outcome);
  ASSERT_TRUE(published.has_value()) << published.error();
  EXPECT_EQ(*published, 1);
  EXPECT_EQ(session.dirtyCount(), 0U);
}

TEST(ProjectionSessionStagedTest, SyncMutatorsRefuseWhileAStagedOpIsInFlight) {
  SessionStack stack("{\"name\":\"old\"}\n");
  auto &session      = *stack.session;
  const auto nameKey = "cfg:" + stack.path.string() + ":/name";
  ASSERT_TRUE(session.editContent(nameKey, "pending"));

  const auto staged = session.prepareRefresh(*stack.adapter);
  ASSERT_TRUE(staged.has_value());

  // Every owner-side path that would touch the lens refuses: the worker
  // may be inside applyContentChange/pollChanged right now.
  EXPECT_FALSE(session.editContent(nameKey, "x").has_value());
  EXPECT_FALSE(session.commit().has_value());
  EXPECT_FALSE(session.revertKey(nameKey).has_value());
  EXPECT_FALSE(session.refresh().has_value());
  EXPECT_FALSE(session.refreshIfChanged());
  EXPECT_EQ(session.dirtyCount(), 1U); // the pending edit survived

  // Consuming the terminal outcome frees the slot.
  const auto cell = mintTaggedOutcome(stack.fixture, IoOutcomeTag::Complete);
  ASSERT_TRUE(session.publishRefresh(cell).has_value());
  EXPECT_TRUE(session.editContent(nameKey, "x").has_value());
}

} // namespace
