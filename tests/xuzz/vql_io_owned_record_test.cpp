// M7-VQL-c C1b: the owned record (design/vortex-io-m7-vql-c.md §12.1). An
// outcome a script names with `as $r` is copied into a script-owned record and
// its service request is retired at once, so the number of named outcomes is
// not capped by the request budget; the record's fields and payload read after
// the request is gone; retention is budgeted and degrades to the service's own
// outcome; and every lease is given back however the script ends.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

#include "common/xanadu/vortex/file_read_host.hpp"
#include "common/xanadu/vortex/owned_outcome.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/vql/script_runner.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "io_frontend_conformance.hpp"

namespace {

using namespace xuzz_conformance;
using xanadu::vql::Parser;
using xanadu::vql::ScriptIoPorts;
using xanadu::vql::ScriptRunner;
using xanadu::vql::ScriptRunResult;
using xanadu::vql::VQLEngine;

constexpr auto readOnCap =
    "effect { let $s := submit($cap, read_bytes(0, 16)); "
    "match $s { admitted($q) => await($q), refused($e) => $e } }";

constexpr IoProgress terminalProgress{.transferredBytes = 2,
                                      .phase            = IoPhase::Terminal};

/// A complete read that retained the harness's two bytes.
auto completeWithPayload(World &w) -> IoOutcome {
  return Complete{w.payloadRange(), {}, terminalProgress};
}

/// The fake-backend world with a runner the test can destroy, whose reads are
/// staged as the harness's plain specification cell.
struct OwnedWorld {
  World w;
  VQLEngine engine{w.core};
  std::vector<CellRef> specs;
  std::unique_ptr<ScriptRunner> runner;

  explicit OwnedWorld(
      const std::size_t retained =
          xanadu::vortex::FileReadHostOptions::defaultBudgetBytes) {
    ScriptIoPorts ports{w.bridge, w.host, w.payloads, w.diagnostics,
                        [this](CapabilityCell, std::uint64_t, std::uint64_t)
                            -> std::expected<CellRef, FrontendDiagnostic> {
                          return specs.emplace_back(w.specification());
                        }};
    ports.retainedPayloadBytes = retained;
    runner = std::make_unique<ScriptRunner>(engine, w.vm, std::move(ports));
    engine.setVariable("cap", w.capability.cell);
  }

  auto run(const std::string &source) -> ScriptRunResult {
    return runner->run(Parser(source).parseQuery());
  }
  /// Free every staged spec but the newest two, whose requests may be live.
  void freeOldSpecs() {
    constexpr std::size_t live = 2;
    while (specs.size() > live) {
      const auto spec = specs.front();
      specs.erase(specs.begin());
      if (const auto inc = w.arena.incarnationOf(spec))
        static_cast<void>(w.arena.freeCell(spec, *inc));
    }
  }
  auto wakeAndResume() -> ScriptRunResult {
    static_cast<void>(w.pump());
    auto woken = runner->takeWake();
    EXPECT_TRUE(woken.has_value());
    if (!woken) return ScriptRunResult::failed({"TEST_NO_WAKE", "no wake", {}});
    return runner->resume(*std::move(woken));
  }
  /// Complete the request the script is parked on with @p outcome and resume.
  auto finish(IoOutcome outcome) -> ScriptRunResult {
    w.complete(w.submissions() - 1, std::move(outcome));
    return wakeAndResume();
  }
  /// Run @p source, finishing each read it parks on with the next of
  /// @p outcomes; the rows it ends with.
  auto drive(const std::string &source, std::vector<IoOutcome> outcomes)
      -> std::vector<CellRef> {
    auto result = run(source);
    for (auto &outcome : outcomes) {
      if (!parked(result)) break;
      result = finish(std::move(outcome));
    }
    const auto *done = std::get_if<ScriptRunResult::Completed>(&result.value);
    EXPECT_NE(done, nullptr) << failure(result);
    return done ? done->value : std::vector<CellRef>{};
  }
  static auto parked(const ScriptRunResult &result) -> bool {
    return std::holds_alternative<ScriptRunResult::Parked>(result.value);
  }
  static auto failure(const ScriptRunResult &result) -> std::string {
    const auto *failed = std::get_if<ScriptRunResult::Failed>(&result.value);
    return failed ? failed->diagnostic.code + ": " + failed->diagnostic.message
                  : std::string{};
  }
  auto text(const CellRef cell) -> std::string { return w.arena.textOf(cell); }
  auto outcomeCell(const CellRef cell) -> OutcomeCell {
    return OutcomeCell{cell, *w.arena.incarnationOf(cell)};
  }
};

std::string namedReads(const std::size_t count) {
  std::string source;
  for (std::size_t i = 0; i < count; ++i)
    source += std::string{readOnCap} + " as $r" + std::to_string(i) + "; ";
  return source;
}

// ---- Criterion 14: named outcomes are not capped by slots ------------------

TEST(VqlIoOwnedRecordTest, AHundredNamedOutcomesInARowAreNotRefusedForSlots) {
  constexpr std::size_t named = 100; // the service holds 8 requests
  OwnedWorld s;
  auto result = s.run(namedReads(named) + "return io_tag($r0)");
  for (std::size_t i = 0; i < named; ++i) {
    ASSERT_TRUE(OwnedWorld::parked(result)) << i << OwnedWorld::failure(result);
    EXPECT_EQ(s.w.service.requestCount(), 1U)
        << "the service holds only the request now running (" << i << ")";
    result = s.finish(completeWithPayload(s.w));
  }
  const auto *done = std::get_if<ScriptRunResult::Completed>(&result.value);
  ASSERT_NE(done, nullptr) << OwnedWorld::failure(result);
  ASSERT_EQ(done->value.size(), 1U);
  EXPECT_EQ(s.text(done->value.front()), "Complete");
  EXPECT_EQ(s.w.service.requestCount(), 0U) << "every named request is retired";
  EXPECT_EQ(s.runner->ownedRecordCount(), named);
}

TEST(VqlIoOwnedRecordTest, EveryNamedOutcomeReadsLongAfterItsRequestIsGone) {
  constexpr std::size_t named = 40;
  OwnedWorld s;
  const auto rows = s.drive(
      namedReads(named) + "return io_tag($r0), io_tag($r39), io_bytes($r17)",
      [&] {
        std::vector<IoOutcome> outcomes;
        for (std::size_t i = 0; i < named; ++i)
          outcomes.emplace_back(completeWithPayload(s.w));
        return outcomes;
      }());
  ASSERT_EQ(rows.size(), 3U);
  EXPECT_EQ(s.text(rows[0]), "Complete");
  EXPECT_EQ(s.text(rows[1]), "Complete");
  EXPECT_EQ(s.text(rows[2]), "2");
}

TEST(VqlIoOwnedRecordTest, EveryFieldOfAnOutcomeReadsFromItsRecord) {
  struct Case {
    const char *name;
    IoOutcome outcome;
    const char *function;
    const char *expected;
  };
  const std::vector<Case> cases{
      {"Failed code",
       Failed{{IoErrorCode::BackendFailure, 3}, {}, terminalProgress},
       "io_error", "BackendFailure"},
      {"Failed detail",
       Failed{{IoErrorCode::BackendFailure, 3}, {}, terminalProgress},
       "io_detail", "3"},
      {"Failed tag",
       Failed{{IoErrorCode::BackendFailure, 3}, {}, terminalProgress}, "io_tag",
       "Failed"},
      {"Cancelled tag", Cancelled{{}, terminalProgress, true}, "io_tag",
       "Cancelled"},
      {"TimedOut bytes", TimedOut{{}, terminalProgress, true}, "io_bytes", "0"},
      {"EndOfStream tag", EndOfStream{terminalProgress}, "io_tag",
       "EndOfStream"},
  };
  for (const auto &test : cases) {
    SCOPED_TRACE(test.name);
    OwnedWorld s;
    const auto rows =
        s.drive(std::string{readOnCap} + " as $a; " + readOnCap +
                    " as $b; return " + test.function + "($a)",
                {test.outcome, Complete{{}, {}, terminalProgress}});
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(s.text(rows[0]), test.expected);
  }
}

TEST(VqlIoOwnedRecordTest, TheUncertainFlagAndProgressAreCopiedByValue) {
  OwnedWorld s;
  const auto rows = s.drive(std::string{readOnCap} + " as $a; " + readOnCap +
                                " as $b; return $a",
                            {TimedOut{{}, terminalProgress, true},
                             Complete{{}, {}, terminalProgress}});
  ASSERT_EQ(rows.size(), 1U);
  const auto record = s.outcomeCell(rows[0]);
  EXPECT_EQ(s.w.bridge.outcomeField(record, "uncertain"), "true");
  // The service publishes the progress a backend streams, not the one inside
  // its terminal result, so a fake backend's outcome reads zero here; the
  // record copies whatever the service's outcome held.
  EXPECT_EQ(s.w.bridge.outcomeField(record, "transferredBytes"), "0");
  EXPECT_EQ(s.w.bridge.outcomeField(record, "phase"), "Terminal");
  EXPECT_EQ(s.w.bridge.outcomeField(record, "owned"), "true");
}

TEST(VqlIoOwnedRecordTest, ARefusalIsCopiedAndItsErrorReadsFromTheRecord) {
  OwnedWorld s;
  s.w.refuseAdmission = true;
  const auto rows     = s.drive(
      std::string{readOnCap} + " as $a; return io_tag($a), io_error($a)", {});
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(s.text(rows[0]), "Rejected");
  EXPECT_EQ(s.text(rows[1]), "InvalidRequest");
  EXPECT_EQ(s.runner->ownedRecordCount(), 1U);
}

TEST(VqlIoOwnedRecordTest, TheRecordIsMarkedAndHasNoRoadBackToTheService) {
  OwnedWorld s;
  const auto rows = s.drive(std::string{readOnCap} + " as $a; return $a",
                            {completeWithPayload(s.w)});
  ASSERT_EQ(rows.size(), 1U);
  const auto &schema = s.w.bridge.schema();
  EXPECT_TRUE(xanadu::vortex::isOwnedOutcome(s.w.arena, schema, rows[0]));
  for (const auto which :
       {IoDimension::Request, IoDimension::Queue, IoDimension::Waiter,
        IoDimension::Clone, IoDimension::Lease, IoDimension::Payload}) {
    EXPECT_FALSE(s.w.arena.linked(rows[0], schema.dim(which), DimVector::POS))
        << dimensionName(which);
  }
  const std::vector<CellRef> roots{rows[0]};
  EXPECT_TRUE(auditLanguageValue(s.w.arena, schema, roots).empty());

  // The audit knows the marker: an owned record with an owner-only rank is a
  // finding, whatever else is true of it.
  const auto stray = s.w.arena.makeCell("stray");
  ASSERT_TRUE(s.w.arena.link(rows[0], schema.dim(IoDimension::Clone),
                             DimVector::POS, stray));
  const auto findings = auditLanguageValue(s.w.arena, schema, roots);
  ASSERT_FALSE(findings.empty());
  EXPECT_NE(findings.front().reason.find("owned outcome record"),
            std::string::npos);
}

TEST(VqlIoOwnedRecordTest, AServiceOutcomeIsNotARecord) {
  OwnedWorld s;
  const auto rows = s.drive(std::string{readOnCap}, {EndOfStream{{}}});
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_FALSE(
      xanadu::vortex::isOwnedOutcome(s.w.arena, s.w.bridge.schema(), rows[0]));
  EXPECT_EQ(s.runner->ownedRecordCount(), 0U) << "an unnamed outcome is kept";
}

// ---- Criterion 14: the payload through a record ---------------------------

TEST(VqlIoOwnedRecordTest, IoPayloadReadsThroughARecordAfterRetirement) {
  OwnedWorld s;
  const auto rows =
      s.drive(std::string{readOnCap} + " as $a; " + readOnCap + " as $b; " +
                  "return io_payload($a), io_bytes($a)",
              {completeWithPayload(s.w), Complete{{}, {}, terminalProgress}});
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(s.text(rows[0]), "ok");
  EXPECT_EQ(s.text(rows[1]), "2");
}

TEST(VqlIoOwnedRecordTest, TheStatusGateAppliesToARecordLikeAServiceOutcome) {
  // EndOfStream exposes no payload whatever the cell graph holds; a Failed
  // that kept bytes does.
  {
    OwnedWorld s;
    const auto rows =
        s.drive(std::string{readOnCap} + " as $a; return io_payload($a)",
                {EndOfStream{terminalProgress}});
    EXPECT_TRUE(rows.empty()) << "no payload row for a tag that carries none";
  }
  {
    OwnedWorld s;
    const auto rows =
        s.drive(std::string{readOnCap} + " as $a; return io_payload($a)",
                {Failed{{IoErrorCode::BackendFailure, 1},
                        s.w.payloadRange(),
                        terminalProgress}});
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(s.text(rows[0]), "ok");
  }
}

TEST(VqlIoOwnedRecordTest, ANamedFinalActionReturnsTheRecordAndItsPayload) {
  OwnedWorld s;
  const auto rows =
      s.drive(std::string{readOnCap} + " as $a", {completeWithPayload(s.w)});
  ASSERT_EQ(rows.size(), 2U) << "the outcome row, then its payload text";
  EXPECT_TRUE(
      xanadu::vortex::isOwnedOutcome(s.w.arena, s.w.bridge.schema(), rows[0]));
  EXPECT_EQ(s.text(rows[1]), "ok");
}

// ---- Criterion 15: retention is bounded and degrades by value -------------

TEST(VqlIoOwnedRecordTest, PastTheRetentionBudgetTheOutcomeStaysTheServices) {
  // Each payload chunk is two bytes: a budget of three holds one record.
  OwnedWorld s(3);
  auto result = s.run(std::string{readOnCap} + " as $a; " + readOnCap +
                      " as $b; " + readOnCap + " as $c; " +
                      "return io_payload($a), io_payload($b), io_tag($c)");
  ASSERT_TRUE(OwnedWorld::parked(result));
  result = s.finish(completeWithPayload(s.w));
  EXPECT_EQ(s.runner->ownedRecordCount(), 1U);
  EXPECT_EQ(s.runner->retainedBytes(), 2U);
  EXPECT_EQ(s.w.service.requestCount(), 1U) << "only the running request";

  ASSERT_TRUE(OwnedWorld::parked(result));
  result = s.finish(completeWithPayload(s.w));
  EXPECT_EQ(s.runner->ownedRecordCount(), 1U) << "the second was not copied";
  EXPECT_EQ(s.runner->retainedBytes(), 2U);
  EXPECT_EQ(s.w.service.requestCount(), 2U)
      << "its request stays admitted, as before the record existed";

  ASSERT_TRUE(OwnedWorld::parked(result));
  result = s.finish(EndOfStream{terminalProgress});
  EXPECT_EQ(s.runner->ownedRecordCount(), 2U)
      << "a payload-free outcome costs nothing and is still copied";
  const auto *done = std::get_if<ScriptRunResult::Completed>(&result.value);
  ASSERT_NE(done, nullptr) << OwnedWorld::failure(result);
  ASSERT_EQ(done->value.size(), 3U);
  EXPECT_EQ(s.text(done->value[0]), "ok") << "the record reads";
  EXPECT_EQ(s.text(done->value[1]), "ok") << "the service outcome still reads";
  EXPECT_EQ(s.text(done->value[2]), "EndOfStream");
}

TEST(VqlIoOwnedRecordTest, EveryEndingReturnsTheLeasesItTook) {
  const auto baseline = [](OwnedWorld &s) { return s.w.payloads.liveLeases(); };

  {
    SCOPED_TRACE("end");
    OwnedWorld s;
    const auto before = baseline(s);
    s.drive(std::string{readOnCap} + " as $a; return io_payload($a)",
            {completeWithPayload(s.w)});
    EXPECT_EQ(s.w.payloads.liveLeases(), before);
    EXPECT_EQ(s.runner->retainedBytes(), 0U);
  }
  {
    SCOPED_TRACE("fail");
    OwnedWorld s;
    s.engine.setVariable("plain", s.w.arena.makeCell("x"));
    const auto before = baseline(s);
    auto result =
        s.run(std::string{readOnCap} + " as $a; return io_tag($plain)");
    ASSERT_TRUE(OwnedWorld::parked(result));
    result = s.finish(completeWithPayload(s.w));
    EXPECT_FALSE(OwnedWorld::failure(result).empty());
    EXPECT_EQ(s.w.payloads.liveLeases(), before);
    EXPECT_EQ(s.runner->ownedRecordCount(), 0U);
    EXPECT_EQ(s.runner->retainedBytes(), 0U);
  }
  {
    SCOPED_TRACE("timeout");
    OwnedWorld s;
    auto now = std::chrono::steady_clock::time_point{};
    s.w.service.setClock([&now] { return now; });
    s.runner->setScriptTimeout(std::chrono::milliseconds{500});
    const auto before = baseline(s);
    auto result       = s.run(namedReads(2));
    ASSERT_TRUE(OwnedWorld::parked(result));
    result = s.finish(completeWithPayload(s.w));
    ASSERT_TRUE(OwnedWorld::parked(result));
    EXPECT_GT(s.w.payloads.liveLeases(), before) << "the record holds one";
    EXPECT_EQ(s.runner->ownedRecordCount(), 1U);
    now += std::chrono::milliseconds{500};
    const auto &parked = std::get<ScriptRunResult::Parked>(result.value);
    ASSERT_TRUE(s.runner->expireIfDue(parked.token));
    EXPECT_EQ(s.w.payloads.liveLeases(), before);
    EXPECT_EQ(s.runner->ownedRecordCount(), 0U);
    EXPECT_EQ(s.runner->retainedBytes(), 0U);
  }
  {
    SCOPED_TRACE("abandon");
    OwnedWorld s;
    const auto before = baseline(s);
    auto result       = s.run(namedReads(2));
    ASSERT_TRUE(OwnedWorld::parked(result));
    result = s.finish(completeWithPayload(s.w));
    ASSERT_TRUE(OwnedWorld::parked(result));
    auto parked      = std::get<ScriptRunResult::Parked>(result.value).token;
    const auto ended = s.runner->resume(
        {std::move(parked), xanadu::vortex::FrontendWake::Abandoned});
    EXPECT_FALSE(OwnedWorld::failure(ended).empty());
    EXPECT_EQ(s.w.payloads.liveLeases(), before);
    EXPECT_EQ(s.runner->ownedRecordCount(), 0U);
  }
  {
    SCOPED_TRACE("runner destroyed mid-script");
    OwnedWorld s;
    const auto before = baseline(s);
    auto result       = s.run(namedReads(3));
    ASSERT_TRUE(OwnedWorld::parked(result));
    result = s.finish(completeWithPayload(s.w));
    ASSERT_TRUE(OwnedWorld::parked(result));
    result = s.finish(completeWithPayload(s.w));
    ASSERT_TRUE(OwnedWorld::parked(result));
    EXPECT_GT(s.w.payloads.liveLeases(), before);
    s.runner.reset();
    EXPECT_EQ(s.w.payloads.liveLeases(), before);
  }
  {
    SCOPED_TRACE("runner destroyed after a finished script");
    OwnedWorld s;
    const auto before = baseline(s);
    const auto rows   = s.drive(std::string{readOnCap} + " as $a; return $a",
                                {completeWithPayload(s.w)});
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(s.w.payloads.liveLeases(), before)
        << "a finished script returned its leases; its rows are cells only";
    s.runner.reset();
    EXPECT_EQ(s.w.payloads.liveLeases(), before);
  }
}

TEST(VqlIoOwnedRecordTest, ARecordOutlivesItsScriptOnlyUntilTheNextAdmission) {
  OwnedWorld s;
  const auto rows = s.drive(std::string{readOnCap} + " as $a; return $a",
                            {completeWithPayload(s.w)});
  ASSERT_EQ(rows.size(), 1U);
  const auto incarnation = s.w.arena.incarnationOf(rows[0]);
  ASSERT_TRUE(incarnation);
  EXPECT_TRUE(s.w.arena.matches(rows[0], *incarnation))
      << "the caller's row is live";
  auto next = s.run(readOnCap);
  ASSERT_TRUE(OwnedWorld::parked(next));
  EXPECT_FALSE(s.w.arena.matches(rows[0], *incarnation))
      << "freed with the shared deletion once the next action is admitted";
  EXPECT_EQ(s.runner->ownedRecordCount(), 0U);
}

TEST(VqlIoOwnedRecordTest, ManyNamedOutcomeRunsLeaveTheArenaBounded) {
  OwnedWorld s;
  const auto once = [&] {
    // The row is the record itself: a row minted from it (io_tag, io_payload)
    // would be a cell the caller owns, which a test that frees nothing would
    // count as growth.
    auto result = s.run(std::string{readOnCap} + " as $r; return $r");
    ASSERT_TRUE(OwnedWorld::parked(result));
    result = s.finish(completeWithPayload(s.w));
    ASSERT_NE(std::get_if<ScriptRunResult::Completed>(&result.value), nullptr)
        << OwnedWorld::failure(result);
    s.freeOldSpecs();
  };
  for (std::size_t i = 0; i < 8; ++i) once();
  const auto warmSlots       = s.w.arena.cellCount();
  const auto warmLive        = s.w.arena.liveCells();
  const auto warmLeases      = s.w.payloads.liveLeases();
  constexpr std::size_t runs = 10'000;
  for (std::size_t i = 0; i < runs; ++i) once();
  EXPECT_EQ(s.w.arena.cellCount(), warmSlots)
      << "the slot high-water mark did not move";
  EXPECT_EQ(s.w.arena.liveCells(), warmLive);
  EXPECT_EQ(s.w.payloads.liveLeases(), warmLeases);
  EXPECT_EQ(s.w.service.requestCount(), 0U);
}

TEST(VqlIoOwnedRecordTest, ManyNamedRefusalRunsLeaveTheArenaBounded) {
  OwnedWorld s;
  s.w.refuseAdmission = true;
  const auto once     = [&] {
    auto result = s.run(std::string{readOnCap} + " as $r; return $r");
    ASSERT_NE(std::get_if<ScriptRunResult::Completed>(&result.value), nullptr)
        << OwnedWorld::failure(result);
    s.freeOldSpecs();
  };
  for (std::size_t i = 0; i < 8; ++i) once();
  const auto warmLive        = s.w.arena.liveCells();
  constexpr std::size_t runs = 1'000;
  for (std::size_t i = 0; i < runs; ++i) once();
  EXPECT_EQ(s.w.arena.liveCells(), warmLive);
}

// ---- The same over real files ----------------------------------------------

struct FileOwnedWorld {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
  VQLEngine engine{core};
  xanadu::vortex::FileReadHost host;
  ScriptRunner runner{engine, vm, xanadu::vql::fileScriptPorts(host)};
  std::filesystem::path dir;

  explicit FileOwnedWorld(xanadu::vortex::FileReadHostOptions options = {})
      : host(vm, arena, options) {
    dir = std::filesystem::temp_directory_path() /
          ("vql_io_owned_" + std::to_string(::getpid()) + "_" +
           std::to_string(counter_++));
    std::filesystem::create_directories(dir);
  }
  ~FileOwnedWorld() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
  auto write(const std::string &name, const std::string &content)
      -> std::filesystem::path {
    const auto path = dir / name;
    std::ofstream(path, std::ios::binary | std::ios::trunc) << content;
    return path;
  }
  auto run(const std::string &source)
      -> std::expected<std::vector<CellRef>, FrontendDiagnostic> {
    return xanadu::vql::driveToCompletion(
        runner, runner.run(Parser(source).parseQuery()), [&] { host.pump(16); },
        [] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); });
  }
  static inline std::uint32_t counter_ = 0;
};

TEST(VqlIoOwnedRecordFileTest, AHundredNamedReadsOfARealFileAllReadBack) {
  FileOwnedWorld f;
  const auto path    = f.write("digits.txt", "0123456789");
  std::string source = "effect { open(\"" + path.string() + "\") } as $f; ";
  constexpr std::size_t named = 100;
  for (std::size_t i = 0; i < named; ++i) {
    source += "effect { let $s := submit($f, read_bytes(" +
              std::to_string(i % 8) +
              ", 2)); match $s { admitted($q) => await($q), refused($e) => "
              "$e } } as $r" +
              std::to_string(i) + "; ";
  }
  source += "return io_payload($r0), io_payload($r99), io_tag($r50)";
  const auto rows = f.run(source);
  ASSERT_TRUE(rows) << rows.error().message;
  ASSERT_EQ(rows->size(), 3U);
  EXPECT_EQ(f.arena.textOf((*rows)[0]), "01");
  EXPECT_EQ(f.arena.textOf((*rows)[1]), "34"); // 99 % 8 == 3
  EXPECT_EQ(f.arena.textOf((*rows)[2]), "Complete");
  // Only the open and its close remain, retired by the next admission; a
  // hundred named reads under a budget of sixteen requests hold none.
  EXPECT_LE(f.host.service().requestCount(), 2U);
}

TEST(VqlIoOwnedRecordFileTest, TheRetentionBudgetFollowsTheHostOptions) {
  xanadu::vortex::FileReadHostOptions options;
  options.retainedPayloadBytes = 4;
  FileOwnedWorld f(options);
  const auto path = f.write("digits.txt", "0123456789");
  const std::string read =
      "effect { let $s := submit($f, read_bytes(0, 4)); match $s { "
      "admitted($q) => await($q), refused($e) => $e } }";
  const auto rows = f.run("effect { open(\"" + path.string() + "\") } as $f; " +
                          read + " as $a; " + read + " as $b; " +
                          "return io_payload($a), io_payload($b)");
  ASSERT_TRUE(rows) << rows.error().message;
  ASSERT_EQ(rows->size(), 2U);
  EXPECT_EQ(f.arena.textOf((*rows)[0]), "0123");
  EXPECT_EQ(f.arena.textOf((*rows)[1]), "0123");
  EXPECT_EQ(f.runner.retainedBytes(), 0U) << "returned at the script's end";
}

} // namespace
