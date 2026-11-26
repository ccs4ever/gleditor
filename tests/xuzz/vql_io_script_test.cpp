// M7-VQL: the VQL script I/O action (`effect { ... }`): grammar and its static
// refusals, the script runner's multiplicity and resume contract over the M1
// delayed fake backend, the shared frontend conformance harness, and real file
// reads through the M4/M5 file endpoint adapter.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

#include "common/xanadu/vortex/file_read_host.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/vql/script_runner.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "io_frontend_conformance.hpp"

namespace {

using namespace xuzz_conformance;
using xanadu::vql::hasIoEffect;
using xanadu::vql::IoEffectStatement;
using xanadu::vql::ParseError;
using xanadu::vql::Parser;
using xanadu::vql::QueryExecutionError;
using xanadu::vql::QueryExecutor;
using xanadu::vql::ScriptIoPorts;
using xanadu::vql::ScriptRunner;
using xanadu::vql::ScriptRunResult;
using xanadu::vql::ScriptWake;
using xanadu::vql::Statement;
using xanadu::vql::VQLEngine;

constexpr auto readOnCap =
    "effect { let $s := submit($cap, read_bytes(0, 16)); "
    "match $s { admitted($q) => await($q), refused($e) => $e } }";

constexpr auto contextCode = "VQL_IO_QUERY_CONTEXT";

auto parseFailure(const std::string &source) -> std::string {
  try {
    Parser(source).parseQuery();
  } catch (const ParseError &error) {
    return error.what();
  }
  return {};
}

// ---- Grammar --------------------------------------------------------------

TEST(VqlIoGrammarTest, AScriptActionIsATopLevelStatement) {
  const auto query = Parser(readOnCap).parseQuery();
  ASSERT_EQ(query.body.statements.size(), 1U);
  const auto *action =
      std::get_if<IoEffectStatement>(&query.body.statements[0]);
  ASSERT_NE(action, nullptr);
  EXPECT_EQ(action->submissionVar, "s");
  EXPECT_EQ(action->capabilityVar, "cap");
  EXPECT_EQ(std::get<std::int64_t>(action->offset), 0);
  EXPECT_EQ(std::get<std::int64_t>(action->maxBytes), 16);
  EXPECT_EQ(action->requestVar, "q");
  EXPECT_EQ(action->refusalVar, "e");
  EXPECT_TRUE(hasIoEffect(query));
}

TEST(VqlIoGrammarTest, OperandsMayBeVariablesAndArmsMayComeInEitherOrder) {
  const auto query =
      Parser("effect { let $s := submit($c, read_bytes($off, $max)); "
             "match $s { refused($r) => $r, admitted($a) => await($a) }; }")
          .parseQuery();
  const auto &action = std::get<IoEffectStatement>(query.body.statements[0]);
  EXPECT_EQ(std::get<std::string>(action.offset), "off");
  EXPECT_EQ(std::get<std::string>(action.maxBytes), "max");
}

TEST(VqlIoGrammarTest, ActionsAndQueryStatementsKeepTheirOrder) {
  const auto query =
      Parser(std::string{"return ##; "} + readOnCap + "; return ##")
          .parseQuery();
  ASSERT_EQ(query.body.statements.size(), 3U);
  EXPECT_FALSE(
      std::holds_alternative<IoEffectStatement>(query.body.statements[0]));
  EXPECT_TRUE(
      std::holds_alternative<IoEffectStatement>(query.body.statements[1]));
  EXPECT_FALSE(
      std::holds_alternative<IoEffectStatement>(query.body.statements[2]));
}

TEST(VqlIoGrammarTest, ARowOrBlockActionPositionIsRefusedWithTheCode) {
  for (const auto *source : {
           "for $x in ## effect { let $s := submit($c, read_bytes(0, 1)); "
           "match $s { admitted($q) => await($q), refused($e) => $e } }",
           "let $x := ## effect { let $s := submit($c, read_bytes(0, 1)); "
           "match $s { admitted($q) => await($q), refused($e) => $e } }",
           "for $x in ## where $x = 1 effect { let $s := submit($c, "
           "read_bytes(0, 1)); match $s { admitted($q) => await($q), "
           "refused($e) => $e } }",
           "if true effect { let $s := submit($c, read_bytes(0, 1)); "
           "match $s { admitted($q) => await($q), refused($e) => $e } }",
       }) {
    SCOPED_TRACE(source);
    EXPECT_NE(parseFailure(source).find(contextCode), std::string::npos);
  }
}

TEST(VqlIoGrammarTest, ANestedScriptBodyIsRefusedWithTheCode) {
  const std::string action =
      "effect { let $s := submit($c, read_bytes(0, 1)); match $s { "
      "admitted($q) => await($q), refused($e) => $e } }";
  for (const auto &source : {
           "def f() := { " + action + " }; f()",
           "watch ##/d.x as $v { " + action + " }",
           "with perspective(p) { " + action + " }",
           "let $f := \\($x) { " + action + " }; return ##",
       }) {
    SCOPED_TRACE(source);
    EXPECT_NE(parseFailure(source).find(contextCode), std::string::npos);
  }
}

TEST(VqlIoGrammarTest, SubmitAwaitAndReadBytesCannotBeCalledInAQuery) {
  for (const auto *source : {
           "return (submit($c, read_bytes(0, 1)))",
           "return (await($q))",
           "for $x in ## where await($x) return $x",
           "return (read_bytes(0, 1))",
           "return (string(submit($c, 1)))",
       }) {
    SCOPED_TRACE(source);
    EXPECT_NE(parseFailure(source).find(contextCode), std::string::npos);
  }
}

TEST(VqlIoGrammarTest, AMalformedActionIsAParseErrorWithoutTheContextCode) {
  for (const auto *source : {
           // No refused arm: a refusal must be matched, never dropped.
           "effect { let $s := submit($c, read_bytes(0, 1)); match $s { "
           "admitted($q) => await($q) } }",
           "effect { let $s := submit($c, read_bytes(0, 1)); match $s { "
           "refused($e) => $e } }",
           // await of a variable the arm did not bind.
           "effect { let $s := submit($c, read_bytes(0, 1)); match $s { "
           "admitted($q) => await($z), refused($e) => $e } }",
           // match of another variable.
           "effect { let $s := submit($c, read_bytes(0, 1)); match $t { "
           "admitted($q) => await($q), refused($e) => $e } }",
           // an offset that is an expression, not a literal or variable.
           "effect { let $s := submit($c, read_bytes(1 + 2, 1)); match $s { "
           "admitted($q) => await($q), refused($e) => $e } }",
           "effect { let $s := submit($c, read_bytes(0, 1)); match $s { "
           "admitted($q) => await($q), admitted($r) => await($r), "
           "refused($e) => $e } }",
       }) {
    SCOPED_TRACE(source);
    const auto message = parseFailure(source);
    EXPECT_FALSE(message.empty());
    EXPECT_EQ(message.find(contextCode), std::string::npos) << message;
  }
}

TEST(VqlIoGrammarTest, AReservedNameCannotBeDefinedEither) {
  for (const auto *name : {"submit", "await", "read_bytes"}) {
    SCOPED_TRACE(name);
    EXPECT_NE(parseFailure(std::string{"def "} + name + "() := 1; return ##")
                  .find(contextCode),
              std::string::npos);
  }
}

TEST(VqlIoGrammarTest, AQueryWithoutAnActionParsesAsBefore) {
  const auto query = Parser("def submit_all() := 1; return ##").parseQuery();
  EXPECT_FALSE(hasIoEffect(query));
}

// ---- Bytecode form, no blocking fallback ---------------------------------

TEST(VqlIoCompileTest, AnActionLowersToSubmitABranchAndAnAwait) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  xanadu::vql::VQLCompiler compiler(core, vm);
  const auto compiled =
      compiler.compile(std::string{"return ##; "} + std::string{readOnCap});
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  ASSERT_EQ(compiled.ioActions.size(), 1U);
  EXPECT_EQ(compiled.ioActions.front().statement, 1U);
  EXPECT_TRUE(compiled.finalIsIoEffect);
  EXPECT_NE(compiled.disassembly.find("SUBMIT"), std::string::npos);
  EXPECT_NE(compiled.disassembly.find("AWAIT"), std::string::npos);
  EXPECT_NE(compiled.disassembly.find("MAKETERM"), std::string::npos);
  using zigzag::vortex::OpcodeKind;
  EXPECT_EQ(vm.getOpcodeKind(compiled.ioActions.front().submitOpcode),
            OpcodeKind::Submit);
  EXPECT_EQ(vm.getOpcodeKind(compiled.ioActions.front().awaitOpcode),
            OpcodeKind::Await);
}

TEST(VqlIoCompileTest, EachActionIsItsOwnSite) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  xanadu::vql::VQLCompiler compiler(core, vm);
  const auto compiled =
      compiler.compile(std::string{readOnCap} + "; " + std::string{readOnCap});
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  ASSERT_EQ(compiled.ioActions.size(), 2U);
  EXPECT_EQ(compiled.ioActions[0].statement, 0U);
  EXPECT_EQ(compiled.ioActions[1].statement, 1U);
  EXPECT_NE(compiled.ioActions[0].awaitOpcode,
            compiled.ioActions[1].awaitOpcode);
}

TEST(VqlIoCompileTest, ASynchronousExecutorRefusesAScriptThatCanPark) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  VQLEngine engine(core);
  QueryExecutor executor(engine, vm);
  const auto executed = executor.execute(std::string{readOnCap});
  ASSERT_FALSE(executed);
  EXPECT_EQ(executed.error().category,
            QueryExecutionError::Category::Unsupported);
  EXPECT_NE(executed.error().message.find("VQL_IO_EFFECT_NEEDS_RUNNER"),
            std::string::npos);
  EXPECT_EQ(executed.error().message.find("NOT_COMPILABLE"), std::string::npos);
}

TEST(VqlIoCompileTest, AProgramRunWithoutAnIoHostFailsByValueAtItsSubmit) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  VQLEngine engine(core);
  QueryExecutor executor(engine, vm);
  const auto begun = executor.begin(Parser(readOnCap).parseQuery());
  ASSERT_FALSE(begun);
  EXPECT_EQ(begun.error().statement, std::optional<std::size_t>{0U});
}

// ---- The runner over the delayed fake backend -----------------------------

/// A VQL session on the harness World's arena, a runner whose reads are staged
/// as the World's plain specification cell, and a record of what was staged.
struct ScriptWorld {
  World w;
  VQLEngine engine{w.core};
  std::vector<std::pair<std::uint64_t, std::uint64_t>> staged;
  /// The harness stages a fresh spec cell per read and nothing here owns it
  /// (the file host does in production), so a long test frees the old ones.
  std::vector<CellRef> specs;
  ScriptRunner runner;

  ScriptWorld()
      : runner(engine, w.vm,
               ScriptIoPorts{w.bridge, w.host, w.payloads, w.diagnostics,
                             [this](CapabilityCell, const std::uint64_t offset,
                                    const std::uint64_t length)
                                 -> std::expected<CellRef, FrontendDiagnostic> {
                               staged.emplace_back(offset, length);
                               return specs.emplace_back(w.specification());
                             }}) {
    engine.setVariable("cap", w.capability.cell);
  }

  auto run(const std::string &source) -> ScriptRunResult {
    return runner.run(Parser(source).parseQuery());
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
  /// Pump the host and resume whatever it woke.
  auto wakeAndResume() -> ScriptRunResult {
    static_cast<void>(w.pump());
    auto woken = runner.takeWake();
    EXPECT_TRUE(woken.has_value());
    if (!woken) {
      return ScriptRunResult::failed({"TEST_NO_WAKE", "no wake", {}});
    }
    return runner.resume(*std::move(woken));
  }
  auto rowTexts(const std::vector<CellRef> &rows) -> std::vector<std::string> {
    std::vector<std::string> texts;
    for (const auto row : rows) texts.push_back(w.arena.textOf(row));
    return texts;
  }
};

auto completedRows(const ScriptRunResult &result)
    -> const std::vector<CellRef> * {
  const auto *done = std::get_if<ScriptRunResult::Completed>(&result.value);
  return done ? &done->value : nullptr;
}
auto parkedToken(const ScriptRunResult &result) -> const FrontendToken * {
  const auto *parked = std::get_if<ScriptRunResult::Parked>(&result.value);
  return parked ? &parked->token : nullptr;
}
auto failedDiagnostic(const ScriptRunResult &result)
    -> const FrontendDiagnostic * {
  const auto *failed = std::get_if<ScriptRunResult::Failed>(&result.value);
  return failed ? &failed->diagnostic : nullptr;
}

TEST(VqlIoScriptTest, AScriptWithNoActionRunsToCompletionAtOnce) {
  ScriptWorld s;
  const auto result = s.run("return ##");
  EXPECT_NE(completedRows(result), nullptr);
  EXPECT_EQ(s.w.submissions(), 0U);
}

TEST(VqlIoScriptTest, AnActionOverManyRowsSubmitsOnceNotPerRow) {
  ScriptWorld s;
  std::vector<CellRef> rows;
  for (int i = 0; i < 5; ++i) rows.push_back(s.w.arena.makeCell("row"));
  s.engine.setVariable("rows", rows);

  // The query stretch yields five rows; the action is outside it and so is
  // reached exactly once, whatever the stretch produced.
  const auto begun =
      s.run(std::string{"for $r in $rows return $r; "} + readOnCap);
  ASSERT_NE(parkedToken(begun), nullptr) << "a pending read parks";
  EXPECT_EQ(s.w.submissions(), 1U);
  EXPECT_EQ(parkedToken(begun)->position, 1U) << "the action's statement index";

  s.w.complete(0, EndOfStream{{}});
  const auto resumed = s.wakeAndResume();
  const auto *done   = completedRows(resumed);
  ASSERT_NE(done, nullptr);
  EXPECT_EQ(s.w.submissions(), 1U) << "resume never submits again";
  EXPECT_EQ(done->size(), 1U) << "the final statement, the action, decides";
}

TEST(VqlIoScriptTest, TheFinalStatementDecidesTheRowsAndLaterQueriesStillRun) {
  ScriptWorld s;
  std::vector<CellRef> rows;
  for (int i = 0; i < 3; ++i) rows.push_back(s.w.arena.makeCell("row"));
  s.engine.setVariable("rows", rows);
  const auto begun =
      s.run(std::string{readOnCap} + "; for $r in $rows return $r");
  ASSERT_NE(parkedToken(begun), nullptr);
  s.w.complete(0, EndOfStream{{}});
  const auto resumed = s.wakeAndResume();
  const auto *done   = completedRows(resumed);
  ASSERT_NE(done, nullptr);
  EXPECT_EQ(done->size(), 3U) << "the query after the action ran after it";
  EXPECT_EQ(s.w.submissions(), 1U);
}

TEST(VqlIoScriptTest, EachReachedActionSubmitsOnceInScriptOrder) {
  ScriptWorld s;
  const std::string three =
      std::string{readOnCap} + "; " + readOnCap + "; " + readOnCap;
  auto result = s.run(three);
  for (std::size_t reached = 1; reached <= 3; ++reached) {
    SCOPED_TRACE(reached);
    const auto *token = parkedToken(result);
    ASSERT_NE(token, nullptr);
    EXPECT_EQ(token->position, reached - 1) << "parked at the reached action";
    EXPECT_EQ(s.w.submissions(), reached);
    s.w.complete(reached - 1, EndOfStream{{}});
    result = s.wakeAndResume();
    EXPECT_EQ(s.w.submissions(), std::min<std::size_t>(reached + 1, 3U))
        << "only the next reached action submits";
    if (reached < 3) ASSERT_NE(parkedToken(result), nullptr);
  }
  EXPECT_NE(completedRows(result), nullptr);
}

TEST(VqlIoScriptTest, RunningTheSameScriptNTimesSubmitsNTimes) {
  ScriptWorld s;
  constexpr std::size_t times = 4;
  for (std::size_t i = 0; i < times; ++i) {
    const auto begun = s.run(readOnCap);
    ASSERT_NE(parkedToken(begun), nullptr);
    s.w.complete(i, EndOfStream{{}});
    ASSERT_NE(completedRows(s.wakeAndResume()), nullptr);
  }
  EXPECT_EQ(s.w.submissions(), times);
}

TEST(VqlIoScriptTest, PendingResumesTheSameRequestAndAnOutcomeIsTheServices) {
  ScriptWorld s;
  const auto begun  = s.run(readOnCap);
  const auto *first = parkedToken(begun);
  ASSERT_NE(first, nullptr);
  const auto request      = first->request;
  const auto continuation = first->continuation;
  EXPECT_EQ(s.runner.parkedCount(), 1U);

  static_cast<void>(s.w.pump());
  EXPECT_FALSE(s.runner.takeWake()) << "not woken while the request is open";

  s.w.complete(0, EndOfStream{{}});
  static_cast<void>(s.w.pump());
  auto woken = s.runner.takeWake();
  ASSERT_TRUE(woken);
  EXPECT_EQ(woken->why, FrontendWake::Resumable);
  EXPECT_EQ(woken->token.request, request);
  EXPECT_EQ(woken->token.continuation, continuation);
  EXPECT_EQ(s.runner.parkedCount(), 0U);

  const auto resumed = s.runner.resume(*std::move(woken));
  const auto *done   = completedRows(resumed);
  ASSERT_NE(done, nullptr);
  const auto polled = s.w.service.poll(request).outcome;
  ASSERT_TRUE(polled);
  EXPECT_EQ(done->front(), polled->cell);
  EXPECT_EQ(s.w.submissions(), 1U);
}

TEST(VqlIoScriptTest, ARefusalIsAScriptValueAndTheScriptGoesOn) {
  ScriptWorld s;
  s.w.refuseAdmission = true;
  std::vector<CellRef> rows{s.w.arena.makeCell("a"), s.w.arena.makeCell("b")};
  s.engine.setVariable("rows", rows);
  const auto result =
      s.run(std::string{readOnCap} + "; for $r in $rows return $r");
  EXPECT_EQ(failedDiagnostic(result), nullptr) << "a refusal is not a failure";
  const auto *done = completedRows(result);
  ASSERT_NE(done, nullptr);
  EXPECT_EQ(done->size(), 2U) << "the statement after the refusal ran";
  EXPECT_EQ(s.w.submissions(), 0U) << "no external work for a refusal";

  const auto refusal   = s.run(readOnCap);
  const auto *rejected = completedRows(refusal);
  ASSERT_NE(rejected, nullptr);
  ASSERT_EQ(rejected->size(), 1U) << "the outcome only: nothing to read";
  const auto incarnation = s.w.arena.incarnationOf(rejected->front());
  ASSERT_TRUE(incarnation);
  EXPECT_EQ(
      ioOutcomeTag(s.w.arena, OutcomeCell{rejected->front(), *incarnation}),
      IoOutcomeTag::Rejected);
}

TEST(VqlIoScriptTest, EveryTerminalStatusIsAScriptValueNeverAQueryError) {
  struct Case {
    const char *name;
    std::function<IoOutcome(World &)> outcome;
    IoOutcomeTag tag;
    bool payload;
  };
  const IoProgress progress{.transferredBytes = 2, .phase = IoPhase::Terminal};
  const std::vector<Case> cases{
      {"Complete",
       [&](World &) -> IoOutcome { return Complete{{}, {}, progress}; },
       IoOutcomeTag::Complete, false},
      {"EndOfStream",
       [&](World &) -> IoOutcome { return EndOfStream{progress}; },
       IoOutcomeTag::EndOfStream, false},
      {"Partial",
       [&](World &w) -> IoOutcome {
         return Partial{w.payloadRange(), PartialCause::BackendFailure,
                        progress};
       },
       IoOutcomeTag::Partial, true},
      {"Failed",
       [&](World &) -> IoOutcome {
         return Failed{{IoErrorCode::BackendFailure, 3}, {}, progress};
       },
       IoOutcomeTag::Failed, false},
      {"FailedWithPayload",
       [&](World &w) -> IoOutcome {
         return Failed{
             {IoErrorCode::BackendFailure, 3}, w.payloadRange(), progress};
       },
       IoOutcomeTag::Failed, true},
      {"Cancelled",
       [&](World &) -> IoOutcome { return Cancelled{{}, progress, false}; },
       IoOutcomeTag::Cancelled, false},
      {"TimedOut",
       [&](World &) -> IoOutcome { return TimedOut{{}, progress, true}; },
       IoOutcomeTag::TimedOut, false},
      {"Unknown", [&](World &) -> IoOutcome { return Unknown{progress}; },
       IoOutcomeTag::Unknown, false},
  };
  for (const auto &test : cases) {
    SCOPED_TRACE(test.name);
    ScriptWorld s;
    const auto begun = s.run(readOnCap);
    ASSERT_NE(parkedToken(begun), nullptr);
    s.w.complete(0, test.outcome(s.w));
    const auto resumed = s.wakeAndResume();
    ASSERT_EQ(failedDiagnostic(resumed), nullptr);
    const auto *rows = completedRows(resumed);
    ASSERT_NE(rows, nullptr);
    ASSERT_FALSE(rows->empty()) << "an outcome is never an empty result";
    const auto incarnation = s.w.arena.incarnationOf(rows->front());
    ASSERT_TRUE(incarnation);
    EXPECT_EQ(ioOutcomeTag(s.w.arena, OutcomeCell{rows->front(), *incarnation}),
              test.tag);
    EXPECT_EQ(rows->size(), test.payload ? 2U : 1U)
        << "payload text is a row only where the status gate allows it";
    if (test.payload) EXPECT_EQ(s.w.arena.textOf((*rows)[1]), "ok");
  }
}

TEST(VqlIoScriptTest, AnUnusableCapabilityOrBoundFailsBeforeAnythingIsStaged) {
  ScriptWorld s;
  const auto plain = s.w.arena.makeCell("not a capability");
  s.engine.setVariable("plain", plain);
  s.engine.setVariable("negative", s.w.arena.makeScalarCell(std::int64_t{-1}));
  s.engine.setVariable("fraction", s.w.arena.makeScalarCell(2.5));
  s.engine.setVariable("word", s.w.arena.makeCell("many"));
  const auto action = [](const char *cap, const char *off, const char *max) {
    return std::string{"effect { let $s := submit("} + cap + ", read_bytes(" +
           off + ", " + max +
           ")); match $s { admitted($q) => await($q), refused($e) => $e } }";
  };
  for (const auto &source : {
           action("$unbound", "0", "1"),
           action("$plain", "0", "1"),
           action("$cap", "$negative", "1"),
           action("$cap", "0", "$fraction"),
           action("$cap", "$word", "1"),
           action("$cap", "$missing", "1"),
       }) {
    SCOPED_TRACE(source);
    const auto result  = s.run(source);
    const auto *failed = failedDiagnostic(result);
    ASSERT_NE(failed, nullptr);
    EXPECT_EQ(failed->code, "VQL_IO_EFFECT_ARGUMENT");
  }
  EXPECT_TRUE(s.staged.empty());
  EXPECT_EQ(s.w.submissions(), 0U);
}

TEST(VqlIoScriptTest, VariableOperandsReachTheStagedRead) {
  ScriptWorld s;
  s.engine.setVariable("off", s.w.arena.makeScalarCell(std::int64_t{6}));
  s.engine.setVariable("max", s.w.arena.makeScalarCell(std::int64_t{5}));
  const auto begun =
      s.run("effect { let $s := submit($cap, read_bytes($off, $max)); "
            "match $s { admitted($q) => await($q), refused($e) => $e } }");
  ASSERT_NE(parkedToken(begun), nullptr);
  ASSERT_EQ(s.staged.size(), 1U);
  EXPECT_EQ(s.staged[0], (std::pair<std::uint64_t, std::uint64_t>{6, 5}));
}

TEST(VqlIoScriptTest, ASegmentErrorIsATypedScriptFailureWithItsPosition) {
  ScriptWorld s;
  // A call to a function nobody defined is a typed Unknown error from the
  // query stretch; it stops the script before the action is reached.
  const auto result  = s.run(std::string{"undefined_fn(1); "} + readOnCap);
  const auto *failed = failedDiagnostic(result);
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->code, "VQL_SCRIPT_FAILED");
  EXPECT_EQ(failed->position, 0U);
  EXPECT_EQ(s.w.submissions(), 0U) << "the failed query stopped the script";
}

TEST(VqlIoScriptTest, DestroyingARunnerCancelsItsWaiterNotItsRequest) {
  World w;
  FrontendToken parked;
  RequestCell request;
  {
    VQLEngine engine(w.core);
    engine.setVariable("cap", w.capability.cell);
    ScriptRunner runner(
        engine, w.vm,
        ScriptIoPorts{w.bridge, w.host, w.payloads, w.diagnostics,
                      [&](CapabilityCell, std::uint64_t, std::uint64_t)
                          -> std::expected<CellRef, FrontendDiagnostic> {
                        return w.specification();
                      }});
    const auto begun  = runner.run(Parser(readOnCap).parseQuery());
    const auto *token = parkedToken(begun);
    ASSERT_NE(token, nullptr);
    parked  = *token;
    request = token->request;
  }
  w.complete(0, EndOfStream{{}});
  static_cast<void>(w.pump());
  EXPECT_TRUE(w.service.poll(request).outcome)
      << "the request completes on its own; cancelling the waiter is not it";
}

TEST(VqlIoScriptTest, ARunnerRetiresEachActionsRequestOnceTheNextReplacesIt) {
  // M1 of the M7a review: a long session holds one finished action's request,
  // not every request it ever made.
  ScriptWorld s;
  std::optional<RequestCell> previous;
  std::optional<OutcomeCell> previousOutcome;
  constexpr std::size_t times = 5;
  for (std::size_t i = 0; i < times; ++i) {
    const auto begun  = s.run(readOnCap);
    const auto *token = parkedToken(begun);
    ASSERT_NE(token, nullptr);
    if (previous) {
      EXPECT_FALSE(s.w.arena.matches(previous->cell, previous->incarnation))
          << "the replaced action's request is retired and freed";
      EXPECT_FALSE(s.w.arena.matches(previousOutcome->cell,
                                     previousOutcome->incarnation));
    }
    const auto request = token->request;
    s.w.complete(i, EndOfStream{{}});
    const auto done = s.wakeAndResume();
    ASSERT_NE(completedRows(done), nullptr);
    previousOutcome = *s.w.service.poll(request).outcome;
    EXPECT_TRUE(
        s.w.arena.matches(previousOutcome->cell, previousOutcome->incarnation))
        << "the rows just returned stay readable until the next action";
    previous = request;
  }
}

TEST(VqlIoScriptTest, SeveralParkedScriptsKeepTheirOwnRequestsInEitherOrder) {
  // M7-VQL gate r1, vortex M2: one runner, N scripts parked together. Each
  // keeps its own submission; resuming in reverse order binds every token to
  // its own request and nothing is retired while it is still awaited.
  for (const bool reverse : {false, true}) {
    SCOPED_TRACE(reverse);
    ScriptWorld s;
    constexpr std::size_t scripts = 5;
    std::vector<RequestCell> requests;
    for (std::size_t i = 0; i < scripts; ++i) {
      const auto begun  = s.run(readOnCap);
      const auto *token = parkedToken(begun);
      ASSERT_NE(token, nullptr);
      requests.push_back(token->request);
    }
    EXPECT_EQ(s.runner.parkedCount(), scripts);
    for (const auto request : requests) {
      EXPECT_TRUE(s.w.arena.matches(request.cell, request.incarnation))
          << "a later script never retires an earlier one's request";
    }
    // The endpoint runs one request at a time: each completion starts the
    // next queued one.
    std::vector<ScriptWake> wakes;
    for (std::size_t i = 0; i < scripts; ++i) {
      s.w.complete(i, EndOfStream{{}});
      static_cast<void>(s.w.pump());
      while (auto woken = s.runner.takeWake())
        wakes.push_back(*std::move(woken));
    }
    ASSERT_EQ(wakes.size(), scripts);
    if (reverse) std::ranges::reverse(wakes);
    for (auto &wake : wakes) {
      const auto request = wake.token.request;
      const auto resumed = s.runner.resume(std::move(wake));
      const auto *done   = completedRows(resumed);
      ASSERT_NE(done, nullptr);
      const auto polled = s.w.service.poll(request).outcome;
      ASSERT_TRUE(polled);
      EXPECT_EQ(done->front(), polled->cell)
          << "the script reads the outcome of its own request";
    }
    EXPECT_EQ(s.w.submissions(), scripts);
    // The next admission retires every finished script's request.
    const auto next = s.run(readOnCap);
    ASSERT_NE(parkedToken(next), nullptr);
    for (const auto request : requests) {
      EXPECT_FALSE(s.w.arena.matches(request.cell, request.incarnation));
    }
  }
}

TEST(VqlIoScriptTest, ASecondRunnerOnTheSameVmIsRefused) {
  ScriptWorld s;
  VQLEngine other(s.w.core);
  EXPECT_THROW(
      ScriptRunner(
          other, s.w.vm,
          ScriptIoPorts{
              s.w.bridge, s.w.host, s.w.payloads, s.w.diagnostics,
              [](CapabilityCell, std::uint64_t,
                 std::uint64_t) -> std::expected<CellRef, FrontendDiagnostic> {
                return std::unexpected{FrontendDiagnostic{"TEST", "none", {}}};
              }}),
      std::logic_error);
  EXPECT_TRUE(s.w.vm.hasSubmitHandler()) << "the first runner keeps its hook";
}

TEST(VqlIoScriptTest, AnAbandonedActivationRetiresItsRequest) {
  ScriptWorld s;
  const auto begun  = s.run(readOnCap);
  const auto *token = parkedToken(begun);
  ASSERT_NE(token, nullptr);
  const auto request = token->request;
  s.w.complete(0, EndOfStream{{}});
  static_cast<void>(s.w.pump());
  auto woken = s.runner.takeWake();
  ASSERT_TRUE(woken);
  woken->why         = FrontendWake::Abandoned;
  const auto resumed = s.runner.resume(*std::move(woken));
  EXPECT_NE(failedDiagnostic(resumed), nullptr);
  EXPECT_FALSE(s.w.arena.matches(request.cell, request.incarnation))
      << "the abandoned script's terminal request was retired";
}

TEST(VqlIoScriptTest, ManyEffectScriptRunsLeaveTheArenaBounded) {
  // M7-VQL gate r1, vortex M3: a parked script cuts its pre-compile mark, so
  // its compiled program (37-40 cells per run) used to stay for the life of
  // the arena. The run now detaches and frees its own cells with the P7
  // deletion when it ends, so a long session's arena is bounded by what is
  // live, not by how many scripts ran.
  ScriptWorld s;
  const auto once = [&] {
    const auto begun = s.run(readOnCap);
    ASSERT_NE(parkedToken(begun), nullptr);
    s.w.complete(s.w.submissions() - 1, EndOfStream{{}});
    ASSERT_NE(completedRows(s.wakeAndResume()), nullptr);
    s.freeOldSpecs();
  };
  for (std::size_t i = 0; i < 8; ++i) once();
  const auto warmSlots       = s.w.arena.cellCount();
  const auto warmLive        = s.w.arena.liveCells();
  constexpr std::size_t runs = 10'000;
  for (std::size_t i = 0; i < runs; ++i) once();
  EXPECT_EQ(s.w.arena.cellCount(), warmSlots)
      << "the slot high-water mark did not move";
  EXPECT_EQ(s.w.arena.liveCells(), warmLive);
}

TEST(VqlIoScriptTest, AnAbandonedScriptFreesItsProgramToo) {
  ScriptWorld s;
  const auto abandonOnce = [&] {
    const auto begun = s.run(readOnCap);
    ASSERT_NE(parkedToken(begun), nullptr);
    s.w.complete(s.w.submissions() - 1, EndOfStream{{}});
    static_cast<void>(s.w.pump());
    auto woken = s.runner.takeWake();
    ASSERT_TRUE(woken);
    woken->why = FrontendWake::Abandoned;
    EXPECT_NE(failedDiagnostic(s.runner.resume(*std::move(woken))), nullptr);
    s.freeOldSpecs();
  };
  for (std::size_t i = 0; i < 8; ++i) abandonOnce();
  const auto warmSlots = s.w.arena.cellCount();
  for (std::size_t i = 0; i < 500; ++i) abandonOnce();
  EXPECT_EQ(s.w.arena.cellCount(), warmSlots);
}

TEST(VqlIoScriptTest, AFinishedScriptsRowsAndOutcomeSurviveItsProgram) {
  ScriptWorld s;
  const auto begun  = s.run(readOnCap);
  const auto *token = parkedToken(begun);
  ASSERT_NE(token, nullptr);
  const auto request = token->request;
  s.w.complete(s.w.submissions() - 1, EndOfStream{{}});
  const auto resumed = s.wakeAndResume();
  const auto *done   = completedRows(resumed);
  ASSERT_NE(done, nullptr);
  ASSERT_FALSE(done->empty());
  for (const auto row : *done) EXPECT_TRUE(s.w.arena.contains(row));
  const auto polled = s.w.service.poll(request).outcome;
  ASSERT_TRUE(polled);
  EXPECT_EQ(done->front(), polled->cell);
}

TEST(VqlIoScriptTest, DestroyingARunnerRetiresItsTerminalRequestOnly) {
  World w;
  RequestCell finished;
  RequestCell running;
  {
    VQLEngine engine(w.core);
    engine.setVariable("cap", w.capability.cell);
    ScriptRunner runner(
        engine, w.vm,
        ScriptIoPorts{w.bridge, w.host, w.payloads, w.diagnostics,
                      [&](CapabilityCell, std::uint64_t, std::uint64_t)
                          -> std::expected<CellRef, FrontendDiagnostic> {
                        return w.specification();
                      }});
    const auto first = runner.run(Parser(readOnCap).parseQuery());
    ASSERT_NE(parkedToken(first), nullptr);
    finished = parkedToken(first)->request;
    w.complete(0, EndOfStream{{}});
    static_cast<void>(w.pump());
    auto woken = runner.takeWake();
    ASSERT_TRUE(woken);
    ASSERT_NE(completedRows(runner.resume(*std::move(woken))), nullptr);
    const auto second = runner.run(Parser(readOnCap).parseQuery());
    ASSERT_NE(parkedToken(second), nullptr);
    running = parkedToken(second)->request;
  }
  EXPECT_FALSE(w.arena.matches(finished.cell, finished.incarnation));
  EXPECT_TRUE(w.arena.matches(running.cell, running.incarnation))
      << "an unfinished request is never cancelled by the runner";
}

// ---- The shared conformance harness ---------------------------------------

/// The VQL driver: its program is the script action itself, run through the
/// real parser, runner and bridge on the harness World.
class VqlScriptDriver final : public ConformanceDriver {
public:
  explicit VqlScriptDriver(World &w) : engine_(w.core) {}

  auto begin(World &w, const CapabilityCell capability,
             const CellRef specification) -> RunResult override {
    engine_.setVariable("cap", capability.cell);
    runner_ = std::make_unique<ScriptRunner>(
        engine_, w.vm,
        ScriptIoPorts{
            w.bridge, w.host, w.payloads, w.diagnostics,
            [specification](CapabilityCell, std::uint64_t, std::uint64_t)
                -> std::expected<CellRef, FrontendDiagnostic> {
              return specification;
            }});
    return translate(w, runner_->run(Parser(readOnCap).parseQuery()));
  }
  auto resume(World &w, FrontendToken token) -> RunResult override {
    return translate(w, runner_->resume(ScriptWake{std::move(token),
                                                   FrontendWake::Resumable}));
  }
  void collectWakes() override {
    while (auto wake = runner_->takeWake()) {
      woken.emplace_back(std::move(wake->token), wake->why);
    }
  }
  [[nodiscard]] auto visibleCells() const -> std::vector<CellRef> override {
    std::vector<CellRef> cells;
    if (const auto submission = runner_->lastSubmission()) {
      cells.push_back(submission->cell);
    }
    cells.insert(cells.end(), outcomes_.begin(), outcomes_.end());
    return cells;
  }
  [[nodiscard]] auto submission() const
      -> std::optional<SubmissionCell> override {
    return runner_->lastSubmission();
  }

private:
  auto translate(World &w, ScriptRunResult result) -> RunResult {
    if (auto *done = std::get_if<ScriptRunResult::Completed>(&result.value)) {
      const auto outcome     = done->value.front();
      const auto incarnation = w.arena.incarnationOf(outcome);
      outcomes_.push_back(outcome);
      return RunResult::completed(OutcomeCell{outcome, *incarnation});
    }
    if (auto *parked = std::get_if<ScriptRunResult::Parked>(&result.value)) {
      return RunResult::parked(std::move(parked->token));
    }
    return RunResult::failed(
        std::get<ScriptRunResult::Failed>(result.value).diagnostic);
  }

  VQLEngine engine_;
  std::unique_ptr<ScriptRunner> runner_;
  std::vector<CellRef> outcomes_;
};

const DriverFactory vqlDriver = [](World &w) {
  return std::make_unique<VqlScriptDriver>(w);
};

XUZZ_FRONTEND_CONFORMANCE(VqlScriptConformance, vqlDriver)

// ---- Real file reads through the M4/M5 file endpoint adapter --------------

/// The arena, core, VM and VQL session of a program that reads real files.
struct FileScriptWorld {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
  VQLEngine engine{core};
  xanadu::vortex::FileReadHost host{vm, arena};
  // One runner for the session, as vquery has: it retires an action's request
  // when the next action or the runner replaces it, so the rows a test reads
  // after `run` returns are still live.
  ScriptRunner runner{engine, vm, xanadu::vql::fileScriptPorts(host)};
  std::filesystem::path dir;

  FileScriptWorld() {
    dir = std::filesystem::temp_directory_path() /
          ("vql_io_script_" + std::to_string(::getpid()) + "_" +
           std::to_string(counter_++));
    std::filesystem::create_directories(dir);
  }
  ~FileScriptWorld() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }

  auto write(const std::string &name, const std::string &content)
      -> std::filesystem::path {
    const auto path = dir / name;
    std::ofstream(path, std::ios::binary | std::ios::trunc) << content;
    return path;
  }
  /// Open @p path through the host and bind $name; the open is polled through
  /// the pump, never waited on synchronously.
  auto open(const std::string &name, const std::filesystem::path &path)
      -> std::expected<void, FrontendDiagnostic> {
    const auto request = host.beginOpen(path.string());
    if (!request) return std::unexpected{request.error()};
    for (int spin = 0; spin < 2000; ++spin) {
      if (const auto opened = host.openedCapability(*request)) {
        if (!*opened) return std::unexpected{opened->error()};
        engine.setVariable(name, (*opened)->cell);
        return {};
      }
      host.pump(16);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::unexpected{FrontendDiagnostic{"TEST_OPEN_TIMEOUT", "open", {}}};
  }
  auto run(const std::string &source)
      -> std::expected<std::vector<CellRef>, FrontendDiagnostic> {
    return xanadu::vql::driveToCompletion(
        runner, runner.run(Parser(source).parseQuery()), [&] { host.pump(16); },
        [] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); });
  }
  auto text(const CellRef cell) -> std::string { return arena.textOf(cell); }
  auto tag(const CellRef cell) -> IoOutcomeTag {
    return ioOutcomeTag(arena, OutcomeCell{cell, *arena.incarnationOf(cell)})
        .value();
  }

  static inline std::uint32_t counter_ = 0;
};

TEST(VqlIoFileTest, ARealFileReadCompletesWithItsPayloadAsAScriptValue) {
  FileScriptWorld f;
  ASSERT_TRUE(f.open("src", f.write("a.txt", "hello world")));
  const auto rows =
      f.run("effect { let $s := submit($src, read_bytes(6, 5)); "
            "match $s { admitted($q) => await($q), refused($e) => $e } }");
  ASSERT_TRUE(rows) << rows.error().message;
  ASSERT_EQ(rows->size(), 2U);
  EXPECT_EQ(f.tag((*rows)[0]), IoOutcomeTag::Complete);
  EXPECT_EQ(f.text((*rows)[1]), "world");
}

TEST(VqlIoFileTest, ReadingPastTheEndIsEndOfStreamWithNoPayloadRow) {
  FileScriptWorld f;
  ASSERT_TRUE(f.open("src", f.write("a.txt", "abc")));
  const auto rows =
      f.run("effect { let $s := submit($src, read_bytes(100, 4)); "
            "match $s { admitted($q) => await($q), refused($e) => $e } }");
  ASSERT_TRUE(rows) << rows.error().message;
  ASSERT_EQ(rows->size(), 1U);
  EXPECT_EQ(f.tag((*rows)[0]), IoOutcomeTag::EndOfStream);
}

TEST(VqlIoFileTest, SessionVariablesCarryTheOffsetAndTheBound) {
  FileScriptWorld f;
  ASSERT_TRUE(f.open("src", f.write("a.txt", "0123456789")));
  f.engine.setVariable("off", f.arena.makeScalarCell(std::int64_t{2}));
  f.engine.setVariable("max", f.arena.makeScalarCell(std::int64_t{3}));
  const auto rows =
      f.run("effect { let $s := submit($src, read_bytes($off, $max)); "
            "match $s { admitted($q) => await($q), refused($e) => $e } }");
  ASSERT_TRUE(rows) << rows.error().message;
  ASSERT_EQ(rows->size(), 2U);
  EXPECT_EQ(f.text((*rows)[1]), "234");
}

TEST(VqlIoFileTest, AMissingFileFailsTheOpenAsATypedDiagnostic) {
  FileScriptWorld f;
  const auto opened = f.open("src", f.dir / "missing.txt");
  ASSERT_FALSE(opened);
  EXPECT_FALSE(opened.error().message.empty());
}

TEST(VqlIoFileTest, ABoundBeyondWhatOneReadMayRetainIsRefusedBeforeSubmit) {
  FileScriptWorld f;
  ASSERT_TRUE(f.open("src", f.write("a.txt", "abc")));
  const auto rows =
      f.run("effect { let $s := submit($src, read_bytes(0, 999999999)); "
            "match $s { admitted($q) => await($q), refused($e) => $e } }");
  ASSERT_FALSE(rows);
  EXPECT_EQ(rows.error().code, "VQL_IO_EFFECT_ARGUMENT");
}

TEST(VqlIoFileTest, TwoActionsReadTwoFilesInOneScript) {
  FileScriptWorld f;
  ASSERT_TRUE(f.open("a", f.write("a.txt", "alpha")));
  ASSERT_TRUE(f.open("b", f.write("b.txt", "beta")));
  const auto rows =
      f.run("effect { let $s := submit($a, read_bytes(0, 5)); match $s { "
            "admitted($q) => await($q), refused($e) => $e } }; "
            "effect { let $t := submit($b, read_bytes(0, 4)); match $t { "
            "admitted($r) => await($r), refused($f) => $f } }");
  ASSERT_TRUE(rows) << rows.error().message;
  ASSERT_EQ(rows->size(), 2U);
  EXPECT_EQ(f.text((*rows)[1]), "beta") << "the final action decides the rows";
}

} // namespace
