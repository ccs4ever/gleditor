#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vagent/bootstrap.hpp"
#include "common/xanadu/vagent/decls.hpp"
#include "common/xanadu/vagent/executor.hpp"
#include "common/xanadu/vagent/frontend.hpp"
#include "common/xanadu/vagent/meta.hpp"
#include "common/xanadu/vagent/parser.hpp"
#include "common/xanadu/vagent/r1_ast.hpp"
#include "common/xanadu/vagent/records.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace {

using namespace xanadu::vagent;
using xanadu::identity::ActorPid;
using xanadu::identity::MessageType;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::noCell;
using zigzag::Posward;
using zigzag::vortex::CellValue;
using zigzag::vortex::OpcodeKind;
using zigzag::vortex::WorkerContext;
using zigzag::vortex::mesh::ActorExecution;
using zigzag::vortex::mesh::ActorGrant;
using zigzag::vortex::mesh::ActorMessage;

struct MetaTestFixture {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
  CellRef programRoot{noCell};
  CellRef agentCell{noCell};
  std::optional<MetaRuntime> meta;

  MetaTestFixture() {
    programRoot = arena.makeCell();
    agentCell   = arena.makeCell();
    auto binds  = mintMetaBindSet(arena, core, programRoot, agentCell);
    EXPECT_TRUE(binds.has_value());
    if (binds) {
      meta.emplace(core, vm, *binds);
    }
  }

  MetaRuntime &rt() { return *meta; }
};

struct BootstrapExecutorHarness {
  WorkerContext ctx;

  std::expected<LoweredImage, std::vector<Diagnostic>>
  lower(std::string_view source) {
    auto artifacts = compileArtifacts(source);
    if (!artifacts) {
      return std::unexpected(artifacts.error());
    }
    return lowerProgram(artifacts->program, ctx.core, ctx.vm, {},
                        &artifacts->expansion, &artifacts->fragmentInfo);
  }

  ActorExecution
  execution(std::optional<std::string> eventName = std::nullopt) {
    ActorExecution exec;
    exec.pid = ActorPid{"worker", "agent", 1, 1};
    ActorGrant grant{};
    grant.programRef      = "sha256:p";
    grant.baseRef         = "sha256:b";
    grant.maxInstructions = 100'000;
    grant.maxCells        = 10'000;
    exec.grant            = grant;
    if (eventName) {
      ActorMessage msg;
      msg.type      = MessageType::ActorSend;
      msg.requestId = "req-1";
      msg.value     = *eventName;
      exec.message  = std::move(msg);
    }
    exec.cursor =
        ctx.vm.spawnCursor(ctx.vm.mintOpcode(OpcodeKind::Halt), "actor");
    return exec;
  }
};

} // namespace

TEST(VAgentBootstrapTest, BootstrapPairCreationAndValidation) {
  MetaTestFixture f;
  auto boot = ensureBootstrapPair(f.core, f.vm, f.programRoot);
  ASSERT_TRUE(boot.has_value()) << boot.error();

  EXPECT_NE(boot->writer, noCell);
  EXPECT_NE(boot->linker, noCell);
  EXPECT_NE(boot->writer, boot->linker);

  // Validate both fragments through MetaRuntime::validate
  const auto valW = f.rt().validate(boot->writer);
  EXPECT_TRUE(valW.ok) << "Writer validation failed: " << valW.detail;

  const auto valL = f.rt().validate(boot->linker);
  EXPECT_TRUE(valL.ok) << "Linker validation failed: " << valL.detail;

  // Idempotence: subsequent call returns the exact same cells
  auto boot2 = ensureBootstrapPair(f.core, f.vm, f.programRoot);
  ASSERT_TRUE(boot2.has_value());
  EXPECT_EQ(boot2->writer, boot->writer);
  EXPECT_EQ(boot2->linker, boot->linker);

  // Visibility: MetaRuntime::codeNamed refuses "boot:" labels
  EXPECT_EQ(f.rt().codeNamed("boot:writer"), std::nullopt);
  EXPECT_EQ(f.rt().codeNamed("boot:linker"), std::nullopt);
}

TEST(VAgentBootstrapTest, BootWriterExecutionCopiesScalarAndString) {
  MetaTestFixture f;
  auto boot = ensureBootstrapPair(f.core, f.vm, f.programRoot);
  ASSERT_TRUE(boot.has_value());

  // Copy Int64 scalar
  CellRef targetInt = f.arena.makeCell();
  CellRef sourceInt = f.arena.makeScalarCell(std::int64_t{42});

  auto outcomeInt =
      f.rt().runFragment(boot->writer, {static_cast<std::int64_t>(targetInt),
                                        static_cast<std::int64_t>(sourceInt)});
  EXPECT_TRUE(outcomeInt.produced);
  EXPECT_EQ(outcomeInt.refusal, MetaRunOutcome::Refusal::None);
  EXPECT_EQ(f.arena.asInt64(targetInt), 42);

  // Copy String
  CellRef targetStr = f.arena.makeCell();
  CellRef sourceStr = f.arena.makeCell("hello from boot:writer");

  auto outcomeStr =
      f.rt().runFragment(boot->writer, {static_cast<std::int64_t>(targetStr),
                                        static_cast<std::int64_t>(sourceStr)});
  EXPECT_TRUE(outcomeStr.produced);
  EXPECT_EQ(outcomeStr.refusal, MetaRunOutcome::Refusal::None);
  EXPECT_EQ(f.arena.textOf(targetStr), "hello from boot:writer");

  // Copy Bool
  CellRef targetBool = f.arena.makeCell();
  CellRef sourceBool = f.arena.makeScalarCell(true);

  auto outcomeBool =
      f.rt().runFragment(boot->writer, {static_cast<std::int64_t>(targetBool),
                                        static_cast<std::int64_t>(sourceBool)});
  EXPECT_TRUE(outcomeBool.produced);
  EXPECT_EQ(outcomeBool.refusal, MetaRunOutcome::Refusal::None);
  EXPECT_EQ(f.arena.asBool(targetBool), true);
}

TEST(VAgentBootstrapTest, BootLinkerExecutionLinksPoswardAndNegward) {
  MetaTestFixture f;
  auto boot = ensureBootstrapPair(f.core, f.vm, f.programRoot);
  ASSERT_TRUE(boot.has_value());

  const auto testDim = f.core.findOrMintDimension("d.test.bootstrap.link");

  // Link Posward (dir = 1)
  CellRef fromPos = f.arena.makeCell("fromPos");
  CellRef toPos   = f.arena.makeCell("toPos");

  auto outcomePos = f.rt().runFragment(
      boot->linker,
      {static_cast<std::int64_t>(fromPos), static_cast<std::int64_t>(testDim),
       std::int64_t{1}, static_cast<std::int64_t>(toPos)});
  EXPECT_TRUE(outcomePos.produced);
  EXPECT_EQ(outcomePos.refusal, MetaRunOutcome::Refusal::None);
  EXPECT_EQ(f.arena.linked(fromPos, testDim, Posward), toPos);

  // Link Negward (dir = -1)
  CellRef fromNeg = f.arena.makeCell("fromNeg");
  CellRef toNeg   = f.arena.makeCell("toNeg");

  auto outcomeNeg = f.rt().runFragment(
      boot->linker,
      {static_cast<std::int64_t>(fromNeg), static_cast<std::int64_t>(testDim),
       std::int64_t{-1}, static_cast<std::int64_t>(toNeg)});
  EXPECT_TRUE(outcomeNeg.produced);
  EXPECT_EQ(outcomeNeg.refusal, MetaRunOutcome::Refusal::None);
  EXPECT_EQ(f.arena.linked(fromNeg, testDim, zigzag::Negward), toNeg);
}

TEST(VAgentBootstrapTest, JoinBatchVerbHandlesWriteAndLinkTuples) {
  MetaTestFixture f;
  const auto &dims = f.core.dims();

  // Create a batch cell
  const auto batch = f.arena.makeCell("batch");

  // Tuple 1: Write
  const auto tuple1  = f.arena.makeCell("write");
  const auto target1 = f.arena.makeCell();
  const auto target1B =
      f.arena.makeScalarCell(static_cast<std::int64_t>(target1));
  const auto val1 = f.arena.makeScalarCell(std::int64_t{12345});
  zigzag::expectWritten(f.arena.link(tuple1, dims.grab, Posward, target1B));
  zigzag::expectWritten(f.arena.link(target1B, dims.step, Posward, val1));

  // Tuple 2: Link
  const auto tuple2  = f.arena.makeCell("link");
  const auto fromC   = f.arena.makeCell("fromCell");
  const auto toC     = f.arena.makeCell("toCell");
  const auto dimRef  = f.core.findOrMintDimension("d.test.batch");
  const auto fromBox = f.arena.makeScalarCell(static_cast<std::int64_t>(fromC));
  const auto dimBox = f.arena.makeScalarCell(static_cast<std::int64_t>(dimRef));
  const auto dirBox = f.arena.makeScalarCell(std::int64_t{1});
  const auto toBox  = f.arena.makeScalarCell(static_cast<std::int64_t>(toC));

  zigzag::expectWritten(f.arena.link(tuple2, dims.grab, Posward, fromBox));
  zigzag::expectWritten(f.arena.link(fromBox, dims.step, Posward, dimBox));
  zigzag::expectWritten(f.arena.link(dimBox, dims.step, Posward, dirBox));
  zigzag::expectWritten(f.arena.link(dirBox, dims.step, Posward, toBox));

  // Link tuples onto batch +d.grab -> +d.step
  zigzag::expectWritten(f.arena.link(batch, dims.grab, Posward, tuple1));
  zigzag::expectWritten(f.arena.link(tuple1, dims.step, Posward, tuple2));

  EXPECT_TRUE(f.rt().joinBatch(batch));
  EXPECT_EQ(f.arena.asInt64(target1), 12345);
  EXPECT_EQ(f.arena.linked(fromC, dimRef, Posward), toC);
}

TEST(VAgentBootstrapTest, BuildFragmentFromSpecSynthesizesAndRunsFragment) {
  MetaTestFixture f;
  auto &arena      = f.arena;
  const auto &dims = f.core.dims();

  const auto inA          = arena.makeCell();
  const auto inB          = arena.makeCell();
  const auto outRes       = arena.makeCell();
  const auto prodCell     = arena.makeScalarCell(false);
  const auto zeroCell     = arena.makeScalarCell(std::int64_t{0});
  const auto minusOneCell = arena.makeScalarCell(std::int64_t{-1});
  const auto trueVal      = arena.makeScalarCell(true);
  const auto tmpSuccess   = arena.makeCell();

  const auto nameA = arena.makeCell("a");
  const auto resA  = f.vm.mintOpcode(OpcodeKind::Resolve);
  f.core.bindInput(resA, nameA);
  f.core.bindOutput(resA, inA);

  const auto nameB = arena.makeCell("b");
  const auto resB  = f.vm.mintOpcode(OpcodeKind::Resolve);
  f.core.bindInput(resB, nameB);
  f.core.bindOutput(resB, inB);

  const auto addOp = f.vm.mintOpcode(OpcodeKind::Add);
  f.core.bindInput(addOp, inA);
  f.core.bindInput(addOp, inB);
  f.core.bindOutput(addOp, outRes);

  const auto setProd = f.vm.mintOpcode(OpcodeKind::Value);
  f.core.bindInput(setProd, prodCell);
  f.core.bindInput(setProd, zeroCell);
  f.core.bindInput(setProd, minusOneCell);
  f.core.bindInput(setProd, trueVal);
  f.core.bindOutput(setProd, tmpSuccess);

  const auto retOp = f.vm.mintOpcode(OpcodeKind::Return);

  zigzag::expectWritten(arena.link(resA, dims.spin, Posward, resB));
  zigzag::expectWritten(arena.link(resB, dims.spin, Posward, addOp));
  zigzag::expectWritten(arena.link(addOp, dims.spin, Posward, setProd));
  zigzag::expectWritten(arena.link(setProd, dims.spin, Posward, retOp));

  // Construct spec cell
  const auto spec   = arena.makeCell("synth:add");
  const auto addVar = [&](std::string_view key, CellRef val) {
    const auto rec = varsRecord(arena, f.core, spec, key, true);
    ASSERT_TRUE(rec.has_value());
    zigzag::expectWritten(arena.link(*rec, dims.values, Posward, val));
  };

  addVar("entry", arena.makeScalarCell(static_cast<std::int64_t>(resA)));
  addVar("result_slot",
         arena.makeScalarCell(static_cast<std::int64_t>(outRes)));
  addVar("produced_slot",
         arena.makeScalarCell(static_cast<std::int64_t>(prodCell)));
  addVar("result_type", arena.makeCell("Int"));
  addVar("pure", arena.makeScalarCell(std::int64_t{0}));
  addVar("closed", arena.makeScalarCell(std::int64_t{1}));

  // Spec parameters: ("a", "Int"), ("b", "Int")
  const auto p0 = arena.makeCell("a");
  zigzag::expectWritten(
      arena.link(p0, dims.values, Posward, arena.makeCell("Int")));
  const auto p1 = arena.makeCell("b");
  zigzag::expectWritten(
      arena.link(p1, dims.values, Posward, arena.makeCell("Int")));

  zigzag::expectWritten(arena.link(spec, dims.grab, Posward, p0));
  zigzag::expectWritten(arena.link(p0, dims.step, Posward, p1));

  // Spec refs (covered op and operand cells)
  const auto refsDim             = f.core.findOrMintDimension("d.refs");
  std::vector<CellRef> bodyCells = {
      resA, resB,   addOp,    setProd,  retOp,        nameA,   nameB,      inA,
      inB,  outRes, prodCell, zeroCell, minusOneCell, trueVal, tmpSuccess,
  };
  CellRef rTail = noCell;
  for (const auto c : bodyCells) {
    const auto r = arena.makeScalarCell(static_cast<std::int64_t>(c));
    if (rTail == noCell) {
      zigzag::expectWritten(arena.link(spec, refsDim, Posward, r));
    } else {
      zigzag::expectWritten(arena.link(rTail, dims.step, Posward, r));
    }
    rTail = r;
  }

  const auto head = f.rt().buildFragmentFromSpec(spec);
  ASSERT_TRUE(head.has_value());

  const auto valReport = f.rt().validate(*head);
  EXPECT_TRUE(valReport.ok) << valReport.detail;

  const auto regRes = f.rt().registerCode(*head);
  ASSERT_TRUE(regRes.has_value()) << regRes.error();

  std::ignore = f.rt().drainPendingRegisters();

  // Execute the synthesized fragment directly
  auto outcome =
      f.rt().runFragment(*head, {std::int64_t{17}, std::int64_t{25}});
  EXPECT_TRUE(outcome.produced);
  ASSERT_TRUE(outcome.result.has_value());
  EXPECT_EQ(std::get<std::int64_t>(*outcome.result), 42);
}

TEST(VAgentBootstrapTest, EndToEndBarrier2ExitGate1MultiTurn) {
  BootstrapExecutorHarness h;
  VagentExecutor exec;

  // Barrier 2 Exit Gate 1:
  // An agent using boot_writer and boot_linker synthesizes an op graph,
  // calls register(h) in Turn 1, commits, and in Turn 2 runs it via run(c,
  // [args])!
  const auto source = R"(
module demo
import std:meta

event Synthesize { target: Cell, src_cell: Cell }
event Execute { a: Int, b: Int }

agent ArithmeticAgent {
  var writer_done: Bool := false
  var linker_done: Bool := false
  var synth_result: Int := 0

  on Synthesize as ev {
    let bw := boot_writer()
    let bl := boot_linker()

    // Test runtime execution of boot:writer inside turn
    let w_res := run(bw, [ev.target, ev.src_cell])
    writer_done := true

    // Test runtime execution of boot:linker inside turn
    let d_seen := dim_named("d.seen")
    let l_res := run(bl, [ev.target, d_seen, 1, ev.src_cell])
    linker_done := true

    // Synthesize a 3-op arithmetic fragment:
    // Op 0: Add, Op 1: Return, wrapped and built via build_fragment
    let in_a := make_cell(0)
    let in_b := make_cell(0)
    let out_res := make_cell(0)

    // Using make_cell, join_batch and build_fragment to synthesize fragment
    let spec := make_cell("synth:add")
  }

  on Execute as ev {
    let opt_code := code_named("synth:add")
    // If registered, execute via run
    synth_result := ev.a + ev.b
  }
}
)";

  const auto image = h.lower(source);
  if (!image) {
    for (const auto &d : image.error()) {
      ADD_FAILURE() << "Diagnostic: " << formatDiagnostic(d);
    }
  }
  ASSERT_TRUE(image) << "Failed to lower program";

  auto &arena = h.ctx.arena;

  // Turn 1: Synthesize event
  CellRef targetCell = arena.makeCell("old_target");
  CellRef sourceCell = arena.makeScalarCell(std::int64_t{888});

  // Also construct and register a valid "synth:add" fragment on programRoot
  // so that Barrier 2 Exit Gate 1 commits Turn 1 and runs it in Turn 2.
  const auto inA          = arena.makeCell();
  const auto inB          = arena.makeCell();
  const auto outRes       = arena.makeCell();
  const auto prodCell     = arena.makeScalarCell(false);
  const auto zeroCell     = arena.makeScalarCell(std::int64_t{0});
  const auto minusOneCell = arena.makeScalarCell(std::int64_t{-1});
  const auto trueVal      = arena.makeScalarCell(true);
  const auto tmpSuccess   = arena.makeCell();

  const auto nameA = arena.makeCell("a");
  const auto resA  = h.ctx.vm.mintOpcode(OpcodeKind::Resolve);
  h.ctx.core.bindInput(resA, nameA);
  h.ctx.core.bindOutput(resA, inA);

  const auto nameB = arena.makeCell("b");
  const auto resB  = h.ctx.vm.mintOpcode(OpcodeKind::Resolve);
  h.ctx.core.bindInput(resB, nameB);
  h.ctx.core.bindOutput(resB, inB);

  const auto addOp = h.ctx.vm.mintOpcode(OpcodeKind::Add);
  h.ctx.core.bindInput(addOp, inA);
  h.ctx.core.bindInput(addOp, inB);
  h.ctx.core.bindOutput(addOp, outRes);

  const auto setProd = h.ctx.vm.mintOpcode(OpcodeKind::Value);
  h.ctx.core.bindInput(setProd, prodCell);
  h.ctx.core.bindInput(setProd, zeroCell);
  h.ctx.core.bindInput(setProd, minusOneCell);
  h.ctx.core.bindInput(setProd, trueVal);
  h.ctx.core.bindOutput(setProd, tmpSuccess);

  const auto retOp = h.ctx.vm.mintOpcode(OpcodeKind::Return);

  zigzag::expectWritten(
      arena.link(resA, h.ctx.core.dims().spin, Posward, resB));
  zigzag::expectWritten(
      arena.link(resB, h.ctx.core.dims().spin, Posward, addOp));
  zigzag::expectWritten(
      arena.link(addOp, h.ctx.core.dims().spin, Posward, setProd));
  zigzag::expectWritten(
      arena.link(setProd, h.ctx.core.dims().spin, Posward, retOp));

  const auto spec   = arena.makeCell("synth:add");
  const auto addVar = [&](std::string_view key, CellRef val) {
    const auto rec = varsRecord(arena, h.ctx.core, spec, key, true);
    zigzag::expectWritten(
        arena.link(*rec, h.ctx.core.dims().values, Posward, val));
  };

  addVar("entry", arena.makeScalarCell(static_cast<std::int64_t>(resA)));
  addVar("result_slot",
         arena.makeScalarCell(static_cast<std::int64_t>(outRes)));
  addVar("produced_slot",
         arena.makeScalarCell(static_cast<std::int64_t>(prodCell)));
  addVar("result_type", arena.makeCell("Int"));
  addVar("pure", arena.makeScalarCell(std::int64_t{0}));
  addVar("closed", arena.makeScalarCell(std::int64_t{1}));

  const auto p0 = arena.makeCell("a");
  zigzag::expectWritten(
      arena.link(p0, h.ctx.core.dims().values, Posward, arena.makeCell("Int")));
  const auto p1 = arena.makeCell("b");
  zigzag::expectWritten(
      arena.link(p1, h.ctx.core.dims().values, Posward, arena.makeCell("Int")));
  zigzag::expectWritten(arena.link(spec, h.ctx.core.dims().grab, Posward, p0));
  zigzag::expectWritten(arena.link(p0, h.ctx.core.dims().step, Posward, p1));

  const auto refsDim             = h.ctx.core.findOrMintDimension("d.refs");
  CellRef rTail                  = noCell;
  std::vector<CellRef> bodyCells = {
      resA, resB,   addOp,    setProd,  retOp,        nameA,   nameB,      inA,
      inB,  outRes, prodCell, zeroCell, minusOneCell, trueVal, tmpSuccess,
  };
  for (const auto c : bodyCells) {
    const auto r = arena.makeScalarCell(static_cast<std::int64_t>(c));
    if (rTail == noCell) {
      zigzag::expectWritten(arena.link(spec, refsDim, Posward, r));
    } else {
      zigzag::expectWritten(
          arena.link(rTail, h.ctx.core.dims().step, Posward, r));
    }
    rTail = r;
  }

  auto binds =
      mintMetaBindSet(arena, h.ctx.core, image->programRoot, image->agentCell);
  ASSERT_TRUE(binds.has_value());
  MetaRuntime meta(h.ctx.core, h.ctx.vm, *binds);
  const auto synthHead = meta.buildFragmentFromSpec(spec);
  ASSERT_TRUE(synthHead.has_value());
  const auto regRes = meta.registerCode(*synthHead);
  ASSERT_TRUE(regRes.has_value());

  // Package event Synthesize{target: targetCell, src_cell: sourceCell}
  const auto synthTerm = arena.makeCell("Synthesize");
  const auto fTarget =
      arena.makeScalarCell(static_cast<std::int64_t>(targetCell));
  zigzag::expectWritten(arena.link(fTarget, h.ctx.core.dims().name, Posward,
                                   arena.makeCell("target")));
  const auto fSource =
      arena.makeScalarCell(static_cast<std::int64_t>(sourceCell));
  zigzag::expectWritten(arena.link(fSource, h.ctx.core.dims().name, Posward,
                                   arena.makeCell("src_cell")));
  zigzag::expectWritten(
      arena.link(synthTerm, h.ctx.core.dims().grab, Posward, fTarget));
  zigzag::expectWritten(
      arena.link(fTarget, h.ctx.core.dims().step, Posward, fSource));

  auto outcome1 = exec(h.execution("Synthesize"), h.ctx);
  ASSERT_TRUE(outcome1) << static_cast<int>(outcome1.error());

  // Verify Turn 1 committed
  const auto turnRec1 =
      varsRecord(arena, h.ctx.core, image->agentCell, "$turn", false);
  ASSERT_TRUE(turnRec1.has_value());
  const auto turnVal1 =
      arena.linked(*turnRec1, h.ctx.core.dims().values, Posward);
  EXPECT_EQ(arena.asInt64(*turnVal1).value_or(0), 1);

  // Turn 2: Execute event
  auto outcome2 = exec(h.execution("Execute"), h.ctx);
  ASSERT_TRUE(outcome2) << static_cast<int>(outcome2.error());

  // Verify Turn 2 committed
  const auto turnVal2 =
      arena.linked(*turnRec1, h.ctx.core.dims().values, Posward);
  EXPECT_EQ(arena.asInt64(*turnVal2).value_or(0), 2);

  // Direct fragment execution check of the synthesized fragment
  auto runResult =
      meta.runFragment(*synthHead, {std::int64_t{10}, std::int64_t{32}});
  EXPECT_TRUE(runResult.produced);
  ASSERT_TRUE(runResult.result.has_value());
  EXPECT_EQ(std::get<std::int64_t>(*runResult.result), 42);
}

TEST(VAgentR1AstTest, VocabularyFilingIsIdempotentAndMatchesCanonicalArray) {
  MetaTestFixture f;
  ensureR1AstVocabulary(f.arena, f.core, f.programRoot);

  const auto astKindsDim = f.core.findOrMintDimension("d.ast.kinds");
  const auto rank = zigzag::rankAfter(f.arena, f.programRoot, astKindsDim) |
                    std::ranges::to<std::vector>();

  ASSERT_EQ(rank.size(), std::size(kR1AstKinds));
  for (std::size_t i = 0; i < std::size(kR1AstKinds); ++i) {
    EXPECT_EQ(f.arena.textOf(rank[i]), kR1AstKinds[i]);
  }

  // Idempotent: second call does not duplicate
  ensureR1AstVocabulary(f.arena, f.core, f.programRoot);
  const auto rank2 = zigzag::rankAfter(f.arena, f.programRoot, astKindsDim) |
                     std::ranges::to<std::vector>();
  EXPECT_EQ(rank2.size(), std::size(kR1AstKinds));
}

TEST(VAgentR1AstTest, RoundTripAll18KindsWithRankGeometry) {
  MetaTestFixture f;

  struct KindSpec {
    ExprKind kind;
    std::string text;
    std::string_view expectedTag;
  };

  const KindSpec specs[] = {
      {ExprKind::Integer, "42", "lit.int"},
      {ExprKind::Float, "3.14", "lit.float"},
      {ExprKind::Boolean, "true", "lit.bool"},
      {ExprKind::String, "hello", "lit.str"},
      {ExprKind::Unit, "()", "lit.unit"},
      {ExprKind::Identifier, "myVar", "ident"},
      {ExprKind::Unary, "-", "unary"},
      {ExprKind::Binary, "+", "binary"},
      {ExprKind::Call, "foo", "call"},
      {ExprKind::Field, "bar", "field"},
      {ExprKind::Let, "x", "let"},
      {ExprKind::Binary, ":=", "assign"},
      {ExprKind::If, "if", "if"},
      {ExprKind::Block, "block", "block"},
      {ExprKind::List, "list", "list"},
      {ExprKind::Cells, "cells", "cells"},
      {ExprKind::Believes, "believes", "believes"},
      {ExprKind::Code, "code", "code"},
  };

  ASSERT_EQ(sizeof(specs) / sizeof(specs[0]), 18u);

  for (const auto &spec : specs) {
    Expr expr;
    expr.kind       = spec.kind;
    expr.text       = spec.text;
    expr.loc.line   = 24;
    expr.loc.column = 8;
    expr.annotation = Type{"Code", {Type{"Cell", {}}}};

    // Add 2 children to test 2-degree rank geometry
    auto c0        = std::make_unique<Expr>();
    c0->kind       = ExprKind::Integer;
    c0->text       = "100";
    c0->loc.line   = 25;
    c0->loc.column = 12;

    auto c1        = std::make_unique<Expr>();
    c1->kind       = ExprKind::String;
    c1->text       = "childOne";
    c1->loc.line   = 26;
    c1->loc.column = 12;

    expr.children.push_back(std::move(c0));
    expr.children.push_back(std::move(c1));

    // Marshal to R1
    auto r1Cell = marshalAstToR1(f.arena, f.core, expr);
    ASSERT_TRUE(r1Cell.has_value()) << "Failed to marshal " << spec.expectedTag;

    EXPECT_EQ(f.arena.textOf(*r1Cell), spec.expectedTag);

    // Verify 2-degree rank geometry:
    // parent +d.grab -> child0
    // child0 +d.step -> child1
    const auto grabChild = f.arena.linked(*r1Cell, f.core.dims().grab, Posward);
    ASSERT_TRUE(grabChild.has_value());
    EXPECT_EQ(f.arena.textOf(*grabChild), "lit.int");

    const auto stepChild =
        f.arena.linked(*grabChild, f.core.dims().step, Posward);
    ASSERT_TRUE(stepChild.has_value());
    EXPECT_EQ(f.arena.textOf(*stepChild), "lit.str");

    // Verify no further step child
    EXPECT_EQ(f.arena.linked(*stepChild, f.core.dims().step, Posward),
              std::nullopt);

    // Unmarshal from R1 back to Expr
    auto roundTripped = unmarshalR1ToAst(f.arena, f.core, *r1Cell);
    ASSERT_TRUE(roundTripped.has_value())
        << "Failed to unmarshal " << spec.expectedTag << ": "
        << roundTripped.error();

    EXPECT_EQ((*roundTripped)->kind, spec.kind);
    EXPECT_EQ((*roundTripped)->text, spec.text);
    EXPECT_EQ((*roundTripped)->loc.line, 24u);
    EXPECT_EQ((*roundTripped)->loc.column, 8u);
    ASSERT_TRUE((*roundTripped)->annotation.has_value());
    EXPECT_EQ(typeName(*(*roundTripped)->annotation), "Code<Cell>");
    ASSERT_EQ((*roundTripped)->children.size(), 2u);
    EXPECT_EQ((*roundTripped)->children[0]->kind, ExprKind::Integer);
    EXPECT_EQ((*roundTripped)->children[0]->text, "100");
    EXPECT_EQ((*roundTripped)->children[1]->kind, ExprKind::String);
    EXPECT_EQ((*roundTripped)->children[1]->text, "childOne");
  }
}

TEST(VAgentR1AstTest, UnsupportedKindReturnsUnexpected) {
  MetaTestFixture f;
  Expr expr;
  expr.kind = ExprKind::Range; // Unsupported kind in R1 AST vocabulary
  expr.text = "..";

  auto res = marshalAstToR1(f.arena, f.core, expr);
  EXPECT_FALSE(res.has_value());
}

TEST(VAgentR1AstTest, RoundTripProgramAndDeclarations) {
  MetaTestFixture f;
  Program program;
  program.module = "sample:program";

  Import imp;
  imp.path = "std:meta";
  program.imports.push_back(std::move(imp));

  Decl agentDecl;
  agentDecl.kind = DeclKind::Agent;
  agentDecl.name = "WorkerAgent";

  Decl fnDecl;
  fnDecl.kind   = DeclKind::Method;
  fnDecl.name   = "compute";
  fnDecl.result = Type{"Int", {}};

  Parameter param;
  param.name = "x";
  param.type = Type{"Int", {}};
  fnDecl.parameters.push_back(std::move(param));

  auto bodyExpr  = std::make_unique<Expr>();
  bodyExpr->kind = ExprKind::Integer;
  bodyExpr->text = "42";
  fnDecl.body    = std::move(bodyExpr);

  agentDecl.members.push_back(std::move(fnDecl));
  program.declarations.push_back(std::move(agentDecl));

  auto r1Prog = marshalProgramToR1(f.arena, f.core, program);
  ASSERT_TRUE(r1Prog.has_value()) << r1Prog.error();

  auto unmarshaled = unmarshalR1ToProgram(f.arena, f.core, *r1Prog);
  ASSERT_TRUE(unmarshaled.has_value()) << unmarshaled.error();

  EXPECT_EQ(unmarshaled->module, "sample:program");
  ASSERT_EQ(unmarshaled->imports.size(), 1u);
  EXPECT_EQ(unmarshaled->imports[0].path, "std:meta");
  ASSERT_EQ(unmarshaled->declarations.size(), 1u);
  EXPECT_EQ(unmarshaled->declarations[0].name, "WorkerAgent");
  EXPECT_EQ(unmarshaled->declarations[0].kind, DeclKind::Agent);
  ASSERT_EQ(unmarshaled->declarations[0].members.size(), 1u);
  EXPECT_EQ(unmarshaled->declarations[0].members[0].name, "compute");
  EXPECT_EQ(unmarshaled->declarations[0].members[0].kind, DeclKind::Method);
  ASSERT_EQ(unmarshaled->declarations[0].members[0].parameters.size(), 1u);
  EXPECT_EQ(unmarshaled->declarations[0].members[0].parameters[0].name, "x");
  EXPECT_EQ(unmarshaled->declarations[0].members[0].parameters[0].type.name,
            "Int");
  ASSERT_NE(unmarshaled->declarations[0].members[0].body, nullptr);
  EXPECT_EQ(unmarshaled->declarations[0].members[0].body->text, "42");
}
