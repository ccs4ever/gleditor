// M7-VQL: the VQL script I/O action (`effect { ... }`): grammar and its static
// refusals, the script runner's multiplicity and resume contract over the M1
// delayed fake backend, the shared frontend conformance harness, and real file
// reads through the M4/M5 file endpoint adapter.

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
  EXPECT_EQ(vm.getOpcodeKind(*compiled.ioActions.front().awaitOpcode),
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
  EXPECT_EQ(w.scheduler->cancels, 1U)
      << "nobody can read the outcome, so the backend is asked to stop";
  EXPECT_EQ(w.service.requestCount(), 1U)
      << "it stays orphaned until the backend ends it";
  w.complete(0, EndOfStream{{}});
  static_cast<void>(w.pump());
  EXPECT_EQ(w.service.requestCount(), 0U)
      << "its terminal is dropped and its slot freed, not held for good";
  EXPECT_FALSE(w.service.poll(request).outcome)
      << "nothing names an orphan's outcome any more";
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

TEST(VqlIoScriptTest, ARollbackNamesTheActivationThatOwnsTheSubmit) {
  // M7-VQL gate r2, vortex minor 1: one choice point under two parked scripts
  // is undone while neither is stepping, so the notice finds no current
  // activation and the entries belong to others. Both requests are orphaned.
  ScriptWorld s;
  const auto outer = s.w.vm.spawnCursor(
      s.w.vm.mintOpcode(zigzag::vortex::OpcodeKind::Nop), "outer choice");
  s.w.vm.pushChoicePoint(outer,
                         s.w.vm.mintOpcode(zigzag::vortex::OpcodeKind::Halt));
  const auto first  = s.run(readOnCap);
  const auto second = s.run(readOnCap);
  ASSERT_NE(parkedToken(first), nullptr);
  ASSERT_NE(parkedToken(second), nullptr);
  ASSERT_EQ(s.w.service.orphanCount(), 0U);
  ASSERT_TRUE(s.w.vm.backtrack(outer));
  EXPECT_EQ(s.w.service.orphanCount(), 2U);
  // Each parked script's waiter went with its request, and its caller is told.
  for (int i = 0; i < 2; ++i) {
    auto woken = s.runner.takeWake();
    ASSERT_TRUE(woken) << "an orphaned parked script wakes its caller";
    EXPECT_EQ(woken->why, FrontendWake::Abandoned);
    const auto resumed = s.runner.resume(*std::move(woken));
    const auto *failed = failedDiagnostic(resumed);
    ASSERT_NE(failed, nullptr);
    EXPECT_EQ(failed->code, frontend_codes::tokenStale);
  }
  EXPECT_FALSE(s.runner.takeWake());
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

/// Every cell the session's dynamic-view registry names must still be live:
/// the registry outlives the script, so a freed entry is a stale handle that a
/// later program's reused slot would match (M7-VQL gate r2, N1).
void expectRegistryLive(ScriptWorld &s) {
  const auto &registry = s.engine.dynamicViews();
  for (const auto &[cell, record] : registry.viewCells) {
    static_cast<void>(record);
    EXPECT_TRUE(s.w.arena.contains(cell)) << "a view cell was freed";
  }
  for (const auto &[cell, record] : registry.shapeHandles) {
    static_cast<void>(record);
    EXPECT_TRUE(s.w.arena.contains(cell)) << "a shape handle was freed";
  }
  for (const auto &[cell, record] : registry.cellLenses) {
    static_cast<void>(record);
    EXPECT_TRUE(s.w.arena.contains(cell)) << "a lens handle was freed";
  }
}

auto finishRead(ScriptWorld &s, const std::string &source)
    -> std::vector<CellRef> {
  const auto begun = s.run(source);
  EXPECT_NE(parkedToken(begun), nullptr);
  s.w.complete(s.w.submissions() - 1, EndOfStream{{}});
  const auto done  = s.wakeAndResume();
  const auto *rows = completedRows(done);
  EXPECT_NE(rows, nullptr);
  return rows ? *rows : std::vector<CellRef>{};
}

TEST(VqlIoScriptTest, APivotAfterAnEffectKeepsItsViewUsableAfterwards) {
  ScriptWorld s;
  s.engine.setVariable("task", s.w.arena.makeCell("task"));
  const auto rows = finishRead(s, std::string{readOnCap} +
                                      "; return $task{ d.1 <=> d.priority }");
  ASSERT_FALSE(rows.empty());
  expectRegistryLive(s);
  // The view's handle is still a live, registered cell, so a later query
  // over it resolves instead of matching a recycled slot.
  s.engine.setVariable("view", rows);
  EXPECT_NO_THROW(s.engine.execute("$view/+d.1/value()"));
  EXPECT_FALSE(s.engine.dynamicViews().viewCells.empty());
}

constexpr auto lensLet = ";\n let $editable := lens(get: \\(.) ./value(), "
                         "put: \\(cell, val) $cell/value($val))";

TEST(VqlIoScriptTest, ALensAfterAnEffectStaysUsableInALaterQuery) {
  ScriptWorld s;
  s.engine.setVariable("item", s.w.arena.makeCell("before"));
  const auto rows = finishRead(s, std::string{readOnCap} + lensLet +
                                      "\n return $item/$editable");
  ASSERT_FALSE(rows.empty());
  expectRegistryLive(s);
  const auto read = s.engine.execute("$item/$editable");
  ASSERT_EQ(read.size(), 1U);
  EXPECT_EQ(s.w.arena.textOf(read.front()), "before");
  s.engine.execute("weave { $item/$editable%\"after\" }");
  const auto written = s.engine.execute("$item/value()");
  ASSERT_EQ(written.size(), 1U);
  EXPECT_EQ(s.w.arena.textOf(written.front()), "after");
}

TEST(VqlIoScriptTest, RegistryEntriesNeverGoStaleUnderSlotReuse) {
  ScriptWorld s;
  s.engine.setVariable("task", s.w.arena.makeCell("task"));
  s.engine.setVariable("item", s.w.arena.makeCell("x"));
  for (std::size_t i = 0; i < 40; ++i) {
    const auto source =
        std::string{readOnCap} +
        (i % 2 == 0 ? "; return $task{ d.1 <=> d.priority }"
                    : std::string{lensLet} + "\n return $item/$editable");
    static_cast<void>(finishRead(s, source));
    s.freeOldSpecs();
    // Plain scripts that mint and free cells keep recycling slots between the
    // registering ones.
    static_cast<void>(finishRead(s, readOnCap));
    s.freeOldSpecs();
    expectRegistryLive(s);
  }
}

TEST(VqlIoScriptTest, AnActionsOutcomeIsNamedForLaterStatementsAndItsTagRead) {
  // M7-VQL-b: `as $r` binds the outcome; io_tag($r) reads its tag as text.
  ScriptWorld s;
  const auto rows =
      finishRead(s, std::string{readOnCap} + " as $r; return io_tag($r)");
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(s.w.arena.textOf(rows.front()), "EndOfStream");
}

TEST(VqlIoScriptTest, TwoBoundActionsKeepBothOutcomesUntilTheScriptEnds) {
  ScriptWorld s;
  const auto begun = s.run(std::string{readOnCap} + " as $a; " + readOnCap +
                           " as $b; return $a");
  ASSERT_NE(parkedToken(begun), nullptr);
  s.w.complete(0, EndOfStream{{}});
  const auto second = s.wakeAndResume();
  ASSERT_NE(parkedToken(second), nullptr);
  s.w.complete(1, EndOfStream{{}});
  const auto done  = s.wakeAndResume();
  const auto *rows = completedRows(done);
  ASSERT_NE(rows, nullptr);
  ASSERT_EQ(rows->size(), 1U);
  EXPECT_TRUE(s.w.arena.contains(rows->front()))
      << "the first outcome survives the second admission";
}

TEST(VqlIoScriptTest, IoTagOfANonOutcomeFailsTheScriptByValue) {
  ScriptWorld s;
  s.engine.setVariable("plain", s.w.arena.makeCell("x"));
  const auto begun = s.run(std::string{readOnCap} + "; return io_tag($plain)");
  ASSERT_NE(parkedToken(begun), nullptr);
  s.w.complete(0, EndOfStream{{}});
  const auto done = s.wakeAndResume();
  EXPECT_NE(failedDiagnostic(done), nullptr);
}

TEST(VqlIoScriptTest, RegistrationsNeverReachedByRowsKeepTheirCells) {
  // M7-VQL gate r3, vql minor 2: a pivot, shape or lens whose result is not a
  // row has nothing but the registry naming its cells, so only keepIfNamed
  // stops the program reclaim from freeing them.
  ScriptWorld s;
  s.engine.setVariable("task", s.w.arena.makeCell("task"));
  s.engine.setVariable("item", s.w.arena.makeCell("x"));
  const auto before = s.engine.dynamicViews().viewCells.size();
  const auto rows =
      finishRead(s, std::string{readOnCap} +
                        "; return count($task{ d.1 <=> d.priority })");
  ASSERT_FALSE(rows.empty());
  EXPECT_GT(s.engine.dynamicViews().viewCells.size() +
                s.engine.dynamicViews().cellLenses.size() +
                s.engine.dynamicViews().shapeHandles.size(),
            before)
      << "the script registered something";
  expectRegistryLive(s);
  s.freeOldSpecs();
  // A following plain script must not take a slot the registry names.
  const auto sizes = s.engine.dynamicViews().viewCells.size();
  static_cast<void>(finishRead(s, readOnCap));
  expectRegistryLive(s);
  EXPECT_EQ(s.engine.dynamicViews().viewCells.size(), sizes);
}

TEST(VqlIoScriptTest, ARolledBackRunLeavesNoStaleRegistryKey) {
  // M7-VQL gate r3, vql minor 1: a run that registers a view and then fails
  // has its cells released; the keys must go with them, or a later mint into
  // the freed slot replaces a key without changing the map's size.
  zigzag::ArenaManifold arena;
  VQLEngine engine(arena);
  engine.setVariable("task", arena.makeCell("task"));
  const auto failed =
      engine.tryExecute("let $v := $task{ d.1 <=> d.priority }; $v/nonesuch()");
  ASSERT_FALSE(failed) << "the script is meant to fail after registering";
  const auto &registry = engine.dynamicViews();
  EXPECT_TRUE(registry.viewCells.empty());
  EXPECT_TRUE(registry.ownerViews.empty());
  for (const auto &[cell, record] : registry.viewCells) {
    static_cast<void>(record);
    EXPECT_TRUE(arena.contains(cell));
  }
  // The freed slots are reused by a script that registers again; the map grows
  // by exactly what it registered and every key is live.
  const auto again = engine.execute("$task{ d.1 <=> d.other }");
  ASSERT_EQ(again.size(), 1U);
  EXPECT_EQ(registry.viewCells.size(), 1U);
  for (const auto &[cell, record] : registry.viewCells) {
    static_cast<void>(record);
    EXPECT_TRUE(arena.contains(cell));
  }
}

TEST(VqlIoScriptTest, AnAbandonedScriptWithARegisteredViewKeepsItsCells) {
  ScriptWorld s;
  s.engine.setVariable("task", s.w.arena.makeCell("task"));
  const auto begun =
      s.run(std::string{readOnCap} + "; return $task{ d.1 <=> d.priority }");
  ASSERT_NE(parkedToken(begun), nullptr);
  s.w.complete(s.w.submissions() - 1, EndOfStream{{}});
  static_cast<void>(s.w.pump());
  auto woken = s.runner.takeWake();
  ASSERT_TRUE(woken);
  woken->why = FrontendWake::Abandoned;
  static_cast<void>(s.runner.resume(*std::move(woken)));
  expectRegistryLive(s);
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

// ---- M7-VQL-b: outcome fields, handles, cancel, deadlines ------------------

constexpr auto startOnCap =
    "effect { let $s := submit($cap, read_bytes(0, 16)); "
    "match $s { admitted($q) => $q, refused($e) => $e } }";

/// Park a read on the fake backend whose outcome the test then decides.
auto finishWith(ScriptWorld &s, const std::string &source, IoOutcome outcome)
    -> std::vector<CellRef> {
  const auto begun = s.run(source);
  EXPECT_NE(parkedToken(begun), nullptr);
  s.w.complete(s.w.submissions() - 1, std::move(outcome));
  const auto done  = s.wakeAndResume();
  const auto *rows = completedRows(done);
  EXPECT_NE(rows, nullptr) << (failedDiagnostic(done) != nullptr
                                   ? failedDiagnostic(done)->message
                                   : "");
  return rows ? *rows : std::vector<CellRef>{};
}

TEST(VqlIoFieldsTest, ACompleteOutcomeAnswersItsBytesAndPayloadAsValues) {
  ScriptWorld s;
  const IoProgress progress{.transferredBytes = 2, .phase = IoPhase::Terminal};
  const auto rows = finishWith(
      s,
      std::string{readOnCap} +
          " as $r; return io_bytes($r); return io_payload($r)",
      Partial{s.w.payloadRange(), PartialCause::BackendFailure, progress});
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(s.w.arena.textOf(rows.front()), "ok");

  ScriptWorld t;
  const auto bytes = finishWith(
      t, std::string{readOnCap} + " as $r; return io_bytes($r)",
      Partial{t.w.payloadRange(), PartialCause::BackendFailure, progress});
  ASSERT_EQ(bytes.size(), 1U);
  EXPECT_EQ(t.w.arena.textOf(bytes.front()), "2");
}

TEST(VqlIoFieldsTest, AFailedOutcomeAnswersItsErrorCodeAndDetail) {
  const IoProgress progress{.transferredBytes = 7, .phase = IoPhase::Terminal};
  {
    ScriptWorld s;
    const auto rows =
        finishWith(s, std::string{readOnCap} + " as $r; return io_error($r)",
                   Failed{{IoErrorCode::BackendFailure, 3}, {}, progress});
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(s.w.arena.textOf(rows.front()), "BackendFailure");
  }
  {
    ScriptWorld s;
    const auto rows =
        finishWith(s, std::string{readOnCap} + " as $r; return io_detail($r)",
                   Failed{{IoErrorCode::BackendFailure, 3}, {}, progress});
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(s.w.arena.textOf(rows.front()), "3");
  }
}

TEST(VqlIoFieldsTest, ARefusalAnswersItsErrorAndHasNoPayloadOrBytes) {
  ScriptWorld s;
  s.w.refuseAdmission = true;
  const auto begun =
      s.run(std::string{readOnCap} + " as $r; return io_error($r)");
  const auto *rows = completedRows(begun);
  ASSERT_NE(rows, nullptr);
  ASSERT_EQ(rows->size(), 1U);
  EXPECT_EQ(s.w.arena.textOf(rows->front()), "InvalidRequest");
  const auto none =
      s.run(std::string{readOnCap} + " as $r; return io_payload($r)");
  ASSERT_NE(completedRows(none), nullptr);
  EXPECT_TRUE(completedRows(none)->empty());
}

TEST(VqlIoFieldsTest, ThePayloadIsGatedByTheTerminalStatus) {
  // EndOfStream carries no payload whatever the cell graph holds; the field
  // answers nothing and is not an error.
  ScriptWorld s;
  const auto rows =
      finishWith(s, std::string{readOnCap} + " as $r; return io_payload($r)",
                 EndOfStream{{}});
  EXPECT_TRUE(rows.empty());
}

TEST(VqlIoFieldsTest, AFieldOfANonOutcomeFailsTheScriptByValue) {
  ScriptWorld s;
  s.engine.setVariable("plain", s.w.arena.makeCell("x"));
  for (const auto *fn : {"io_payload", "io_bytes", "io_error", "io_detail"}) {
    SCOPED_TRACE(fn);
    const auto result = s.run(std::string{"return "} + fn + "($plain)");
    EXPECT_NE(failedDiagnostic(result), nullptr);
  }
}

TEST(VqlIoGrammarTest, TheFieldFunctionsTakeExactlyOneArgument) {
  ScriptWorld s;
  const auto result = s.run("return io_bytes()");
  EXPECT_NE(failedDiagnostic(result), nullptr);
}

// -- Grammar of the new actions --------------------------------------------

TEST(VqlIoGrammarTest, WithinTakesALiteralOrAVariableAndAUnit) {
  const auto ms = Parser("effect { let $s := submit($c, read_bytes(0, 1)) "
                         "within 250 ms; match $s { admitted($q) => await($q), "
                         "refused($e) => $e } }")
                      .parseQuery();
  const auto &a = std::get<IoEffectStatement>(ms.body.statements[0]);
  ASSERT_TRUE(a.within);
  EXPECT_EQ(std::get<std::int64_t>(a.within->amount), 250);
  EXPECT_EQ(a.within->unitMillis, 1);
  const auto sec = Parser("effect { let $s := submit($c, read_bytes(0, 1)) "
                          "within $t s; match $s { admitted($q) => await($q), "
                          "refused($e) => $e } }")
                       .parseQuery();
  const auto &b = std::get<IoEffectStatement>(sec.body.statements[0]);
  EXPECT_EQ(std::get<std::string>(b.within->amount), "t");
  EXPECT_EQ(b.within->unitMillis, 1000);
}

TEST(VqlIoGrammarTest, ADetachedReadYieldsTheHandleAndNamesIt) {
  const auto query = Parser(std::string{startOnCap} + " as $h").parseQuery();
  const auto &a    = std::get<IoEffectStatement>(query.body.statements[0]);
  EXPECT_TRUE(a.detached);
  EXPECT_EQ(a.resultVar, std::optional<std::string>{"h"});
}

TEST(VqlIoGrammarTest, AwaitCancelAndOpenAreScriptActions) {
  const auto query =
      Parser("effect { await($h) } as $r; effect { cancel($h) } as $c; "
             "effect { open(\"a b.txt\") } as $f; effect { open($p) }")
          .parseQuery();
  ASSERT_EQ(query.body.statements.size(), 4U);
  using Kind         = IoEffectStatement::Kind;
  const auto &await  = std::get<IoEffectStatement>(query.body.statements[0]);
  const auto &cancel = std::get<IoEffectStatement>(query.body.statements[1]);
  const auto &open   = std::get<IoEffectStatement>(query.body.statements[2]);
  const auto &openV  = std::get<IoEffectStatement>(query.body.statements[3]);
  EXPECT_EQ(await.kind, Kind::Await);
  EXPECT_EQ(await.handleVar, "h");
  EXPECT_EQ(cancel.kind, Kind::Cancel);
  EXPECT_EQ(open.kind, Kind::Open);
  EXPECT_EQ(open.openPath.text, "a b.txt");
  EXPECT_FALSE(open.openPath.isVariable);
  EXPECT_TRUE(openV.openPath.isVariable);
  EXPECT_EQ(open.resultVar, std::optional<std::string>{"f"});
}

TEST(VqlIoGrammarTest, TheNewActionsAreRefusedInRowAndNestedPositions) {
  for (const auto *action : {"effect { await($h) }", "effect { cancel($h) }",
                             "effect { open(\"x\") }"}) {
    for (const auto &source : {
             std::string{"for $x in ## "} + action,
             std::string{"let $x := ## "} + action,
             std::string{"def f() := { "} + action + " }; f()",
             std::string{"watch ##/d.x as $v { "} + action + " }",
         }) {
      SCOPED_TRACE(source);
      EXPECT_NE(parseFailure(source).find(contextCode), std::string::npos);
    }
  }
}

TEST(VqlIoGrammarTest, AMalformedNewActionIsAParseError) {
  for (const auto *source : {
           "effect { let $s := submit($c, read_bytes(0, 1)) within 5; match $s "
           "{ admitted($q) => await($q), refused($e) => $e } }",
           "effect { let $s := submit($c, read_bytes(0, 1)) within 5 days; "
           "match $s { admitted($q) => await($q), refused($e) => $e } }",
           "effect { let $s := submit($c, read_bytes(0, 1)); match $s { "
           "admitted($q) => $z, refused($e) => $e } }",
           "effect { await(7) }",
           "effect { cancel() }",
           "effect { open(7) }",
           "effect { open() }",
           "effect { await($h) as $r }",
       }) {
    SCOPED_TRACE(source);
    const auto message = parseFailure(source);
    EXPECT_FALSE(message.empty());
    EXPECT_EQ(message.find(contextCode), std::string::npos) << message;
  }
}

// -- Detached reads, await and cancel --------------------------------------

TEST(VqlIoHandleTest, ADetachedReadStartsAndDoesNotWait) {
  ScriptWorld s;
  const auto result = s.run(std::string{startOnCap} + " as $h");
  const auto *rows  = completedRows(result);
  ASSERT_NE(rows, nullptr) << "a started read does not park the script";
  EXPECT_EQ(s.w.submissions(), 1U);
  ASSERT_EQ(rows->size(), 1U);
  const auto incarnation = s.w.arena.incarnationOf(rows->front());
  ASSERT_TRUE(incarnation);
  EXPECT_EQ(ioCellTag(s.w.arena, rows->front(), *incarnation),
            IoCellTag::Request);
}

TEST(VqlIoHandleTest, AwaitTakesTheSameRequestAndNeverSubmitsAgain) {
  ScriptWorld s;
  const auto begun =
      s.run(std::string{startOnCap} + " as $h; effect { await($h) } as $r; "
                                      "return io_tag($r)");
  const auto *token = parkedToken(begun);
  ASSERT_NE(token, nullptr);
  EXPECT_EQ(token->position, 1U) << "parked at the await action";
  EXPECT_EQ(s.w.submissions(), 1U);
  s.w.complete(0, EndOfStream{{}});
  const auto done  = s.wakeAndResume();
  const auto *rows = completedRows(done);
  ASSERT_NE(rows, nullptr);
  ASSERT_EQ(rows->size(), 1U);
  EXPECT_EQ(s.w.arena.textOf(rows->front()), "EndOfStream");
  EXPECT_EQ(s.w.submissions(), 1U);
}

TEST(VqlIoHandleTest, AwaitOfARefusalIsTheRefusalAsAValue) {
  ScriptWorld s;
  s.w.refuseAdmission = true;
  const auto done =
      s.run(std::string{startOnCap} + " as $h; effect { await($h) } as $r; "
                                      "return io_tag($r)");
  const auto *rows = completedRows(done);
  ASSERT_NE(rows, nullptr) << "awaiting a refusal is not an error";
  ASSERT_EQ(rows->size(), 1U);
  EXPECT_EQ(s.w.arena.textOf(rows->front()), "Rejected");
  EXPECT_EQ(s.w.submissions(), 0U);
}

TEST(VqlIoHandleTest, CancelMakesTheOutcomeCancelledAsAValue) {
  ScriptWorld s;
  const auto begun =
      s.run(std::string{startOnCap} +
            " as $h; effect { cancel($h) } as $c; return io_tag($c)");
  const auto *token = parkedToken(begun);
  ASSERT_NE(token, nullptr) << "the cancel waits for the backend to stop";
  EXPECT_EQ(token->position, 1U);
  // The backend honours the stop request.
  s.w.complete(0, Cancelled{{}, {.phase = IoPhase::Terminal}, false});
  const auto done  = s.wakeAndResume();
  const auto *rows = completedRows(done);
  ASSERT_NE(rows, nullptr);
  ASSERT_EQ(rows->size(), 1U);
  EXPECT_EQ(s.w.arena.textOf(rows->front()), "Cancelled");
  EXPECT_EQ(s.w.submissions(), 1U);
}

TEST(VqlIoHandleTest, ACancelThatLosesTheRaceKeepsTheCompletionsOutcome) {
  ScriptWorld s;
  const auto begun =
      s.run(std::string{startOnCap} +
            " as $h; effect { cancel($h) } as $c; return io_tag($c)");
  ASSERT_NE(parkedToken(begun), nullptr);
  // The read finished before the backend saw the stop.
  s.w.complete(0, EndOfStream{{}});
  const auto done  = s.wakeAndResume();
  const auto *rows = completedRows(done);
  ASSERT_NE(rows, nullptr);
  EXPECT_EQ(s.w.arena.textOf(rows->front()), "EndOfStream");
}

TEST(VqlIoHandleTest, CancellingATerminalRequestIsATypedNoOpValue) {
  ScriptWorld t;
  const auto first = t.run(std::string{startOnCap} +
                           " as $h; effect { await($h) } as $w; "
                           "effect { cancel($h) } as $c; return io_tag($c)");
  ASSERT_NE(parkedToken(first), nullptr);
  t.w.complete(0, EndOfStream{{}});
  const auto done  = t.wakeAndResume();
  const auto *rows = completedRows(done);
  EXPECT_EQ(t.w.scheduler->entries.size(), 1U);
  ASSERT_NE(rows, nullptr) << (failedDiagnostic(done) != nullptr
                                   ? failedDiagnostic(done)->message
                                   : "");
  ASSERT_EQ(rows->size(), 1U);
  EXPECT_EQ(t.w.arena.textOf(rows->front()), "EndOfStream")
      << "the request's own terminal outcome is the value";
}

TEST(VqlIoHandleTest, CancellingARefusalIsTheRefusalAsAValue) {
  ScriptWorld s;
  s.w.refuseAdmission = true;
  const auto done =
      s.run(std::string{startOnCap} + " as $h; effect { cancel($h) } as $c; "
                                      "return io_tag($c)");
  const auto *rows = completedRows(done);
  ASSERT_NE(rows, nullptr);
  EXPECT_EQ(s.w.arena.textOf(rows->front()), "Rejected");
}

TEST(VqlIoHandleTest, AHandleThatIsNeitherRequestNorOutcomeFailsByValue) {
  ScriptWorld s;
  s.engine.setVariable("plain", s.w.arena.makeCell("x"));
  for (const auto *action :
       {"effect { await($plain) }", "effect { cancel($plain) }"}) {
    SCOPED_TRACE(action);
    const auto result  = s.run(action);
    const auto *failed = failedDiagnostic(result);
    ASSERT_NE(failed, nullptr);
    EXPECT_EQ(failed->code, "VQL_IO_EFFECT_ARGUMENT");
  }
}

TEST(VqlIoHandleTest, ADetachedReadNobodyAwaitsIsRetiredOnceItEnds) {
  ScriptWorld s;
  const auto first = s.run(std::string{startOnCap} + " as $h");
  ASSERT_NE(completedRows(first), nullptr);
  const auto before = s.w.submissions();
  s.w.complete(0, EndOfStream{{}});
  static_cast<void>(s.w.pump());
  // The next script's admission retires the finished one, running or not.
  const auto second = s.run(std::string{startOnCap} + " as $h");
  ASSERT_NE(completedRows(second), nullptr);
  EXPECT_EQ(s.w.submissions(), before + 1U);
}

// -- Deadlines --------------------------------------------------------------

TEST(VqlIoDeadlineTest, WithinTimesOutAsAValueOnAFakeClock) {
  ScriptWorld s;
  auto now = std::chrono::steady_clock::time_point{};
  s.w.service.setClock([&now] { return now; });
  const auto begun = s.run(
      "effect { let $s := submit($cap, read_bytes(0, 16)) within 250 ms; "
      "match $s { admitted($q) => await($q), refused($e) => $e } } as $r; "
      "return io_tag($r)");
  ASSERT_NE(parkedToken(begun), nullptr);
  now += std::chrono::milliseconds{249};
  static_cast<void>(s.w.pump());
  EXPECT_FALSE(s.runner.takeWake()) << "before the deadline nothing happens";
  now += std::chrono::milliseconds{1};
  // The fake backend honours the stop the service asked for.
  static_cast<void>(s.w.pump());
  s.w.complete(0, Cancelled{{}, {.phase = IoPhase::Terminal}, false});
  const auto done  = s.wakeAndResume();
  const auto *rows = completedRows(done);
  ASSERT_NE(rows, nullptr);
  ASSERT_EQ(rows->size(), 1U);
  EXPECT_EQ(s.w.arena.textOf(rows->front()), "TimedOut")
      << "a deadline is a value, never a QueryError or empty rows";
}

TEST(VqlIoDeadlineTest, ARequestThatFinishesFirstKeepsItsOutcome) {
  ScriptWorld s;
  auto now = std::chrono::steady_clock::time_point{};
  s.w.service.setClock([&now] { return now; });
  const auto begun = s.run(
      "effect { let $s := submit($cap, read_bytes(0, 16)) within 2 s; "
      "match $s { admitted($q) => await($q), refused($e) => $e } } as $r; "
      "return io_tag($r)");
  ASSERT_NE(parkedToken(begun), nullptr);
  s.w.complete(0, EndOfStream{{}});
  now += std::chrono::seconds{5};
  const auto done  = s.wakeAndResume();
  const auto *rows = completedRows(done);
  ASSERT_NE(rows, nullptr);
  EXPECT_EQ(s.w.arena.textOf(rows->front()), "EndOfStream");
}

TEST(VqlIoDeadlineTest, AVariableBoundWithinIsReadAtTheAction) {
  ScriptWorld s;
  auto now = std::chrono::steady_clock::time_point{};
  s.w.service.setClock([&now] { return now; });
  s.engine.setVariable("t", s.w.arena.makeScalarCell(std::int64_t{3}));
  const auto begun =
      s.run("effect { let $s := submit($cap, read_bytes(0, 16)) within $t s; "
            "match $s { admitted($q) => await($q), refused($e) => $e } }");
  ASSERT_NE(parkedToken(begun), nullptr);
  now += std::chrono::milliseconds{2999};
  static_cast<void>(s.w.pump());
  s.w.complete(0, EndOfStream{{}});
  EXPECT_NE(completedRows(s.wakeAndResume()), nullptr);
}

TEST(VqlIoDeadlineTest, ANegativeOrNonNumericWithinFailsBeforeAnythingRuns) {
  ScriptWorld s;
  s.engine.setVariable("neg", s.w.arena.makeScalarCell(std::int64_t{-1}));
  s.engine.setVariable("word", s.w.arena.makeCell("soon"));
  for (const auto *bound : {"$neg", "$word"}) {
    SCOPED_TRACE(bound);
    const auto result =
        s.run(std::string{"effect { let $s := submit($cap, read_bytes(0, 16)) "
                          "within "} +
              bound +
              " ms; match $s { admitted($q) => await($q), refused($e) => $e } "
              "}");
    const auto *failed = failedDiagnostic(result);
    ASSERT_NE(failed, nullptr);
    EXPECT_EQ(failed->code, "VQL_IO_EFFECT_ARGUMENT");
  }
  EXPECT_EQ(s.w.submissions(), 0U);
}

TEST(VqlIoDeadlineTest, AScriptPastItsOverallDeadlineIsAbandonedStably) {
  ScriptWorld s;
  auto now = std::chrono::steady_clock::time_point{};
  s.w.service.setClock([&now] { return now; });
  s.runner.setScriptTimeout(std::chrono::milliseconds{500});
  const auto begun  = s.run(readOnCap);
  const auto *token = parkedToken(begun);
  ASSERT_NE(token, nullptr);
  const auto parkedToken0 = *token;
  now += std::chrono::milliseconds{499};
  EXPECT_FALSE(s.runner.expireIfDue(parkedToken0)) << "inside the deadline";
  now += std::chrono::milliseconds{1};
  const auto expired = s.runner.expireIfDue(parkedToken0);
  ASSERT_TRUE(expired);
  EXPECT_EQ(expired->code, "VQL_SCRIPT_TIMEOUT");
  EXPECT_EQ(expired->position, 0U);
  EXPECT_EQ(s.runner.parkedCount(), 0U);
  EXPECT_FALSE(s.runner.expireIfDue(parkedToken0))
      << "an abandoned script is not abandoned twice";
}

// vortex gate r1 M1: the request a timed-out script awaited is asked to stop,
// whatever the program's choice points do when it is dropped (a rolled-back
// request is orphaned, and an orphan is never cancelled by itself). No script
// shape available here holds a scoped choice point over the await, so this
// pins the stop and the freed budget on the plain path.
TEST(VqlIoDeadlineTest, ATimedOutScriptsAwaitedRequestIsAskedToStop) {
  ScriptWorld s;
  auto now = std::chrono::steady_clock::time_point{};
  s.w.service.setClock([&now] { return now; });
  s.runner.setScriptTimeout(std::chrono::milliseconds{500});
  const auto begun = s.run(readOnCap);
  ASSERT_NE(parkedToken(begun), nullptr);
  ASSERT_EQ(s.w.scheduler->cancels, 0U);
  now += std::chrono::milliseconds{500};
  ASSERT_TRUE(s.runner.expireIfDue(*parkedToken(begun)));
  EXPECT_EQ(s.w.scheduler->cancels, 1U)
      << "the backend was told to stop, as the diagnostic says";
  // The backend ends; its terminal is dropped unpublished and the slot freed.
  s.w.complete(0, Cancelled{{}, {}, false});
  static_cast<void>(s.w.pump());
  EXPECT_EQ(s.w.service.requestCount(), 0U)
      << "no request is left holding the budget";
}

// vql gate r1 MINOR 3: a request's own `within` is not cut short by the
// script's overall timeout; the script waits until that deadline plus its own
// timeout so the TimedOut outcome, a value, is what it reads.
TEST(VqlIoDeadlineTest, AWithinLongerThanTheScriptTimeoutIsNotCutShort) {
  ScriptWorld s;
  auto now = std::chrono::steady_clock::time_point{};
  s.w.service.setClock([&now] { return now; });
  s.runner.setScriptTimeout(std::chrono::milliseconds{500});
  const auto begun =
      s.run("effect { let $s := submit($cap, read_bytes(0, 16)) within 2 s; "
            "match $s { admitted($q) => await($q), refused($e) => $e } }");
  ASSERT_NE(parkedToken(begun), nullptr);
  const auto token = *parkedToken(begun);
  now += std::chrono::milliseconds{2499};
  EXPECT_FALSE(s.runner.expireIfDue(token))
      << "inside the request's deadline plus the script timeout";
  now += std::chrono::milliseconds{1};
  EXPECT_TRUE(s.runner.expireIfDue(token));
}

TEST(VqlIoDeadlineTest, AScriptWithoutADeadlineWaitsAsLongAsItLikes) {
  ScriptWorld s;
  auto now = std::chrono::steady_clock::time_point{};
  s.w.service.setClock([&now] { return now; });
  const auto begun = s.run(readOnCap);
  ASSERT_NE(parkedToken(begun), nullptr);
  now += std::chrono::hours{24};
  EXPECT_FALSE(s.runner.expireIfDue(*parkedToken(begun)));
}

TEST(VqlIoDeadlineTest, TheDriverReportsTheTimeoutAndLeavesNoBudgetBehind) {
  ScriptWorld s;
  auto now = std::chrono::steady_clock::time_point{};
  s.w.service.setClock([&now] { return now; });
  s.runner.setScriptTimeout(std::chrono::milliseconds{10});
  const auto rows = xanadu::vql::driveToCompletion(
      s.runner, s.run(readOnCap), [&] { static_cast<void>(s.w.pump()); },
      [&] { now += std::chrono::milliseconds{4}; });
  ASSERT_FALSE(rows);
  EXPECT_EQ(rows.error().code, "VQL_SCRIPT_TIMEOUT");
  EXPECT_EQ(s.w.scheduler->cancels, 1U) << "the awaited request was stopped";
  s.w.complete(0, Cancelled{{}, {}, false});
  static_cast<void>(s.w.pump());
  EXPECT_EQ(s.w.service.requestCount(), 0U) << "no budget left behind";
}

// vql gate r2 N2: a request's own deadline extends the script's only while the
// request runs; once it ends early a later unbounded await is cut at the
// script's own deadline.
TEST(VqlIoDeadlineTest, AnExtensionEndsWithTheRequestThatEarnedIt) {
  ScriptWorld s;
  auto now = std::chrono::steady_clock::time_point{};
  s.w.service.setClock([&now] { return now; });
  s.runner.setScriptTimeout(std::chrono::milliseconds{500});
  const auto begun =
      s.run("effect { let $s := submit($cap, read_bytes(0, 16)) within 5 s; "
            "match $s { admitted($q) => await($q), refused($e) => $e } }; " +
            std::string{readOnCap});
  ASSERT_NE(parkedToken(begun), nullptr);
  now += std::chrono::milliseconds{100};
  s.w.complete(0, EndOfStream{{}});
  const auto second = s.wakeAndResume();
  const auto *token = parkedToken(second);
  ASSERT_NE(token, nullptr) << "the second read waits";
  now =
      std::chrono::steady_clock::time_point{} + std::chrono::milliseconds{499};
  EXPECT_FALSE(s.runner.expireIfDue(*token));
  now += std::chrono::milliseconds{1};
  EXPECT_TRUE(s.runner.expireIfDue(*token))
      << "the finished request's extension is gone";
}

TEST(VqlIoDeadlineTest, AnExtensionHoldsWhileItsRequestStillRuns) {
  ScriptWorld s;
  auto now = std::chrono::steady_clock::time_point{};
  s.w.service.setClock([&now] { return now; });
  s.runner.setScriptTimeout(std::chrono::milliseconds{500});
  const auto begun =
      s.run("effect { let $s := submit($cap, read_bytes(0, 16)) within 5 s; "
            "match $s { admitted($q) => $q, refused($e) => $e } } as $h; " +
            std::string{readOnCap});
  const auto *token = parkedToken(begun);
  ASSERT_NE(token, nullptr);
  now += std::chrono::milliseconds{500};
  EXPECT_FALSE(s.runner.expireIfDue(*token))
      << "the detached bounded request still runs";
}

// vortex gate r2 MINOR 1 and 4: what the runner leaves behind when it goes,
// and a close the service keeps refusing.
struct OwnedRunnerWorld {
  World w;
  VQLEngine engine{w.core};
  std::size_t closeTries{};
  std::unique_ptr<ScriptRunner> runner;

  OwnedRunnerWorld() {
    engine.setVariable("cap", w.capability.cell);
    runner = std::make_unique<ScriptRunner>(
        engine, w.vm,
        ScriptIoPorts{w.bridge, w.host, w.payloads, w.diagnostics,
                      [this](CapabilityCell, std::uint64_t, std::uint64_t)
                          -> std::expected<CellRef, FrontendDiagnostic> {
                        return w.specification();
                      },
                      [this](std::string_view)
                          -> std::expected<SubmissionCell, FrontendDiagnostic> {
                        return w.bridge.submit(w.capability, w.specification());
                      },
                      [this](CapabilityCell cap)
                          -> std::expected<SubmissionCell, FrontendDiagnostic> {
                        ++closeTries;
                        w.refuseAdmission = true;
                        auto refused = w.bridge.submit(cap, w.specification());
                        w.refuseAdmission = false;
                        return refused;
                      },
                      [this](OutcomeCell) -> std::optional<CapabilityCell> {
                        return w.capability;
                      }});
  }
};

TEST(VqlIoOpenTest, ARefusedCloseIsNotRetriedAtEveryReap) {
  OwnedRunnerWorld o;
  const auto begun = o.runner->run(
      Parser("effect { open(\"x\") } as $f; return io_tag($f)").parseQuery());
  ASSERT_NE(parkedToken(begun), nullptr);
  o.w.complete(0, Complete{{}, {}, {}});
  static_cast<void>(o.w.pump());
  auto woken = o.runner->takeWake();
  ASSERT_TRUE(woken.has_value());
  const auto done = o.runner->resume(*std::move(woken));
  ASSERT_NE(completedRows(done), nullptr);
  EXPECT_EQ(o.closeTries, 1U) << "the close was tried once and refused";
  const auto cells    = o.w.arena.liveCells();
  constexpr int reaps = 20;
  for (int i = 0; i < reaps; ++i) static_cast<void>(o.runner->takeWake());
  EXPECT_EQ(o.closeTries, 1U)
      << "no close finished, so nothing could have cleared the refusal";
  EXPECT_EQ(o.w.arena.liveCells(), cells)
      << "no refusal cell is minted and retired per reap";
}

TEST(VqlIoScriptTest, DestroyingARunnerStopsAReadThatNeverEnds) {
  OwnedRunnerWorld o;
  const auto result =
      o.runner->run(Parser(std::string{startOnCap} + " as $h").parseQuery());
  ASSERT_NE(completedRows(result), nullptr);
  ASSERT_EQ(o.w.service.requestCount(), 1U);
  o.runner.reset();
  EXPECT_EQ(o.w.scheduler->cancels, 1U) << "the orphan was asked to stop";
  EXPECT_EQ(o.w.service.requestCount(), 1U) << "still accounted for";
  // The backend honours the stop.
  o.w.complete(0, Cancelled{{}, {.phase = IoPhase::Terminal}, false});
  static_cast<void>(o.w.pump());
  EXPECT_EQ(o.w.service.requestCount(), 0U) << "the slot is free again";
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
  xanadu::vortex::FileReadHost host;
  // One runner for the session, as vquery has: it retires an action's request
  // when the next action or the runner replaces it, so the rows a test reads
  // after `run` returns are still live.
  ScriptRunner runner{engine, vm, xanadu::vql::fileScriptPorts(host)};
  std::filesystem::path dir;

  explicit FileScriptWorld(xanadu::vortex::FileReadHostOptions options = {})
      : host(vm, arena, options) {
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

// ---- M7-VQL-b over real files: script-level open, close, handles ------------

/// Pump until @p done or a generous limit; every wait is a pump, never a block.
template <typename Done> bool pumpUntil(FileScriptWorld &f, const Done &done) {
  const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (!done() && std::chrono::steady_clock::now() < limit) {
    f.host.pump(16);
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return done();
}

auto openAction(const std::filesystem::path &path, const char *name) {
  return std::string{"effect { open(\""} + path.string() + "\") } as $" + name;
}

constexpr auto readFrom = [](const char *cap, int offset, int bound) {
  return std::string{"effect { let $s := submit($"} + cap + ", read_bytes(" +
         std::to_string(offset) + ", " + std::to_string(bound) +
         ")); match $s { admitted($q) => await($q), refused($e) => $e } }";
};

TEST(VqlIoOpenTest, AScriptOpensReadsAndClosesItsOwnFile) {
  FileScriptWorld f;
  const auto path = f.write("a.txt", "hello world");
  const auto rows = f.run(openAction(path, "f") + "; " + readFrom("f", 6, 5));
  ASSERT_TRUE(rows) << rows.error().message;
  ASSERT_EQ(rows->size(), 2U);
  EXPECT_EQ(f.tag((*rows)[0]), IoOutcomeTag::Complete);
  EXPECT_EQ(f.text((*rows)[1]), "world");
  EXPECT_TRUE(pumpUntil(f, [&] {
    static_cast<void>(f.runner.takeWake());
    return f.host.adapter().openedCount() == 0;
  })) << "the capability's lifetime is the script's: the file is closed";
}

TEST(VqlIoOpenTest, TheOpenOutcomeIsTheRowAndTheNameIsTheCapability) {
  FileScriptWorld f;
  const auto path = f.write("a.txt", "abc");
  const auto rows = f.run(openAction(path, "f") + "; return io_tag($f)");
  ASSERT_TRUE(rows) << rows.error().message;
  ASSERT_EQ(rows->size(), 1U);
  EXPECT_EQ(f.text(rows->front()), "Capability")
      << "the name is the capability the open minted";
  const auto statement = f.run(openAction(path, "g"));
  ASSERT_TRUE(statement) << statement.error().message;
  ASSERT_EQ(statement->size(), 1U);
  EXPECT_EQ(f.tag(statement->front()), IoOutcomeTag::Complete);
}

TEST(VqlIoOpenTest, ACapabilityDoesNotOutliveItsScript) {
  FileScriptWorld f;
  const auto path = f.write("a.txt", "hello world");
  const auto kept = f.run(openAction(path, "f") + "; return $f");
  ASSERT_TRUE(kept) << kept.error().message;
  ASSERT_EQ(kept->size(), 1U);
  f.engine.setVariable("old", kept->front());
  ASSERT_TRUE(pumpUntil(f, [&] {
    static_cast<void>(f.runner.takeWake());
    return f.host.adapter().openedCount() == 0;
  }));
  const auto later = f.run(readFrom("old", 0, 5));
  // Closed means refused: a typed script failure or a non-Complete outcome,
  // never the file's bytes.
  if (later) {
    ASSERT_FALSE(later->empty());
    EXPECT_NE(f.tag(later->front()), IoOutcomeTag::Complete);
  } else {
    EXPECT_FALSE(later.error().message.empty());
  }
}

// vql gate r1 M2: an open nobody bound is closed with the script as well.
TEST(VqlIoOpenTest, AnUnboundOpenIsClosedAtTheEndOfTheScript) {
  FileScriptWorld f;
  const auto path = f.write("a.txt", "abc");
  const auto rows = f.run("effect { open(\"" + path.string() + "\") }");
  ASSERT_TRUE(rows) << rows.error().message;
  EXPECT_TRUE(pumpUntil(f, [&] {
    static_cast<void>(f.runner.takeWake());
    return f.host.adapter().openedCount() == 0;
  })) << "the file an unbound open minted was closed";
}

// vortex gate r1 M2: at the request budget the close of the script's own file
// is still admitted (a close frees what it holds, so it is never refused for
// budget), and the descriptor closes.
TEST(VqlIoOpenTest, TheCloseIsAdmittedWhenTheScriptFilledTheBudget) {
  xanadu::vortex::FileReadHostOptions options;
  constexpr std::size_t openAndTwoReads = 3;
  options.requests                      = openAndTwoReads;
  FileScriptWorld f(options);
  const auto path = f.write("a.txt", "hello world");
  const auto rows = f.run(openAction(path, "f") + "; " + readFrom("f", 0, 5) +
                          "; " + readFrom("f", 6, 5));
  ASSERT_TRUE(rows) << rows.error().message;
  EXPECT_EQ(f.host.service().requestCount(), openAndTwoReads + 1)
      << "the script filled the budget and the close went beyond it";
  EXPECT_TRUE(pumpUntil(f, [&] {
    static_cast<void>(f.runner.takeWake());
    return f.host.adapter().openedCount() == 0;
  })) << "the descriptor closed although the budget was full";
}

// vortex gate r1 MINOR 3: the rows of a finished script are retired before the
// next script's first admission, so the budget they filled is free for it.
TEST(VqlIoOpenTest, AFinishedScriptsRowsDoNotRefuseTheNextScriptsFirstAction) {
  xanadu::vortex::FileReadHostOptions options;
  constexpr std::size_t openAndRead = 2;
  options.requests                  = openAndRead;
  FileScriptWorld f(options);
  const auto a = f.write("a.txt", "hello world");
  const auto b = f.write("b.txt", "second");
  ASSERT_TRUE(f.run(openAction(a, "f") + "; " + readFrom("f", 0, 5)));
  const auto rows = f.run(openAction(b, "g") + "; return io_tag($g)");
  ASSERT_TRUE(rows) << rows.error().message;
  ASSERT_EQ(rows->size(), 1U);
  EXPECT_EQ(f.text(rows->front()), "Capability")
      << "the open was admitted, not refused for the first script's rows";
}

// vql gate r1 MINOR 1: a read through a name whose open minted no capability
// says which open and how it ended.
TEST(VqlIoOpenTest, AReadThroughAFailedOpenNamesTheOpenAndItsTag) {
  FileScriptWorld f;
  const auto rows = f.run(openAction(f.dir / "missing.txt", "f") + "; " +
                          readFrom("f", 0, 5));
  ASSERT_FALSE(rows);
  EXPECT_EQ(rows.error().code, "VQL_IO_EFFECT_ARGUMENT");
  EXPECT_NE(rows.error().message.find("$f"), std::string::npos)
      << rows.error().message;
  EXPECT_NE(rows.error().message.find("Failed"), std::string::npos)
      << rows.error().message;
  EXPECT_EQ(rows.error().position, 1U);
}

// vortex gate r1 M3: a runner destroyed with a read still running and a close
// just admitted leaves no request in the service: nothing was left to retire
// them, so each would hold a budget slot for good.
TEST(VqlIoOpenTest, DestroyingARunnerLeavesNoServiceRequestBehind) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
  VQLEngine engine{core};
  xanadu::vortex::FileReadHost host{vm, arena};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("vql_io_dtor_" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  const auto path = dir / "a.txt";
  std::ofstream(path, std::ios::binary | std::ios::trunc) << "hello world";
  {
    auto runner = std::make_unique<ScriptRunner>(
        engine, vm, xanadu::vql::fileScriptPorts(host));
    // The read is detached (its handle is the row) and the file opens first,
    // so at the script's end the read runs and the close is just admitted.
    const auto rows = xanadu::vql::driveToCompletion(
        *runner,
        runner->run(Parser(std::string{"effect { open(\""} + path.string() +
                           "\") } as $f; effect { let $s := submit($f, "
                           "read_bytes(0, 5)); match $s { admitted($q) => $q, "
                           "refused($e) => $e } }")
                        .parseQuery()),
        [&] { host.pump(16); },
        [] { std::this_thread::sleep_for(std::chrono::milliseconds{1}); });
    ASSERT_TRUE(rows) << rows.error().message;
    ASSERT_GE(host.service().requestCount(), 2U);
    runner.reset();
  }
  const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (host.service().requestCount() != 0 &&
         std::chrono::steady_clock::now() < limit) {
    host.pump(16);
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  EXPECT_EQ(host.service().requestCount(), 0U)
      << "every request the runner held was accounted for";
  EXPECT_EQ(host.adapter().openedCount(), 0U);
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(VqlIoOpenTest, AMissingFileIsAnOutcomeValueNotAScriptFailure) {
  FileScriptWorld f;
  const auto rows =
      f.run(openAction(f.dir / "missing.txt", "f") + "; return io_tag($f)");
  ASSERT_TRUE(rows) << rows.error().message;
  ASSERT_EQ(rows->size(), 1U);
  EXPECT_EQ(f.text(rows->front()), "Failed");
  EXPECT_EQ(f.host.adapter().openedCount(), 0U);
}

TEST(VqlIoOpenTest, ReadingThroughAFailedOpenFailsTheScriptWithItsStatement) {
  FileScriptWorld f;
  const auto rows = f.run(openAction(f.dir / "missing.txt", "f") + "; " +
                          readFrom("f", 0, 5));
  ASSERT_FALSE(rows);
  EXPECT_EQ(rows.error().position, 1U);
}

TEST(VqlIoOpenTest, ThePathMayBeAVariableAndTwoFilesBothClose) {
  FileScriptWorld f;
  const auto a = f.write("a.txt", "alpha");
  const auto b = f.write("b.txt", "beta");
  f.engine.setVariable("where", f.arena.makeCell(b.string()));
  const auto rows =
      f.run(openAction(a, "x") + "; effect { open($where) } as $y; " +
            readFrom("x", 0, 5) + "; " + readFrom("y", 0, 4));
  ASSERT_TRUE(rows) << rows.error().message;
  ASSERT_EQ(rows->size(), 2U);
  EXPECT_EQ(f.text((*rows)[1]), "beta");
  EXPECT_TRUE(pumpUntil(f, [&] {
    static_cast<void>(f.runner.takeWake());
    return f.host.adapter().openedCount() == 0;
  }));
}

TEST(VqlIoOpenTest, AFailingScriptStillClosesWhatItOpened) {
  FileScriptWorld f;
  const auto path = f.write("a.txt", "abc");
  const auto rows = f.run(openAction(path, "f") + "; " + readFrom("f", 0, 5) +
                          "; return io_tag($nothing)");
  ASSERT_FALSE(rows);
  EXPECT_TRUE(pumpUntil(f, [&] {
    static_cast<void>(f.runner.takeWake());
    return f.host.adapter().openedCount() == 0;
  }));
}

TEST(VqlIoOpenTest, AnEmptyPathIsRefusedBeforeAnythingOpens) {
  FileScriptWorld f;
  const auto empty = f.run("effect { open(\"\") }");
  ASSERT_FALSE(empty);
  EXPECT_EQ(empty.error().code, "VQL_IO_EFFECT_ARGUMENT");
}

TEST(VqlIoOpenTest, AHostThatCannotOpenFilesRefusesTheAction) {
  ScriptWorld s;
  const auto result  = s.run("effect { open(\"/nowhere\") }");
  const auto *failed = failedDiagnostic(result);
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->code, "VQL_IO_EFFECT_ARGUMENT");
}

TEST(VqlIoOpenTest, ARealFileOpenPastItsDeadlineDoesNotLoseItsCapability) {
  // The injected clock jumps past the open deadline before the first pump.
  // A completion already in hand wins the race, so the open is Complete.
  auto now = std::chrono::steady_clock::time_point{};
  xanadu::vortex::FileReadHostOptions options;
  options.clock = [&now] { return now; };
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
  xanadu::vortex::FileReadHost host{vm, arena, options};
  const auto path = std::filesystem::temp_directory_path() /
                    ("vql_io_deadline_" + std::to_string(::getpid()) + ".txt");
  std::ofstream(path) << "abc";
  const auto open = host.beginOpen(path.string());
  ASSERT_TRUE(open);
  now += std::chrono::hours{1};
  std::optional<std::expected<CapabilityCell, FrontendDiagnostic>> result;
  for (int spin = 0; spin < 2000 && !result; ++spin) {
    result = host.openedCapability(*open);
    if (!result) {
      host.pump(16);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  std::error_code ec;
  std::filesystem::remove(path, ec);
  ASSERT_TRUE(result.has_value());
  // Either the worker's completion beat the stop request (a capability) or
  // the deadline won (a typed diagnostic naming TimedOut): never a hang.
  if (!*result)
    EXPECT_NE(result->error().message.find("TimedOut"), std::string::npos);
}

TEST(VqlIoHandleFileTest, ADetachedReadIsAwaitedAsTheSameRequest) {
  FileScriptWorld f;
  ASSERT_TRUE(f.open("src", f.write("a.txt", "hello world")));
  const auto rows =
      f.run("effect { let $s := submit($src, read_bytes(0, 5)); match $s { "
            "admitted($q) => $q, refused($e) => $e } } as $h; "
            "effect { await($h) } as $r; return io_payload($r)");
  ASSERT_TRUE(rows) << rows.error().message;
  ASSERT_EQ(rows->size(), 1U);
  EXPECT_EQ(f.text(rows->front()), "hello");
}

TEST(VqlIoHandleFileTest, CancelOfARealReadIsCancelledOrItsOwnCompletion) {
  // A small read may finish before the stop request lands; both are values,
  // and the script never fails.
  FileScriptWorld f;
  ASSERT_TRUE(f.open("src", f.write("a.txt", "hello world")));
  const auto rows =
      f.run("effect { let $s := submit($src, read_bytes(0, 5)); match $s { "
            "admitted($q) => $q, refused($e) => $e } } as $h; "
            "effect { cancel($h) } as $c; return io_tag($c)");
  ASSERT_TRUE(rows) << rows.error().message;
  ASSERT_EQ(rows->size(), 1U);
  const auto tag = f.text(rows->front());
  EXPECT_TRUE(tag == "Cancelled" || tag == "Complete") << tag;
}

TEST(VqlIoHandleFileTest, WithinThatIsNotReachedLeavesTheReadComplete) {
  FileScriptWorld f;
  ASSERT_TRUE(f.open("src", f.write("a.txt", "hello world")));
  const auto rows = f.run(
      "effect { let $s := submit($src, read_bytes(6, 5)) within 30 s; "
      "match $s { admitted($q) => await($q), refused($e) => $e } } as $r; "
      "return io_bytes($r)");
  ASSERT_TRUE(rows) << rows.error().message;
  ASSERT_EQ(rows->size(), 1U);
  EXPECT_EQ(f.text(rows->front()), "5");
}

TEST(VqlIoHandleFileTest, TheFieldFunctionsReadARealOutcome) {
  FileScriptWorld f;
  ASSERT_TRUE(f.open("src", f.write("a.txt", "hello world")));
  const auto rows =
      f.run("effect { let $s := submit($src, read_bytes(0, 5)); match $s { "
            "admitted($q) => await($q), refused($e) => $e } } as $r; "
            "return io_tag($r); return io_payload($r)");
  ASSERT_TRUE(rows) << rows.error().message;
  ASSERT_EQ(rows->size(), 1U);
  EXPECT_EQ(f.text(rows->front()), "hello");
}

} // namespace
