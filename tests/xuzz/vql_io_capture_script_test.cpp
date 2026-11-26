// M7-VQL-c C3: `effect { capture($r) } as $c` (design/vortex-io-m7-vql-c.md
// §3, §4.5, §6.2, §11, §16 criteria 6-8). A capture is not one request, so it
// is a bytecode loop over the capture host's three relations; its value is the
// receipt (or the outcome that refused or ended it), and io_captured($c) is
// the arena cells whose content is the permanent permascroll span. Everything
// is headless: real files in a temp directory, a real UserPermascroll and
// capture store, and a gated sink where a capture has to be caught mid-append.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

#include "common/xanadu/result_slice.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/capture_coordinator.hpp"
#include "common/xanadu/vortex/capture_journal.hpp"
#include "common/xanadu/vortex/file_read_host.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/vql/script_runner.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "io_frontend_conformance.hpp"
#include "vql_capture_world.hpp"

namespace {

using namespace xuzz_conformance;
namespace fs = std::filesystem;
using xanadu::vortex::CaptureByteSink;
using xanadu::vortex::CaptureHostOptions;
using xanadu::vortex::CapturePublisher;
using xanadu::vortex::UserPermascrollCaptureSink;
using xanadu::vql::Parser;
using xanadu::vql::ScriptRunner;
using xanadu::vql::ScriptRunResult;
using xanadu::vql::VQLEngine;

using namespace vql_capture_world;

// ---- Grammar ----------------------------------------------------------------

TEST(VqlIoCaptureGrammarTest, CaptureTakesAnOutcomeAndAnOptionalRange) {
  const auto query =
      Parser(captureOf("r", "c") + captureOf("r", "d", "2, 5")).parseQuery();
  ASSERT_EQ(query.body.statements.size(), 2U);
  const auto &whole =
      std::get<xanadu::vql::IoEffectStatement>(query.body.statements[0]);
  EXPECT_EQ(whole.kind, xanadu::vql::IoEffectStatement::Kind::Capture);
  EXPECT_EQ(whole.captureVar, "r");
  EXPECT_FALSE(whole.captureRanged);
  const auto &part =
      std::get<xanadu::vql::IoEffectStatement>(query.body.statements[1]);
  EXPECT_TRUE(part.captureRanged);
  EXPECT_EQ(std::get<std::int64_t>(part.offset), 2);
  EXPECT_EQ(std::get<std::int64_t>(part.maxBytes), 5);
}

TEST(VqlIoCaptureGrammarTest,
     CaptureIsContextualSoAnOrdinaryFunctionKeepsItsName) {
  EXPECT_NO_THROW(
      Parser("def pure capture(): Int := 1; capture()").parseQuery());
}

TEST(VqlIoCaptureGrammarTest, CaptureIsRefusedWhereSubmitIs) {
  for (const std::string &source : {
           "for $x in ## " + captureOf("r", "c"),
           "def f() := { " + captureOf("r", "c") + " }; f()",
       }) {
    SCOPED_TRACE(source);
    try {
      Parser(source).parseQuery();
      FAIL() << "parsed";
    } catch (const xanadu::vql::ParseError &error) {
      EXPECT_NE(std::string{error.what()}.find("VQL_IO_QUERY_CONTEXT"),
                std::string::npos);
    }
  }
}

// ---- Capture of what a read retained ---------------------------------------

TEST(VqlIoCaptureTest, ACapturedOutcomeBecomesCellsThatAreThePermanentSpan) {
  CaptureWorld w;
  const auto path  = w.put("in.txt", "hello capture world");
  const auto cells = w.captured(open(path, "f") + read("f", "0, 64", "r") +
                                captureOf("r", "c"));
  ASSERT_EQ(cells.size(), 1U);
  EXPECT_EQ(w.quotedText(cells.front()), "hello capture world");
  // Content that already has an address: a permascroll span, no scratch.
  const auto spans = w.arena.contentOf(cells.front());
  ASSERT_EQ(spans.size(), 1U);
  EXPECT_NE(spans.front().scroll, xanadu::scratchScroll);
  EXPECT_EQ(spans.front().scroll, xanadu::localScroll);
  EXPECT_EQ(w.perma->read(spans.front()), "hello capture world");
  EXPECT_EQ(w.appendedBytes(), std::string{"hello capture world"}.size());
}

TEST(VqlIoCaptureTest, TheReceiptIsTheValueAndIoTagNamesIt) {
  CaptureWorld w;
  const auto path = w.put("in.txt", "receipt bytes");
  const auto rows =
      w.rows(open(path, "f") + read("f", "0, 64", "r") + captureOf("r", "c") +
             "return io_tag($c), io_tag($r), io_error($c), "
             "io_payload($c), io_bytes($c)");
  EXPECT_EQ(rows, (std::vector<std::string>{"CaptureReceipt", "Complete"}));
}

TEST(VqlIoCaptureTest, AServiceOutcomeCapturesLikeAnOwnedRecord) {
  // retainedPayloadBytes of 1 declines every copy, so `as $r` stays the
  // service's outcome and its request stays admitted.
  CaptureWorld w{1};
  const auto path  = w.put("in.txt", "service outcome");
  const auto cells = w.captured(open(path, "f") + read("f", "0, 64", "r") +
                                captureOf("r", "c"));
  ASSERT_EQ(cells.size(), 1U);
  EXPECT_EQ(w.quotedText(cells.front()), "service outcome");
  EXPECT_EQ(w.runner->ownedRecordCount(), 0U);
}

TEST(VqlIoCaptureTest,
     AnOwnedRecordKeepsItsObservationAndCapturesAfterRetirement) {
  CaptureWorld w;
  const auto path  = w.put("in.txt", "owned record bytes");
  const auto first = w.run(open(path, "f") + read("f", "0, 64", "r") +
                           captureOf("r", "c") + "return $r");
  ASSERT_TRUE(first.has_value());
  const auto record = first->front();
  const auto kept   = w.runner->ownedObservation(record);
  ASSERT_TRUE(kept.has_value()) << "the record keeps the observation by value";
  EXPECT_EQ(kept->locator, path);
  EXPECT_EQ(kept->length, std::string{"owned record bytes"}.size());
  EXPECT_EQ(kept->offset, 0U);
}

TEST(VqlIoCaptureTest, ARangeCapturesOnlyTheSelectedBytes) {
  CaptureWorld w;
  const auto path  = w.put("in.txt", "0123456789abcdef");
  const auto cells = w.captured(open(path, "f") + read("f", "0, 64", "r") +
                                captureOf("r", "c", "4, 6"));
  ASSERT_EQ(cells.size(), 1U);
  EXPECT_EQ(w.quotedText(cells.front()), "456789");
  EXPECT_EQ(w.appendedBytes(), 6U);
}

TEST(VqlIoCaptureTest, OverlappingCapturesAppendOnlyTheUncoveredBytes) {
  CaptureWorld w;
  const auto path = w.put("in.txt", "0123456789abcdef");
  const auto cells =
      w.captured(open(path, "f") + read("f", "0, 64", "r") +
                 captureOf("r", "a", "0, 10") + captureOf("r", "c", "5, 10"));
  // [5,10) was covered by the first capture, [10,15) is the only append.
  EXPECT_EQ(w.appendedBytes(), 15U);
  // The second capture's content reads as the bytes it selected, from however
  // many permascroll pieces they now live in.
  std::string text;
  for (const auto cell : cells) text += w.quotedText(cell);
  EXPECT_EQ(text, "56789abcde");
}

TEST(VqlIoCaptureTest,
     ASecondReadOfTheSameFileIsANewObservationAndAppendsAgain) {
  CaptureWorld w;
  const auto path = w.put("in.txt", "same file");
  w.captured(open(path, "f") + read("f", "0, 64", "r") + captureOf("r", "c"));
  EXPECT_EQ(w.appendedBytes(), 9U);
  w.captured(open(path, "f") + read("f", "0, 64", "r") + captureOf("r", "c"));
  EXPECT_EQ(w.appendedBytes(), 18U) << "content equality is not origin";
}

TEST(VqlIoCaptureTest, CapturingTheSameSelectionTwiceAppendsOnce) {
  CaptureWorld w;
  const auto path  = w.put("in.txt", "twice over");
  const auto cells = w.captured(open(path, "f") + read("f", "0, 64", "r") +
                                captureOf("r", "a") + captureOf("r", "c"));
  EXPECT_EQ(w.appendedBytes(), 10U);
  ASSERT_EQ(cells.size(), 1U);
  EXPECT_EQ(w.quotedText(cells.front()), "twice over");
}

TEST(VqlIoCaptureTest, ReadingNeverAppendsToThePermascroll) {
  CaptureWorld w;
  const auto path   = w.put("in.txt", "only read");
  const auto before = w.permascrollBytes();
  const auto rows   = w.rows(open(path, "f") + read("f", "0, 64", "r") +
                             "return io_payload($r), io_bytes($r)");
  EXPECT_EQ(rows, (std::vector<std::string>{"only read", "9"}));
  EXPECT_EQ(w.permascrollBytes(), before);
  EXPECT_TRUE(w.coordinator().pinnedCaptures().empty());
}

// ---- Provenance -------------------------------------------------------------

TEST(VqlIoCaptureTest, ProvenanceIsThePublishedReceiptFoundByTheSharedAddress) {
  CaptureWorld w;
  const auto path  = w.put("in.txt", "found by address");
  const auto cells = w.captured(open(path, "f") + read("f", "0, 64", "r") +
                                captureOf("r", "c"));
  ASSERT_EQ(cells.size(), 1U);
  const auto span = w.arena.contentOf(cells.front()).front();
  // The capture store holds a receipt whose range piece is that very span, and
  // its provenance rank says where the bytes came from.
  const auto keys = w.publisher->publishedKeys();
  ASSERT_TRUE(keys.has_value());
  ASSERT_EQ(keys->size(), 1U);
  const auto published = w.publisher->published(*keys->begin());
  ASSERT_TRUE(published.has_value());
  ASSERT_TRUE(published->has_value());
  ASSERT_EQ((*published)->pieces.size(), 1U);
  EXPECT_EQ((*published)->pieces.front().span, span);
  EXPECT_EQ((*published)->pieces.front().srcOffset, 0U);
  EXPECT_TRUE(fs::exists(w.base / "capture-store" / "ops.nodes"))
      << "the snapshot is durable before the capture reads Committed";
}

TEST(VqlIoCaptureTest, TheJournalOrderIsAppendAppendedCommitted) {
  CaptureWorld w;
  const auto path = w.put("in.txt", "journal order");
  w.captured(open(path, "f") + read("f", "0, 64", "r") + captureOf("r", "c"));
  using Kind = xanadu::vortex::CaptureJournalRecordKind;
  xanadu::vortex::CaptureJournal journal(w.base / "capture.journal",
                                         std::nullopt);
  std::vector<Kind> kinds;
  for (const auto &record : journal.recover().records)
    kinds.push_back(record.kind);
  EXPECT_EQ(kinds,
            (std::vector<Kind>{Kind::Append, Kind::Appended, Kind::Committed}));
}

// ---- Refusals as values
// -------------------------------------------------------

TEST(VqlIoCaptureTest, AnOutcomeWithNoPayloadIsRefusedAsAValue) {
  CaptureWorld w;
  const auto path = w.put("empty.txt", "");
  const auto rows =
      w.rows(open(path, "f") + read("f", "0, 64", "r") + captureOf("r", "c") +
             "return io_tag($r), io_tag($c), io_error($c), io_detail($c)");
  ASSERT_EQ(rows.size(), 4U);
  EXPECT_EQ(rows[0], "EndOfStream");
  EXPECT_EQ(rows[1], "Rejected");
  EXPECT_EQ(rows[2], "Unresolved");
  EXPECT_EQ(rows[3], "1");
  EXPECT_EQ(w.appendedBytes(), 0U);
}

TEST(VqlIoCaptureTest, ARangeOutsideThePayloadIsARejectedValue) {
  CaptureWorld w;
  const auto path = w.put("in.txt", "short");
  const auto rows =
      w.rows(open(path, "f") + read("f", "0, 64", "r") +
             captureOf("r", "c", "3, 10") + "return io_tag($c), io_error($c)");
  EXPECT_EQ(rows, (std::vector<std::string>{"Rejected", "InvalidRequest"}));
}

TEST(VqlIoCaptureTest, ACaptureOfANonOutcomeIsTheTypedDiagnostic) {
  CaptureWorld w;
  const auto path = w.put("in.txt", "x");
  EXPECT_EQ(w.failure(open(path, "f") + captureOf("f", "c")),
            "VQL_IO_CAPTURE_SOURCE");
}

TEST(VqlIoCaptureTest, AnUnboundSourceFailsBeforeAnythingRuns) {
  CaptureWorld w;
  EXPECT_EQ(w.failure(captureOf("nothing", "c")), "VQL_IO_EFFECT_ARGUMENT");
}

TEST(VqlIoCaptureTest,
     AHostWithNoCaptureStackStopsTheScriptBeforeAnythingRuns) {
  CaptureWorld w{xanadu::vortex::FileReadHostOptions::defaultBudgetBytes,
                 /*attach=*/false};
  const auto path = w.put("in.txt", "unavailable");
  EXPECT_EQ(w.failure(open(path, "f") + read("f", "0, 64", "r") +
                      captureOf("r", "c")),
            "VQL_IO_CAPTURE_UNAVAILABLE");
  EXPECT_EQ(w.permascrollBytes(), 0U);
}

TEST(VqlIoCaptureTest, AReceiptCannotBeAwaitedOrCancelled) {
  CaptureWorld w;
  const auto path = w.put("in.txt", "no handle");
  const auto prefix =
      open(path, "f") + read("f", "0, 64", "r") + captureOf("r", "c");
  EXPECT_EQ(w.failure(prefix + "effect { cancel($c) }"),
            "VQL_IO_EFFECT_ARGUMENT");
  EXPECT_EQ(w.failure(prefix + "effect { await($c) }"),
            "VQL_IO_EFFECT_ARGUMENT");
}

TEST(VqlIoCaptureTest, ARequestWithinBoundStillCapturesWhatItRetained) {
  CaptureWorld w;
  const auto path  = w.put("in.txt", "within then capture");
  const auto cells = w.captured(
      open(path, "f") + read("f", "0, 64", "r", "5 s") + captureOf("r", "c"));
  ASSERT_EQ(cells.size(), 1U);
  EXPECT_EQ(w.quotedText(cells.front()), "within then capture");
}

TEST(VqlIoCaptureTest, IoCapturedOfAnythingButAReceiptAnswersNothing) {
  CaptureWorld w;
  const auto path = w.put("in.txt", "not a receipt");
  const auto rows = w.rows(open(path, "f") + read("f", "0, 64", "r") +
                           "return io_captured($r), io_tag($r)");
  EXPECT_EQ(rows, (std::vector<std::string>{"Complete"}));
}

// ---- Abandon and pin
// ------------------------------------------------------------

TEST(VqlIoCaptureAbandonTest,
     ATimeoutMidCapturePinsItsIntervalsAndALaterCaptureStillRuns) {
  CaptureWorld w;
  const auto path = w.put("in.txt", "stuck mid append");
  w.runner->setScriptTimeout(std::chrono::milliseconds{1});
  w.sink->close();
  auto result = w.start(open(path, "f") + read("f", "0, 64", "r") +
                        captureOf("r", "c") + "return io_tag($c)");
  // Drive the open and the read; the capture's append then blocks in the sink.
  std::optional<xanadu::vortex::FrontendDiagnostic> expired;
  for (int turns = 0; turns < 20000 && !expired; ++turns) {
    if (auto *parked = std::get_if<ScriptRunResult::Parked>(&result.value)) {
      w.host->pump(16);
      if (auto woken = w.runner->takeWake()) {
        result = w.runner->resume(std::move(*woken));
        continue;
      }
      if (w.sink->entered() >= 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        expired = w.runner->expireIfDue(parked->token);
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    } else {
      break;
    }
  }
  ASSERT_TRUE(expired.has_value()) << "the script is abandoned mid-capture";
  EXPECT_EQ(expired->code, "VQL_SCRIPT_TIMEOUT");
  // The capture's cell died with the script: past AppendRequested it is
  // retired uncertain and its intervals pinned, never finished in the
  // background.
  EXPECT_EQ(w.coordinator().pinnedCaptures().size(), 1U);
  w.sink->open();
  w.runner->setScriptTimeout(std::nullopt);
  const auto again = w.rows(open(path, "g") + read("g", "0, 64", "r") +
                            captureOf("r", "c") + "return io_tag($c)");
  EXPECT_EQ(again, (std::vector<std::string>{"CaptureReceipt"}))
      << "a fresh read is a fresh observation: nothing overlaps";
}

TEST(VqlIoCaptureAbandonTest,
     AReconcileReleaseFreesAPinnedCaptureTheScriptAbandoned) {
  CaptureWorld w;
  const auto path = w.put("in.txt", "pinned overlap");
  w.runner->setScriptTimeout(std::chrono::milliseconds{1});
  w.sink->close();
  auto result =
      w.start(open(path, "f") + read("f", "0, 64", "r") + captureOf("r", "c"));
  std::optional<xanadu::vortex::FrontendDiagnostic> expired;
  for (int turns = 0; turns < 20000 && !expired; ++turns) {
    auto *parked = std::get_if<ScriptRunResult::Parked>(&result.value);
    if (parked == nullptr) break;
    w.host->pump(16);
    if (auto woken = w.runner->takeWake()) {
      result = w.runner->resume(std::move(*woken));
      continue;
    }
    if (w.sink->entered() >= 1) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      expired = w.runner->expireIfDue(parked->token);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  ASSERT_TRUE(expired.has_value());
  const auto pinned = w.coordinator().pinnedCaptures();
  ASSERT_EQ(pinned.size(), 1U);
  w.sink->open();
  // Release is the owner's decision. (The abandoned script's observation is
  // unreachable, so the pin cannot block another capture in this process; it
  // is what `:captures` reports and what a restart's recovery would keep.)
  const auto released = w.coordinator().reconcile(
      pinned.front(), xanadu::vortex::ReconcileRelease{});
  ASSERT_TRUE(released.has_value());
  EXPECT_TRUE(w.coordinator().pinnedCaptures().empty());
}

TEST(VqlIoCaptureAbandonTest,
     DestroyingTheRunnerMidCapturePinsAndResubmitsNothing) {
  CaptureWorld w;
  const auto path = w.put("in.txt", "runner destroyed");
  w.sink->close();
  auto result =
      w.start(open(path, "f") + read("f", "0, 64", "r") + captureOf("r", "c"));
  for (int turns = 0; turns < 20000; ++turns) {
    if (!std::holds_alternative<ScriptRunResult::Parked>(result.value)) break;
    w.host->pump(16);
    if (auto woken = w.runner->takeWake()) {
      result = w.runner->resume(std::move(*woken));
      continue;
    }
    if (w.sink->entered() >= 1) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(w.sink->waitEntered(1)) << "the append is inside the sink";
  w.runner.reset();
  EXPECT_EQ(w.coordinator().pinnedCaptures().size(), 1U);
  w.sink->open();
  // The append still lands (it was never cancelled), and nothing re-drives it.
  for (int turns = 0; turns < 2000; ++turns) {
    w.host->pump(16);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (w.permascrollBytes() >= std::string{"runner destroyed"}.size()) break;
  }
  EXPECT_EQ(w.permascrollBytes(), std::string{"runner destroyed"}.size());
  EXPECT_EQ(w.coordinator().pinnedCaptures().size(), 1U);
  EXPECT_EQ(w.host->capture()->liveRuns(), 0U);
}

namespace {
/// Park a script mid-capture with the append held inside the sink.
auto parkMidCapture(CaptureWorld &w, const std::string &path)
    -> ScriptRunResult {
  w.sink->close();
  auto result =
      w.start(open(path, "f") + read("f", "0, 64", "r") + captureOf("r", "c"));
  for (int turns = 0; turns < 20000; ++turns) {
    if (!std::holds_alternative<ScriptRunResult::Parked>(result.value)) break;
    w.host->pump(16);
    if (auto woken = w.runner->takeWake()) {
      result = w.runner->resume(std::move(*woken));
      continue;
    }
    if (w.sink->entered() >= 1) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return result;
}
} // namespace

TEST(VqlIoCaptureAbandonTest, ADroppedParkedTokenStillPinsAtRunnerDestruction) {
  CaptureWorld w;
  const auto path = w.put("in.txt", "token dropped");
  {
    auto result = parkMidCapture(w, path);
    ASSERT_TRUE(w.sink->waitEntered(1)) << "the append is inside the sink";
    // The caller discards its token: parked_ is then the activation's only
    // owner, and the runner must still release the capture it holds.
    [[maybe_unused]] const auto dropped = std::move(result);
  }
  w.runner.reset();
  EXPECT_EQ(w.coordinator().pinnedCaptures().size(), 1U);
  EXPECT_EQ(w.host->capture()->liveRuns(), 0U);
}

// A capture the runner never reached is abandoned by the host's destructor
// while the service is alive; the canary for the order the members die in.
TEST(VqlIoCaptureAbandonTest, DestroyingTheHostWithALiveCaptureIsSafe) {
  CaptureWorld w;
  const auto path = w.put("in.txt", "host destroyed");
  auto result     = parkMidCapture(w, path);
  ASSERT_TRUE(w.sink->waitEntered(1));
  // The runner is leaked on purpose: destroying it first would release the
  // capture, and this test is about a run only the host still holds.
  [[maybe_unused]] static ScriptRunner *leaked = nullptr;
  leaked                                       = w.runner.release();
  w.sink->open();
  w.host.reset();
  SUCCEED();
}

// ---- Recovery
// -----------------------------------------------------------------

TEST(VqlIoCaptureRecoveryTest, RecoveryRunsOnTheFirstCaptureNotBefore) {
  CaptureWorld w;
  const auto path = w.put("in.txt", "lazy recovery");
  EXPECT_FALSE(w.coordinator().recovered());
  // A script that captures nothing starts no recovery.
  w.rows(open(path, "f") + read("f", "0, 64", "r") + "return io_tag($r)");
  EXPECT_FALSE(w.coordinator().recovered());
  w.captured(open(path, "f") + read("f", "0, 64", "r") + captureOf("r", "c"));
  EXPECT_TRUE(w.coordinator().recovered());
}

TEST(VqlIoCaptureRecoveryTest, ACrashedSessionsOpenIntentIsReportedOnFirstUse) {
  const auto base = fs::weakly_canonical(fs::temp_directory_path()) /
                    ("vql_io_capture_crash_" + std::to_string(::getpid()));
  fs::create_directories(base);
  {
    // The journal a crashed session left: an Append intent and no outcome.
    xanadu::vortex::CaptureJournal journal(base / "capture.journal",
                                           std::nullopt);
    xanadu::vortex::CaptureJournalRecord record;
    record.appendLength = 11;
    record.intervals    = {xanadu::vortex::CaptureSourceInterval{
           .sourceKind = xanadu::vortex::PayloadSourceKind::Durable,
           .source     = xanadu::localScroll,
           .generation = 0,
           .occurrence = 7,
           .srcOffset  = 0,
           .srcLength  = 11,
           .dstOffset  = 0}};
    ASSERT_TRUE(
        journal
            .appendRecord(xanadu::vortex::CaptureJournalRecordKind::Append, 1,
                          record)
            .has_value());
  }
  {
    CaptureWorld w{xanadu::vortex::FileReadHostOptions::defaultBudgetBytes,
                   true, base};
    const auto path = w.put("in.txt", "after crash");
    EXPECT_FALSE(w.coordinator().recovered());
    w.captured(open(path, "f") + read("f", "0, 64", "r") + captureOf("r", "c"));
    ASSERT_TRUE(w.coordinator().recovered());
    const auto &report = w.host->capture()->recoveredReport();
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->orphanBytes, 11U);
    EXPECT_EQ(w.coordinator().pinnedCaptures().size(), 1U);
  }
}

// ---- Arena cleanup
// ----------------------------------------------------------------

TEST(VqlIoCaptureTest, TheCaptureHostHoldsNoRunOnceTheNextScriptAdmits) {
  CaptureWorld w;
  const auto path = w.put("in.txt", "bounded runs");
  for (int run = 0; run < 5; ++run) {
    w.captured(open(path, "f") + read("f", "0, 64", "r") + captureOf("r", "c"));
  }
  // The last script's receipt is kept until the next admission, no longer.
  EXPECT_LE(w.host->capture()->liveRuns(), 1U);
  w.rows(open(path, "f") + "return io_tag($f)");
  EXPECT_EQ(w.host->capture()->liveRuns(), 0U);
}

// ---- What the result slice can carry (design §18 check (a))
// ------------------------

// The arena alone cannot say where a captured cell's bytes live: it has no
// store behind a bare makeCell(PrimediaSpan) cell, so `quotedContent` answers
// nothing. vquery's resultRows supplies the capture store for such a cell
// (vquery_e2e_test pins that through the CLI); this pins both halves of what
// it relies on: the arena stays silent, and a Store over the author's
// permascroll can carry the span without a copy.
TEST(VqlIoCaptureTest, AnArenaCannotQuoteACapturedCellButTheCaptureStoreCan) {
  CaptureWorld w;
  const auto path  = w.put("in.txt", "quoted into a result");
  const auto cells = w.captured(open(path, "f") + read("f", "0, 64", "r") +
                                captureOf("r", "c"));
  ASSERT_EQ(cells.size(), 1U);
  EXPECT_FALSE(w.arena.quotedContent(cells.front()).has_value());
  const auto span = w.arena.contentOf(cells.front()).front();
  xanadu::Store output(w.perma);
  const xanadu::ResultRow row{
      .text   = "quoted into a result",
      .source = {},
      .quote  = xanadu::QuotedSpans{.store = w.store.get(), .spans = {span}}};
  const auto appended = w.appendedBytes();
  EXPECT_NO_THROW(xanadu::writeResultSlice(output, std::span{&row, 1}));
  const auto back = xanadu::readResultSlice(output, output.latest());
  ASSERT_EQ(back.size(), 1U);
  EXPECT_EQ(back.front().text, "quoted into a result");
  // Transcluded, not retyped: the row's content is the captured span itself.
  ASSERT_TRUE(back.front().quote.has_value());
  ASSERT_EQ(back.front().quote->spans.size(), 1U);
  EXPECT_EQ(back.front().quote->spans.front(), span);
  EXPECT_EQ(w.appendedBytes(), appended);
}

// ---- Persistability (design §4.7, criterion 9) -----------------------------

/// `read("f", ...)` then a row that exposes the payload as text.
const auto exposePayload = std::string{"return io_payload($r)"};

TEST(VqlIoPersistabilityTest, NothingIsKnownBeforeAScriptHasFinished) {
  CaptureWorld w;
  EXPECT_FALSE(w.runner->persistability().has_value());
}

TEST(VqlIoPersistabilityTest, AnUncapturedExposureNeedsCapture) {
  CaptureWorld w;
  const auto path = w.put("in.txt", "transient bytes");
  ASSERT_TRUE(w.run(open(path, "f") + read("f", "0, 64", "r") + exposePayload)
                  .has_value());
  const auto verdict = w.runner->persistability();
  ASSERT_TRUE(verdict.has_value());
  EXPECT_FALSE(verdict->persistable());
  EXPECT_EQ(verdict->exposed, 1U);
  EXPECT_EQ(verdict->uncovered, 1U);
}

TEST(VqlIoPersistabilityTest, ACapturedPayloadMayBeExposedAndPersisted) {
  CaptureWorld w;
  const auto path = w.put("in.txt", "captured then shown");
  ASSERT_TRUE(w.run(open(path, "f") + read("f", "0, 64", "r") +
                    captureOf("r", "c") + exposePayload)
                  .has_value());
  const auto verdict = w.runner->persistability();
  ASSERT_TRUE(verdict.has_value());
  EXPECT_TRUE(verdict->persistable());
  EXPECT_EQ(verdict->exposed, 1U);
}

TEST(VqlIoPersistabilityTest, TheCaptureMustHappenBeforeTheScriptEnds) {
  // The verdict is the run's, taken when it ends: a later script's capture
  // does not make an earlier script's rows persistable.
  CaptureWorld w;
  const auto path = w.put("in.txt", "late capture");
  ASSERT_TRUE(w.run(open(path, "f") + read("f", "0, 64", "r") + exposePayload)
                  .has_value());
  ASSERT_TRUE(w.run(open(path, "f") + read("f", "0, 64", "r") +
                    captureOf("r", "c") + "return 1")
                  .has_value());
  EXPECT_TRUE(w.runner->persistability()->persistable());
  ASSERT_TRUE(w.run(open(path, "f") + read("f", "0, 64", "r") + exposePayload)
                  .has_value());
  EXPECT_FALSE(w.runner->persistability()->persistable())
      << "a second read is a new observation, uncaptured";
}

TEST(VqlIoPersistabilityTest, PartialCoverageStillNeedsCapture) {
  CaptureWorld w;
  const auto path = w.put("in.txt", "0123456789abcdef");
  ASSERT_TRUE(w.run(open(path, "f") + read("f", "0, 64", "r") +
                    captureOf("r", "c", "0, 4") + exposePayload)
                  .has_value());
  const auto verdict = w.runner->persistability();
  ASSERT_TRUE(verdict.has_value());
  EXPECT_FALSE(verdict->persistable())
      << "the other twelve bytes were never captured";
  EXPECT_EQ(verdict->uncovered, 1U);
}

TEST(VqlIoPersistabilityTest, TwoCapturesThatTogetherCoverThePayloadCover) {
  CaptureWorld w;
  const auto path = w.put("in.txt", "0123456789");
  ASSERT_TRUE(w.run(open(path, "f") + read("f", "0, 64", "r") +
                    captureOf("r", "a", "0, 4") + captureOf("r", "b", "4, 6") +
                    exposePayload)
                  .has_value());
  EXPECT_TRUE(w.runner->persistability()->persistable());
}

TEST(VqlIoPersistabilityTest, DerivedTextFollowsTheCoverageOfWhatItCameFrom) {
  CaptureWorld w;
  const auto path    = w.put("in.txt", "derive me");
  const auto derived = std::string{"return string(io_payload($r))"};
  ASSERT_TRUE(
      w.run(open(path, "f") + read("f", "0, 64", "r") + derived).has_value());
  EXPECT_FALSE(w.runner->persistability()->persistable())
      << "a value computed from uncaptured text was still exposed as text";
  ASSERT_TRUE(w.run(open(path, "f") + read("f", "0, 64", "r") +
                    captureOf("r", "c") + derived)
                  .has_value());
  EXPECT_TRUE(w.runner->persistability()->persistable());
}

TEST(VqlIoPersistabilityTest, ReturningAnOutcomeNamesItsPayloadToo) {
  CaptureWorld w;
  const auto path = w.put("in.txt", "named by handle");
  ASSERT_TRUE(w.run(open(path, "f") + read("f", "0, 64", "r") + "return $r")
                  .has_value());
  EXPECT_FALSE(w.runner->persistability()->persistable());
  ASSERT_TRUE(w.run(open(path, "f") + read("f", "0, 64", "r") +
                    captureOf("r", "c") + "return $r")
                  .has_value());
  EXPECT_TRUE(w.runner->persistability()->persistable());
}

TEST(VqlIoPersistabilityTest,
     ReadingWithoutTurningTheBytesIntoTextIsNoExposure) {
  CaptureWorld w;
  const auto path = w.put("in.txt", "read, never shown");
  ASSERT_TRUE(
      w.run(open(path, "f") + read("f", "0, 64", "r") + "return io_tag($r)")
          .has_value());
  const auto verdict = w.runner->persistability();
  ASSERT_TRUE(verdict.has_value());
  EXPECT_EQ(verdict->exposed, 0U);
  EXPECT_TRUE(verdict->persistable())
      << "no payload became text, so there is nothing to capture";
}

TEST(VqlIoPersistabilityTest, ACaptureOfARecordAndAServiceOutcomeCoverAlike) {
  for (const std::size_t retained :
       {xanadu::vortex::FileReadHostOptions::defaultBudgetBytes,
        std::size_t{1}}) {
    SCOPED_TRACE(retained);
    CaptureWorld w{retained};
    const auto path = w.put("in.txt", "either kind of outcome");
    ASSERT_TRUE(w.run(open(path, "f") + read("f", "0, 64", "r") + exposePayload)
                    .has_value());
    EXPECT_FALSE(w.runner->persistability()->persistable());
    ASSERT_TRUE(w.run(open(path, "f") + read("f", "0, 64", "r") +
                      captureOf("r", "c") + exposePayload)
                    .has_value());
    EXPECT_TRUE(w.runner->persistability()->persistable());
  }
}

TEST(VqlIoPersistabilityTest, WithoutACaptureStackAnyExposureIsUncovered) {
  // A host that never attached one: the script cannot capture and nothing it
  // exposed is covered.
  CaptureWorld w{xanadu::vortex::FileReadHostOptions::defaultBudgetBytes,
                 false};
  const auto path = w.put("in.txt", "no stack");
  ASSERT_TRUE(w.run(open(path, "f") + read("f", "0, 64", "r") + exposePayload)
                  .has_value());
  EXPECT_FALSE(w.runner->persistability()->persistable());
  ASSERT_TRUE(w.run(open(path, "f") + "return io_tag($f)").has_value());
  EXPECT_TRUE(w.runner->persistability()->persistable());
}

TEST(VqlIoPersistabilityTest, TheCaptureStackIsBuiltOnlyWhenAScriptCaptures) {
  CaptureWorld w{xanadu::vortex::FileReadHostOptions::defaultBudgetBytes,
                 false};
  int built  = 0;
  auto ports = xanadu::vql::fileScriptPorts(*w.host);
  ports.ensureCaptureHost =
      [&]() -> std::expected<xanadu::vortex::CaptureHost *, std::string> {
    ++built;
    return &w.host->attachCapture(
        *w.sink, w.publisher.get(),
        CaptureHostOptions{.journalPath = w.base / "capture.journal"});
  };
  w.runner.reset();
  w.runner = std::make_unique<ScriptRunner>(w.engine, w.vm, std::move(ports));
  const auto path = w.put("in.txt", "lazy");
  ASSERT_TRUE(w.run(open(path, "f") + read("f", "0, 64", "r") + exposePayload)
                  .has_value());
  EXPECT_EQ(built, 0) << "a plain read builds no journal";
  EXPECT_FALSE(w.runner->persistability()->persistable());
  ASSERT_TRUE(w.run(open(path, "f") + read("f", "0, 64", "r") +
                    captureOf("r", "c") + exposePayload)
                  .has_value());
  EXPECT_EQ(built, 1);
  EXPECT_TRUE(w.runner->persistability()->persistable());
}

TEST(VqlIoPersistabilityTest, AStackThatCannotBeBuiltIsTheTypedRefusal) {
  CaptureWorld w{xanadu::vortex::FileReadHostOptions::defaultBudgetBytes,
                 false};
  auto ports = xanadu::vql::fileScriptPorts(*w.host);
  ports.ensureCaptureHost =
      []() -> std::expected<xanadu::vortex::CaptureHost *, std::string> {
    return std::unexpected("the capture store is unreadable");
  };
  w.runner.reset();
  w.runner = std::make_unique<ScriptRunner>(w.engine, w.vm, std::move(ports));
  const auto path = w.put("in.txt", "x");
  const auto result =
      w.run(open(path, "f") + read("f", "0, 64", "r") + captureOf("r", "c"));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, "VQL_IO_CAPTURE_UNAVAILABLE");
  EXPECT_NE(result.error().message.find("unreadable"), std::string::npos);
}

} // namespace

// ---- Leaks (design criterion 11) -------------------------------------------

namespace {

/// Free a run's rows as the next query of a session would; they are the
/// caller's, so the bound is on everything else.
void dropRows(CaptureWorld &w,
              const std::expected<std::vector<zigzag::CellRef>,
                                  xanadu::vortex::FrontendDiagnostic> &result) {
  ASSERT_TRUE(result.has_value());
  for (const auto cell : *result) {
    if (const auto incarnation = w.arena.incarnationOf(cell))
      static_cast<void>(w.arena.freeCell(cell, *incarnation));
  }
}

TEST(VqlIoCaptureLeakTest, ManyCaptureRunsLeaveTheArenaAndTheStacksBounded) {
  CaptureWorld w;
  const auto path   = w.put("in.txt", "leak probe bytes");
  const auto script = open(path, "f") + read("f", "0, 64", "r") +
                      captureOf("r", "c") + "return io_tag($c)";
  constexpr std::size_t warmRuns = 8;
  constexpr std::size_t runs     = 1'000;
  for (std::size_t i = 0; i < warmRuns; ++i) dropRows(w, w.run(script));
  // An admission frees the previous run's receipt, record and scaffolding;
  // measure both ends at the same point of that cycle.
  const auto idle = open(path, "g") + "return io_tag($g)";
  dropRows(w, w.run(idle));
  const auto warmSlots = w.arena.cellCount();
  const auto warmLive  = w.arena.liveCells();
  for (std::size_t i = 0; i < runs; ++i) dropRows(w, w.run(script));
  dropRows(w, w.run(idle));
  EXPECT_EQ(w.arena.cellCount(), warmSlots) << "the slot high-water mark";
  EXPECT_EQ(w.arena.liveCells(), warmLive) << "live arena cells";
  EXPECT_LE(w.host->service().requestCount(), 3U) << "service requests";
  EXPECT_LE(w.host->capture()->liveRuns(), 1U) << "capture runs";
  const auto counts = w.coordinator().stateCounts();
  EXPECT_EQ(counts.stepGraphs, 0U) << "endpoint/capability/spec scaffolding";
  // Each run is a fresh observation, so each keeps one translation until the
  // map's own bound evicts it; that is the coordinator's budget, not a leak.
  EXPECT_LE(counts.translationSlots, warmRuns + runs);
  EXPECT_TRUE(w.coordinator().pinnedCaptures().empty());
  EXPECT_EQ(w.runner->ownedRecordCount(), 0U);
}

} // namespace

namespace {

/// How a script is abandoned while its capture's append is inside the sink.
enum class Abandon { Timeout, DestroyedRunner };

/// Start a script that captures, hold its append in the sink, abandon the
/// script, then let the orphaned append finish: the step is never cancelled,
/// so the residue it leaves can only be freed once it has ended.
void abandonMidCapture(CaptureWorld &w, const std::string &path,
                       const Abandon how) {
  constexpr auto scriptTimeout = std::chrono::milliseconds{1};
  constexpr auto settle        = std::chrono::milliseconds{2};
  constexpr auto idleWait      = std::chrono::milliseconds{1};
  constexpr int turnBound      = 20'000;
  w.sink->close();
  const auto already = w.sink->entered();
  if (how == Abandon::Timeout) w.runner->setScriptTimeout(scriptTimeout);
  auto result    = w.start(open(path, "f") + read("f", "0, 64", "r") +
                           captureOf("r", "c") + "return io_tag($c)");
  bool abandoned = false;
  for (int turns = 0; turns < turnBound && !abandoned; ++turns) {
    auto *parked = std::get_if<ScriptRunResult::Parked>(&result.value);
    if (parked == nullptr) break;
    w.host->pump(16);
    if (auto woken = w.runner->takeWake()) {
      result = w.runner->resume(std::move(*woken));
      continue;
    }
    if (w.sink->entered() <= already) {
      std::this_thread::sleep_for(idleWait);
      continue;
    }
    if (how == Abandon::Timeout) {
      std::this_thread::sleep_for(settle);
      abandoned = w.runner->expireIfDue(parked->token).has_value();
    } else {
      abandoned = true;
    }
  }
  ASSERT_TRUE(abandoned) << "the script never reached its append";
  if (how == Abandon::DestroyedRunner) {
    w.runner.reset();
    w.runner = std::make_unique<ScriptRunner>(
        w.engine, w.vm, xanadu::vql::fileScriptPorts(*w.host));
  } else {
    w.runner->setScriptTimeout(std::nullopt);
  }
  w.sink->open();
  // The append lands on its own; nothing resubmits it.
  for (int turns = 0; turns < turnBound && w.host->service().requestCount() > 1;
       ++turns) {
    w.host->pump(16);
    std::this_thread::sleep_for(idleWait);
  }
}

void expectAbandonedCapturesBounded(const Abandon how, const std::size_t runs) {
  CaptureWorld w;
  const auto path        = w.put("in.txt", "leak probe bytes");
  const auto idle        = open(path, "g") + "return io_tag($g)";
  const auto settleArena = [&] {
    // Two admissions retire the previous run's closes and scaffolding.
    for (int i = 0; i < 3; ++i) dropRows(w, w.run(idle));
  };
  constexpr std::size_t warmRuns = 3;
  for (std::size_t i = 0; i < warmRuns; ++i) abandonMidCapture(w, path, how);
  settleArena();
  const auto warmSlots = w.arena.cellCount();
  const auto warmLive  = w.arena.liveCells();
  for (std::size_t i = 0; i < runs; ++i) abandonMidCapture(w, path, how);
  settleArena();
  EXPECT_EQ(w.arena.cellCount(), warmSlots) << "the slot high-water mark";
  EXPECT_EQ(w.arena.liveCells(), warmLive) << "live arena cells";
  EXPECT_LE(w.host->service().requestCount(), 3U) << "service requests";
  EXPECT_EQ(w.coordinator().stateCounts().stepGraphs, 0U)
      << "an orphaned step's scaffolding is freed once the step ends";
  EXPECT_LE(w.host->capture()->liveRuns(), 1U) << "capture runs";
  EXPECT_EQ(w.runner->ownedRecordCount(), 0U);
  // The pin is the design: one per abandoned capture, each reconcilable.
  const auto pinned = w.coordinator().pinnedCaptures();
  EXPECT_EQ(pinned.size(), warmRuns + runs);
  for (const auto id : pinned) {
    EXPECT_TRUE(
        w.coordinator().reconcile(id, xanadu::vortex::ReconcileRelease{}));
  }
  EXPECT_TRUE(w.coordinator().pinnedCaptures().empty());
}

constexpr std::size_t abandonedRuns = 1'000;

TEST(VqlIoCaptureLeakTest, ManyTimedOutCapturesLeaveTheArenaBounded) {
  expectAbandonedCapturesBounded(Abandon::Timeout, abandonedRuns);
}

TEST(VqlIoCaptureLeakTest, ManyDestroyedRunnersMidCaptureLeaveTheArenaBounded) {
  expectAbandonedCapturesBounded(Abandon::DestroyedRunner, abandonedRuns);
}

} // namespace
