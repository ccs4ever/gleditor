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
#include <iterator>
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

auto read(const std::string &cap, const std::string &range,
          const std::string &name, const std::string &within = {})
    -> std::string {
  return "effect { let $s := submit($" + cap + ", read_bytes(" + range + "))" +
         (within.empty() ? "" : " within " + within) +
         "; match $s { admitted($q) => await($q), refused($e) => $e } } as $" +
         name + "; ";
}

auto open(const std::string &path, const std::string &name) -> std::string {
  return "effect { open(\"" + path + "\") } as $" + name + "; ";
}

auto captureOf(const std::string &source, const std::string &name,
               const std::string &range = {}) -> std::string {
  return "effect { capture($" + source + (range.empty() ? "" : ", " + range) +
         ") } as $" + name + "; ";
}

/// A sink over the real permascroll whose append waits at a gate, so a
/// capture can be caught between its intent and its bytes.
class GatedSink final : public CaptureByteSink {
public:
  explicit GatedSink(xanadu::UserPermascroll &permascroll)
      : inner_(permascroll) {}
  auto append(const std::span<const std::byte> bytes)
      -> xanadu::PrimediaSpan override {
    {
      std::unique_lock lock(mutex_);
      ++entered_;
      changed_.notify_all();
      changed_.wait(lock, [&] { return !closed_; });
    }
    return inner_.append(bytes);
  }
  auto sync() -> bool override { return inner_.sync(); }
  auto highWaterBytes() const -> std::uint64_t override {
    return inner_.highWaterBytes();
  }
  auto read(const xanadu::PrimediaSpan &span) const
      -> std::optional<std::vector<std::byte>> override {
    return inner_.read(span);
  }
  void close() {
    std::scoped_lock lock(mutex_);
    closed_ = true;
  }
  void open() {
    {
      std::scoped_lock lock(mutex_);
      closed_ = false;
    }
    changed_.notify_all();
  }
  [[nodiscard]] auto entered() -> int {
    std::scoped_lock lock(mutex_);
    return entered_;
  }
  /// Wait until a worker is parked inside append().
  auto waitEntered(const int count = 1) -> bool {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, std::chrono::seconds(10),
                             [&] { return entered_ >= count; });
  }

private:
  UserPermascrollCaptureSink inner_;
  std::mutex mutex_;
  std::condition_variable changed_;
  bool closed_{false};
  int entered_{};
};

/// Everything a capture script needs: a file host, a real permascroll, a
/// capture store and journal, and a runner.
struct CaptureWorld {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
  VQLEngine engine{core};
  fs::path base;
  std::shared_ptr<xanadu::UserPermascroll> perma;
  std::unique_ptr<GatedSink> sink;
  std::unique_ptr<xanadu::Store> store;
  std::unique_ptr<CapturePublisher> publisher;
  std::unique_ptr<xanadu::vortex::FileReadHost> host;
  std::unique_ptr<ScriptRunner> runner;

  explicit CaptureWorld(
      const std::size_t retained =
          xanadu::vortex::FileReadHostOptions::defaultBudgetBytes,
      const bool attach = true, const fs::path &reuse = {}) {
    static std::uint32_t counter = 0;
    base = reuse.empty() ? fs::weakly_canonical(fs::temp_directory_path()) /
                               ("vql_io_capture_" + std::to_string(::getpid()) +
                                "_" + std::to_string(counter++))
                         : reuse;
    fs::create_directories(base / "data");
    xanadu::UserPermascroll::Config config;
    config.storageDir = base / "permascroll";
    perma             = std::make_shared<xanadu::UserPermascroll>(config);
    sink              = std::make_unique<GatedSink>(*perma);
    store             = std::make_unique<xanadu::Store>(perma);
    publisher =
        std::make_unique<CapturePublisher>(*store, base / "capture-store");
    xanadu::vortex::FileReadHostOptions options;
    options.retainedPayloadBytes = retained;
    host = std::make_unique<xanadu::vortex::FileReadHost>(vm, arena, options);
    if (attach) {
      host->attachCapture(
          *sink, publisher.get(),
          CaptureHostOptions{.journalPath = base / "capture.journal"});
    }
    runner = std::make_unique<ScriptRunner>(
        engine, vm, xanadu::vql::fileScriptPorts(*host));
  }
  ~CaptureWorld() {
    sink->open();
    runner.reset();
    host.reset();
    std::error_code ec;
    fs::remove_all(base, ec);
  }
  auto put(const std::string &name, const std::string &content) -> std::string {
    const auto path = base / "data" / name;
    std::ofstream(path, std::ios::binary | std::ios::trunc) << content;
    return path.string();
  }
  auto start(const std::string &source) -> ScriptRunResult {
    return runner->run(Parser(source).parseQuery());
  }
  auto run(const std::string &source)
      -> std::expected<std::vector<zigzag::CellRef>, FrontendDiagnostic> {
    return xanadu::vql::driveToCompletion(
        *runner, start(source), [&] { host->pump(16); },
        [] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); });
  }
  auto rows(const std::string &source) -> std::vector<std::string> {
    const auto result = run(source);
    EXPECT_TRUE(result.has_value())
        << (result ? "" : result.error().code + ": " + result.error().message);
    std::vector<std::string> out;
    if (result)
      for (const auto cell : *result) out.push_back(arena.textOf(cell));
    return out;
  }
  auto failure(const std::string &source) -> std::string {
    const auto result = run(source);
    return result ? std::string{} : result.error().code;
  }
  [[nodiscard]] auto permascrollBytes() const -> std::uint64_t {
    return sink->highWaterBytes();
  }
  /// What the captures appended: the lengths on the journal's Append intents.
  /// The permascroll also grows by the publication's own scalar text, which is
  /// not capture content, so its size is not the measure.
  [[nodiscard]] auto appendedBytes() const -> std::uint64_t {
    xanadu::vortex::CaptureJournal journal(base / "capture.journal",
                                           std::nullopt);
    std::uint64_t total = 0;
    for (const auto &record : journal.recover().records) {
      if (record.kind == xanadu::vortex::CaptureJournalRecordKind::Append)
        total += record.appendLength;
    }
    return total;
  }
  auto captured(const std::string &prefix) -> std::vector<zigzag::CellRef> {
    const auto result = run(prefix + "return io_captured($c)");
    EXPECT_TRUE(result.has_value())
        << (result ? "" : result.error().code + ": " + result.error().message);
    return result ? *result : std::vector<zigzag::CellRef>{};
  }
  /// The text a captured cell quotes, read from the permascroll.
  [[nodiscard]] auto quotedText(const zigzag::CellRef cell) const
      -> std::string {
    return arena.textOf(cell, *perma);
  }
  [[nodiscard]] auto coordinator() -> xanadu::vortex::CaptureCoordinator & {
    return host->capture()->coordinator();
  }
};

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

TEST(VqlIoCaptureTest, ACapturedCellNamesNoStoreSoOnlyAQuotedSpanCarriesIt) {
  CaptureWorld w;
  const auto path  = w.put("in.txt", "quoted into a result");
  const auto cells = w.captured(open(path, "f") + read("f", "0, 64", "r") +
                                captureOf("r", "c"));
  ASSERT_EQ(cells.size(), 1U);
  // The arena has no store behind this cell, so resultRows' quotedContent()
  // finds nothing and the row would be typed in again.
  EXPECT_FALSE(w.arena.quotedContent(cells.front()).has_value());
  // But the span is in the author's permascroll, and a Store over that
  // permascroll can quote it: writeResultSlice carries it without a copy.
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

} // namespace
