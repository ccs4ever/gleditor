// M1 harness (actor-executor §11.3): a lowered program image plus one
// WorkerContext drives VagentExecutor turns directly — no mesh plumbing —
// asserting the §3.2 protocol: base-CP/mark discipline, $turn, the
// bounded journal, `on` dispatch, and zero residuals.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "../../apps/common/xanadu/vagent/decls.hpp"
#include "../../apps/common/xanadu/vagent/executor.hpp"
#include "../../apps/common/xanadu/vagent/frontend.hpp"
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

} // namespace
