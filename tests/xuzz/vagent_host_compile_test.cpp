#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vagent/compiler.hpp"
#include "common/xanadu/vagent/decls.hpp"
#include "common/xanadu/vagent/executor.hpp"
#include "common/xanadu/vagent/frontend.hpp"
#include "common/xanadu/vagent/host_compile.hpp"
#include "common/xanadu/vagent/meta.hpp"
#include "common/xanadu/vagent/records.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace {

using namespace xanadu::vagent;
using xanadu::identity::ActorPid;
using xanadu::identity::MessageType;
using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::vortex::CellValue;
using zigzag::vortex::OpcodeKind;
using zigzag::vortex::WorkerContext;
using zigzag::vortex::mesh::ActorExecution;
using zigzag::vortex::mesh::ActorGrant;
using zigzag::vortex::mesh::ActorMessage;

struct MetaCompileHarness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
  CellRef programRoot{zigzag::noCell};
  CellRef agentCell{zigzag::noCell};
  std::optional<MetaRuntime> meta;

  MetaCompileHarness() {
    programRoot = arena.makeCell();
    agentCell   = arena.makeCell();
    auto binds  = mintMetaBindSet(arena, core, programRoot, agentCell);
    EXPECT_TRUE(binds.has_value());
    if (binds) meta.emplace(core, vm, *binds);
  }

  MetaRuntime &rt() { return *meta; }
};

struct ExecutorHarness {
  WorkerContext ctx;
  CellRef programRoot{zigzag::noCell};
  std::optional<MetaRuntime> meta;

  std::expected<LoweredImage, std::vector<Diagnostic>>
  lower(std::string_view source) {
    auto artifacts = compileArtifacts(source);
    if (!artifacts) {
      return std::unexpected(artifacts.error());
    }
    auto image = lowerProgram(artifacts->program, ctx.core, ctx.vm, {},
                              &artifacts->expansion, &artifacts->fragmentInfo);
    if (image) {
      programRoot = image->programRoot;
      auto binds =
          mintMetaBindSet(ctx.arena, ctx.core, programRoot, image->agentCell);
      if (binds) meta.emplace(ctx.core, ctx.vm, *binds);
    }
    return image;
  }

  ActorExecution execution(std::optional<std::string> event = std::nullopt,
                           std::uint64_t spawnSequence      = 1) {
    ActorExecution exec;
    exec.pid   = ActorPid{"worker", "agent", 1, spawnSequence};
    exec.grant = ActorGrant{.programRef      = "sha256:p",
                            .baseRef         = "sha256:b",
                            .maxInstructions = 100'000,
                            .maxCells        = 10'000};
    if (event) {
      ActorMessage msg;
      msg.type      = MessageType::ActorSend;
      msg.requestId = "req-1";
      msg.value     = *event;
      exec.message  = std::move(msg);
    }
    exec.cursor =
        ctx.vm.spawnCursor(ctx.vm.mintOpcode(OpcodeKind::Halt), "actor");
    return exec;
  }
};

// ---------------------------------------------------------------------------
// 1. Direct compilation of pure arithmetic expressions
// ---------------------------------------------------------------------------

TEST(VagentHostCompileTest, PureArithmeticWithGrabParameters) {
  MetaCompileHarness h;

  // Case A: Parameter chain on +d.grab -> +d.step (x: Int, y: Int)
  const auto paramsCell = h.arena.makeCell();
  const auto px         = h.arena.makeCell("x");
  const auto tx         = h.arena.makeCell("Int");
  zigzag::expectWritten(
      h.arena.link(px, h.core.dims().values, DimVector::POS, tx));

  const auto py = h.arena.makeCell("y");
  const auto ty = h.arena.makeCell("Int");
  zigzag::expectWritten(
      h.arena.link(py, h.core.dims().values, DimVector::POS, ty));

  zigzag::expectWritten(
      h.arena.link(px, h.core.dims().step, DimVector::POS, py));
  zigzag::expectWritten(
      h.arena.link(paramsCell, h.core.dims().grab, DimVector::POS, px));

  const auto srcCell = h.arena.makeCell("x + y * 2");

  const auto head = hostCompileFragment(h.rt(), h.core, h.vm, "custom:math",
                                        paramsCell, "Int", srcCell);
  if (!head) {
    if (const auto rec = varsRecord(h.arena, h.core, h.rt().agentCell(),
                                    "fault:custom:math", false)) {
      if (const auto why =
              h.arena.linked(*rec, h.core.dims().values, DimVector::POS)) {
        std::cerr << "RECORDED FAULT NOW: " << h.arena.textOf(*why)
                  << std::endl;
      }
    }
  }
  ASSERT_TRUE(head.has_value());

  // Marks == 0 so registration is immediately committed
  EXPECT_EQ(h.rt().registration(*head), "committed");

  const auto report = h.rt().validate(*head);
  EXPECT_TRUE(report.ok) << report.detail;

  EXPECT_EQ(h.rt().resultTypeOf(*head), "Int");

  // Execute: x = 10, y = 16 -> 10 + 16 * 2 = 42
  const auto outcome = h.rt().runFragment(
      *head, {CellValue(std::int64_t{10}), CellValue(std::int64_t{16})});
  EXPECT_EQ(outcome.refusal, MetaRunOutcome::Refusal::None);
  EXPECT_TRUE(outcome.produced);
  EXPECT_EQ(outcome.result, CellValue(std::int64_t{42}));
}

TEST(VagentHostCompileTest, PureArithmeticWithListParameters) {
  MetaCompileHarness h;

  // Case B: List cell on +d.step (["x:Int", "y:Int"])
  const auto paramsCell = h.arena.makeCell("List");
  const auto elemX      = h.arena.makeCell("x:Int");
  const auto elemY      = h.arena.makeCell("y:Int");

  zigzag::expectWritten(
      h.arena.link(paramsCell, h.core.dims().step, DimVector::POS, elemX));
  zigzag::expectWritten(
      h.arena.link(elemX, h.core.dims().step, DimVector::POS, elemY));

  const auto srcCell = h.arena.makeCell("x + y * 2");

  const auto head = hostCompileFragment(
      h.rt(), h.core, h.vm, "custom:math_list", paramsCell, "Int", srcCell);
  ASSERT_TRUE(head.has_value());
  EXPECT_EQ(h.rt().registration(*head), "committed");
  EXPECT_TRUE(h.rt().validate(*head).ok);

  const auto outcome = h.rt().runFragment(
      *head, {CellValue(std::int64_t{10}), CellValue(std::int64_t{16})});
  EXPECT_EQ(outcome.refusal, MetaRunOutcome::Refusal::None);
  EXPECT_TRUE(outcome.produced);
  EXPECT_EQ(outcome.result, CellValue(std::int64_t{42}));
}

TEST(VagentHostCompileTest, MultipleParametersThreeArgs) {
  MetaCompileHarness h;

  const auto paramsCell = h.arena.makeCell("List");
  const auto e1         = h.arena.makeCell("a:Int");
  const auto e2         = h.arena.makeCell("b:Int");
  const auto e3         = h.arena.makeCell("c:Int");
  zigzag::expectWritten(
      h.arena.link(paramsCell, h.core.dims().step, DimVector::POS, e1));
  zigzag::expectWritten(
      h.arena.link(e1, h.core.dims().step, DimVector::POS, e2));
  zigzag::expectWritten(
      h.arena.link(e2, h.core.dims().step, DimVector::POS, e3));

  const auto srcCell = h.arena.makeCell("a + b * c");
  const auto head    = hostCompileFragment(h.rt(), h.core, h.vm, "custom:poly",
                                           paramsCell, "Int", srcCell);
  ASSERT_TRUE(head.has_value());

  // a = 2, b = 3, c = 4 -> 2 + 3 * 4 = 14
  const auto outcome = h.rt().runFragment(*head, {CellValue(std::int64_t{2}),
                                                  CellValue(std::int64_t{3}),
                                                  CellValue(std::int64_t{4})});
  EXPECT_TRUE(outcome.produced);
  EXPECT_EQ(outcome.result, CellValue(std::int64_t{14}));
}

// ---------------------------------------------------------------------------
// 2. Refusals and diagnostic fault records
// ---------------------------------------------------------------------------

TEST(VagentHostCompileTest, RejectsReservedLabelsAndRecordsFault) {
  MetaCompileHarness h;
  const auto srcCell = h.arena.makeCell("42");

  // Reserved label prefixes: "boot:", "decl:", "patch:"
  const auto headBoot = hostCompileFragment(h.rt(), h.core, h.vm, "boot:bad",
                                            zigzag::noCell, "Int", srcCell);
  EXPECT_FALSE(headBoot.has_value());
  const auto faultBoot = h.rt().varOf(h.agentCell, "fault:boot:bad");
  ASSERT_TRUE(faultBoot.has_value());
  EXPECT_EQ(h.arena.textOf(*faultBoot), "reserved label prefix");

  const auto headDecl = hostCompileFragment(h.rt(), h.core, h.vm, "decl:bad",
                                            zigzag::noCell, "Int", srcCell);
  EXPECT_FALSE(headDecl.has_value());
  const auto faultDecl = h.rt().varOf(h.agentCell, "fault:decl:bad");
  ASSERT_TRUE(faultDecl.has_value());
  EXPECT_EQ(h.arena.textOf(*faultDecl), "reserved label prefix");

  const auto headPatch = hostCompileFragment(h.rt(), h.core, h.vm, "patch:bad",
                                             zigzag::noCell, "Int", srcCell);
  EXPECT_FALSE(headPatch.has_value());
  const auto faultPatch = h.rt().varOf(h.agentCell, "fault:patch:bad");
  ASSERT_TRUE(faultPatch.has_value());
  EXPECT_EQ(h.arena.textOf(*faultPatch), "reserved label prefix");

  // Empty label returns nullopt without writing a fault record
  const auto headEmpty = hostCompileFragment(h.rt(), h.core, h.vm, "",
                                             zigzag::noCell, "Int", srcCell);
  EXPECT_FALSE(headEmpty.has_value());
}

TEST(VagentHostCompileTest, RejectsSyntaxErrorsAndRecordsFault) {
  MetaCompileHarness h;
  const auto srcCell = h.arena.makeCell("x + * 2");

  const auto head = hostCompileFragment(h.rt(), h.core, h.vm, "custom:syntax",
                                        zigzag::noCell, "Int", srcCell);
  EXPECT_FALSE(head.has_value());
  const auto fault = h.rt().varOf(h.agentCell, "fault:custom:syntax");
  ASSERT_TRUE(fault.has_value());
  EXPECT_FALSE(h.arena.textOf(*fault).empty());
}

TEST(VagentHostCompileTest, RejectsTypeMismatchAndRecordsFault) {
  MetaCompileHarness h;
  // Body produces Str ("wrong"), but resultType requests Int
  const auto srcCell = h.arena.makeCell("\"wrong\"");

  const auto head = hostCompileFragment(h.rt(), h.core, h.vm, "custom:type_err",
                                        zigzag::noCell, "Int", srcCell);
  EXPECT_FALSE(head.has_value());
  const auto fault = h.rt().varOf(h.agentCell, "fault:custom:type_err");
  ASSERT_TRUE(fault.has_value());
  EXPECT_NE(h.arena.textOf(*fault).find("expected Int, got Str"),
            std::string::npos);
}

TEST(VagentHostCompileTest, RejectsEmptyResultOrMissingSource) {
  MetaCompileHarness h;
  const auto srcCell = h.arena.makeCell("42");

  const auto headEmptyRes = hostCompileFragment(
      h.rt(), h.core, h.vm, "custom:no_res", zigzag::noCell, "", srcCell);
  EXPECT_FALSE(headEmptyRes.has_value());
  const auto faultRes = h.rt().varOf(h.agentCell, "fault:custom:no_res");
  ASSERT_TRUE(faultRes.has_value());
  EXPECT_EQ(h.arena.textOf(*faultRes), "missing result type");

  const auto headBadSrc =
      hostCompileFragment(h.rt(), h.core, h.vm, "custom:no_src", zigzag::noCell,
                          "Int", zigzag::noCell);
  EXPECT_FALSE(headBadSrc.has_value());
  const auto faultSrc = h.rt().varOf(h.agentCell, "fault:custom:no_src");
  ASSERT_TRUE(faultSrc.has_value());
  EXPECT_EQ(h.arena.textOf(*faultSrc), "invalid source cell");
}

// ---------------------------------------------------------------------------
// 3. Fuel billing and consumption
// ---------------------------------------------------------------------------

TEST(VagentHostCompileTest, FuelBudgetExhaustionThrows) {
  MetaCompileHarness h;
  const auto srcCell = h.arena.makeCell("1 + 2 + 3 + 4 + 5 + 6");

  // Budget of 2 work units is far smaller than the length of srcCell text
  h.arena.setExecutionLimits(10'000, 2, 10'000);

  EXPECT_THROW((void)hostCompileFragment(h.rt(), h.core, h.vm,
                                         "custom:fuel_test", zigzag::noCell,
                                         "Int", srcCell),
               std::runtime_error);
}

// ---------------------------------------------------------------------------
// 4. as_body narrowing
// ---------------------------------------------------------------------------

TEST(VagentHostCompileTest, AsBodyNarrowsUnitNullaryFragment) {
  MetaCompileHarness h;

  const auto unitSrc  = h.arena.makeCell("()");
  const auto headUnit = hostCompileFragment(
      h.rt(), h.core, h.vm, "custom:unit_body", zigzag::noCell, "()", unitSrc);
  ASSERT_TRUE(headUnit.has_value());

  const auto body = h.rt().asBody(*headUnit);
  ASSERT_TRUE(body.has_value());
  EXPECT_EQ(*body, *headUnit);

  // Non-unit fragment cannot narrow to body
  const auto intSrc  = h.arena.makeCell("42");
  const auto headInt = hostCompileFragment(
      h.rt(), h.core, h.vm, "custom:int_val", zigzag::noCell, "Int", intSrc);
  ASSERT_TRUE(headInt.has_value());
  EXPECT_FALSE(h.rt().asBody(*headInt).has_value());

  // Non-nullary fragment (arity > 0) cannot narrow to body
  const auto paramsCell = h.arena.makeCell("List");
  const auto paramX     = h.arena.makeCell("x:Int");
  zigzag::expectWritten(
      h.arena.link(paramsCell, h.core.dims().step, DimVector::POS, paramX));
  const auto headParam = hostCompileFragment(
      h.rt(), h.core, h.vm, "custom:param_fn", paramsCell, "()", unitSrc);
  ASSERT_TRUE(headParam.has_value());
  EXPECT_FALSE(h.rt().asBody(*headParam).has_value());
}

// ---------------------------------------------------------------------------
// 5. Stage 3 Exit Gate: 2-Turn End-to-End .vagent Agent Test
// ---------------------------------------------------------------------------

TEST(VagentHostCompileTest, Stage3ExitGateTwoTurnAgentCompileAndExecute) {
  ExecutorHarness h;
  VagentExecutor exec;

  // Turn 1 compiles a new fragment via compile_fragment inside an on handler.
  // Turn 2 narrows the compiled head with as_body and installs it via
  // learn_body onto a plan clause, and executes it.
  const auto image =
      h.lower("module demo\n"
              "import std:meta\n"
              "event Setup { params: Cell, src: Cell }\n"
              "goal Work() perform\n"
              "agent A {\n"
              "  var compiled: Opt<Code<Cell>> := none\n"
              "  on Setup as ev {\n"
              "    compiled := compile_fragment(\"custom:plan1\", ev.params, "
              "\"()\", ev.src)\n"
              "  }\n"
              "  plan default_plan for Work {\n"
              "  }\n"
              "}\n");
  ASSERT_TRUE(image);

  auto &arena = h.ctx.arena;

  // Construct Setup event payload carrying nullary params and "()" srcCell
  const auto paramsCell = arena.makeCell("List");
  const auto srcCell    = arena.makeCell("()");

  const auto term = arena.makeCell("Setup");

  const auto fieldParams =
      arena.makeScalarCell(static_cast<std::int64_t>(paramsCell));
  zigzag::expectWritten(arena.link(fieldParams, h.ctx.core.dims().name,
                                   DimVector::POS, arena.makeCell("params")));
  zigzag::expectWritten(
      arena.link(zigzag::rankTail(arena, term, h.ctx.core.dims().step),
                 h.ctx.core.dims().step, DimVector::POS, fieldParams));

  const auto fieldSrc =
      arena.makeScalarCell(static_cast<std::int64_t>(srcCell));
  zigzag::expectWritten(arena.link(fieldSrc, h.ctx.core.dims().name,
                                   DimVector::POS, arena.makeCell("src")));
  zigzag::expectWritten(
      arena.link(zigzag::rankTail(arena, term, h.ctx.core.dims().step),
                 h.ctx.core.dims().step, DimVector::POS, fieldSrc));

  exec.deliver(arena, h.ctx.core,
               Delivery{.agent       = image->agentCell,
                        .percept     = "Setup",
                        .payloadCell = term});

  // Turn 1 executes: on Setup runs compile_fragment
  const auto t1Result = exec(h.execution(), h.ctx);
  ASSERT_TRUE(t1Result.has_value());
  EXPECT_EQ(arena.outstandingMarks(), 0u);

  // The compiled head was registered into d.code after commit drained
  // d.pending-reg
  ASSERT_TRUE(h.meta.has_value());
  const auto head = h.meta->codeNamed("custom:plan1");
  ASSERT_TRUE(head.has_value());
  EXPECT_EQ(h.meta->registration(*head), "committed");
  EXPECT_TRUE(h.meta->validate(*head).ok);

  // Turn 2: Narrow with as_body and install via learn_body onto plan
  // default_plan
  const auto narrowed = h.meta->asBody(*head);
  ASSERT_TRUE(narrowed.has_value());
  EXPECT_EQ(*narrowed, *head);

  const auto clause = h.meta->planNamed("default_plan");
  ASSERT_TRUE(clause.has_value());

  const auto learned = h.meta->learnBody(*clause, *narrowed);
  ASSERT_TRUE(learned.has_value()) << (learned ? "" : learned.error());

  // Verify the clause's body (+d.spin) is now the compiled body
  const auto installedBody = h.meta->bodyOf(*clause);
  ASSERT_TRUE(installedBody.has_value());
  const auto spin =
      arena.linked(*clause, h.ctx.core.dims().spin, DimVector::POS);
  ASSERT_TRUE(spin.has_value());
  EXPECT_EQ(
      arena.linked(*installedBody, h.meta->binds().dims.entry, DimVector::POS),
      spin);

  // Execute the newly installed plan body
  const auto runOutcome = h.meta->runFragment(*installedBody, {});
  EXPECT_EQ(runOutcome.refusal, MetaRunOutcome::Refusal::None);
  EXPECT_TRUE(runOutcome.produced);
}

} // namespace
