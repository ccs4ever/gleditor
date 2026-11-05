// M1 harness (actor-executor §11.3): a lowered program image plus one
// WorkerContext drives VagentExecutor turns directly — no mesh plumbing —
// asserting the §3.2 protocol: base-CP/mark discipline, $turn, the
// bounded journal, `on` dispatch, and zero residuals.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "../../apps/common/xanadu/vagent/decls.hpp"
#include "../../apps/common/xanadu/vagent/executor.hpp"
#include "../../apps/common/xanadu/vagent/frontend.hpp"
#include "../../apps/common/xanadu/vagent/meta.hpp"
#include "../../apps/common/xanadu/vagent/records.hpp"

namespace {

using namespace xanadu::vagent;
using xanadu::identity::ActorPid;
using xanadu::identity::MessageType;
using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;
using zigzag::vortex::WorkerContext;
using zigzag::vortex::mesh::ActorExecution;
using zigzag::vortex::mesh::ActorGrant;
using zigzag::vortex::mesh::ActorMessage;

struct ExecutorHarness {
  WorkerContext ctx;

  std::expected<LoweredImage, std::vector<Diagnostic>>
  lower(std::string_view source) {
    auto artifacts = compileArtifacts(source);
    if (!artifacts) {
      for (const auto &d : artifacts.error())
        fprintf(stderr, "frontend: %s\n", d.detail.c_str());
      return std::unexpected(artifacts.error());
    }
    auto image = lowerProgram(artifacts->program, ctx.core, ctx.vm);
    if (!image)
      for (const auto &d : image.error())
        fprintf(stderr, "lower: %s\n", d.detail.c_str());
    return image;
  }

  ActorExecution execution(std::optional<std::string> event = std::nullopt) {
    ActorExecution exec;
    exec.pid   = ActorPid{"worker", "agent", 1, 1};
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

  std::optional<std::int64_t> turnOf(CellRef agent) {
    const auto rec = varsRecord(ctx.arena, ctx.core, agent, "$turn", false);
    if (!rec) return std::nullopt;
    const auto value =
        ctx.arena.linked(*rec, ctx.core.dims().values, DimVector::POS);
    return value ? ctx.arena.asInt64(*value) : std::nullopt;
  }
};

TEST(VagentExecutorTest, WakeTurnBumpsTurnAndLeavesNoResiduals) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\nagent A { on Tick { } }");
  ASSERT_TRUE(image);
  const auto outcome = exec(h.execution(), h.ctx);
  ASSERT_TRUE(outcome) << static_cast<int>(outcome.error());
  EXPECT_EQ(*outcome, "ok");
  EXPECT_EQ(h.turnOf(image->agentCell), 1);
  EXPECT_EQ(h.ctx.arena.outstandingMarks(), 0u);
  // A wake journals no percept.
  const auto seen = h.ctx.core.findOrMintDimension("d.seen");
  EXPECT_TRUE(zigzag::rankAfter(h.ctx.arena, image->agentCell, seen).empty());
}

TEST(VagentExecutorTest, MessageTurnJournalsThePerceptAndDispatches) {
  ExecutorHarness h;
  VagentExecutor exec{[](std::string_view key) -> std::optional<std::int64_t> {
    if (key == "vagent.journal_size") return 2;
    return std::nullopt;
  }};
  const auto image = h.lower("module demo\nevent Tick\nevent Tock\nagent A { "
                             "on Tick { } on Tock { } }");
  ASSERT_TRUE(image);
  const auto tick = h.execution("Tick");
  ASSERT_TRUE(exec(tick, h.ctx));
  EXPECT_EQ(h.turnOf(image->agentCell), 1);
  const auto seen      = h.ctx.core.findOrMintDimension("d.seen");
  const auto journalOf = [&] {
    return zigzag::rankAfter(h.ctx.arena, image->agentCell, seen) |
           std::ranges::to<std::vector>();
  };
  auto journal = journalOf();
  ASSERT_EQ(journal.size(), 1u);
  EXPECT_EQ(h.ctx.arena.textOf(journal.front()), "Tick");
  // Bound enforced on the next turn: the head drops, newest survives.
  ASSERT_TRUE(exec(h.execution("Tock"), h.ctx));
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  journal = journalOf();
  ASSERT_EQ(journal.size(), 2u);
  EXPECT_EQ(h.ctx.arena.textOf(journal[0]), "Tock");
  EXPECT_EQ(h.ctx.arena.textOf(journal[1]), "Tick");
  EXPECT_EQ(h.turnOf(image->agentCell), 3);
  EXPECT_EQ(h.ctx.arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, NoAgentCellIsATurnFault) {
  ExecutorHarness h;
  VagentExecutor exec;
  // A program without an agent decl fails the turn, never commits.
  const auto image = h.lower("module m\nevent Tick");
  ASSERT_TRUE(image);
  const auto outcome = exec(h.execution(), h.ctx);
  EXPECT_FALSE(outcome);
  EXPECT_EQ(outcome.error(), zigzag::vortex::mesh::ActorError::ExecutionFailed);
  EXPECT_EQ(h.ctx.arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, PendingEventsDrainDuringSettleWithoutJournaling) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image = h.lower("module demo\nevent Poke\nevent Tick\n"
                             "agent A { on Poke { } on Tick { } }");
  ASSERT_TRUE(image);
  auto &arena      = h.ctx.arena;
  const auto pend  = h.ctx.core.findOrMintDimension("d.pending");
  const auto queue = [&](std::string_view name) {
    zigzag::expectWritten(
        arena.link(zigzag::rankTail(arena, image->agentCell, pend), pend,
                   DimVector::POS, arena.makeCell(std::string{name})));
  };
  queue("Poke");
  queue("Tick");
  queue("NoHandler"); // unknown events dispatch to nothing, still drain

  const auto outcome = exec(h.execution(), h.ctx);
  ASSERT_TRUE(outcome) << static_cast<int>(outcome.error());
  EXPECT_TRUE(zigzag::rankAfter(arena, image->agentCell, pend).empty());
  // Internal events are not percepts — the journal stays empty.
  const auto seen = h.ctx.core.findOrMintDimension("d.seen");
  EXPECT_TRUE(zigzag::rankAfter(arena, image->agentCell, seen).empty());
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

// -- §3.1/§3.8 belief store: predicate banks, provenance, the four verbs ------

TEST(VagentBeliefTest, BelieveBelievesForgetReviseRoundTrip) {
  ExecutorHarness h;
  const auto image =
      h.lower("module demo\nevent Tick\nagent A { on Tick { } }");
  ASSERT_TRUE(image);
  auto binds = mintMetaBindSet(h.ctx.arena, h.ctx.core, h.ctx.core.home(),
                               image->agentCell);
  ASSERT_TRUE(binds) << binds.error();
  MetaRuntime meta(h.ctx.core, h.ctx.vm, *binds);

  auto &arena     = h.ctx.arena;
  const auto wet  = arena.makeCell("cloudy");
  const auto dry  = arena.makeCell("sunny");
  const auto peer = arena.makeCell("peer-a");

  const auto clause = meta.believe("weather", wet, peer, 7);
  ASSERT_TRUE(clause) << clause.error();
  EXPECT_TRUE(meta.believes("weather", wet));
  EXPECT_FALSE(meta.believes("weather", dry));
  EXPECT_EQ(meta.beliefsOf("weather").size(), 1u);

  // §3.1 shape: the clause sits on the bank's +d.clause rank with
  // +d.grab → term and +d.source → the {peer, turn} record.
  const auto bank = meta.beliefBank("weather");
  ASSERT_TRUE(bank.has_value());
  EXPECT_EQ(arena.textOf(*bank), "weather");
  const auto grab =
      arena.linked(*clause, h.ctx.core.dims().grab, DimVector::POS);
  ASSERT_TRUE(grab.has_value());
  EXPECT_EQ(*grab, wet);
  const auto sourceDim = h.ctx.core.findDimension("d.source");
  ASSERT_TRUE(sourceDim.has_value());
  const auto source = arena.linked(*clause, *sourceDim, DimVector::POS);
  ASSERT_TRUE(source.has_value());
  const auto peerSlot = varsRecord(arena, h.ctx.core, *source, "peer", false);
  ASSERT_TRUE(peerSlot.has_value());
  EXPECT_EQ(arena.linked(*peerSlot, h.ctx.core.dims().values, DimVector::POS),
            std::optional{peer});
  const auto turnSlot = varsRecord(arena, h.ctx.core, *source, "turn", false);
  ASSERT_TRUE(turnSlot.has_value());
  const auto turnCell =
      arena.linked(*turnSlot, h.ctx.core.dims().values, DimVector::POS);
  ASSERT_TRUE(turnCell.has_value());
  EXPECT_EQ(arena.asInt64(*turnCell), 7);

  // forget unlinks the matching clause; revise swaps the term in one call.
  EXPECT_EQ(meta.forget("weather", wet), 1u);
  EXPECT_FALSE(meta.believes("weather", wet));
  ASSERT_TRUE(meta.believe("weather", wet, peer, 8));
  ASSERT_TRUE(meta.revise("weather", wet, dry, peer, 9));
  EXPECT_FALSE(meta.believes("weather", wet));
  EXPECT_TRUE(meta.believes("weather", dry));
  EXPECT_EQ(meta.beliefsOf("weather").size(), 1u);
}

TEST(VagentBeliefTest, CompoundTermsMatchStructurallyNotByIdentity) {
  ExecutorHarness h;
  const auto image =
      h.lower("module demo\nevent Tick\nagent A { on Tick { } }");
  ASSERT_TRUE(image);
  auto binds = mintMetaBindSet(h.ctx.arena, h.ctx.core, h.ctx.core.home(),
                               image->agentCell);
  ASSERT_TRUE(binds) << binds.error();
  MetaRuntime meta(h.ctx.core, h.ctx.vm, *binds);

  auto &arena     = h.ctx.arena;
  const auto step = h.ctx.core.dims().step;
  // Two distinct cells encoding the same functor tree threat(fang).
  const auto makeTerm = [&]() {
    const auto t = arena.makeCell("threat");
    zigzag::expectWritten(
        arena.link(t, step, DimVector::POS, arena.makeCell("fang")));
    return t;
  };
  const auto first = makeTerm();
  const auto twin  = makeTerm();
  const auto other = [&]() {
    const auto t = arena.makeCell("threat");
    zigzag::expectWritten(
        arena.link(t, step, DimVector::POS, arena.makeCell("swarm")));
    return t;
  }();
  const auto peer = arena.makeCell("peer-a");

  ASSERT_TRUE(meta.believe("alert", first, peer, 1));
  EXPECT_TRUE(meta.believes("alert", twin));
  EXPECT_FALSE(meta.believes("alert", other));
  // Equality is order- and arity-sensitive.
  EXPECT_EQ(meta.forget("alert", other), 0u);
  EXPECT_EQ(meta.forget("alert", twin), 1u);
}

TEST(VagentBeliefTest, EvidenceEdgeLinksTheJournaledPercept) {
  ExecutorHarness h;
  const auto image =
      h.lower("module demo\nevent Tick\nagent A { on Tick { } }");
  ASSERT_TRUE(image);
  auto binds = mintMetaBindSet(h.ctx.arena, h.ctx.core, h.ctx.core.home(),
                               image->agentCell);
  ASSERT_TRUE(binds) << binds.error();
  MetaRuntime meta(h.ctx.core, h.ctx.vm, *binds);

  auto &arena        = h.ctx.arena;
  const auto term    = arena.makeCell("rain");
  const auto peer    = arena.makeCell("peer-b");
  const auto percept = arena.makeCell("percept:1");

  const auto clause = meta.believe("weather", term, peer, 3, percept);
  ASSERT_TRUE(clause) << clause.error();
  const auto evidenceDim = h.ctx.core.findDimension("d.evidence");
  ASSERT_TRUE(evidenceDim.has_value());
  EXPECT_EQ(arena.linked(*clause, *evidenceDim, DimVector::POS),
            std::optional{percept});
}

} // namespace
