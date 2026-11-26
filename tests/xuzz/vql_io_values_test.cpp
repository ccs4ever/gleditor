// M7-VQL-d: values (design/vortex-io-m7-vql-d.md). An outcome `match` in
// script syntax (exhaustive, `_` allowed, a missing tag is a diagnostic before
// anything runs); `await($h) as $r` as an owned record that leaves `$h`
// awaitable; a script-local `let` as a write source, offset or length; the pure
// `join` aggregate over rows; and the payload-less outcome constructor. No new
// world effect: nothing here submits more than the reads and writes a script
// names, which the tests count.

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

#include "common/xanadu/vortex/file_read_host.hpp"
#include "common/xanadu/vortex/owned_outcome.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/io_match.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/vql/script_runner.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "io_frontend_conformance.hpp"

namespace {

using namespace xuzz_conformance;
namespace fs = std::filesystem;
using xanadu::vql::hasIoEffect;
using xanadu::vql::IoMatchStatement;
using xanadu::vql::IoMatchTag;
using xanadu::vql::ParseError;
using xanadu::vql::Parser;
using xanadu::vql::QueryExecutionError;
using xanadu::vql::QueryExecutor;
using xanadu::vql::ScriptIoPorts;
using xanadu::vql::ScriptRunner;
using xanadu::vql::ScriptRunResult;
using xanadu::vql::VQLEngine;

constexpr auto matchCode = "VQL_IO_MATCH";

constexpr IoProgress terminalProgress{.transferredBytes = 2,
                                      .phase            = IoPhase::Terminal};

auto parseFailure(const std::string &source) -> std::string {
  try {
    Parser(source).parseQuery();
  } catch (const ParseError &error) {
    return error.what();
  }
  return {};
}

/// A read of $cap awaited and named, the shape every match test starts from.
auto readAs(const std::string &name) -> std::string {
  return "effect { let $s := submit($cap, read_bytes(0, 16)); "
         "match $s { admitted($q) => await($q), refused($e) => $e } } as $" +
         name + "; ";
}

/// The same read started and not awaited: `$name` is the request handle.
auto detachedAs(const std::string &name) -> std::string {
  return "effect { let $s := submit($cap, read_bytes(0, 16)); "
         "match $s { admitted($q) => $q, refused($e) => $e } } as $" +
         name + "; ";
}

// ---- Grammar and static checks ---------------------------------------------

TEST(VqlIoMatchGrammarTest, ArmsPatternsAndBindersParse) {
  const auto query =
      Parser(readAs("r") +
             "match $r { complete($p) => $p, partial($p, $why) => $why, "
             "failed(_, $d) => $d, end_of_stream => (\"eof\"), "
             "timed_out($u) => $u, _ => (io_tag($r)) }")
          .parseQuery();
  ASSERT_EQ(query.body.statements.size(), 2U);
  const auto &match = std::get<IoMatchStatement>(query.body.statements[1]);
  EXPECT_EQ(match.subject, "r");
  ASSERT_EQ(match.arms.size(), 6U);
  EXPECT_EQ(match.arms[0].tag, IoMatchTag::Complete);
  EXPECT_EQ(match.arms[0].binders, (std::vector<std::optional<std::string>>{
                                       std::optional<std::string>{"p"}}));
  EXPECT_EQ(match.arms[2].binders.size(), 2U);
  EXPECT_FALSE(match.arms[2].binders[0].has_value()) << "'_' binds nothing";
  EXPECT_EQ(match.arms[3].tag, IoMatchTag::EndOfStream);
  EXPECT_FALSE(match.arms[5].tag.has_value()) << "the '_' arm";
  EXPECT_TRUE(hasIoEffect(query));
}

TEST(VqlIoMatchGrammarTest, AMatchOnAReadMustNameEveryTagItCanEndWith) {
  // A read can end with twelve tags; a match that stops at the obvious ones
  // is the "forgot end_of_stream" bug this refusal exists to end.
  const auto message =
      parseFailure(readAs("r") + "match $r { complete($p) => $p, "
                                 "failed($c, $d) => $c }");
  EXPECT_NE(message.find(matchCode), std::string::npos) << message;
  EXPECT_NE(message.find("end_of_stream"), std::string::npos) << message;
  EXPECT_NE(message.find("rejected"), std::string::npos) << message;
}

TEST(VqlIoMatchGrammarTest, AWildcardOrEveryTagIsExhaustive) {
  EXPECT_EQ(parseFailure(readAs("r") + "match $r { complete($p) => $p, "
                                       "_ => (\"other\") }"),
            "");
  // A read's twelve tags (conflict included: the file adapter ends a raced
  // read with it), spelled out, with no wildcard.
  EXPECT_EQ(
      parseFailure(readAs("r") +
                   "match $r { rejected(_, _) => (1), complete(_) => (1), "
                   "end_of_stream => (1), partial(_, _) => (1), "
                   "failed(_, _) => (1), cancelled(_) => (1), "
                   "timed_out(_) => (1), overflow => (1), "
                   "message_too_large => (1), stale => (1), conflict => (1), "
                   "unknown => (1) }"),
      "");
}

TEST(VqlIoMatchGrammarTest, ATagTheOperationCannotEndWithIsRefused) {
  // A write has no end of stream, no overflow and no stale read.
  const auto write =
      std::string{"effect { let $s := submit($cap, sync()); match $s { "
                  "admitted($q) => await($q), refused($e) => $e } } as $w; "};
  for (const auto *tag : {"end_of_stream", "overflow", "stale"}) {
    SCOPED_TRACE(tag);
    const auto message =
        parseFailure(write + "match $w { " + tag + " => (1), _ => (2) }");
    EXPECT_NE(message.find(matchCode), std::string::npos) << message;
    EXPECT_NE(message.find("can end with"), std::string::npos) << message;
  }
  // `conflict` ends a write (generation CAS) and a read (the generation moved
  // around the transfer), so both accept an arm for it.
  EXPECT_EQ(parseFailure(write + "match $w { conflict => (1), _ => (2) }"), "");
  EXPECT_EQ(
      parseFailure(readAs("r") + "match $r { conflict => (1), _ => (2) }"), "");
  // Without it a read's exhaustive match is refused by name.
  const auto message = parseFailure(
      readAs("r") + "match $r { rejected(_, _) => (1), complete(_) => (1), "
                    "end_of_stream => (1), partial(_, _) => (1), "
                    "failed(_, _) => (1), cancelled(_) => (1), "
                    "timed_out(_) => (1), overflow => (1), "
                    "message_too_large => (1), stale => (1), unknown => (1) }");
  EXPECT_NE(message.find("conflict"), std::string::npos) << message;
}

TEST(VqlIoMatchGrammarTest, AWriteIsExhaustiveOverItsOwnEightTags) {
  const auto write =
      std::string{"effect { let $s := submit($cap, sync()); match $s { "
                  "admitted($q) => await($q), refused($e) => $e } } as $w; "};
  EXPECT_EQ(parseFailure(write + "match $w { rejected(_, _) => (1), "
                                 "complete(_) => (1), partial(_, _) => (1), "
                                 "failed(_, _) => (1), cancelled(_) => (1), "
                                 "timed_out(_) => (1), conflict => (1), "
                                 "unknown => (1) }"),
            "");
  const auto message = parseFailure(
      write + "match $w { complete(_) => (1), failed(_, _) => (1) }");
  EXPECT_NE(message.find("conflict"), std::string::npos) << message;
}

TEST(VqlIoMatchGrammarTest, BadPatternsAreRefusedWithTheCode) {
  const auto source = readAs("r");
  struct Case {
    const char *what;
    std::string text;
  };
  const std::vector<Case> cases{
      {"an unknown tag", "match $r { complet($p) => $p, _ => (1) }"},
      {"a repeated tag",
       "match $r { complete($p) => $p, complete($q) => $q, _ => (1) }"},
      {"an arm after the wildcard",
       "match $r { _ => (1), complete($p) => $p }"},
      {"too few binders", "match $r { complete => (1), _ => (2) }"},
      {"too many binders", "match $r { complete($a, $b) => $a, _ => (2) }"},
      {"binders on the wildcard", "match $r { _($x) => (2) }"},
      {"no arms", "match $r { }"},
  };
  for (const auto &test : cases) {
    SCOPED_TRACE(test.what);
    const auto message = parseFailure(source + test.text);
    EXPECT_NE(message.find(matchCode), std::string::npos) << message;
  }
}

TEST(VqlIoMatchGrammarTest, ANameThatIsNotAnOutcomeIsRefusedWhereItIsKnown) {
  // A detached read names a request handle, an open a capability, a capture
  // a receipt: none is an outcome, and each says what to do instead.
  const auto handle = parseFailure(detachedAs("h") + "match $h { _ => (1) }");
  EXPECT_NE(handle.find(matchCode), std::string::npos) << handle;
  EXPECT_NE(handle.find("await"), std::string::npos) << handle;
  const auto open =
      parseFailure("effect { open(\"x\") } as $f; match $f { _ => (1) }");
  EXPECT_NE(open.find(matchCode), std::string::npos) << open;
  EXPECT_NE(open.find("io_tag"), std::string::npos) << open;
  const auto capture =
      parseFailure(readAs("r") + "effect { capture($r) } as $c; match $c { "
                                 "_ => (1) }");
  EXPECT_NE(capture.find(matchCode), std::string::npos) << capture;
}

TEST(VqlIoMatchGrammarTest, AwaitOfAHandleKeepsTheOperationOfTheRead) {
  // `await($h) as $r` is an outcome of the read that made $h, so the match
  // after it is checked against a read's tags.
  const auto source = detachedAs("h") + "effect { await($h) } as $r; ";
  EXPECT_EQ(parseFailure(source + "match $r { complete($p) => $p, _ => (1) }"),
            "");
  // `end_of_stream` is a read's alone, so refusing it on a write's tags and
  // accepting it here shows the read's table was the one applied.
  EXPECT_EQ(
      parseFailure(source + "match $r { end_of_stream => (1), _ => (2) }"), "");
}

TEST(VqlIoMatchGrammarTest, AnUnknownSubjectNeedsEveryTagOrAWildcard) {
  // A variable an earlier script bound is not known to be a read or a write,
  // so only the twelve tags together, or `_`, cover it.
  const auto message = parseFailure("match $r { complete($p) => $p }");
  EXPECT_NE(message.find(matchCode), std::string::npos) << message;
  EXPECT_NE(message.find("conflict"), std::string::npos) << message;
  EXPECT_EQ(parseFailure("match $r { complete($p) => $p, _ => (0) }"), "");
}

TEST(VqlIoMatchGrammarTest, ALetThatRebindsTheNameForgetsWhatItWas) {
  // After `let $r`, $r is no longer the read's outcome, so the match is
  // checked as an unknown subject (stricter), not as a read.
  const auto message = parseFailure(readAs("r") + "let $r := 1; match $r { "
                                                  "complete($p) => $p }");
  EXPECT_NE(message.find(matchCode), std::string::npos) << message;
}

TEST(VqlIoMatchGrammarTest, AMatchIsRefusedWhereAnActionIs) {
  for (const auto *source : {
           "def f() := { match $r { _ => (1) } }; f()",
           "watch ##/d.x as $v { match $r { _ => (1) } }",
           "with perspective(p) { match $r { _ => (1) } }",
       }) {
    SCOPED_TRACE(source);
    EXPECT_NE(parseFailure(source).find("VQL_IO_QUERY_CONTEXT"),
              std::string::npos);
  }
}

TEST(VqlIoMatchGrammarTest, ThePatternTableIsOneStatedFact) {
  using xanadu::vql::ioMatchArity;
  using xanadu::vql::ioMatchTagName;
  using xanadu::vql::ioMatchTagOf;
  using xanadu::vql::IoOperation;
  using xanadu::vql::ioTagsOf;
  for (std::int64_t i = 0;
       i < static_cast<std::int64_t>(xanadu::vql::ioMatchTagCount); ++i) {
    const auto tag = static_cast<IoMatchTag>(i);
    EXPECT_EQ(ioMatchTagOf(ioMatchTagName(tag)), tag);
    EXPECT_LE(ioMatchArity(tag), 2U);
  }
  EXPECT_FALSE(ioMatchTagOf("Complete")) << "spellings are snake_case";
  // Rejected, the admission refusal, ends every operation.
  for (const auto operation :
       {IoOperation::Read, IoOperation::Write, IoOperation::Any}) {
    EXPECT_NE(ioTagsOf(operation) & xanadu::vql::ioTagBit(IoMatchTag::Rejected),
              0U);
  }
}

// ---- The compiled shape ----------------------------------------------------

TEST(VqlIoMatchCompileTest, AMatchLowersToARelationAndBranchesNotASubmit) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  xanadu::vql::VQLCompiler compiler(core, vm);
  const auto compiled = compiler.compile(
      readAs("r") + "match $r { complete($p) => $p, _ => (io_tag($r)) }");
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  // The read is one action; the match is a second site that never waits.
  ASSERT_EQ(compiled.ioActions.size(), 2U);
  EXPECT_TRUE(compiled.ioActions[0].awaitOpcode.has_value());
  EXPECT_FALSE(compiled.ioActions[1].awaitOpcode.has_value());
  EXPECT_EQ(compiled.ioActions[1].statement, 1U);
  EXPECT_FALSE(compiled.finalIsIoEffect);
  EXPECT_NE(compiled.disassembly.find("io_match_tag"), std::string::npos);
  std::size_t submits = 0;
  for (const auto &site : compiled.ioActions)
    if (vm.getOpcodeKind(site.submitOpcode) ==
        zigzag::vortex::OpcodeKind::Submit)
      ++submits;
  EXPECT_EQ(submits, 1U) << "a match adds no Submit";
}

TEST(VqlIoMatchCompileTest, ASynchronousExecutorRefusesAMatch) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  VQLEngine engine(core);
  QueryExecutor executor(engine, vm);
  const auto executed = executor.execute("match $r { _ => (1) }");
  ASSERT_FALSE(executed);
  EXPECT_NE(executed.error().message.find("VQL_IO_EFFECT_NEEDS_RUNNER"),
            std::string::npos);
}

// ---- The runner over the fake backend --------------------------------------

struct ValueWorld {
  World w;
  VQLEngine engine{w.core};
  std::vector<CellRef> specs;
  std::unique_ptr<ScriptRunner> runner;

  explicit ValueWorld(
      const std::size_t retained =
          xanadu::vortex::FileReadHostOptions::defaultBudgetBytes,
      xanadu::vql::QueryExecutionOptions options = {}) {
    ScriptIoPorts ports{w.bridge, w.host, w.payloads, w.diagnostics,
                        [this](CapabilityCell, std::uint64_t, std::uint64_t)
                            -> std::expected<CellRef, FrontendDiagnostic> {
                          return specs.emplace_back(w.specification());
                        }};
    ports.retainedPayloadBytes = retained;
    runner =
        std::make_unique<ScriptRunner>(engine, w.vm, std::move(ports), options);
    engine.setVariable("cap", w.capability.cell);
  }

  auto run(const std::string &source) -> ScriptRunResult {
    return runner->run(Parser(source).parseQuery());
  }
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
  auto finish(IoOutcome outcome) -> ScriptRunResult {
    w.complete(w.submissions() - 1, std::move(outcome));
    return wakeAndResume();
  }
  static auto parked(const ScriptRunResult &result) -> bool {
    return std::holds_alternative<ScriptRunResult::Parked>(result.value);
  }
  static auto failure(const ScriptRunResult &result) -> std::string {
    const auto *failed = std::get_if<ScriptRunResult::Failed>(&result.value);
    return failed ? failed->diagnostic.code + ": " + failed->diagnostic.message
                  : std::string{};
  }
  static auto code(const ScriptRunResult &result) -> std::string {
    const auto *failed = std::get_if<ScriptRunResult::Failed>(&result.value);
    return failed ? failed->diagnostic.code : std::string{};
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
  auto texts(const std::vector<CellRef> &rows) -> std::vector<std::string> {
    std::vector<std::string> out;
    for (const auto row : rows) out.push_back(w.arena.textOf(row));
    return out;
  }
  auto completeWithPayload() -> IoOutcome {
    return Complete{w.payloadRange(), {}, terminalProgress};
  }
};

using Texts = std::vector<std::string>;

constexpr auto describeAll =
    "match $r { rejected($c, $d) => (\"rejected\"), complete($p) => $p, "
    "end_of_stream => (\"eof\"), partial($p, $why) => $why, "
    "failed($c, $d) => $c, cancelled($u) => $u, timed_out($u) => $u, "
    "overflow => (\"overflow\"), message_too_large => (\"big\"), "
    "stale => (\"stale\"), conflict => (\"conflict\"), "
    "unknown => (\"unknown\") }";

TEST(VqlIoMatchRunTest, EveryTerminalTagSelectsItsOwnArmAndBindsItsFields) {
  struct Case {
    const char *name;
    IoOutcome outcome;
    Texts expected;
  };
  ValueWorld probe;
  const std::vector<Case> cases{
      {"Complete",
       Complete{probe.w.payloadRange(), {}, terminalProgress},
       {"ok"}},
      {"EndOfStream", EndOfStream{terminalProgress}, {"eof"}},
      {"Partial",
       Partial{probe.w.payloadRange(), PartialCause::BackendFailure,
               terminalProgress},
       {"BackendFailure"}},
      {"Failed",
       Failed{{IoErrorCode::BackendFailure, 3}, {}, terminalProgress},
       {"BackendFailure"}},
      {"Cancelled", Cancelled{{}, terminalProgress, true}, {"true"}},
      {"TimedOut", TimedOut{{}, terminalProgress, false}, {"false"}},
      {"Unknown", Unknown{terminalProgress}, {"unknown"}},
      {"Conflict",
       Conflict{Generation{1}, Generation{2}, terminalProgress},
       {"conflict"}},
  };
  for (const auto &test : cases) {
    SCOPED_TRACE(test.name);
    ValueWorld s;
    // Complete's payload range belongs to the probe's resolver; use a fresh
    // one for this world.
    auto outcome = test.outcome;
    if (auto *complete = std::get_if<Complete>(&outcome))
      complete->payload = s.w.payloadRange();
    if (auto *partial = std::get_if<Partial>(&outcome))
      partial->payload = s.w.payloadRange();
    const auto rows = s.drive(readAs("r") + describeAll, {std::move(outcome)});
    EXPECT_EQ(s.texts(rows), test.expected);
    EXPECT_EQ(s.w.submissions(), 1U) << "a match submits nothing";
  }
}

TEST(VqlIoMatchRunTest, ATagNoArmNamedIsATypedRefusalNotAnEmptyResult) {
  // The parser checks a match against a table that can lag an adapter (it did
  // for a read's `conflict`), so the compiled match must itself end in a
  // refusal when it has no `_`. The arm is removed after parsing to stand in
  // for such a table.
  ValueWorld s;
  auto query = Parser(readAs("r") + "match $r { complete(_) => (\"c\"), "
                                    "_ => (\"other\") }")
                   .parseQuery();
  auto &match = std::get<IoMatchStatement>(query.body.statements[1]);
  ASSERT_EQ(match.arms.size(), 2U);
  match.arms.pop_back();
  auto result = s.runner->run(std::move(query));
  ASSERT_TRUE(ValueWorld::parked(result));
  result = s.finish(EndOfStream{terminalProgress});
  EXPECT_EQ(ValueWorld::code(result), matchCode) << ValueWorld::failure(result);
}

TEST(VqlIoMatchRunTest, RejectedBindsTheTypedErrorAndItsDetail) {
  ValueWorld s;
  s.w.refuseAdmission = true;
  const auto rows     = s.drive(
      readAs("r") + "match $r { rejected($c, $d) => $c, _ => (\"other\") }",
      {});
  EXPECT_EQ(s.texts(rows), (Texts{"InvalidRequest"}));
  const auto detail = s.drive(
      readAs("r") + "match $r { rejected(_, $d) => $d, _ => (\"other\") }", {});
  EXPECT_EQ(s.texts(detail), (Texts{"9"}));
}

TEST(VqlIoMatchRunTest, TheWildcardTakesWhateverNoArmNamed) {
  ValueWorld s;
  const auto rows = s.drive(
      readAs("r") + "match $r { complete($p) => $p, _ => (io_tag($r)) }",
      {EndOfStream{terminalProgress}});
  EXPECT_EQ(s.texts(rows), (Texts{"EndOfStream"}));
}

TEST(VqlIoMatchRunTest, ABinderForAFieldTheOutcomeLacksIsEmptyNotAnError) {
  // A complete read that retained no bytes: $p is an empty stream, the arm's
  // row is nothing, and the script still completes.
  ValueWorld s;
  const auto rows =
      s.drive(readAs("r") + "match $r { complete($p) => $p, _ => (1) }",
              {Complete{{}, {}, terminalProgress}});
  EXPECT_TRUE(rows.empty());
}

TEST(VqlIoMatchRunTest, AnArmMayUseTheBinderInAValueExpression) {
  ValueWorld s;
  const auto rows =
      s.drive(readAs("r") + "match $r { complete($p) => (string($p) + \"!\"), "
                            "_ => (\"no\") }",
              {s.completeWithPayload()});
  EXPECT_EQ(s.texts(rows), (Texts{"ok!"}));
}

TEST(VqlIoMatchRunTest, TheStatementAfterAMatchStillRuns) {
  ValueWorld s;
  const auto rows =
      s.drive(readAs("r") + "match $r { complete($p) => $p, _ => (\"x\") }; "
                            "return io_tag($r)",
              {s.completeWithPayload()});
  EXPECT_EQ(s.texts(rows), (Texts{"Complete"}));
}

TEST(VqlIoMatchRunTest, ABinderIsScopedToItsArm) {
  // $p of the complete arm is not visible in the arm after it, so the second
  // arm reading `$p` reads whatever $p was before (nothing here): the compile
  // does not leak the binding.
  ValueWorld s;
  const auto rows =
      s.drive(readAs("r") + "match $r { complete($p) => $p, _ => ($p) }",
              {EndOfStream{terminalProgress}});
  EXPECT_TRUE(rows.empty());
  const auto after = s.drive(readAs("r") + "match $r { complete($p) => $p, "
                                           "_ => (\"y\") }; return $p",
                             {s.completeWithPayload()});
  EXPECT_TRUE(after.empty()) << "$p did not outlive the match";
}

TEST(VqlIoMatchRunTest, MatchBindersExposeThePayloadLikeIoPayloadDoes) {
  // The exposure set (design §4.7) must see a payload a binder turned into
  // text, or `-o` would persist uncaptured bytes.
  ValueWorld s;
  const auto before = s.runner->persistability();
  EXPECT_FALSE(before.has_value());
  const auto rows =
      s.drive(readAs("r") + "match $r { complete($p) => $p, _ => (\"x\") }",
              {s.completeWithPayload()});
  ASSERT_EQ(rows.size(), 1U);
  const auto after = s.runner->persistability();
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(after->exposed, 1U);
  EXPECT_EQ(after->uncovered, 1U);
}

TEST(VqlIoMatchRunTest, AMatchOnAHandleOrACapabilityIsATypedRefusalAtRunTime) {
  // $cap is a session variable, so the parser does not know what it is; the
  // host does, and a capability is no outcome.
  ValueWorld s;
  const auto result = s.run("match $cap { _ => (1) }");
  EXPECT_EQ(ValueWorld::code(result), matchCode) << ValueWorld::failure(result);
  EXPECT_EQ(s.w.submissions(), 0U);
}

TEST(VqlIoMatchRunTest, AStaleNameIsTheTypedStaleRefusalNotAnArm) {
  ValueWorld s;
  ASSERT_EQ(
      s.drive(readAs("old") + "return $old", {s.completeWithPayload()}).size(),
      1U);
  // The next script's admission retires the finished one's records, so `$old`
  // names a freed slot from then on, as every other reader of it is told.
  ASSERT_EQ(
      s.drive(readAs("g") + "return $g", {s.completeWithPayload()}).size(), 1U);
  const auto next = s.run("match $old { _ => (\"arm\") }");
  EXPECT_EQ(ValueWorld::code(next), "VQL_IO_STALE_BINDING")
      << ValueWorld::failure(next);
}

TEST(VqlIoMatchRunTest, EveryOneOfTwelveTagsDispatchesFromAnOutcomeCell) {
  // The parser's table has twelve tags; the runner must number each the same.
  // Detached outcome cells give all twelve, including the four a fake read
  // backend does not produce.
  using xanadu::vortex::IoOutcomeTag;
  const std::vector<std::pair<IoOutcomeTag, std::string>> tags{
      {IoOutcomeTag::Rejected, "rejected"},
      {IoOutcomeTag::Complete, "complete"},
      {IoOutcomeTag::EndOfStream, "end_of_stream"},
      {IoOutcomeTag::Partial, "partial"},
      {IoOutcomeTag::Failed, "failed"},
      {IoOutcomeTag::Cancelled, "cancelled"},
      {IoOutcomeTag::TimedOut, "timed_out"},
      {IoOutcomeTag::Overflow, "overflow"},
      {IoOutcomeTag::MessageTooLarge, "message_too_large"},
      {IoOutcomeTag::Stale, "stale"},
      {IoOutcomeTag::Conflict, "conflict"},
      {IoOutcomeTag::Unknown, "unknown"},
  };
  ValueWorld s;
  const std::string arms =
      "match $o { rejected(_, _) => (\"rejected\"), complete(_) => "
      "(\"complete\"), end_of_stream => (\"end_of_stream\"), partial(_, _) => "
      "(\"partial\"), failed(_, _) => (\"failed\"), cancelled(_) => "
      "(\"cancelled\"), timed_out(_) => (\"timed_out\"), overflow => "
      "(\"overflow\"), message_too_large => (\"message_too_large\"), stale => "
      "(\"stale\"), conflict => (\"conflict\"), unknown => (\"unknown\") }";
  for (const auto &[tag, name] : tags) {
    SCOPED_TRACE(name);
    const auto cell = xanadu::vortex::makeDetachedOutcomeCell(
        s.w.arena, s.w.bridge.schema(), tag);
    ASSERT_TRUE(cell);
    s.engine.setVariable("o", cell->cell);
    const auto result = s.run(arms);
    const auto *done  = std::get_if<ScriptRunResult::Completed>(&result.value);
    ASSERT_NE(done, nullptr) << ValueWorld::failure(result);
    EXPECT_EQ(s.texts(done->value), (Texts{name}));
  }
}

// ---- await($h) as $r: an owned record that leaves $h awaitable -------------

TEST(VqlIoAwaitRecordTest,
     ANamedAwaitIsAnOwnedRecordAndTheHandleStaysAwaitable) {
  ValueWorld s;
  auto result = s.run(detachedAs("h") + "effect { await($h) } as $r; "
                                        "effect { await($h) } as $again; "
                                        "return io_tag($r), io_tag($again), "
                                        "io_payload($r)");
  ASSERT_TRUE(ValueWorld::parked(result)) << ValueWorld::failure(result);
  result           = s.finish(s.completeWithPayload());
  const auto *done = std::get_if<ScriptRunResult::Completed>(&result.value);
  ASSERT_NE(done, nullptr) << ValueWorld::failure(result);
  EXPECT_EQ(s.texts(done->value), (Texts{"Complete", "Complete", "ok"}));
  EXPECT_EQ(s.w.submissions(), 1U) << "awaiting again never submits";
  EXPECT_EQ(s.runner->ownedRecordCount(), 2U)
      << "each named await is its own record";
}

TEST(VqlIoAwaitRecordTest,
     TheRequestIsKeptWhileTheScriptRunsAndRetiredAtItsEnd) {
  ValueWorld s;
  auto result = s.run(detachedAs("h") + "effect { await($h) } as $r; "
                                        "return io_tag($r)");
  ASSERT_TRUE(ValueWorld::parked(result));
  result = s.finish(s.completeWithPayload());
  ASSERT_NE(std::get_if<ScriptRunResult::Completed>(&result.value), nullptr);
  // Retired at the next admission, with the rest of the finished script.
  EXPECT_EQ(s.w.service.requestCount(), 1U) << "the handle's request is held";
  result = s.run(detachedAs("g") + "return io_tag($g)");
  ASSERT_NE(std::get_if<ScriptRunResult::Completed>(&result.value), nullptr)
      << ValueWorld::failure(result);
  EXPECT_EQ(s.w.service.requestCount(), 1U)
      << "only the new script's own request is left";
}

TEST(VqlIoAwaitRecordTest, TheRecordHasTheFieldsOfTheOutcomeItCopied) {
  ValueWorld s;
  const auto rows =
      s.drive(detachedAs("h") + "effect { await($h) } as $r; "
                                "return $r",
              {Failed{{IoErrorCode::BackendFailure, 3}, {}, terminalProgress}});
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_TRUE(
      xanadu::vortex::isOwnedOutcome(s.w.arena, s.w.bridge.schema(), rows[0]));
  const auto record = OutcomeCell{rows[0], *s.w.arena.incarnationOf(rows[0])};
  EXPECT_EQ(s.w.bridge.outcomeField(record, "errorCode"), "BackendFailure");
}

TEST(VqlIoAwaitRecordTest, ANamedCancelIsARecordToo) {
  ValueWorld s;
  auto result      = s.run(detachedAs("h") + "effect { cancel($h) } as $c; "
                                                  "effect { await($h) } as $r; "
                                                  "return io_tag($c), io_tag($r)");
  const auto *done = std::get_if<ScriptRunResult::Completed>(&result.value);
  if (done == nullptr) {
    ASSERT_TRUE(ValueWorld::parked(result)) << ValueWorld::failure(result);
    result = s.finish(Cancelled{{}, terminalProgress, false});
    done   = std::get_if<ScriptRunResult::Completed>(&result.value);
  }
  ASSERT_NE(done, nullptr) << ValueWorld::failure(result);
  ASSERT_EQ(done->value.size(), 2U);
  EXPECT_EQ(s.w.arena.textOf(done->value[0]), "Cancelled");
  EXPECT_EQ(s.w.arena.textOf(done->value[1]), "Cancelled");
}

TEST(VqlIoAwaitRecordTest, AnAwaitOfARefusedHandleIsACopyOfTheRefusal) {
  ValueWorld s;
  s.w.refuseAdmission = true;
  const auto rows     = s.drive(detachedAs("h") + "effect { await($h) } as $r; "
                                                      "effect { await($h) } as $r2; "
                                                      "return io_tag($r), io_tag($r2), "
                                                      "io_error($r)",
                                {});
  EXPECT_EQ(s.texts(rows), (Texts{"Rejected", "Rejected", "InvalidRequest"}));
}

TEST(VqlIoAwaitRecordTest,
     PastTheRetentionBudgetTheNameStaysTheServicesOutcome) {
  // The two payload bytes pin a whole chunk; a budget below it declines the
  // copy, and the name then reads the service's own outcome, as a named read
  // does. The handle is unharmed either way.
  constexpr std::size_t tooSmall = 1;
  ValueWorld s(tooSmall);
  const auto rows = s.drive(detachedAs("h") + "effect { await($h) } as $r; "
                                              "effect { await($h) } as $a; "
                                              "return io_tag($r), io_tag($a)",
                            {s.completeWithPayload()});
  EXPECT_EQ(s.texts(rows), (Texts{"Complete", "Complete"}));
  EXPECT_EQ(s.runner->ownedRecordCount(), 0U);
}

TEST(VqlIoAwaitRecordTest,
     ManyNamedAwaitRunsLeaveTheArenaAndTheServiceBounded) {
  ValueWorld s;
  const auto once = [&] {
    auto result = s.run(detachedAs("h") + "effect { await($h) } as $r; "
                                          "effect { await($h) } as $a; "
                                          "return $r");
    ASSERT_TRUE(ValueWorld::parked(result));
    result = s.finish(s.completeWithPayload());
    ASSERT_NE(std::get_if<ScriptRunResult::Completed>(&result.value), nullptr)
        << ValueWorld::failure(result);
    s.freeOldSpecs();
  };
  for (std::size_t i = 0; i < 8; ++i) once();
  const auto warmSlots       = s.w.arena.cellCount();
  const auto warmLive        = s.w.arena.liveCells();
  const auto warmLeases      = s.w.payloads.liveLeases();
  constexpr std::size_t runs = 10'000;
  for (std::size_t i = 0; i < runs; ++i) once();
  EXPECT_EQ(s.w.arena.cellCount(), warmSlots);
  EXPECT_EQ(s.w.arena.liveCells(), warmLive);
  EXPECT_EQ(s.w.payloads.liveLeases(), warmLeases);
  EXPECT_LE(s.w.service.requestCount(), 1U);
}

TEST(VqlIoAwaitRecordTest, AnAwaitOfAHandleAnEarlierScriptAdmittedIsOwnedToo) {
  // The detached read outlives the script that started it; the name made by
  // awaiting it later must still be a record, not the service's outcome, which
  // the next admission frees along with the request.
  ValueWorld s;
  auto first = s.run(detachedAs("h") + "return 1");
  ASSERT_NE(std::get_if<ScriptRunResult::Completed>(&first.value), nullptr)
      << ValueWorld::failure(first);
  auto second = s.run("effect { await($h) } as $r; return io_tag($r)");
  ASSERT_TRUE(ValueWorld::parked(second)) << ValueWorld::failure(second);
  second           = s.finish(s.completeWithPayload());
  const auto *done = std::get_if<ScriptRunResult::Completed>(&second.value);
  ASSERT_NE(done, nullptr) << ValueWorld::failure(second);
  EXPECT_EQ(s.texts(done->value), (Texts{"Complete"}));
  EXPECT_EQ(s.runner->ownedRecordCount(), 1U)
      << "the name is the script's own record";
  // The record is released with the next admission; the name then answers the
  // typed stale refusal (`io_tag` has no stale check of its own, `match` does).
  const auto third = s.run(detachedAs("g") + "return 1");
  ASSERT_NE(std::get_if<ScriptRunResult::Completed>(&third.value), nullptr)
      << ValueWorld::failure(third);
  const auto fourth = s.run("match $r { _ => (1) }");
  EXPECT_EQ(ValueWorld::code(fourth), "VQL_IO_STALE_BINDING")
      << ValueWorld::failure(fourth);
}

TEST(VqlIoAwaitRecordTest, ManyRefusedNamedAwaitRunsLeaveTheArenaBounded) {
  // A refusal has no request whose reaping frees its Rejected outcome, so the
  // runner's retire must; the record copy is the script's own.
  ValueWorld s;
  s.w.refuseAdmission = true;
  const auto once     = [&] {
    auto result      = s.run(detachedAs("h") + "effect { await($h) } as $r; "
                                                            "return io_tag($r)");
    const auto *done = std::get_if<ScriptRunResult::Completed>(&result.value);
    ASSERT_NE(done, nullptr) << ValueWorld::failure(result);
    for (const auto row : done->value) {
      if (const auto incarnation = s.w.arena.incarnationOf(row))
        static_cast<void>(s.w.arena.freeCell(row, *incarnation));
    }
    s.freeOldSpecs();
  };
  for (std::size_t i = 0; i < 8; ++i) once();
  const auto warmLive        = s.w.arena.liveCells();
  constexpr std::size_t runs = 10'000;
  for (std::size_t i = 0; i < runs; ++i) once();
  EXPECT_EQ(s.w.arena.liveCells(), warmLive);
}

TEST(VqlIoAwaitRecordTest, ManyPlainDetachedRefusalRunsLeaveTheArenaBounded) {
  ValueWorld s;
  s.w.refuseAdmission = true;
  const auto once     = [&] {
    auto result      = s.run(detachedAs("h") + "return io_tag($h)");
    const auto *done = std::get_if<ScriptRunResult::Completed>(&result.value);
    ASSERT_NE(done, nullptr) << ValueWorld::failure(result);
    for (const auto row : done->value) {
      if (const auto incarnation = s.w.arena.incarnationOf(row))
        static_cast<void>(s.w.arena.freeCell(row, *incarnation));
    }
    s.freeOldSpecs();
  };
  for (std::size_t i = 0; i < 8; ++i) once();
  const auto warmLive        = s.w.arena.liveCells();
  constexpr std::size_t runs = 10'000;
  for (std::size_t i = 0; i < runs; ++i) once();
  EXPECT_EQ(s.w.arena.liveCells(), warmLive);
}

TEST(VqlIoMatchRunTest, ManyMatchRunsLeaveTheArenaAndTheServiceBounded) {
  ValueWorld s;
  const auto once = [&] {
    auto result = s.run(readAs("r") + "match $r { complete($p) => $p, "
                                      "end_of_stream => (\"eof\"), "
                                      "_ => (io_tag($r)) }");
    ASSERT_TRUE(ValueWorld::parked(result));
    result           = s.finish(s.completeWithPayload());
    const auto *done = std::get_if<ScriptRunResult::Completed>(&result.value);
    ASSERT_NE(done, nullptr) << ValueWorld::failure(result);
    // The payload text is a row the caller owns; a test that frees nothing
    // would count it as growth.
    for (const auto row : done->value) {
      if (const auto incarnation = s.w.arena.incarnationOf(row))
        static_cast<void>(s.w.arena.freeCell(row, *incarnation));
    }
    s.freeOldSpecs();
  };
  for (std::size_t i = 0; i < 8; ++i) once();
  const auto warmSlots       = s.w.arena.cellCount();
  const auto warmLive        = s.w.arena.liveCells();
  const auto warmLeases      = s.w.payloads.liveLeases();
  constexpr std::size_t runs = 10'000;
  for (std::size_t i = 0; i < runs; ++i) once();
  EXPECT_EQ(s.w.arena.cellCount(), warmSlots);
  EXPECT_EQ(s.w.arena.liveCells(), warmLive);
  EXPECT_EQ(s.w.payloads.liveLeases(), warmLeases);
  EXPECT_LE(s.w.service.requestCount(), 1U);
}

// ---- Real files: a script-local `let` as a source, an offset, a length ----

/// One submit of @p description on $cap, awaited, bound to $name.
auto act(const std::string &cap, const std::string &description,
         const std::string &name, const std::string &within = {})
    -> std::string {
  return "effect { let $s := submit($" + cap + ", " + description + ")" +
         (within.empty() ? "" : " within " + within) +
         "; match $s { admitted($q) => await($q), refused($e) => $e } } as $" +
         name + "; ";
}

auto open(const std::string &path, const std::string &mode,
          const std::string &name) -> std::string {
  return "effect { open(\"" + path + "\") " + mode + " } as $" + name + "; ";
}

auto slurp(const fs::path &path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// A runner over a real file host whose write grant is `root/` of a fresh
/// tree.
struct FileWorld {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
  VQLEngine engine{core};
  fs::path base;
  fs::path root;
  std::unique_ptr<xanadu::vortex::FileReadHost> host;
  std::unique_ptr<ScriptRunner> runner;

  explicit FileWorld(xanadu::vql::QueryExecutionOptions options      = {},
                     xanadu::vortex::FileReadHostOptions hostOptions = {}) {
    static std::uint32_t counter = 0;
    base = fs::weakly_canonical(fs::temp_directory_path()) /
           ("vql_io_values_" + std::to_string(::getpid()) + "_" +
            std::to_string(counter++));
    root = base / "root";
    fs::create_directories(root);
    hostOptions.writeGrant = xanadu::vortex::writeRootGrant({root});
    host =
        std::make_unique<xanadu::vortex::FileReadHost>(vm, arena, hostOptions);
    runner = std::make_unique<ScriptRunner>(
        engine, vm, xanadu::vql::fileScriptPorts(*host), options);
  }
  ~FileWorld() {
    runner.reset();
    host.reset();
    std::error_code ec;
    fs::remove_all(base, ec);
  }
  auto put(const fs::path &path, const std::string &content) -> fs::path {
    std::ofstream(path, std::ios::binary | std::ios::trunc) << content;
    return path;
  }
  auto run(const std::string &source)
      -> std::expected<std::vector<zigzag::CellRef>, FrontendDiagnostic> {
    return xanadu::vql::driveToCompletion(
        *runner, runner->run(Parser(source).parseQuery()),
        [&] { host->pump(16); },
        [] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); });
  }
  auto rows(const std::string &source) -> Texts {
    const auto result = run(source);
    EXPECT_TRUE(result.has_value())
        << (result ? "" : result.error().code + ": " + result.error().message);
    Texts out;
    if (result)
      for (const auto cell : *result) out.push_back(arena.textOf(cell));
    return out;
  }
  auto failure(const std::string &source) -> std::string {
    const auto result = run(source);
    return result ? std::string{} : result.error().code;
  }
  /// Bind @p count text cells "r0", "r1", ... to the session variable $rows.
  void bindRows(const std::size_t count) {
    std::vector<zigzag::CellRef> cells;
    cells.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
      cells.push_back(arena.makeCell("r" + std::to_string(i)));
    engine.setVariable("rows", cells);
  }
};

TEST(VqlIoLetSourceTest, ALetOfTheSameScriptIsAWriteSource) {
  FileWorld f;
  const auto path = (f.root / "report.txt").string();
  const auto rows = f.rows("let $text := \"hello world\"; " +
                           open(path, "for write create", "o") +
                           act("o", "write_bytes(0, $text)", "w") +
                           "return io_tag($w), io_bytes($w)");
  EXPECT_EQ(rows, (Texts{"Complete", "11"}));
  EXPECT_EQ(slurp(path), "hello world");
}

TEST(VqlIoLetSourceTest, ALetIsAnAppendSourceAndAnOffsetAndALength) {
  FileWorld f;
  const auto path = f.put(f.root / "grid.txt", "aaaaaaaaaa").string();
  const auto rows = f.rows("let $patch := \"XY\"; let $at := 3; "
                           "let $len := 4; " +
                           open(path, "for write", "o") +
                           act("o", "write_bytes($at, $patch)", "w") +
                           act("o", "read_bytes($at, $len)", "r") +
                           "return io_tag($w), io_payload($r)");
  EXPECT_EQ(rows, (Texts{"Complete", "XYaa"}));
  EXPECT_EQ(slurp(path), "aaaXYaaaaa");
  const auto appended = f.put(f.root / "log.txt", "a;").string();
  static_cast<void>(
      f.rows("let $line := \"b;\"; " + open(appended, "for append", "o") +
             act("o", "append_bytes($line)", "w") + "return io_tag($w)"));
  EXPECT_EQ(slurp(appended), "a;b;");
}

TEST(VqlIoLetSourceTest, ALetBoundToAnOutcomeIsAnOutcomeSourceToo) {
  // The existing rule for a named outcome, reached through a let.
  FileWorld f;
  const auto from = f.put(f.root / "from.txt", "bytes\n").string();
  const auto to   = (f.root / "to.txt").string();
  static_cast<void>(
      f.rows(open(from, "", "i") + act("i", "read_bytes(0, 64)", "r") +
             "let $copy := $r; " + open(to, "for write create", "o") +
             act("o", "write_bytes(0, $copy)", "w") + "return io_tag($w)"));
  EXPECT_EQ(slurp(to), "bytes\n");
}

TEST(VqlIoLetSourceTest, ANumericLetIsWrittenAsItsCanonicalText) {
  // A computed number has no spooled text; it must be written as the digits
  // `join` and `string()` give it, not as zero bytes reported Complete.
  const std::vector<std::pair<std::string, std::string>> cases{
      {"42", "42"}, {"1 + 2", "3"}, {"2.5", "2.5"}, {"join(1, \",\")", "1"}};
  for (const auto &[expression, expected] : cases) {
    SCOPED_TRACE(expression);
    FileWorld f;
    const auto path = (f.root / "num.txt").string();
    const auto rows = f.rows("let $t := " + expression + "; " +
                             open(path, "for write create", "o") +
                             act("o", "write_bytes(0, $t)", "w") +
                             "return io_tag($w), io_bytes($w)");
    EXPECT_EQ(rows, (Texts{"Complete", std::to_string(expected.size())}));
    EXPECT_EQ(slurp(path), expected);
  }
}

TEST(VqlIoLetSourceTest, ALetThatHoldsNoValueIsATypedRefusalAndWritesNothing) {
  FileWorld f;
  const auto path = (f.root / "never.txt").string();
  // io_payload of a read that retained none answers no row, so the let holds
  // an empty stream, which is not the empty file "" a write could mean.
  const auto code = f.failure(
      open(path, "for write create", "o") + act("o", "sync()", "d") +
      "effect { open(\"" + (f.root / "empty.txt").string() +
      "\") for write create } as $e; " + act("e", "read_bytes(0, 8)", "r") +
      "let $t := io_payload($r); " + act("o", "write_bytes(0, $t)", "w") +
      "return 1");
  EXPECT_EQ(code, "VQL_IO_EFFECT_ARGUMENT");
  EXPECT_EQ(slurp(path), "") << "nothing was written";
}

TEST(VqlIoLetSourceTest, ALetOfSeveralValuesIsATypedRefusalNotTheFirstOfThem) {
  FileWorld f;
  f.bindRows(3);
  const auto path = (f.root / "many.txt").string();
  const auto code =
      f.failure("let $t := $rows; " + open(path, "for write create", "o") +
                act("o", "write_bytes(0, $t)", "w") + "return 1");
  EXPECT_EQ(code, "VQL_IO_EFFECT_ARGUMENT");
  EXPECT_EQ(slurp(path), "");
}

TEST(VqlIoLetSourceTest, ALetOfANonNumberIsATypedOffsetRefusal) {
  FileWorld f;
  const auto path = f.put(f.root / "n.txt", "abcdef").string();
  const auto code =
      f.failure("let $at := \"far\"; " + open(path, "for write", "o") +
                act("o", "write_bytes($at, \"x\")", "w") + "return 1");
  EXPECT_EQ(code, "VQL_IO_EFFECT_ARGUMENT");
  EXPECT_EQ(slurp(path), "abcdef");
}

TEST(VqlIoLetSourceTest, ASessionVariableStillWorksAndAnUnboundNameStillFails) {
  FileWorld f;
  f.engine.setVariable("text", f.arena.makeCell("session text"));
  const auto path = (f.root / "s.txt").string();
  static_cast<void>(f.rows(open(path, "for write create", "o") +
                           act("o", "write_bytes(0, $text)", "w") +
                           "return io_tag($w)"));
  EXPECT_EQ(slurp(path), "session text");
  EXPECT_EQ(f.failure(open(path, "for write", "o2") +
                      act("o2", "write_bytes(0, $nowhere)", "w")),
            "VQL_IO_EFFECT_ARGUMENT");
  EXPECT_EQ(slurp(path), "session text") << "never written as its own name";
}

TEST(VqlIoLetSourceTest, ALetAfterTheActionDoesNotCoverIt) {
  // The name must be bound before the action reaches it.
  FileWorld f;
  const auto path = (f.root / "late.txt").string();
  EXPECT_EQ(f.failure(open(path, "for write create", "o") +
                      act("o", "write_bytes(0, $late)", "w") +
                      "let $late := \"x\"; return 1"),
            "VQL_IO_EFFECT_ARGUMENT");
}

// ---- join -------------------------------------------------------------------

struct JoinWorld : ::testing::Test {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
  VQLEngine engine{core};
  /// The synchronous executor runs a join: it holds no action, so it needs
  /// no runner.
  auto exec(const std::string &source, std::size_t cycles = 100'000)
      -> std::expected<std::vector<zigzag::CellRef>, QueryExecutionError> {
    QueryExecutor executor(engine, vm, {.maxCycles = cycles});
    return executor.execute(source);
  }
  auto bind(const std::vector<std::string> &values) {
    std::vector<zigzag::CellRef> cells;
    for (const auto &value : values) cells.push_back(arena.makeCell(value));
    engine.setVariable("rows", cells);
  }
  auto one(const std::string &source, std::size_t cycles = 100'000)
      -> std::string {
    const auto result = exec(source, cycles);
    EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().message);
    if (!result || result->size() != 1U) return "<no single row>";
    return arena.textOf(result->front());
  }
};

TEST_F(JoinWorld, JoinsRowsWithTheSeparatorBetweenEachPair) {
  bind({"a", "b", "c"});
  EXPECT_EQ(one("join($rows, \", \")"), "a, b, c");
  EXPECT_EQ(one("join($rows, \"\")"), "abc");
  EXPECT_EQ(one("join($rows, \"\\n\")"), "a\nb\nc");
}

TEST_F(JoinWorld, OneRowHasNoSeparatorAndNoRowsIsTheEmptyText) {
  bind({"solo"});
  EXPECT_EQ(one("join($rows, \"--\")"), "solo");
  bind({});
  EXPECT_EQ(one("join($rows, \"--\")"), "")
      << "present and empty, like a zero count";
  EXPECT_EQ(one("join($absent, \"--\")"), "");
}

TEST_F(JoinWorld, NumbersAreJoinedAsTheirCanonicalText) {
  const auto cells = std::vector<zigzag::CellRef>{
      arena.makeScalarCell(std::int64_t{12}), arena.makeCell("x"),
      arena.makeScalarCell(std::int64_t{7})};
  engine.setVariable("rows", cells);
  EXPECT_EQ(one("join($rows, \"+\")"), "12+x+7");
}

TEST_F(JoinWorld, TextThatLooksLikeANumberStaysText) {
  bind({"1", "2", "3"});
  EXPECT_EQ(one("join($rows, \"\")"), "123")
      << "text, not the integer 6 a numeric add would give";
  EXPECT_EQ(one("join($rows, \",\")"), "1,2,3");
}

TEST_F(JoinWorld, TheSeparatorIsExactlyOneValue) {
  bind({"a", "b"});
  EXPECT_FALSE(exec("join($rows, $rows)").has_value());
  EXPECT_FALSE(exec("join($rows, $absent)").has_value());
}

TEST_F(JoinWorld, ArityAndNamedArgumentsAreCheckedBeforeAnyArgumentRuns) {
  bind({"a"});
  EXPECT_FALSE(exec("join($rows)").has_value());
  EXPECT_FALSE(exec("join($rows, \",\", \"x\")").has_value());
  EXPECT_FALSE(exec("join(rows: $rows, \",\")").has_value());
}

TEST_F(JoinWorld, ATenThousandRowJoinEqualsTheFoldOfPlus) {
  constexpr std::size_t rows = 10'000;
  std::vector<std::string> values;
  std::string expected;
  for (std::size_t i = 0; i < rows; ++i) {
    values.push_back("row" + std::to_string(i));
    expected += (i == 0 ? "" : ",") + values.back();
  }
  bind(values);
  // The loop costs a few cycles a row; the default budget is the host's to
  // raise for a script this size, which is the point of having one.
  constexpr std::size_t cycles = 1'000'000;
  EXPECT_EQ(one("join($rows, \",\")", cycles), expected);
}

// ---- join keeps what its rows quote ----------------------------------------

constexpr xanadu::PrimediaSpan quotedHome{
    .scroll = xanadu::localScroll, .start = 140, .length = 4};
constexpr xanadu::PrimediaSpan quotedAway{
    .scroll = xanadu::localScroll, .start = 900, .length = 6};

TEST_F(JoinWorld, ALoneQuotingRowIsReturnedItselfNotRetyped) {
  const auto row = arena.makeCell(quotedHome);
  engine.setVariable("rows", std::vector{row});
  const auto result = exec("join($rows, \",\")");
  ASSERT_TRUE(result.has_value()) << result.error().message;
  ASSERT_EQ(result->size(), 1U);
  EXPECT_EQ(result->front(), row) << "the quotation, not a copy of its text";
  const auto spans = arena.contentOf(row);
  ASSERT_EQ(spans.size(), 1U);
  EXPECT_EQ(spans[0].start, quotedHome.start);
}

TEST_F(JoinWorld, QuotingRowsAreJoinedAsTheRunOfTheirOwnSpans) {
  const auto home = arena.makeCell(quotedHome);
  const auto away = arena.makeCell(quotedAway);
  engine.setVariable("rows", std::vector{home, away});
  const auto result = exec("join($rows, \"; \")");
  ASSERT_TRUE(result.has_value()) << result.error().message;
  ASSERT_EQ(result->size(), 1U);
  EXPECT_NE(result->front(), home);
  EXPECT_NE(result->front(), away);
  const auto spans = std::vector<xanadu::PrimediaSpan>(
      arena.contentOf(result->front()).begin(),
      arena.contentOf(result->front()).end());
  ASSERT_EQ(spans.size(), 3U) << "row, separator, row";
  EXPECT_EQ(spans[0].start, quotedHome.start);
  EXPECT_EQ(spans[0].length, quotedHome.length);
  EXPECT_EQ(arena.scratchTextOf(spans[1]), "; ")
      << "only the separator is new text";
  EXPECT_EQ(spans[2].start, quotedAway.start);
  EXPECT_EQ(spans[2].length, quotedAway.length);
  EXPECT_EQ(arena.contentOf(home).size(), 1U) << "the sources are untouched";
}

TEST_F(JoinWorld, ANumberBesideAQuotingRowIsTheOnlyOtherNewText) {
  const auto home  = arena.makeCell(quotedHome);
  const auto count = arena.makeScalarCell(std::int64_t{42});
  engine.setVariable("rows", std::vector{home, count});
  const auto result = exec("join($rows, \"\")");
  ASSERT_TRUE(result.has_value()) << result.error().message;
  const auto spans = arena.contentOf(result->front());
  ASSERT_EQ(spans.size(), 2U);
  EXPECT_EQ(spans[0].start, quotedHome.start);
  EXPECT_EQ(arena.scratchTextOf(spans[1]), "42");
}

TEST_F(JoinWorld, ALoneNumberIsItsTextNotTheNumberCell) {
  const auto count = arena.makeScalarCell(std::int64_t{42});
  engine.setVariable("rows", std::vector{count});
  EXPECT_EQ(one("join($rows, \",\")"), "42");
}

TEST_F(JoinWorld, AJoinPastTheBoundIsRefusedByCodeNotTruncated) {
  constexpr std::size_t bound = 64;
  std::vector<std::string> values(20, "0123456789");
  bind(values);
  QueryExecutor executor(engine, vm, {.maxJoinedTextBytes = bound});
  const auto result = executor.execute("join($rows, \"\")");
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().message.find("VQL_IO_BOUND"), std::string::npos)
      << result.error().message;
  // Exactly at the bound is fine; one byte past is not.
  bind({std::string(bound, 'x')});
  EXPECT_TRUE(executor.execute("join($rows, \"\")").has_value());
  bind({std::string(bound, 'x'), "y"});
  EXPECT_FALSE(executor.execute("join($rows, \"\")").has_value());
}

TEST_F(JoinWorld, TheDefaultBoundIsTheDefaultWriteBound) {
  static_assert(xanadu::vql::defaultJoinedTextBytes ==
                xanadu::vortex::FileReadHostOptions::defaultWriteBytes);
  EXPECT_EQ(xanadu::vql::QueryExecutionOptions{}.maxJoinedTextBytes,
            xanadu::vortex::FileReadHostOptions::defaultWriteBytes);
}

TEST(VqlIoJoinTest, ARunnerWritesTheJoinedRowsOfAQuery) {
  FileWorld f({.maxCycles = 1'000'000});
  f.bindRows(1000);
  const auto path = (f.root / "joined.txt").string();
  const auto rows = f.rows("let $text := join($rows, \"\\n\"); " +
                           open(path, "for write create", "o") +
                           act("o", "write_bytes(0, $text)", "w") +
                           "return io_tag($w), io_bytes($w)");
  std::string expected;
  for (std::size_t i = 0; i < 1000; ++i)
    expected += (i == 0 ? "" : "\n") + ("r" + std::to_string(i));
  EXPECT_EQ(rows, (Texts{"Complete", std::to_string(expected.size())}));
  EXPECT_EQ(slurp(path), expected);
}

TEST(VqlIoJoinTest, AJoinPastTheWriteBoundIsATypedRefusalInAScript) {
  constexpr std::size_t bound = 128;
  FileWorld f({.maxJoinedTextBytes = bound}, {});
  f.bindRows(200);
  const auto path = (f.root / "toobig.txt").string();
  EXPECT_EQ(f.failure("let $text := join($rows, \",\"); " +
                      open(path, "for write create", "o") +
                      act("o", "write_bytes(0, $text)", "w") + "return 1"),
            "VQL_IO_BOUND");
  EXPECT_EQ(slurp(path), "") << "the file the script opened was not written";
}

TEST(VqlIoJoinTest, ManyJoinAndWriteRunsLeaveTheArenaAndTheServiceBounded) {
  FileWorld f;
  f.bindRows(4);
  const auto path = (f.root / "bounded.txt").string();
  static_cast<void>(
      f.rows(open(path, "for write create", "seed") + "return io_tag($seed)"));
  const auto once = [&] {
    // Nothing frees `$text` by hand: rebinding it is what releases the last
    // run's cell.
    auto result = f.run(
        "let $text := join($rows, \";\"); " + open(path, "for write", "o") +
        act("o", "write_bytes(0, $text)", "w") + "return io_tag($w)");
    ASSERT_TRUE(result.has_value());
    for (const auto cell : *result) {
      if (const auto incarnation = f.arena.incarnationOf(cell))
        static_cast<void>(f.arena.freeCell(cell, *incarnation));
    }
  };
  constexpr std::size_t warmRuns = 8;
  constexpr std::size_t runs     = 10'000;
  for (std::size_t i = 0; i < warmRuns; ++i) once();
  const auto warmSlots    = f.arena.cellCount();
  const auto warmLive     = f.arena.liveCells();
  const auto warmRequests = f.host->service().requestCount();
  for (std::size_t i = 0; i < runs; ++i) once();
  EXPECT_EQ(f.arena.cellCount(), warmSlots);
  EXPECT_EQ(f.arena.liveCells(), warmLive);
  EXPECT_EQ(f.host->service().requestCount(), warmRequests);
  EXPECT_EQ(slurp(path), "r0;r1;r2;r3");
}

// ---- The payload-less outcome constructor ----------------------------------

TEST(VqlIoSummaryTest, ASummaryKeepsTheTagTheErrorAndTheUncertainFlag) {
  ValueWorld s;
  const auto rows = s.drive(readAs("r") + "return $r",
                            {TimedOut{{}, terminalProgress, true}});
  ASSERT_EQ(rows.size(), 1U);
  const auto source = OutcomeCell{rows[0], *s.w.arena.incarnationOf(rows[0])};
  auto summary      = xanadu::vortex::OwnedOutcome::summary(
      s.w.arena, s.w.bridge.schema(), source);
  ASSERT_TRUE(summary);
  const auto tag = xanadu::vortex::ioOutcomeTag(s.w.arena, summary->cell);
  ASSERT_TRUE(tag);
  EXPECT_EQ(*tag, IoOutcomeTag::TimedOut);
  EXPECT_EQ(s.w.bridge.outcomeField(summary->cell, "uncertain"), "true");
  EXPECT_EQ(s.w.bridge.outcomeField(summary->cell, "owned"), "true");
  EXPECT_FALSE(s.w.bridge.outcomeField(summary->cell, "phase"))
      << "no Progress rank";
  EXPECT_FALSE(summary->payload.has_value());
  EXPECT_FALSE(summary->observation.has_value());
  summary->freeCells(s.w.arena, s.w.bridge.schema());
}

TEST(VqlIoSummaryTest, ASummaryFitsWhereTheFullCopyIsRefused) {
  ValueWorld s;
  const auto rows = s.drive(readAs("r") + "return $r",
                            {TimedOut{{}, terminalProgress, true}});
  ASSERT_EQ(rows.size(), 1U);
  const auto source  = OutcomeCell{rows[0], *s.w.arena.incarnationOf(rows[0])};
  const auto &schema = s.w.bridge.schema();
  const auto live    = s.w.arena.liveCells();
  auto small = xanadu::vortex::OwnedOutcome::summary(s.w.arena, schema, source);
  ASSERT_TRUE(small);
  const auto smallCells = s.w.arena.liveCells() - live;
  small->freeCells(s.w.arena, schema);
  auto full = xanadu::vortex::OwnedOutcome::copy(s.w.arena, schema,
                                                 s.w.payloads, source, {});
  ASSERT_TRUE(full);
  const auto fullCells = s.w.arena.liveCells() - live;
  full->freeCells(s.w.arena, schema);
  EXPECT_LT(smallCells, fullCells) << "the summary mints fewer cells";

  // Leave room for the summary and not for the copy: fill the free slots
  // until only the summary's worth remain, then forbid new ones.
  std::vector<zigzag::CellRef> filler;
  while (s.w.arena.cellCount() - s.w.arena.liveCells() > smallCells)
    filler.push_back(s.w.arena.makeCell("filler"));
  s.w.arena.setAllocationLimitForTesting(
      static_cast<std::uint32_t>(s.w.arena.cellCount()));
  bool copyRefused = false;
  try {
    auto refused = xanadu::vortex::OwnedOutcome::copy(s.w.arena, schema,
                                                      s.w.payloads, source, {});
    copyRefused  = !refused;
    if (refused) refused->freeCells(s.w.arena, schema);
  } catch (const std::exception &) {
    copyRefused = true;
  }
  EXPECT_TRUE(copyRefused) << "the full copy does not fit";
  auto fits = xanadu::vortex::OwnedOutcome::summary(s.w.arena, schema, source);
  EXPECT_TRUE(fits) << "the summary does";
  if (fits) fits->freeCells(s.w.arena, schema);
  s.w.arena.setAllocationLimitForTesting(
      std::numeric_limits<std::uint32_t>::max());
}

} // namespace
