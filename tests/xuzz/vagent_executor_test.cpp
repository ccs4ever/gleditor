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
  // A wake journals no new percept — the lone entry is the first
  // turn's Initialize.
  const auto seen = h.ctx.core.findOrMintDimension("d.seen");
  const auto journal =
      zigzag::rankAfter(h.ctx.arena, image->agentCell, seen) |
      std::ranges::to<std::vector>();
  ASSERT_EQ(journal.size(), 1u);
  EXPECT_EQ(h.ctx.arena.textOf(journal.front()), "Initialize");
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
  // [Initialize, Tick]: creation's percept journals ahead of the
  // message's on the first turn.
  ASSERT_EQ(journal.size(), 2u);
  EXPECT_EQ(h.ctx.arena.textOf(journal.front()), "Initialize");
  EXPECT_EQ(h.ctx.arena.textOf(journal[1]), "Tick");
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
  // Internal events are not percepts — the journal holds only the
  // first turn's Initialize.
  const auto seen = h.ctx.core.findOrMintDimension("d.seen");
  const auto journal =
      zigzag::rankAfter(arena, image->agentCell, seen) |
      std::ranges::to<std::vector>();
  ASSERT_EQ(journal.size(), 1u);
  EXPECT_EQ(arena.textOf(journal.front()), "Initialize");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, HostVerbStatementRunsThroughTheSegmentSpine) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\npredicate weather(s: Str)\n"
              "agent A { on Tick { believe weather(\"cloudy\") } }");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;

  // The clause carries a +d.step spine: one host-step member.
  const auto step   = h.ctx.core.dims().step;
  const auto clause = [&]() -> CellRef {
    for (const CellRef module :
         zigzag::rankAfter(arena, h.ctx.core.home(), h.ctx.core.dims().stdlib))
      for (const CellRef sym :
           zigzag::rankAfter(arena, module, h.ctx.core.dims().vars))
        if (arena.textOf(sym) == "Tick")
          return *zigzag::rankAfter(arena, sym, h.ctx.core.dims().clause)
                      .begin();
    return zigzag::noCell;
  }();
  ASSERT_NE(clause, zigzag::noCell);
  const auto spine =
      zigzag::rankAfter(arena, clause, step) | std::ranges::to<std::vector>();
  ASSERT_EQ(spine.size(), 1u);
  const auto verbDim  = h.ctx.core.findOrMintDimension("d.verb");
  const auto verbCell = arena.linked(spine[0], verbDim, DimVector::POS);
  ASSERT_TRUE(verbCell.has_value());
  EXPECT_EQ(arena.textOf(*verbCell), "believe");

  const auto outcome = exec(h.execution("Tick"), h.ctx);
  ASSERT_TRUE(outcome) << static_cast<int>(outcome.error());

  // The host step ran between segments: the weather bank holds
  // weather(cloudy), and the journal still saw the percept.
  auto binds =
      mintMetaBindSet(arena, h.ctx.core, h.ctx.core.home(), image->agentCell);
  ASSERT_TRUE(binds) << binds.error();
  MetaRuntime meta(h.ctx.core, h.ctx.vm, *binds);
  const auto bank = meta.beliefBank("weather");
  ASSERT_TRUE(bank.has_value());
  const auto facts = meta.beliefsOf("weather");
  ASSERT_EQ(facts.size(), 1u);
  const auto grab =
      arena.linked(facts[0], h.ctx.core.dims().grab, DimVector::POS);
  ASSERT_TRUE(grab.has_value());
  EXPECT_EQ(arena.textOf(*grab), "weather");
  const auto arg =
      zigzag::rankAfter(arena, *grab, step) | std::ranges::to<std::vector>();
  ASSERT_EQ(arg.size(), 1u);
  EXPECT_EQ(arena.textOf(arg[0]), "cloudy");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

// -- §6.3-amended selection: virtual weight-order, where fuel, tried ----------

struct SelectorFixture {
  ExecutorHarness h;
  std::unique_ptr<MetaRuntime> meta;

  CellRef symbol(std::string_view name) {
    for (const CellRef module : zigzag::rankAfter(
             h.ctx.arena, h.ctx.core.home(), h.ctx.core.dims().stdlib))
      for (const CellRef sym :
           zigzag::rankAfter(h.ctx.arena, module, h.ctx.core.dims().vars))
        if (h.ctx.arena.textOf(sym) == name) return sym;
    return zigzag::noCell;
  }

  void setup(std::string_view source) {
    const auto image = h.lower(source);
    EXPECT_TRUE(image.has_value());
    if (!image) return;
    auto binds = mintMetaBindSet(h.ctx.arena, h.ctx.core, h.ctx.core.home(),
                                 image->agentCell);
    EXPECT_TRUE(binds.has_value());
    if (!binds) return;
    meta = std::make_unique<MetaRuntime>(h.ctx.core, h.ctx.vm, *binds);
  }

  std::vector<CellRef> clauses(std::string_view goal) {
    return zigzag::rankAfter(h.ctx.arena, symbol(goal),
                             h.ctx.core.dims().clause) |
           std::ranges::to<std::vector>();
  }
};

TEST(VagentSelectTest, WhereGuardsSkipFailingClauses) {
  SelectorFixture f;
  f.setup("module demo\ngoal Work() perform\n"
          "agent A {\n"
          "  plan slow for Work where true == false { }\n"
          "  plan fast for Work { }\n"
          "}");
  ASSERT_TRUE(f.meta);
  const auto bank = f.clauses("Work");
  ASSERT_EQ(bank.size(), 2u);
  // Rank order puts slow first, but its where-guard fails to
  // produce — selection lands on fast.
  const auto picked = f.meta->selectClause(f.symbol("Work"));
  ASSERT_TRUE(picked) << picked.error();
  ASSERT_TRUE(picked->has_value());
  EXPECT_EQ(picked->value().first, bank[1]);
}

TEST(VagentSelectTest, WeightsOrderSelectionAndTriedSkips) {
  SelectorFixture f;
  f.setup("module demo\ngoal Other() perform\n"
          "agent A {\n"
          "  plan heavy for Other { }\n"
          "  plan light for Other { }\n"
          "}");
  ASSERT_TRUE(f.meta);
  const auto bank = f.clauses("Other");
  ASSERT_EQ(bank.size(), 2u);

  // Equal (absent) weights — rank position breaks the tie.
  auto picked = f.meta->selectClause(f.symbol("Other"));
  ASSERT_TRUE(picked && picked->has_value());
  EXPECT_EQ(picked->value().first, bank[0]);

  // A d.vars weight record flips the order — the virtual order reads
  // the ledger, no relinking.
  ASSERT_TRUE(f.meta->setWeight(bank[1], 5.0).has_value());
  picked = f.meta->selectClause(f.symbol("Other"));
  ASSERT_TRUE(picked && picked->has_value());
  EXPECT_EQ(picked->value().first, bank[1]);

  // The tried-set skips the leader — selection falls to the next
  // untried clause whose guard holds.
  picked = f.meta->selectClause(f.symbol("Other"), {bank[1]});
  ASSERT_TRUE(picked && picked->has_value());
  EXPECT_EQ(picked->value().first, bank[0]);

  // Everything tried — exhaustion is an honest empty.
  picked = f.meta->selectClause(f.symbol("Other"), {bank[0], bank[1]});
  ASSERT_TRUE(picked);
  EXPECT_FALSE(picked->has_value());
}

TEST(VagentExecutorTest, EmitMintsAnOutboxRecord) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image = h.lower("module demo\nevent Tick\nevent Tock\n"
                             "agent A { on Tick { emit Tock{} } }");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;

  const auto outcome = exec(h.execution("Tick"), h.ctx);
  ASSERT_TRUE(outcome) << static_cast<int>(outcome.error());

  // §3.7: one cell-legible entry on +d.outbox — kind/payload/id
  // fields readable through the vars records.
  const auto outbox  = h.ctx.core.findOrMintDimension("d.outbox");
  const auto entries = zigzag::rankAfter(arena, image->agentCell, outbox) |
                       std::ranges::to<std::vector>();
  ASSERT_EQ(entries.size(), 1u);
  const auto fieldOf = [&](std::string_view name) -> CellRef {
    const auto rec = varsRecord(arena, h.ctx.core, entries[0], name, false);
    if (!rec) return zigzag::noCell;
    const auto v = arena.linked(*rec, h.ctx.core.dims().values, DimVector::POS);
    return v ? *v : zigzag::noCell;
  };
  EXPECT_EQ(arena.textOf(fieldOf("kind")), "emit");
  // The payload rides a scalar ref — program-image term cells take
  // one inbound edge per dim, so records never edge-link them.
  const auto payloadRef = arena.asInt64(fieldOf("payload"));
  ASSERT_TRUE(payloadRef.has_value());
  EXPECT_EQ(arena.textOf(static_cast<CellRef>(*payloadRef)), "Tock");
  // §3.7's idempotence triple: sender-name hash + turn + position.
  const auto expected =
      std::format("{}:{}:{}", std::hash<std::string>{}("A"), 1, 0);
  EXPECT_EQ(arena.textOf(fieldOf("id")), expected);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, EmitMarshalsRecordFieldsOntoThePayload) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image = h.lower("module demo\nevent Tick\nevent Seen { v: Int }\n"
                             "agent A { on Tick { emit Seen{v: 1} } }");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;

  const auto outcome = exec(h.execution("Tick"), h.ctx);
  ASSERT_TRUE(outcome) << static_cast<int>(outcome.error());

  // The payload cell carries the event name; fields marshal as
  // +d.step children each +d.name-labeled — textOf still reads Seen.
  const auto outbox  = h.ctx.core.findOrMintDimension("d.outbox");
  const auto entries = zigzag::rankAfter(arena, image->agentCell, outbox) |
                       std::ranges::to<std::vector>();
  ASSERT_EQ(entries.size(), 1u);
  const auto rec = varsRecord(arena, h.ctx.core, entries[0], "payload", false);
  ASSERT_TRUE(rec.has_value());
  const auto payloadScalar =
      arena.linked(*rec, h.ctx.core.dims().values, DimVector::POS);
  ASSERT_TRUE(payloadScalar.has_value());
  const auto payloadRef = arena.asInt64(*payloadScalar);
  ASSERT_TRUE(payloadRef.has_value());
  const auto payload = static_cast<CellRef>(*payloadRef);
  EXPECT_EQ(arena.textOf(payload), "Seen");
  const auto fields =
      zigzag::rankAfter(arena, payload, h.ctx.core.dims().step) |
      std::ranges::to<std::vector>();
  ASSERT_EQ(fields.size(), 1u);
  const auto fname =
      arena.linked(fields[0], h.ctx.core.dims().name, DimVector::POS);
  ASSERT_TRUE(fname.has_value());
  EXPECT_EQ(arena.textOf(*fname), "v");
  EXPECT_EQ(arena.textOf(fields[0]), "1");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, OnHandlerGuardEvaluatesBeforeTheBody) {
  ExecutorHarness h;
  VagentExecutor exec;
  // The [guard] is a real filter now — before this, dispatch ran every
  // clause's body unconditionally. `emit` makes the selection visible:
  // const-false's handler stages nothing, const-true's does.
  const auto image = h.lower(
      "module demo\nevent Tick\nevent Fired\nevent Nope\n"
      "agent A {\n"
      "  on Tick [2 > 3] { emit Nope{} }\n"
      "  on Tick [1 < 2] { emit Fired{} }\n"
      "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;

  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  const auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 1u);
  EXPECT_EQ(outbox[0].payload, "Fired");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, OnHandlerGuardArityBindsFromTheDeliveredPayload) {
  ExecutorHarness h;
  VagentExecutor exec;
  // A param'd event's clause declares the event's fields as its guard
  // head's params — `where true` on Ping is arity 1, not arity 0. A
  // name-only percept can't satisfy the contract, so dispatch reads
  // the arity refusal as a falsy guard and skips the clause; the
  // delivered record supplies `n` by name and the handler fires.
  const auto image = h.lower(
      "module demo\nevent Ping { n: Int }\nevent Fired\n"
      "agent A { on Ping { emit Fired{} } }");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;

  ASSERT_TRUE(exec(h.execution("Ping"), h.ctx));
  auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  EXPECT_TRUE(outbox.empty())
      << "name-only Ping can't bind the declared field — handler "
         "skips, not faults";

  const auto term  = arena.makeCell("Ping");
  const auto field = arena.makeCell("5");
  zigzag::expectWritten(arena.link(field, h.ctx.core.dims().name,
                                   DimVector::POS, arena.makeCell("n")));
  zigzag::expectWritten(
      arena.link(zigzag::rankTail(arena, term, h.ctx.core.dims().step),
                 h.ctx.core.dims().step, DimVector::POS, field));
  exec.deliver(arena, h.ctx.core,
               Delivery{.agent       = image->agentCell,
                        .percept     = "Ping",
                        .payloadCell = term});
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  outbox = VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 1u);
  EXPECT_EQ(outbox[0].payload, "Fired");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, OnHandlerFieldAccessReadsTheDeliveredPayload) {
  ExecutorHarness h;
  VagentExecutor exec;
  // `as a` binds the delivered term and `a.severity` lowers to #FIND —
  // a name-keyed hop over the payload's +d.step rank. A loud alarm
  // fires; a quiet one fails the guard; a bare name-only percept can't
  // bind `severity` at all and the clause skips without faulting.
  const auto image = h.lower(
      "module demo\nevent Alarm { severity: Int }\nevent Loud\n"
      "agent A { on Alarm as a [a.severity > 5] { emit Loud{} } }");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;

  const auto alarm = [&](std::string_view text, std::int64_t bits) {
    const auto term  = arena.makeCell(text);
    const auto field = arena.makeCell(text);
    zigzag::expectWritten(arena.setValueBits(
        field, xanadu::ValueKind::Int64,
        std::bit_cast<std::uint64_t>(bits)));
    zigzag::expectWritten(
        arena.link(field, h.ctx.core.dims().name, DimVector::POS,
                   arena.makeCell("severity")));
    zigzag::expectWritten(
        arena.link(zigzag::rankTail(arena, term, h.ctx.core.dims().step),
                   h.ctx.core.dims().step, DimVector::POS, field));
    return term;
  };
  exec.deliver(arena, h.ctx.core,
               Delivery{.agent       = image->agentCell,
                        .percept     = "Alarm",
                        .payloadCell = alarm("Alarm", 9)});
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 1u);
  EXPECT_EQ(outbox[0].payload, "Loud");

  // drainOutbox reads without consuming — a failed guard shows as the
  // count *not* growing, not as an empty rank.
  exec.deliver(arena, h.ctx.core,
               Delivery{.agent       = image->agentCell,
                        .percept     = "Alarm",
                        .payloadCell = alarm("Alarm", 2)});
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  EXPECT_EQ(outbox.size(), 1u) << "severity 2 must fail the guard";

  // A name-only percept can't bind the declared `severity` param —
  // the arity refusal skips the clause before the guard runs, and the
  // turn still completes cleanly.
  exec.deliver(arena, h.ctx.core,
               Delivery{.agent       = image->agentCell,
                        .percept     = "Alarm",
                        .payloadCell = arena.makeCell("Alarm")});
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  EXPECT_EQ(outbox.size(), 1u);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, ConsumeSpendsTheEventForLaterHandlersOnly) {
  ExecutorHarness h;
  VagentExecutor exec;
  // The guide's consume contract: a matching handler that runs
  // `consume` stops *later* handlers of the same event; statements
  // after consume inside its own body still run, and a handler whose
  // guard fails never consumes — later handlers still see the event.
  const auto image = h.lower(
      "module demo\nevent Tick\nevent Big\nevent Mid\nevent Small\n"
      "agent A {\n"
      "  on Tick [2 > 3] { emit Small{}; consume }\n"
      "  on Tick { emit Big{}; consume; emit Mid{} }\n"
      "  on Tick { emit Small{} }\n"
      "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;

  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  const auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 2u);
  EXPECT_EQ(outbox[0].payload, "Big");
  EXPECT_EQ(outbox[1].payload, "Mid");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, HandlerPriorityOrdersBeforeDeclarationOrder) {
  ExecutorHarness h;
  VagentExecutor exec;
  // §3.5's merged order: priority first (higher runs earlier), ties
  // keep declaration order. The first-declared handler carries no
  // priority, so priority 2's handler emits ahead of it.
  const auto image = h.lower(
      "module demo\nevent Tick\nevent Low\nevent High\n"
      "agent A {\n"
      "  on Tick { emit Low{} }\n"
      "  on Tick priority 2 { emit High{} }\n"
      "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;

  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  const auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 2u);
  EXPECT_EQ(outbox[0].payload, "High");
  EXPECT_EQ(outbox[1].payload, "Low");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, AgentVarReadsInGuardsAndWritesThroughBodies) {
  ExecutorHarness h;
  VagentExecutor exec;
  // The guide's counting shape: `var` mints the agent's state slot,
  // the guard reads it live, and `high := high + 1` writes through
  // the record's holder at run time — durable across turns.
  const auto image = h.lower(
      "module demo\nevent Tick\nevent Done\n"
      "agent A {\n"
      "  var high: Int := 0\n"
      "  on Tick [high < 2] { high := high + 1; emit Done{} }\n"
      "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  const auto highOf = [&]() -> std::int64_t {
    const auto rec = varsRecord(arena, h.ctx.core, image->agentCell,
                                "high", false);
    if (!rec) return -1;
    const auto holder =
        arena.linked(*rec, h.ctx.core.dims().values, DimVector::POS);
    if (!holder) return -1;
    const auto rendered = h.ctx.core.render(*holder);
    if (!rendered) return -1;
    if (const auto *n = std::get_if<std::int64_t>(&*rendered))
      return *n;
    return -1;
  };

  // Ticks 1-2 satisfy high<2 and count; tick 3 fails the guard, so
  // neither the write nor the emit happens.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  EXPECT_EQ(highOf(), 2);
  const auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 2u);
  EXPECT_EQ(outbox[0].payload, "Done");
  EXPECT_EQ(outbox[1].payload, "Done");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, EmitInAndForMarshalOntoSeparateFields) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\nevent Ping\n"
              "agent A { on Tick { emit Ping{} in inner for parent } }");
  ASSERT_TRUE(image);

  const auto outcome = exec(h.execution("Tick"), h.ctx);
  ASSERT_TRUE(outcome) << static_cast<int>(outcome.error());

  const auto outbox =
      VagentExecutor::drainOutbox(h.ctx.arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 1u);
  EXPECT_EQ(outbox[0].payload, "Ping");
  // `in` is the delivery scope, `for` the recipients — the record
  // carries both; the adapter reads target as the delivery address.
  EXPECT_EQ(outbox[0].scope, "inner");
  EXPECT_EQ(outbox[0].target, "parent");
  EXPECT_EQ(h.ctx.arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, DrainReadsTheOutboxAndDuplicatesAreDetectable) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image = h.lower("module demo\nevent Tick\nevent Tock\n"
                             "agent A { on Tick { emit Tock{} } }");
  ASSERT_TRUE(image);

  const auto outcome = exec(h.execution("Tick"), h.ctx);
  ASSERT_TRUE(outcome) << static_cast<int>(outcome.error());

  // §3.7's drain is adapter C++ — pure rankAfter/linked reads after
  // commit, consumeWork-free by construction.
  const auto first =
      VagentExecutor::drainOutbox(h.ctx.arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(first.size(), 1u);
  EXPECT_EQ(first[0].kind, "emit");
  EXPECT_EQ(first[0].payload, "Tock");
  EXPECT_EQ(first[0].id,
            std::format("{}:{}:{}", std::hash<std::string>{}("A"), 1, 0));

  // Entries persist: a re-drain returns the same id, which is what the
  // host binding compares to flag a duplicate rather than double-act.
  const auto second =
      VagentExecutor::drainOutbox(h.ctx.arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(second.size(), 1u);
  EXPECT_EQ(second[0].id, first[0].id);
  EXPECT_EQ(h.ctx.arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, AdoptMintsAnIntentionAndRunsItsSpine) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\npredicate weather(s: Str)\n"
              "goal Work() perform\n"
              "agent A {\n"
              "  on Tick { adopt Work() }\n"
              "  plan w for Work { believe weather(\"fog\") }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;

  const auto outcome = exec(h.execution("Tick"), h.ctx);
  ASSERT_TRUE(outcome) << static_cast<int>(outcome.error());

  // The adopt host step minted the intention; settle ran the plan's
  // one-member spine — the belief landed, and the intention retired.
  auto binds =
      mintMetaBindSet(arena, h.ctx.core, h.ctx.core.home(), image->agentCell);
  ASSERT_TRUE(binds) << binds.error();
  MetaRuntime meta(h.ctx.core, h.ctx.vm, *binds);
  EXPECT_TRUE(meta.believes("weather", [&] {
    const auto t = arena.makeCell("weather");
    zigzag::expectWritten(arena.link(t, h.ctx.core.dims().step, DimVector::POS,
                                     arena.makeCell("fog")));
    return t;
  }()));
  const auto intentDim = h.ctx.core.findOrMintDimension("d.intention");
  EXPECT_TRUE(zigzag::rankAfter(arena, image->agentCell, intentDim).empty());
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, AwaitSuspendsAndTheWakeResumes) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\nevent Grant\nevent Done\n"
              "goal Work() perform\n"
              "agent A {\n"
              "  on Tick { adopt Work() }\n"
              "  plan w for Work { await Grant as g within 5s; emit Done{} }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena     = h.ctx.arena;
  const auto iDim = h.ctx.core.findOrMintDimension("d.intention");

  // Turn 1 — Tick adopts; the spine's first member is the await host
  // step: the intention parks (waiting, step index past the member)
  // and Done does *not* emit yet.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  {
    const auto parked = zigzag::rankAfter(arena, image->agentCell, iDim) |
                        std::ranges::to<std::vector>();
    ASSERT_EQ(parked.size(), 1u);
    auto binds =
        mintMetaBindSet(arena, h.ctx.core, h.ctx.core.home(), image->agentCell);
    ASSERT_TRUE(binds);
    MetaRuntime meta(h.ctx.core, h.ctx.vm, *binds);
    EXPECT_EQ(meta.intentionField(parked.front(), "waiting"), 1);
    EXPECT_EQ(meta.intentionField(parked.front(), "ready"), 0);
    EXPECT_EQ(meta.intentionField(parked.front(), "awaitDeadline"), 6);
  }
  EXPECT_TRUE(
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell).empty());

  // Turn 2 — a bare wake finds no Grant in the journal: still parked.
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  EXPECT_EQ((zigzag::rankAfter(arena, image->agentCell, iDim) |
             std::ranges::to<std::vector>())
                .size(),
            1u);

  // Turn 3 — Grant journals, dispatch finds no handler, and the settle
  // walker's match links +d.evidence, readies the intention, and the
  // resumed spine emits Done and retires.
  ASSERT_TRUE(exec(h.execution("Grant"), h.ctx));
  EXPECT_TRUE(zigzag::rankAfter(arena, image->agentCell, iDim).empty());
  const auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 1u);
  EXPECT_EQ(outbox[0].payload, "Done");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, AwaitGuardFiltersTheWakePercept) {
  ExecutorHarness h;
  VagentExecutor exec;
  // `await Grant as g [g.ok > 5]`: the bracket is a wake filter —
  // a Grant whose `ok` fails the guard leaves the intention parked
  // and the scan keeps looking, so a later matching percept still
  // resumes the plan.
  const auto image =
      h.lower("module demo\nevent Tick\nevent Grant { ok: Int }\n"
              "event Done\ngoal Work() perform\n"
              "agent A {\n"
              "  on Tick { adopt Work() }\n"
              "  plan w for Work {"
              " await Grant as g [g.ok > 5] within 9s; emit Done{} }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena     = h.ctx.arena;
  const auto iDim = h.ctx.core.findOrMintDimension("d.intention");

  const auto grant = [&](std::int64_t ok) {
    const auto term  = arena.makeCell("Grant");
    const auto field = arena.makeCell(std::to_string(ok));
    zigzag::expectWritten(arena.setValueBits(
        field, xanadu::ValueKind::Int64,
        std::bit_cast<std::uint64_t>(ok)));
    zigzag::expectWritten(
        arena.link(field, h.ctx.core.dims().name, DimVector::POS,
                   arena.makeCell("ok")));
    zigzag::expectWritten(
        arena.link(zigzag::rankTail(arena, term, h.ctx.core.dims().step),
                   h.ctx.core.dims().step, DimVector::POS, field));
    return term;
  };

  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  ASSERT_EQ((zigzag::rankAfter(arena, image->agentCell, iDim) |
             std::ranges::to<std::vector>())
                .size(),
            1u);

  // Grant{ok: 2} fails the guard — still parked, no Done.
  exec.deliver(arena, h.ctx.core,
               Delivery{.agent       = image->agentCell,
                        .percept     = "Grant",
                        .payloadCell = grant(2)});
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  EXPECT_EQ((zigzag::rankAfter(arena, image->agentCell, iDim) |
             std::ranges::to<std::vector>())
                .size(),
            1u);
  EXPECT_TRUE(
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell)
          .empty());

  // Grant{ok: 9} satisfies it — the scan skips the first percept on
  // the guard and resumes on the second.
  exec.deliver(arena, h.ctx.core,
               Delivery{.agent       = image->agentCell,
                        .percept     = "Grant",
                        .payloadCell = grant(9)});
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  EXPECT_TRUE(zigzag::rankAfter(arena, image->agentCell, iDim).empty());
  const auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 1u);
  EXPECT_EQ(outbox[0].payload, "Done");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, FaultBackoffRetriesThenDropWhenCancels) {
  ExecutorHarness h;
  // backoff=0 turns the park into a next-turn retry; the cap of two
  // faults reaches the drop boundary on the third settle.
  VagentExecutor exec{[](std::string_view key) -> std::optional<std::int64_t> {
    if (key == "vagent.intention_backoff") return 0;
    if (key == "vagent.max_intention_faults") return 2;
    return std::nullopt;
  }};
  const auto image = h.lower("module demo\nevent Tick\n"
                             "goal Work() perform { drop when true == true }\n"
                             "goal Missing() perform\n"
                             "agent A {\n"
                             "  on Tick { adopt Work() }\n"
                             "  plan w for Work { adopt Missing() }\n"
                             "}");
  ASSERT_TRUE(image);
  auto &arena     = h.ctx.arena;
  const auto iDim = h.ctx.core.findOrMintDimension("d.intention");

  const auto field = [&](std::string_view name) {
    const auto parked = zigzag::rankAfter(arena, image->agentCell, iDim) |
                        std::ranges::to<std::vector>();
    if (parked.empty()) return std::optional<std::int64_t>{};
    auto binds =
        mintMetaBindSet(arena, h.ctx.core, h.ctx.core.home(), image->agentCell);
    MetaRuntime meta(h.ctx.core, h.ctx.vm, *binds);
    return meta.intentionField(parked.front(), name);
  };

  // Turn 1 — adopt runs; the plan's adopt of a plan-less goal fails:
  // faults=1, faulted, blocked_until=turn (backoff 0).
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  EXPECT_EQ(field("faults"), 1);
  EXPECT_EQ(field("faulted"), 1);

  // Turn 2 — backoff expired: the same member retries and fails,
  // faults=2 — at the cap next settle.
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  EXPECT_EQ(field("faults"), 2);

  // Turn 3 — at the cap the goal's `drop when true` is the
  // reconsideration boundary: the intention drops off the rank.
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  EXPECT_TRUE(zigzag::rankAfter(arena, image->agentCell, iDim).empty());
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, AchieveDelegatesAndTheChildsRetireResumes) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\nevent Done\n"
              "goal Outer() perform\ngoal Inner() perform\n"
              "agent A {\n"
              "  on Tick { adopt Outer() }\n"
              "  plan o for Outer { achieve Inner(); emit Done{} }\n"
              "  plan i for Inner { }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena     = h.ctx.arena;
  const auto iDim = h.ctx.core.findOrMintDimension("d.intention");

  // Turn 1 — adopt Outer; the spine's achieve parks the parent and
  // mints the child: two records on the rank, Done not yet emitted.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  {
    const auto live = zigzag::rankAfter(arena, image->agentCell, iDim) |
                      std::ranges::to<std::vector>();
    ASSERT_EQ(live.size(), 2u);
    auto binds =
        mintMetaBindSet(arena, h.ctx.core, h.ctx.core.home(), image->agentCell);
    MetaRuntime meta(h.ctx.core, h.ctx.vm, *binds);
    // The parent (rank-first, adopted before the child was minted)
    // is the delegated one.
    EXPECT_EQ(meta.intentionField(live.front(), "delegated"), 1);
    EXPECT_EQ(meta.intentionField(live.front(), "ready"), 0);
  }
  EXPECT_TRUE(
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell).empty());

  // Turn 2 — the child's empty body retires and wakes the delegator;
  // only the parent remains, ready to step.
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  {
    const auto live = zigzag::rankAfter(arena, image->agentCell, iDim) |
                      std::ranges::to<std::vector>();
    ASSERT_EQ(live.size(), 1u);
    auto binds =
        mintMetaBindSet(arena, h.ctx.core, h.ctx.core.home(), image->agentCell);
    MetaRuntime meta(h.ctx.core, h.ctx.vm, *binds);
    EXPECT_EQ(meta.intentionField(live.front(), "ready"), 1);
  }

  // Turn 3 — the resumed spine emits Done and the parent retires.
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  EXPECT_TRUE(zigzag::rankAfter(arena, image->agentCell, iDim).empty());
  const auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 1u);
  EXPECT_EQ(outbox[0].payload, "Done");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, AchieveGoalFaultsWhenSucceedWhenFails) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image = h.lower("module demo\nevent Tick\n"
                             "goal Work() achieve { succeed when 1 == 2 }\n"
                             "agent A {\n"
                             "  on Tick { adopt Work() }\n"
                             "  plan w for Work { }\n"
                             "}");
  ASSERT_TRUE(image);
  auto &arena     = h.ctx.arena;
  const auto iDim = h.ctx.core.findOrMintDimension("d.intention");

  // The empty plan "completes" — but an achieve goal's end without
  // `succeed when` is a failed plan: the intention faults onto the
  // ladder instead of retiring.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  const auto live = zigzag::rankAfter(arena, image->agentCell, iDim) |
                    std::ranges::to<std::vector>();
  ASSERT_EQ(live.size(), 1u);
  auto binds =
      mintMetaBindSet(arena, h.ctx.core, h.ctx.core.home(), image->agentCell);
  ASSERT_TRUE(binds);
  MetaRuntime meta(h.ctx.core, h.ctx.vm, *binds);
  EXPECT_EQ(meta.intentionField(live.front(), "faulted"), 1);
  EXPECT_EQ(meta.intentionField(live.front(), "faults"), 1);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, TellStagesACrossAgentAssertionInTheOutbox) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\n"
              "predicate reading(s: Str, v: Int)\n"
              "agent A { on Tick { tell parent reading(\"left\", 4) } }");
  ASSERT_TRUE(image);

  const auto outcome = exec(h.execution("Tick"), h.ctx);
  ASSERT_TRUE(outcome) << static_cast<int>(outcome.error());

  // §3.7 staging: the tell is an outbox record with the peer on
  // target and the marshaled term on payload — receiver-side assert
  // is the adapter's delivery work.
  const auto outbox =
      VagentExecutor::drainOutbox(h.ctx.arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 1u);
  EXPECT_EQ(outbox[0].kind, "tell");
  EXPECT_EQ(outbox[0].target, "parent");
  EXPECT_EQ(outbox[0].payload, "reading");
  EXPECT_EQ(h.ctx.arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, ToldFactAssertsInTheReceiversOwnTurn) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\n"
              "predicate reading(s: Str, v: Int)\n"
              "agent A { on Tick { tell self reading(\"left\", 4) } }");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;

  // `tell self` resolves the implicit to A's member name at stage
  // time; route → deliver stages the inbox record; the receiver's
  // own settle asserts the fact.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  const auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 1u);
  EXPECT_EQ(outbox[0].target, "A");
  const auto deliveries = VagentExecutor::route(
      arena, h.ctx.core, h.ctx.core.home(), outbox.front());
  ASSERT_EQ(deliveries.size(), 1u);
  EXPECT_EQ(deliveries.front().kind, "tell");
  exec.deliver(arena, h.ctx.core, deliveries.front());

  ASSERT_TRUE(exec(h.execution(), h.ctx));
  auto binds =
      mintMetaBindSet(arena, h.ctx.core, h.ctx.core.home(), image->agentCell);
  ASSERT_TRUE(binds) << binds.error();
  MetaRuntime meta(h.ctx.core, h.ctx.vm, *binds);
  const auto facts = meta.beliefsOf("reading");
  ASSERT_EQ(facts.size(), 1u);
  const auto grab =
      arena.linked(facts[0], h.ctx.core.dims().grab, DimVector::POS);
  ASSERT_TRUE(grab.has_value());
  EXPECT_EQ(arena.textOf(*grab), "reading");
  const auto args = zigzag::rankAfter(arena, *grab, h.ctx.core.dims().step) |
                    std::ranges::to<std::vector>();
  ASSERT_EQ(args.size(), 2u);
  EXPECT_EQ(arena.textOf(args[0]), "left");
  EXPECT_EQ(arena.textOf(args[1]), "4");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, BoundAdoptAndDropTaskRoundTripTheHandle) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Start\nevent Cancel\nevent Done\n"
              "event Grant\n"
              "goal Work() perform\n"
              "agent A {\n"
              "  var task: Opt<Intention> := none\n"
              "  on Start { task := adopt Work() }\n"
              "  on Cancel { drop task; emit Done{} }\n"
              "  plan w for Work { await Grant as g within 9s }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena     = h.ctx.arena;
  const auto iDim = h.ctx.core.findOrMintDimension("d.intention");

  // Turn 1 — `task := adopt` hoists: the child intention is adopted
  // AND the agent's vars record `task` holds its ref as a scalar.
  ASSERT_TRUE(exec(h.execution("Start"), h.ctx));
  const auto parked = zigzag::rankAfter(arena, image->agentCell, iDim) |
                      std::ranges::to<std::vector>();
  ASSERT_EQ(parked.size(), 1u);
  const auto taskRec =
      varsRecord(arena, h.ctx.core, image->agentCell, "task", false);
  ASSERT_TRUE(taskRec.has_value());
  const auto handle =
      arena.linked(*taskRec, h.ctx.core.dims().values, DimVector::POS);
  ASSERT_TRUE(handle.has_value());
  EXPECT_EQ(arena.asInt64(*handle).value_or(-1),
            static_cast<std::int64_t>(parked.front()));

  // Turn 2 — `drop task` resolves the handle back to the record and
  // cancels it; the same spine still emits Done.
  ASSERT_TRUE(exec(h.execution("Cancel"), h.ctx));
  EXPECT_TRUE(zigzag::rankAfter(arena, image->agentCell, iDim).empty());
  const auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 1u);
  EXPECT_EQ(outbox[0].payload, "Done");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, SpawnMintsTheChildAndRouteResolvesIt) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image = h.lower("module demo\nevent Boot\nevent Ping\nevent Pong\n"
                             "agent A {\n"
                             "  var child: Opt<Agent> := none\n"
                             "  on Boot { child := spawn A() in inner;\n"
                             "            emit Ping{} for child;\n"
                             "            emit Pong{} in inner }\n"
                             "}");
  ASSERT_TRUE(image);
  auto &arena       = h.ctx.arena;
  const auto agents = h.ctx.core.findOrMintDimension("d.agents");
  const auto dirs   = h.ctx.core.findOrMintDimension("d.dirs");

  ASSERT_TRUE(exec(h.execution("Boot"), h.ctx));

  // The spawn minted a real directory member with its own $-seeds.
  CellRef dirCell = noCell;
  for (const CellRef d : zigzag::rankAfter(arena, h.ctx.core.home(), dirs))
    dirCell = d;
  ASSERT_NE(dirCell, noCell);
  // A's static image member plus the spawned instance sit on the
  // directory rank — M0 lowers one agent decl, so the spawned child
  // is a second instance of A's own declaration.
  const auto members = zigzag::rankAfter(arena, dirCell, agents) |
                       std::ranges::to<std::vector>();
  ASSERT_EQ(members.size(), 2u);
  const auto childRec =
      varsRecord(arena, h.ctx.core, image->agentCell, "child", false);
  ASSERT_TRUE(childRec.has_value());
  const auto bound =
      arena.linked(*childRec, h.ctx.core.dims().values, DimVector::POS);
  ASSERT_TRUE(bound.has_value());
  const auto spawned = static_cast<CellRef>(arena.asInt64(*bound).value_or(-1));
  // The bound handle is the spawned member — an A-named cell that
  // isn't the running agent's own cell.
  const auto spawnedName =
      arena.linked(spawned, h.ctx.core.dims().name, DimVector::POS);
  ASSERT_TRUE(spawnedName.has_value());
  EXPECT_EQ(arena.textOf(*spawnedName), "A");
  EXPECT_NE(spawned, image->agentCell);

  // The spawn stages its wire request first — the member cell rides
  // as the payload so the drain can noteSpawnRequest and send — and
  // it is never a percept: route refuses it outright.
  const auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 3u);
  EXPECT_EQ(outbox[0].kind, "spawn");
  EXPECT_EQ(outbox[0].payloadCell, spawned);
  EXPECT_FALSE(outbox[0].id.empty());
  EXPECT_TRUE(VagentExecutor::route(arena, h.ctx.core, h.ctx.core.home(),
                                    outbox[0])
                  .empty());

  // The staged emit resolves `for child` to the member's name at
  // stage time; route matches every A-named member — sender and
  // spawned child both receive the percept.
  const auto deliveries = VagentExecutor::route(
      arena, h.ctx.core, h.ctx.core.home(), outbox[1]);
  EXPECT_EQ(deliveries.size(), 2u);
  EXPECT_TRUE(std::ranges::any_of(
      deliveries, [&](const Delivery &d) { return d.agent == spawned; }));
  EXPECT_TRUE(std::ranges::all_of(
      deliveries, [](const Delivery &d) { return d.percept == "Ping"; }));

  // `in inner` broadcasts into the space: the spawned member carries
  // +d.space "inner", the declared agent carries none — only the
  // child is in scope.
  EXPECT_EQ(outbox[2].scope, "inner");
  const auto scoped =
      VagentExecutor::route(arena, h.ctx.core, h.ctx.core.home(), outbox[2]);
  ASSERT_EQ(scoped.size(), 1u);
  EXPECT_EQ(scoped.front().agent, spawned);
  EXPECT_EQ(scoped.front().percept, "Pong");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, SpawnedMemberRunsItsOwnTurnOnItsSequence) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image = h.lower("module demo\nevent Boot\nevent Ping\nevent Done\n"
                             "agent A {\n"
                             "  var child: Opt<Agent> := none\n"
                             "  on Boot { child := spawn A() in inner;\n"
                             "            emit Ping{} for child }\n"
                             "  on Ping { emit Done{} }\n"
                             "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;

  // Boot spawns the second member and emits at it; `for child`
  // resolves to the A member name so both members get the percept.
  // The spawn's wire entry leads the outbox — [1] is the Ping stage.
  ASSERT_TRUE(exec(h.execution("Boot"), h.ctx));
  const auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 2u);
  EXPECT_EQ(outbox[0].kind, "spawn");
  const auto deliveries = VagentExecutor::route(
      arena, h.ctx.core, h.ctx.core.home(), outbox[1]);
  ASSERT_EQ(deliveries.size(), 2u);
  CellRef spawned = noCell;
  for (const auto &d : deliveries) {
    exec.deliver(arena, h.ctx.core, d);
    if (d.agent != image->agentCell) spawned = d.agent;
  }
  ASSERT_NE(spawned, noCell);

  // Sequence 1 is the declared agent; sequence 2 is the spawned
  // member — each runs its own settle over its own ranks. Both
  // dispatch the shared `on Ping` clause against their own outbox.
  ASSERT_TRUE(exec(h.execution(std::nullopt, 1), h.ctx));
  ASSERT_TRUE(exec(h.execution(std::nullopt, 2), h.ctx));
  const auto firstOut =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  const auto spawnedOut =
      VagentExecutor::drainOutbox(arena, h.ctx.core, spawned);
  // The parent's rank is spawn entry + its own Ping stage + Done;
  // the child's is only its own Done — no child minted a spawn.
  ASSERT_EQ(firstOut.size(), 3u);
  EXPECT_EQ(firstOut[2].payload, "Done");
  ASSERT_EQ(spawnedOut.size(), 1u);
  EXPECT_EQ(spawnedOut[0].payload, "Done");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, AwaitAsBindsTheFiredPerceptAgentSide) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\nevent Done\nevent Grant\n"
              "goal Wait() perform\n"
              "agent A {\n"
              "  on Tick { adopt Wait() }\n"
              "  plan w for Wait { await Grant as g within 9s;\n"
              "                    emit Done{} }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;

  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  ASSERT_TRUE(exec(h.execution("Grant"), h.ctx));

  // `as g` lands the fired percept's handle on the agent's vars —
  // the same cell the +d.seen journal holds.
  CellRef seen = noCell;
  for (const CellRef s : zigzag::rankAfter(
           arena, image->agentCell, h.ctx.core.findOrMintDimension("d.seen")))
    if (arena.textOf(s) == "Grant") seen = s;
  ASSERT_NE(seen, noCell);
  const auto gRec = varsRecord(arena, h.ctx.core, image->agentCell, "g", false);
  ASSERT_TRUE(gRec.has_value());
  const auto gVal =
      arena.linked(*gRec, h.ctx.core.dims().values, DimVector::POS);
  ASSERT_TRUE(gVal.has_value());
  EXPECT_EQ(arena.asInt64(*gVal).value_or(-1), static_cast<std::int64_t>(seen));
  EXPECT_TRUE(zigzag::rankAfter(arena, image->agentCell,
                                h.ctx.core.findOrMintDimension("d.intention"))
                  .empty());
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, DeliveredIdDeduplicatesRepeatedDelivery) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image = h.lower("module demo\nevent Tick\nevent Ping\n"
                             "agent A { on Tick { emit Ping{} } }");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;

  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  const auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 1u);
  const auto deliveries = VagentExecutor::route(
      arena, h.ctx.core, h.ctx.core.home(), outbox.front());
  ASSERT_EQ(deliveries.size(), 1u);

  // §3.7's at-least-once-then-detect: delivering the same entry
  // twice journals the id and skips the second staging entirely.
  exec.deliver(arena, h.ctx.core, deliveries.front());
  exec.deliver(arena, h.ctx.core, deliveries.front());
  const auto seen =
      zigzag::rankAfter(arena, image->agentCell,
                        h.ctx.core.findOrMintDimension("d.seen")) |
      std::ranges::to<std::vector>();
  const auto pending =
      zigzag::rankAfter(arena, image->agentCell,
                        h.ctx.core.findOrMintDimension("d.pending")) |
      std::ranges::to<std::vector>();
  const auto delivered =
      zigzag::rankAfter(arena, image->agentCell,
                        h.ctx.core.findOrMintDimension("d.delivered")) |
      std::ranges::to<std::vector>();
  // The journal also holds the Tick message itself — the duplicate
  // check is on the Ping entries the delivery staged.
  const auto pings = std::ranges::count_if(
      seen, [&](CellRef c) { return arena.textOf(c) == "Ping"; });
  EXPECT_EQ(pings, 1);
  EXPECT_EQ(pending.size(), 1u);
  ASSERT_EQ(delivered.size(), 1u);
  EXPECT_EQ(arena.textOf(delivered.front()), outbox.front().id);
}

TEST(VagentExecutorTest, BeliefAwaitTestsTheBankAndWakesOnTheFact) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\nevent Done\n"
              "predicate price(item: Str, qty: Int)\n"
              "goal Work() perform\n"
              "agent A {\n"
              "  on Tick { adopt Work() }\n"
              "  plan w for Work { await price(\"oil\", 4) within 9s;\n"
              "                    emit Done{} }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;

  // Tick parks the intention — the price bank is empty.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  const auto intentionDim = h.ctx.core.findOrMintDimension("d.intention");
  const auto parked = zigzag::rankAfter(arena, image->agentCell, intentionDim) |
                      std::ranges::to<std::vector>();
  ASSERT_EQ(parked.size(), 1u);

  // A price("oil", 5) fact doesn't satisfy the structural match —
  // the marshal's own tell-term minting makes the delivered cell.
  const auto tell = [&](std::string_view qty) {
    const auto term = arena.makeCell("price");
    zigzag::expectWritten(arena.link(
        zigzag::rankTail(arena, term, h.ctx.core.dims().step),
        h.ctx.core.dims().step, DimVector::POS, arena.makeCell("oil")));
    zigzag::expectWritten(
        arena.link(zigzag::rankTail(arena, term, h.ctx.core.dims().step),
                   h.ctx.core.dims().step, DimVector::POS,
                   arena.makeCell(std::string{qty})));
    exec.deliver(arena, h.ctx.core,
                 Delivery{.agent       = image->agentCell,
                          .kind        = "tell",
                          .payloadCell = term,
                          .sender      = image->agentCell});
  };
  tell("5");
  ASSERT_TRUE(exec(h.execution(std::nullopt), h.ctx));
  EXPECT_TRUE(zigzag::rankAfter(arena, image->agentCell,
                                h.ctx.core.findOrMintDimension("d.outbox"))
                  .empty());

  // The matching fact wakes the park into the emit — receiver-side
  // belief assertion and the belief-form scan run in the same settle.
  tell("4");
  ASSERT_TRUE(exec(h.execution(std::nullopt), h.ctx));
  const auto out =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out.front().payload, "Done");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, BeliefAwaitSatisfiedImmediatelyNeverParks) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\nevent Done\n"
              "predicate price(item: Str, qty: Int)\n"
              "goal Work() perform\n"
              "agent A {\n"
              "  on Tick { believe price(\"oil\", 4); adopt Work() }\n"
              "  plan w for Work { await price(\"oil\", 4) within 9s;\n"
              "                    emit Done{} }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;

  // The handler's believe asserts the fact; the adopted plan's await
  // step sees it in the Tick settle and advances without parking —
  // but §3.5's one-member-per-settle stepping means the emit rides
  // the next settle.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  EXPECT_TRUE(zigzag::rankAfter(arena, image->agentCell,
                                h.ctx.core.findOrMintDimension("d.outbox"))
                  .empty());
  ASSERT_TRUE(exec(h.execution(std::nullopt), h.ctx));
  const auto out =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out.front().payload, "Done");
  EXPECT_TRUE(zigzag::rankAfter(arena, image->agentCell,
                                h.ctx.core.findOrMintDimension("d.intention"))
                  .empty());
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, MaintainMonitorsHoldsAndFiresARestorer) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\nevent Drop\n"
              "predicate level(value: Int)\n"
              "goal Enough() maintain { holds believes level(?v) from self }\n"
              "agent A {\n"
              "  on Tick { adopt Enough() }\n"
              "  on Drop { forget level(9) }\n"
              "  plan restore for Enough { believe level(9) }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena             = h.ctx.arena;
  const auto intentionDim = h.ctx.core.findOrMintDimension("d.intention");
  auto binds =
      mintMetaBindSet(arena, h.ctx.core, h.ctx.core.home(), image->agentCell);
  ASSERT_TRUE(binds) << binds.error();
  MetaRuntime meta(h.ctx.core, h.ctx.vm, *binds);
  const auto levelHolds = [&](std::string_view value) {
    const auto t = arena.makeCell("level");
    zigzag::expectWritten(arena.link(t, h.ctx.core.dims().step, DimVector::POS,
                                     arena.makeCell(std::string{value})));
    return meta.believes("level", t);
  };

  // The monitor mints on Tick; the same settle sees the empty bank
  // and fires the restorer as a delegated subgoal.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  EXPECT_EQ((zigzag::rankAfter(arena, image->agentCell, intentionDim) |
             std::ranges::to<std::vector>())
                .size(),
            2u);

  // The restorer runs the plan and retires — its delegator write-
  // back readies the monitor, whose re-check now sees the fact.
  ASSERT_TRUE(exec(h.execution(std::nullopt), h.ctx));
  ASSERT_TRUE(exec(h.execution(std::nullopt), h.ctx));
  EXPECT_TRUE(levelHolds("9"));

  // Only the monitor survives — the child is off the rank and no
  // new restorer fired once holds was satisfied.
  const auto live = zigzag::rankAfter(arena, image->agentCell, intentionDim) |
                    std::ranges::to<std::vector>();
  ASSERT_EQ(live.size(), 1u);

  // A lapse refires the cycle: forget removes the fact, the next
  // settle's monitor sees holds fail and fires another restorer.
  ASSERT_TRUE(exec(h.execution("Drop"), h.ctx));
  ASSERT_TRUE(exec(h.execution(std::nullopt), h.ctx));
  ASSERT_TRUE(exec(h.execution(std::nullopt), h.ctx));
  EXPECT_TRUE(levelHolds("9"));
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, MaintainStaysDormantWhileHoldsIsTrue) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\n"
              "predicate level(value: Int)\n"
              "goal Enough() maintain { holds believes level(?v) from self }\n"
              "agent A {\n"
              "  on Tick { believe level(9); adopt Enough() }\n"
              "  plan restore for Enough { believe level(0) }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;

  // The fact already held at adoption — the monitor never fires the
  // restorer: one intention, no delegation, no level(0) belief.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  ASSERT_TRUE(exec(h.execution(std::nullopt), h.ctx));
  const auto live =
      zigzag::rankAfter(arena, image->agentCell,
                        h.ctx.core.findOrMintDimension("d.intention")) |
      std::ranges::to<std::vector>();
  ASSERT_EQ(live.size(), 1u);
  const auto subgoal = h.ctx.core.findOrMintDimension("d.subgoal");
  EXPECT_TRUE(zigzag::rankAfter(arena, live.front(), subgoal).empty());
  auto binds =
      mintMetaBindSet(arena, h.ctx.core, h.ctx.core.home(), image->agentCell);
  ASSERT_TRUE(binds) << binds.error();
  MetaRuntime meta(h.ctx.core, h.ctx.vm, *binds);
  const auto levelHolds = [&](std::string_view value) {
    const auto t = arena.makeCell("level");
    zigzag::expectWritten(arena.link(t, h.ctx.core.dims().step, DimVector::POS,
                                     arena.makeCell(std::string{value})));
    return meta.believes("level", t);
  };
  EXPECT_TRUE(levelHolds("9"));
  EXPECT_FALSE(levelHolds("0"));
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, MaintainHoldsConjunctsAndTheScalarHalf) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\n"
              "predicate level(value: Int)\n"
              "goal Fails() maintain { holds believes level(?v) from self & "
              "3 > 4 }\n"
              "goal Passes() maintain { holds believes level(?v) from self "
              "& 3 > 2 }\n"
              "agent A {\n"
              "  on Tick { believe level(9); adopt Fails(); adopt Passes() }\n"
              "  plan fix for Fails { believe level(0) }\n"
              "  plan noop for Passes { }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena             = h.ctx.arena;
  const auto intentionDim = h.ctx.core.findOrMintDimension("d.intention");
  auto binds =
      mintMetaBindSet(arena, h.ctx.core, h.ctx.core.home(), image->agentCell);
  ASSERT_TRUE(binds) << binds.error();
  MetaRuntime meta(h.ctx.core, h.ctx.vm, *binds);
  const auto levelHolds = [&](std::string_view value) {
    const auto t = arena.makeCell("level");
    zigzag::expectWritten(arena.link(t, h.ctx.core.dims().step, DimVector::POS,
                                     arena.makeCell(std::string{value})));
    return meta.believes("level", t);
  };

  // level(9) already held at adoption: Passes's `& 3 > 2` remainder
  // keeps it dormant while Fails's `& 3 > 4` fires its restorer
  // anyway — the scalar half ANDs with the belief conjunct.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  EXPECT_EQ((zigzag::rankAfter(arena, image->agentCell, intentionDim) |
             std::ranges::to<std::vector>())
                .size(),
            3u);
  ASSERT_TRUE(exec(h.execution(std::nullopt), h.ctx));
  EXPECT_TRUE(levelHolds("0"));

  // The scalar half still fails on the re-check — Fails refires on
  // the next settle rather than staying dormant.
  ASSERT_TRUE(exec(h.execution(std::nullopt), h.ctx));
  EXPECT_EQ((zigzag::rankAfter(arena, image->agentCell, intentionDim) |
             std::ranges::to<std::vector>())
                .size(),
            3u);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, MaintainHoldsCapturedNamesFeedTheScalarHalf) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\n"
              "predicate level(value: Int)\n"
              "goal Low() maintain { holds believes level(?v) from self & "
              "v > 5 }\n"
              "goal High() maintain { holds believes level(?v) from self "
              "& v > 10 }\n"
              "agent A {\n"
              "  on Tick { believe level(9); adopt Low(); adopt High() }\n"
              "  plan bump for High { believe level(11) }\n"
              "  plan noop for Low { }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena             = h.ctx.arena;
  const auto intentionDim = h.ctx.core.findOrMintDimension("d.intention");
  const auto count        = [&] {
    return (zigzag::rankAfter(arena, image->agentCell, intentionDim) |
            std::ranges::to<std::vector>())
        .size();
  };

  // level(9): Low's `v > 5` holds — dormant monitor. High's `v > 10`
  // rejects the same capture, so its restorer fires: two monitors
  // plus the delegated bump intention.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  EXPECT_EQ(count(), 3u);
  ASSERT_TRUE(exec(h.execution(std::nullopt), h.ctx));

  // bump asserted level(11) beside level(9): the re-check unifies
  // ?v → 9, the scalar half rejects it, and the walk backtracks to
  // level(11) — `v > 10` holds on the second env and High goes
  // dormant instead of refiring forever.
  ASSERT_TRUE(exec(h.execution(std::nullopt), h.ctx));
  ASSERT_TRUE(exec(h.execution(std::nullopt), h.ctx));
  EXPECT_EQ(count(), 2u);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, MaintainHoldsVariablesConstrainShareAndWild) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\n"
              "predicate pair(a: Int, b: Int)\n"
              "predicate tag(v: Int)\n"
              "goal Same() maintain { holds believes pair(?v, ?v) from "
              "self & believes tag(?v) from self }\n"
              "goal Wild() maintain { holds believes pair(?_, ?w) from "
              "self & w == 4 }\n"
              "agent A {\n"
              "  on Tick { believe pair(3, 4); believe tag(3); "
              "adopt Same(); adopt Wild() }\n"
              "  plan fix for Same { believe pair(3, 3) }\n"
              "  plan noop for Wild { }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena             = h.ctx.arena;
  const auto intentionDim = h.ctx.core.findOrMintDimension("d.intention");
  const auto count        = [&] {
    return (zigzag::rankAfter(arena, image->agentCell, intentionDim) |
            std::ranges::to<std::vector>())
        .size();
  };

  // pair(3, 4): Same's second `?v` is a constraint, not a rebind —
  // 4 ≠ 3 fails the conjunct, so its restorer fires. Wild's `?_`
  // binds nothing, `?w` takes 4, and `w == 4` keeps Wild dormant:
  // Same's monitor and restorer plus Wild's monitor.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  EXPECT_EQ(count(), 3u);

  // fix asserted pair(3, 3): now `?v` binds 3 at both positions and
  // the shared env carries v = 3 into tag(?v), which tag(3)
  // satisfies — the conjunction holds on the shared capture.
  ASSERT_TRUE(exec(h.execution(std::nullopt), h.ctx));
  ASSERT_TRUE(exec(h.execution(std::nullopt), h.ctx));
  ASSERT_TRUE(exec(h.execution(std::nullopt), h.ctx));
  EXPECT_EQ(count(), 2u);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, InitialBeliefSatisfiesMaintainMonitors) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\n"
              "predicate level(value: Int)\n"
              "goal Low() maintain { holds believes level(5) from self }\n"
              "agent A {\n"
              "  believe level(5)\n"
              "  on Tick { adopt Low() }\n"
              "  plan restore for Low { believe level(9) }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena             = h.ctx.arena;
  const auto intentionDim = h.ctx.core.findOrMintDimension("d.intention");
  const auto count        = [&] {
    return (zigzag::rankAfter(arena, image->agentCell, intentionDim) |
            std::ranges::to<std::vector>())
        .size();
  };

  // The member `believe level(5)` minted at image time — the maintain
  // check sees the creation fact and Low stays a dormant monitor:
  // restore's restorer never fires. Without the initial fact the
  // check would fail and the plan would spawn.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  EXPECT_EQ(count(), 1u);
  ASSERT_TRUE(exec(h.execution(std::nullopt), h.ctx));
  ASSERT_TRUE(exec(h.execution(std::nullopt), h.ctx));
  EXPECT_EQ(count(), 1u);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, OnInitializeFiresOnceAheadOfTheFirstPercept) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\nevent Ready\nevent Ping\n"
              "agent A {\n"
              "  on Initialize { emit Ready{} }\n"
              "  on Tick { emit Ping{} }\n"
              "}");
  ASSERT_TRUE(image);

  // Creation order lands `on Initialize` on the agent's first exec
  // turn, ahead of that turn's own percept — the outbox reads
  // [Ready, Ping], not [Ping, Ready].
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  auto out = VagentExecutor::drainOutbox(h.ctx.arena, h.ctx.core,
                                       image->agentCell);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0].payload, "Ready");
  EXPECT_EQ(out[1].payload, "Ping");

  // The $initialized flag record is durable — later turns never
  // re-fire it.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  out = VagentExecutor::drainOutbox(h.ctx.arena, h.ctx.core,
                                    image->agentCell);
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[2].payload, "Ping");
  EXPECT_EQ(h.ctx.arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, GoalDroppedRaiseDispatchesTheHandlerNextTurn) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Start\nevent Cancel\nevent Done\n"
              "event Grant\nevent Down\n"
              "goal Work() perform\n"
              "agent A {\n"
              "  var task: Opt<Intention> := none\n"
              "  on Start { task := adopt Work() }\n"
              "  on Cancel { drop task; emit Done{} }\n"
              "  on GoalDropped [occurrence.goal == \"Work\"] { emit "
              "Down{} }\n"
              "  plan w for Work { await Grant as g within 9s }\n"
              "}");
  ASSERT_TRUE(image);

  ASSERT_TRUE(exec(h.execution("Start"), h.ctx));
  // Turn 2 — `drop task` unlinks the intention and raises GoalDropped:
  // journaled now, enqueued on +d.pending so the handler dispatches
  // through the drain rather than inside the settling spine. A raise
  // during percept dispatch drains in this same turn — only raises
  // inside the intention walk wait for the next.
  ASSERT_TRUE(exec(h.execution("Cancel"), h.ctx));
  auto out = VagentExecutor::drainOutbox(h.ctx.arena, h.ctx.core,
                                       image->agentCell);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0].payload, "Done");
  // The guard reads `goal` off the marshaled payload's named fields.
  EXPECT_EQ(out[1].payload, "Down");
  EXPECT_EQ(h.ctx.arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, GoalAchievedRaiseDispatchesOnRetire) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Start\nevent Won\n"
              "goal G() perform\n"
              "agent A {\n"
              "  on Start { adopt G() }\n"
              "  on GoalAchieved [occurrence.goal == \"G\"] { emit "
              "Won{} }\n"
              "  plan p for G { }\n"
              "}");
  ASSERT_TRUE(image);

  // The plan completes and the intention retires on the Start turn
  // itself; GoalAchieved's handler runs the turn after.
  ASSERT_TRUE(exec(h.execution("Start"), h.ctx));
  auto out = VagentExecutor::drainOutbox(h.ctx.arena, h.ctx.core,
                                       image->agentCell);
  EXPECT_TRUE(out.empty());
  ASSERT_TRUE(exec(h.execution(std::nullopt), h.ctx));
  out = VagentExecutor::drainOutbox(h.ctx.arena, h.ctx.core,
                                    image->agentCell);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].payload, "Won");
}

TEST(VagentExecutorTest, PlanFailedRaiseCarriesTheFaultCause) {
  ExecutorHarness h;
  VagentExecutor exec{[](std::string_view key)
                          -> std::optional<std::int64_t> {
    if (key == "vagent.intention_backoff") return 0;
    return std::nullopt;
  }};
  const auto image =
      h.lower("module demo\nevent Start\nevent PF\n"
              "goal Work() perform\n"
              "goal Missing() perform\n"
              "agent A {\n"
              "  on Start { adopt Work() }\n"
              "  on PlanFailed [occurrence.cause == \"body\"] { emit "
              "PF{} }\n"
              "  plan w for Work { adopt Missing() }\n"
              "}");
  ASSERT_TRUE(image);

  // The plan body's adopt fails — the fault raises
  // PlanFailed{intention, plan, cause} and the ladder parks for
  // retry; the handler dispatches the turn after.
  ASSERT_TRUE(exec(h.execution("Start"), h.ctx));
  ASSERT_TRUE(exec(h.execution(std::nullopt), h.ctx));
  const auto out = VagentExecutor::drainOutbox(h.ctx.arena, h.ctx.core,
                                              image->agentCell);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].payload, "PF");
}

TEST(VagentExecutorTest, GoalFailedRaisesOnceAtTheFaultCap) {
  ExecutorHarness h;
  VagentExecutor exec{[](std::string_view key)
                          -> std::optional<std::int64_t> {
    if (key == "vagent.intention_backoff") return 0;
    if (key == "vagent.max_intention_faults") return 2;
    return std::nullopt;
  }};
  const auto image =
      h.lower("module demo\nevent Start\nevent GF\n"
              "goal Work() perform\n"
              "goal Missing() perform\n"
              "agent A {\n"
              "  on Start { adopt Work() }\n"
              "  on GoalFailed [occurrence.goal == \"Work\"] { emit "
              "GF{} }\n"
              "  plan w for Work { adopt Missing() }\n"
              "}");
  ASSERT_TRUE(image);

  // backoff 0 retries the member each settle; at the second fault the
  // goal is out of plans — GoalFailed raises once (the parked record
  // stays inspectable) and the handler fires next turn.
  ASSERT_TRUE(exec(h.execution("Start"), h.ctx));
  ASSERT_TRUE(exec(h.execution(std::nullopt), h.ctx));
  ASSERT_TRUE(exec(h.execution(std::nullopt), h.ctx));
  ASSERT_TRUE(exec(h.execution(std::nullopt), h.ctx));
  auto out = VagentExecutor::drainOutbox(h.ctx.arena, h.ctx.core,
                                       image->agentCell);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].payload, "GF");

  // The flag record gates the raise: later settles never re-fire it.
  ASSERT_TRUE(exec(h.execution(std::nullopt), h.ctx));
  ASSERT_TRUE(exec(h.execution(std::nullopt), h.ctx));
  out = VagentExecutor::drainOutbox(h.ctx.arena, h.ctx.core,
                                    image->agentCell);
  EXPECT_EQ(out.size(), 1u);
  EXPECT_EQ(h.ctx.arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, ConditionFaultRaisesOnceAcrossRefusals) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Start\nevent Grant\n"
              "goal G() achieve { succeed when 1 > 0 }\n"
              "agent A {\n"
              "  on Start { adopt G() }\n"
              "  plan p for G { await Grant as g within 9s }\n"
              "}");
  ASSERT_TRUE(image);
  const auto seenDim = h.ctx.core.findOrMintDimension("d.seen");
  const auto journal = [&] {
    std::vector<std::string> names;
    for (const CellRef cell :
         zigzag::rankAfter(h.ctx.arena, image->agentCell, seenDim))
      names.emplace_back(h.ctx.arena.textOf(cell));
    return names;
  };

  // Turn 1: adopt parks on the await.
  ASSERT_TRUE(exec(h.execution("Start"), h.ctx));

  // Turn 2: Grant wakes the spine past its end, where the succeedTest
  // runs under a starved grant — a VmFault, not a failure — so §6.4's
  // isolation parks the intention "unchanged" and raises
  // ConditionFault once.
  auto starved = h.execution("Grant");
  starved.grant.maxInstructions = 1;
  ASSERT_TRUE(exec(starved, h.ctx));

  // Turn 3: still starved, the re-test refuses again — the flag gates
  // the second raise. (No `on` handler exists for either lifecycle
  // event, so the pending drain runs no fragments a starved turn
  // could fault on.)
  starved                     = h.execution(std::nullopt);
  starved.grant.maxInstructions = 1;
  ASSERT_TRUE(exec(starved, h.ctx));
  EXPECT_EQ(std::ranges::count(journal(), "ConditionFault"), 1);

  // Turn 4: clean grant — the test produces, the intention retires.
  ASSERT_TRUE(exec(h.execution(std::nullopt), h.ctx));
  const auto names = journal();
  EXPECT_EQ(std::ranges::count(names, "ConditionFault"), 1);
  EXPECT_EQ(std::ranges::count(names, "GoalAchieved"), 1);
  EXPECT_EQ(h.ctx.arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, RecordLiteralsEmitAndFieldAccessReads) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\nevent Hit\n"
              "record Point { x: Int, y: Int }\n"
              "agent A {\n"
              "  on Tick [Point{x: 9, y: 0}.x > 5] { emit Hit{} }\n"
              "  on Tick [Point{x: 2, y: 0}.x > 5] { emit Hit{} }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena         = h.ctx.arena;
  const auto outboxAt = h.ctx.core.findOrMintDimension("d.outbox");
  const auto count    = [&] {
    return (zigzag::rankAfter(arena, image->agentCell, outboxAt) |
            std::ranges::to<std::vector>())
        .size();
  };
  // The record literal mints a term cell (functor + +d.name-labelled
  // +d.step fields); the guard's `.x` reads it through #FIND — the
  // first guard's 9 > 5 fires, the second's 2 > 5 fails.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  EXPECT_EQ(count(), 1u);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, RecordArgsMarshalIntoBeliefFacts) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\n"
              "record Point { x: Int, y: Int }\n"
              "predicate at(site: Point)\n"
              "goal Near() maintain { holds believes at(Point{x: ?v, "
              "y: ?w}) from self & v == 1 & w == 2 }\n"
              "goal Far() maintain { holds believes at(Point{x: ?v, "
              "y: ?w}) from self & v == 9 }\n"
              "agent A {\n"
              "  on Tick { believe at(Point{x: 1, y: 2}); "
              "adopt Near(); adopt Far() }\n"
              "  plan noop for Near { }\n"
              "  plan fix for Far { }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena             = h.ctx.arena;
  const auto intentionDim = h.ctx.core.findOrMintDimension("d.intention");
  const auto count        = [&] {
    return (zigzag::rankAfter(arena, image->agentCell, intentionDim) |
            std::ranges::to<std::vector>())
        .size();
  };
  // The record fact at(Point{x:1,y:2}) flattens to the same +d.step
  // spelling the pattern carries: ?v captures 1, ?w captures 2, and
  // Near's scalar half holds while Far's `v == 9` fires its
  // restorer — two monitors plus the delegated fix.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  EXPECT_EQ(count(), 3u);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, PickedStreaksJournalOnStatsEachTurn) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\nagent A { on Tick { } }");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;

  const auto statField = [&](std::string_view name) -> std::int64_t {
    const auto rec =
        varsRecord(arena, h.ctx.core, image->agentCell, "$stats", false);
    if (!rec) return -1;
    const auto cell =
        arena.linked(*rec, h.ctx.core.dims().values, DimVector::POS);
    if (!cell) return -1;
    const auto field = varsRecord(arena, h.ctx.core, *cell, name, false);
    if (!field) return -1;
    const auto v =
        arena.linked(*field, h.ctx.core.dims().values, DimVector::POS);
    return v ? arena.asInt64(*v).value_or(-1) : -1;
  };

  auto turn = h.execution("Tick");
  turn.wakeStreak = 0;
  turn.msgStreak  = 2;
  ASSERT_TRUE(exec(turn, h.ctx));
  auto wake = h.execution(std::nullopt);
  wake.wakeStreak = 1;
  wake.msgStreak  = 0;
  ASSERT_TRUE(exec(wake, h.ctx));

  EXPECT_EQ(statField("wakeStreak"), 1);
  EXPECT_EQ(statField("msgStreak"), 0);
  EXPECT_EQ(statField("msgs"), 1);
  EXPECT_EQ(statField("wakes"), 1);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, AskElseSuppliesTheValueOnExpiry) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\nevent Done\n"
              "question stock(item: Str): Int\n"
              "goal Work() perform\n"
              "agent A {\n"
              "  on Tick { adopt Work() }\n"
              "  plan w for Work { let r := ask self.stock(\"oil\") within 1s\n"
              "                    else -1; emit Done{} }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena     = h.ctx.arena;
  const auto iDim = h.ctx.core.findOrMintDimension("d.intention");

  // Turn 1 — adopt runs the spine's ask step: the request stages and
  // the intention parks on awaitDeadline = turn+1.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  {
    const auto parked = zigzag::rankAfter(arena, image->agentCell, iDim) |
                        std::ranges::to<std::vector>();
    ASSERT_EQ(parked.size(), 1u);
    auto binds =
        mintMetaBindSet(arena, h.ctx.core, h.ctx.core.home(), image->agentCell);
    ASSERT_TRUE(binds);
    MetaRuntime meta(h.ctx.core, h.ctx.vm, *binds);
    EXPECT_EQ(meta.intentionField(parked.front(), "awaitDeadline"), 2);
  }

  // Turn 2 — a bare wake at turn 2 meets the deadline: the else head
  // produces -1, r binds it, and the wait completes rather than
  // faulting.
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  const auto r =
      varsRecord(arena, h.ctx.core, image->agentCell, "r", false);
  ASSERT_TRUE(r.has_value());
  const auto rv = arena.linked(*r, h.ctx.core.dims().values, DimVector::POS);
  ASSERT_TRUE(rv.has_value());
  EXPECT_EQ(arena.asInt64(*rv).value_or(0), -1);

  // Turn 3 — the readied spine advances and emits Done.
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  EXPECT_TRUE(zigzag::rankAfter(arena, image->agentCell, iDim).empty());
  const auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  EXPECT_TRUE(std::ranges::any_of(
      outbox, [](const OutboxEntry &e) {
        return e.payload == "Done";
      }));
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, AwaitElseCompletesAnExpiredBareWait) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\nevent Grant\nevent Done\n"
              "goal Work() perform\n"
              "agent A {\n"
              "  on Tick { adopt Work() }\n"
              "  plan w for Work { await Grant as g within 1s else 0;\n"
              "                    emit Done{} }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena     = h.ctx.arena;
  const auto iDim = h.ctx.core.findOrMintDimension("d.intention");

  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  // Expiry runs the else for effect: the bound name gets the produced
  // value and the plan completes — no fault lands on the record.
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  {
    const auto parked = zigzag::rankAfter(arena, image->agentCell, iDim) |
                        std::ranges::to<std::vector>();
    ASSERT_EQ(parked.size(), 1u);
    auto binds =
        mintMetaBindSet(arena, h.ctx.core, h.ctx.core.home(), image->agentCell);
    ASSERT_TRUE(binds);
    MetaRuntime meta(h.ctx.core, h.ctx.vm, *binds);
    EXPECT_EQ(meta.intentionField(parked.front(), "faulted"), 0);
  }
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  EXPECT_TRUE(zigzag::rankAfter(arena, image->agentCell, iDim).empty());
  const auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  EXPECT_TRUE(std::ranges::any_of(
      outbox, [](const OutboxEntry &e) {
        return e.payload == "Done";
      }));
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, AdoptArgsBindPositionallyIntoTheWhereGuard) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\nevent High\nevent Low\n"
              "goal Work(v: Int) perform\n"
              "agent A {\n"
              "  on Tick { adopt Work(4) }\n"
              "  plan a for Work(v) where v > 3 { emit High{} }\n"
              "  plan b for Work(v) where v <= 3 { emit Low{} }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;

  // Tick adopts Work(4): the args chain stores 4, selectClause binds
  // it positionally into each where-guard — `v > 3` selects plan a.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  const auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 1u);
  EXPECT_EQ(outbox[0].payload, "High");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, UnlowerableBooleanConditionsRefuseNotStub) {
  ExecutorHarness h;
  // `u` is bound by nothing — the scalar remainder has no meaning
  // for it and must refuse rather than stub `true`.
  EXPECT_FALSE(h.lower(
      "module demo\npredicate level(value: Int)\n"
      "goal Enough() maintain { holds believes level(?v) from self & "
      "u >= 1 }\n"
      "agent A { plan fix for Enough { believe level(9) } }"));
  // A `where` guard is the same Boolean test position — a belief
  // query there refuses instead of silently passing selection.
  EXPECT_FALSE(h.lower(
      "module demo\nevent Tick\npredicate p(value: Int)\n"
      "goal Work() achieve { succeed when 1 == 1 }\n"
      "agent A { plan p for Work where believes p(?x) { } }"));
}

TEST(VagentExecutorTest, BoundAskWakesOnDeliveredAnswerAndBindsIt) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image = h.lower(
      "module demo\nevent Tick\nevent Done\n"
      "question stock(item: Str): Int\n"
      "goal Outer() perform\n"
      "agent A {\n"
      "  on Tick { adopt Outer() }\n"
      "  plan o for Outer { let r := ask parent.stock(\"oil\") within 9s;\n"
      "                     emit Done{} }\n"
      "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;

  // Turn 1 — the ask stages and parks; `let r` hoisted its result
  // name onto the step, which the park copies to awaitBind.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  ASSERT_EQ(
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell).size(),
      1u);

  // The adapter's answer lands through route's write half — a
  // correlated journal entry plus a pending trigger on the
  // receiver's own ranks; the correlation is the request's outbox
  // id, and the answer value rides the payload cell.
  const auto request =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  exec.deliver(arena, h.ctx.core,
               Delivery{.agent       = image->agentCell,
                        .percept     = "stock",
                        .payloadCell =
                            arena.makeScalarCell(std::int64_t{7}),
                        .correlation = request.front().id});
  ASSERT_TRUE(exec(h.execution(), h.ctx));

  // The seen entry matched the parked ask, the pending entry
  // dispatched, and the bound name holds the answer's value — a
  // correlated reply binds what the receiver produced, not a handle.
  const auto rRec = varsRecord(arena, h.ctx.core, image->agentCell, "r", false);
  ASSERT_TRUE(rRec.has_value());
  const auto rVal =
      arena.linked(*rRec, h.ctx.core.dims().values, DimVector::POS);
  ASSERT_TRUE(rVal.has_value());
  EXPECT_EQ(arena.asInt64(*rVal).value_or(-1), 7);

  // The spine resumed into emit and the intention retired.
  const auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 2u);
  EXPECT_EQ(outbox[1].payload, "Done");
  EXPECT_TRUE(zigzag::rankAfter(arena, image->agentCell,
                                h.ctx.core.findOrMintDimension("d.intention"))
                  .empty());
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, AchieveByStagesRemotelyAndWithinBoundsTheWait) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\n"
              "goal Outer() perform\n"
              "goal Inner() perform\n"
              "agent A {\n"
              "  on Tick { adopt Outer() }\n"
              "  plan o for Outer { achieve Inner() by parent within 2s }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena     = h.ctx.arena;
  const auto iDim = h.ctx.core.findOrMintDimension("d.intention");
  auto binds =
      mintMetaBindSet(arena, h.ctx.core, h.ctx.core.home(), image->agentCell);
  ASSERT_TRUE(binds) << binds.error();
  MetaRuntime meta(h.ctx.core, h.ctx.vm, *binds);

  // Turn 1 — the remote achieve stages {kind,target,payload,id} for
  // the adapter and parks the parent delegated on the within bound.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  const auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 1u);
  EXPECT_EQ(outbox[0].kind, "achieve");
  EXPECT_EQ(outbox[0].target, "parent");
  EXPECT_EQ(outbox[0].payload, "Inner");
  const auto live = zigzag::rankAfter(arena, image->agentCell, iDim) |
                    std::ranges::to<std::vector>();
  ASSERT_EQ(live.size(), 1u); // the delegating parent alone — no local child
  EXPECT_EQ(meta.intentionField(live.front(), "delegated"), 1);
  EXPECT_EQ(meta.intentionField(live.front(), "awaitDeadline"), 3);

  // Turns 2–3 — the bound expires without a delivered completion:
  // the parent un-parks into the fault ladder.
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  EXPECT_EQ(meta.intentionField(live.front(), "delegated"), 1);
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  EXPECT_EQ(meta.intentionField(live.front(), "delegated"), 0);
  EXPECT_EQ(meta.intentionField(live.front(), "faulted"), 1);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, RemoteAchieveAdoptsReceiverSideAndCompletes) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\nevent ChildDone\nevent Done\n"
              "goal Deliver() perform\n"
              "goal Outer() perform\n"
              "agent A {\n"
              "  on Tick { adopt Outer() }\n"
              "  plan d for Deliver { emit ChildDone{} }\n"
              "  plan o for Outer { achieve Deliver() by self within 9s;\n"
              "                     emit Done{} }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena     = h.ctx.arena;
  const auto iDim = h.ctx.core.findOrMintDimension("d.intention");

  // Turn 1 — the remote achieve stages with `self` resolved to the
  // member name and the parent's ref riding `delegator`.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  const auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 1u);
  EXPECT_EQ(outbox[0].kind, "achieve");
  EXPECT_EQ(outbox[0].target, "A");
  EXPECT_EQ(outbox[0].payload, "Deliver");
  const auto parentRef = outbox[0].delegator;
  ASSERT_NE(parentRef, noCell);

  // The adapter routes and delivers the delegation — the receiver's
  // own turn owns the adoption.
  const auto deliveries = VagentExecutor::route(
      arena, h.ctx.core, h.ctx.core.home(), outbox.front());
  ASSERT_EQ(deliveries.size(), 1u);
  exec.deliver(arena, h.ctx.core, deliveries.front());

  // Turn 2 — the inbox drain adopts Deliver as the parked parent's
  // subgoal; the child runs in the same settle, emits ChildDone and
  // retires, which readies the parent.
  ASSERT_TRUE(exec(h.execution(), h.ctx));

  // Turn 3 — the parent resumes its spine into emit Done.
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  const auto after =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(after.size(), 3u);
  EXPECT_EQ(after[1].payload, "ChildDone");
  EXPECT_EQ(after[2].payload, "Done");
  EXPECT_TRUE(zigzag::rankAfter(arena, image->agentCell, iDim).empty());
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, SpawnSeedsParentLineageAndKinAcceptDelegation) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Boot\nevent ChildDone\nevent Done\n"
              "goal Deliver() perform\n"
              "goal Outer() perform\n"
              "agent A {\n"
              "  var child: Opt<Agent> := none\n"
              "  on Boot { child := spawn A(); adopt Outer() }\n"
              "  plan d for Deliver { emit ChildDone{} }\n"
              "  plan o for Outer { achieve Deliver() by child within 9s;\n"
              "                     emit Done{} }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena     = h.ctx.arena;
  const auto iDim = h.ctx.core.findOrMintDimension("d.intention");

  // Boot spawns the member and parks Outer on the delegation; the
  // `by child` name resolves to A, so self and the spawned member
  // both receive the achieve — and both accept it: self trivially,
  // the spawned member because §7.1's default admits the parent.
  ASSERT_TRUE(exec(h.execution("Boot"), h.ctx));
  const auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  // The spawn's wire entry leads; [1] is the delegation's stage.
  ASSERT_EQ(outbox.size(), 2u);
  EXPECT_EQ(outbox[0].kind, "spawn");
  EXPECT_EQ(outbox[1].kind, "achieve");
  const auto deliveries = VagentExecutor::route(
      arena, h.ctx.core, h.ctx.core.home(), outbox[1]);
  ASSERT_EQ(deliveries.size(), 2u);
  CellRef spawned = noCell;
  for (const auto &d : deliveries) {
    exec.deliver(arena, h.ctx.core, d);
    if (d.agent != image->agentCell) spawned = d.agent;
  }
  ASSERT_NE(spawned, noCell);

  // The spawn seeded the lineage record the accepts default reads.
  const auto parent =
      varsRecord(arena, h.ctx.core, spawned, "$parent", false);
  ASSERT_TRUE(parent.has_value());
  const auto pv =
      arena.linked(*parent, h.ctx.core.dims().values, DimVector::POS);
  ASSERT_TRUE(pv.has_value());
  EXPECT_EQ(arena.asInt64(*pv).value_or(0),
            static_cast<std::int64_t>(image->agentCell));

  // Sequence 2 is the spawned member: its turn adopts Deliver —
  // the child runs the shared plan and finishes, which readies the
  // delegating parent on the declared agent's rank.
  ASSERT_TRUE(exec(h.execution(std::nullopt, 2), h.ctx));
  const auto spawnedOut =
      VagentExecutor::drainOutbox(arena, h.ctx.core, spawned);
  ASSERT_EQ(spawnedOut.size(), 1u);
  EXPECT_EQ(spawnedOut[0].payload, "ChildDone");
  EXPECT_TRUE(zigzag::rankAfter(arena, spawned, iDim).empty());

  // The declared agent's own delivery adopted a local child the same
  // way; its completion plus the remote child's ready the parent.
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  const auto after =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(after.size(), 4u);
  // Rank order settles the readied parent ahead of the freshly
  // adopted local child — Done before its own ChildDone; [0]/[1]
  // are the spawn and achieve stages.
  EXPECT_EQ(after[2].payload, "Done");
  EXPECT_EQ(after[3].payload, "ChildDone");
  EXPECT_TRUE(zigzag::rankAfter(arena, image->agentCell, iDim).empty());
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, DelegatedAchieveRefusedByNonKinRunsTheElse) {
  ExecutorHarness h;
  VagentExecutor exec;
  // §7.1: accepts(sender, goal_name) defaults to the lineage — a
  // declared Stranger is no kin to A, so the delegation comes back
  // `refused` through the same outbox the ask reply rides.
  const auto image =
      h.lower("module demo\nevent Tick\nevent Done\n"
              "goal Work() perform\n"
              "goal Outer() perform\n"
              "agent A {\n"
              "  var stranger: Opt<Agent> := none\n"
              "  on Tick { adopt Outer() }\n"
              "  plan w for Work { }\n"
              "  plan o for Outer { achieve Work() by stranger "
              "within 9s else cause;\n"
              "                     emit Done{} }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena     = h.ctx.arena;
  const auto iDim = h.ctx.core.findOrMintDimension("d.intention");
  auto binds =
      mintMetaBindSet(arena, h.ctx.core, h.ctx.core.home(), image->agentCell);
  ASSERT_TRUE(binds) << binds.error();
  MetaRuntime meta(h.ctx.core, h.ctx.vm, *binds);

  // M0 lowers one agent decl, so the non-kin member is a directory
  // entry the adapter would have registered — a named rank member
  // carrying no $parent lineage — and `by stranger` reaches it
  // through the bound var exactly like a spawn-result handle.
  const auto agents   = h.ctx.core.findOrMintDimension("d.agents");
  const auto dirs     = h.ctx.core.findOrMintDimension("d.dirs");
  CellRef dirCell     = noCell;
  for (const CellRef d :
       zigzag::rankAfter(arena, h.ctx.core.home(), dirs))
    dirCell = d;
  ASSERT_NE(dirCell, noCell);
  const auto stranger = arena.makeCell();
  zigzag::expectWritten(
      arena.link(zigzag::rankTail(arena, dirCell, agents), agents,
                 DimVector::POS, stranger));
  zigzag::expectWritten(
      arena.link(stranger, h.ctx.core.dims().name, DimVector::POS,
                 arena.makeCell("Stranger")));
  const auto var =
      varsRecord(arena, h.ctx.core, image->agentCell, "stranger", true);
  ASSERT_TRUE(var.has_value());
  zigzag::expectWritten(
      arena.link(*var, h.ctx.core.dims().values, DimVector::POS,
                 arena.makeScalarCell(static_cast<std::int64_t>(stranger))));

  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  const auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 1u);
  EXPECT_EQ(outbox[0].kind, "achieve");
  const auto delegator = outbox[0].delegator;
  ASSERT_NE(delegator, noCell);
  const auto requests = VagentExecutor::route(
      arena, h.ctx.core, h.ctx.core.home(), outbox[0]);
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests.front().agent, stranger);
  exec.deliver(arena, h.ctx.core, requests.front());

  // Stranger's turn (sequence 2 — the second declared member): the
  // accepts default admits no adoption, so instead of an intention
  // its outbox carries a `refuse` addressed to the sender's name
  // with the parked intention echoed as the delegator handle.
  ASSERT_TRUE(exec(h.execution(std::nullopt, 2), h.ctx));
  EXPECT_TRUE(zigzag::rankAfter(arena, stranger, iDim).empty());
  const auto refusals =
      VagentExecutor::drainOutbox(arena, h.ctx.core, stranger);
  ASSERT_EQ(refusals.size(), 1u);
  EXPECT_EQ(refusals[0].kind, "refuse");
  EXPECT_EQ(refusals[0].target, "A");
  EXPECT_EQ(refusals[0].delegator, delegator);

  const auto replies = VagentExecutor::route(
      arena, h.ctx.core, h.ctx.core.home(), refusals[0]);
  ASSERT_EQ(replies.size(), 1u);
  exec.deliver(arena, h.ctx.core, replies.front());

  // The refusal marks the parked intention; its settle runs the
  // `else` with cause `refused` — recovery, not a fault — and the
  // spine resumes on the next turn like every else path.
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  EXPECT_EQ(meta.intentionField(delegator, "faulted").value_or(0), 0);
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  const auto after =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(after.size(), 2u);
  EXPECT_EQ(after[1].payload, "Done");
  EXPECT_TRUE(zigzag::rankAfter(arena, image->agentCell, iDim).empty());
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, StopTargetsOnlySelfOrSpawnedChildren) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Boot\nevent Tock\nevent Done\n"
              "agent A {\n"
              "  var child: Opt<Agent> := none\n"
              "  var stranger: Opt<Agent> := none\n"
              "  on Boot { child := spawn A(); stop child; emit Done{} }\n"
              "  on Tock { stop stranger }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  const auto agents = h.ctx.core.findOrMintDimension("d.agents");
  const auto dirs   = h.ctx.core.findOrMintDimension("d.dirs");
  CellRef dirCell   = noCell;
  for (const CellRef d :
       zigzag::rankAfter(arena, h.ctx.core.home(), dirs))
    dirCell = d;
  ASSERT_NE(dirCell, noCell);
  const auto stranger = arena.makeCell();
  zigzag::expectWritten(
      arena.link(zigzag::rankTail(arena, dirCell, agents), agents,
                 DimVector::POS, stranger));
  zigzag::expectWritten(
      arena.link(stranger, h.ctx.core.dims().name, DimVector::POS,
                 arena.makeCell("Stranger")));
  const auto var =
      varsRecord(arena, h.ctx.core, image->agentCell, "stranger", true);
  ASSERT_TRUE(var.has_value());
  zigzag::expectWritten(
      arena.link(*var, h.ctx.core.dims().values, DimVector::POS,
                 arena.makeScalarCell(static_cast<std::int64_t>(stranger))));

  // `stop child` — the spawned member's $parent is the running
  // agent, so §4.5's orderly stop applies to it.
  ASSERT_TRUE(exec(h.execution("Boot"), h.ctx));
  CellRef spawned   = noCell;
  for (const CellRef dir : zigzag::rankAfter(arena, h.ctx.core.home(), dirs))
    for (const CellRef m : zigzag::rankAfter(arena, dir, agents))
      if (m != image->agentCell &&
          arena.textOf(*arena.linked(m, h.ctx.core.dims().name,
                                     DimVector::POS)) == "A")
        spawned = m;
  ASSERT_NE(spawned, noCell);
  const auto flag =
      varsRecord(arena, h.ctx.core, spawned, "destroying", false);
  ASSERT_TRUE(flag.has_value());
  EXPECT_EQ(arena.asInt64(*arena.linked(*flag, h.ctx.core.dims().values,
                                        DimVector::POS))
                .value_or(0),
            1);

  // `stop stranger` — no lineage either way; the host step refuses
  // and the member carries no destroy flag.
  EXPECT_FALSE(exec(h.execution("Tock"), h.ctx));
  EXPECT_FALSE(
      varsRecord(arena, h.ctx.core, stranger, "destroying", false)
          .has_value());
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, AskStagesTheQuestionAndWakesOnTheAnswer) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\nevent Done\n"
              "question stock(item: Str): Int\n"
              "goal Outer() perform\n"
              "agent A {\n"
              "  on Tick { adopt Outer() }\n"
              "  plan o for Outer { ask parent.stock(\"oil\") within 2s;\n"
              "                     emit Done{} }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena     = h.ctx.arena;
  const auto iDim = h.ctx.core.findOrMintDimension("d.intention");

  // Turn 1 — the ask stages the {kind,target,payload,id} request and
  // parks the intention waiting on a same-named answer percept.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  const auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 1u);
  EXPECT_EQ(outbox[0].kind, "ask");
  EXPECT_EQ(outbox[0].target, "parent");
  EXPECT_EQ(outbox[0].payload, "stock");
  ASSERT_EQ((zigzag::rankAfter(arena, image->agentCell, iDim) |
             std::ranges::to<std::vector>())
                .size(),
            1u);

  // Turn 2 — the adapter's answer echoes the request id as its
  // correlation; an uncorrelated same-named percept does NOT wake
  // the asker first.
  exec.deliver(arena, h.ctx.core,
               Delivery{.agent = image->agentCell, .percept = "stock"});
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  ASSERT_EQ((zigzag::rankAfter(arena, image->agentCell, iDim) |
             std::ranges::to<std::vector>())
                .size(),
            1u)
      << "uncorrelated percept must not wake the correlated ask";

  exec.deliver(arena, h.ctx.core,
               Delivery{.agent       = image->agentCell,
                        .percept     = "stock",
                        .correlation = outbox[0].id});
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  const auto after =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(after.size(), 2u);
  EXPECT_EQ(after[1].payload, "Done");
  EXPECT_TRUE(zigzag::rankAfter(arena, image->agentCell, iDim).empty());
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, AskRunsTheReceiversAnswerAndBindsTheReply) {
  ExecutorHarness h;
  VagentExecutor exec;
  // The §7.1 round trip: the request lands in the receiver's inbox,
  // its own turn runs `answer stock` and stages the reply, and the
  // sender's wake binds the produced value to `r`.
  const auto image =
      h.lower("module demo\nevent Tick\nevent Done\n"
              "question stock(item: Str): Int\n"
              "goal Outer() perform\n"
              "agent A {\n"
              "  on Tick { adopt Outer() }\n"
              "  answer stock(item: Str): Int { 42 }\n"
              "  plan o for Outer { let r := ask self.stock(\"oil\") "
              "within 9s else -1;\n"
              "                     emit Done{} }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena     = h.ctx.arena;
  const auto iDim = h.ctx.core.findOrMintDimension("d.intention");

  // Turn 1 — the ask stages and parks on the request id.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 1u);
  EXPECT_EQ(outbox[0].kind, "ask");
  EXPECT_EQ(outbox[0].target, "A");

  // The adapter delivers the request; the receiver's own turn runs
  // the answer head and stages the correlated reply.
  const auto requests = VagentExecutor::route(
      arena, h.ctx.core, h.ctx.core.home(), outbox[0]);
  ASSERT_EQ(requests.size(), 1u);
  exec.deliver(arena, h.ctx.core, requests.front());
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 2u);
  EXPECT_EQ(outbox[1].kind, "answer");
  EXPECT_EQ(outbox[1].percept, "stock");
  EXPECT_EQ(outbox[1].correlation, outbox[0].id);
  EXPECT_TRUE(outbox[1].fault.empty());

  const auto replies = VagentExecutor::route(
      arena, h.ctx.core, h.ctx.core.home(), outbox[1]);
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_EQ(replies[0].percept, "stock");
  EXPECT_EQ(replies[0].correlation, outbox[0].id);
  exec.deliver(arena, h.ctx.core, replies.front());

  // Turn 3 — the wake binds the answer's VALUE: `r` reads 42, not a
  // handle, and the resumed spine emits Done.
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  const auto r =
      varsRecord(arena, h.ctx.core, image->agentCell, "r", false);
  ASSERT_TRUE(r.has_value());
  const auto rv = arena.linked(*r, h.ctx.core.dims().values, DimVector::POS);
  ASSERT_TRUE(rv.has_value());
  EXPECT_EQ(arena.asInt64(*rv).value_or(0), 42);
  outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  EXPECT_TRUE(std::ranges::any_of(outbox, [](const OutboxEntry &e) {
    return e.payload == "Done";
  }));
  EXPECT_TRUE(zigzag::rankAfter(arena, image->agentCell, iDim).empty());
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, AskToAnUnansweredQuestionRepliesWithTheFault) {
  ExecutorHarness h;
  VagentExecutor exec;
  // No `answer ghost` exists — §7.1's receiver default replies the
  // fault `unanswered`, which runs the asker's `else` with that cause
  // rather than binding a payload.
  const auto image =
      h.lower("module demo\nevent Tick\nevent Done\n"
              "question ghost(item: Str): Str\n"
              "goal Outer() perform\n"
              "agent A {\n"
              "  on Tick { adopt Outer() }\n"
              "  plan o for Outer { let r := ask self.ghost(\"x\") "
              "within 9s else cause;\n"
              "                     emit Done{} }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena     = h.ctx.arena;
  const auto iDim = h.ctx.core.findOrMintDimension("d.intention");

  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 1u);
  const auto requests = VagentExecutor::route(
      arena, h.ctx.core, h.ctx.core.home(), outbox[0]);
  ASSERT_EQ(requests.size(), 1u);
  exec.deliver(arena, h.ctx.core, requests.front());
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 2u);
  EXPECT_EQ(outbox[1].kind, "answer");
  EXPECT_EQ(outbox[1].fault, "unanswered");

  const auto replies = VagentExecutor::route(
      arena, h.ctx.core, h.ctx.core.home(), outbox[1]);
  ASSERT_EQ(replies.size(), 1u);
  exec.deliver(arena, h.ctx.core, replies.front());
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  // The else bound `r` to the cause text, and the wait completed
  // without faulting the intention — the readied spine resumes on
  // the next settle like every else recovery.
  const auto r =
      varsRecord(arena, h.ctx.core, image->agentCell, "r", false);
  ASSERT_TRUE(r.has_value());
  const auto rv = arena.linked(*r, h.ctx.core.dims().values, DimVector::POS);
  ASSERT_TRUE(rv.has_value());
  EXPECT_EQ(arena.textOf(*rv), "unanswered");
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  EXPECT_TRUE(std::ranges::any_of(outbox, [](const OutboxEntry &e) {
    return e.payload == "Done";
  }));
  EXPECT_TRUE(zigzag::rankAfter(arena, image->agentCell, iDim).empty());
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, AnswerOverThe32KiBBoundAnswersRemoteFault) {
  ExecutorHarness h;
  VagentExecutor exec;
  // §7.1: the answer must fit 32 KiB — the wire's ActorSend value
  // cap. A bigger inline result replies `remote_fault` rather than
  // shipping a frame the adapter must refuse anyway.
  const auto big = std::string(33 * 1024, 'x');
  const auto image =
      h.lower("module demo\nevent Tick\nevent Done\n"
              "question big(): Str\n"
              "goal Outer() perform\n"
              "agent A {\n"
              "  on Tick { adopt Outer() }\n"
              "  answer big(): Str { \"" + big + "\" }\n"
              "  plan o for Outer { ask self.big() within 9s; emit Done{} }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;

  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 1u);
  const auto requests = VagentExecutor::route(
      arena, h.ctx.core, h.ctx.core.home(), outbox[0]);
  ASSERT_EQ(requests.size(), 1u);
  exec.deliver(arena, h.ctx.core, requests.front());
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 2u);
  EXPECT_EQ(outbox[1].kind, "answer");
  EXPECT_EQ(outbox[1].fault, "remote_fault");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, AskWithAMalformedTermAnswersRemoteFault) {
  ExecutorHarness h;
  VagentExecutor exec;
  // A request whose term arity mismatches the answer head refuses
  // the fragment — the reply is the typed fault `remote_fault`, not
  // silence the asker would wait out on the deadline.
  const auto image =
      h.lower("module demo\nevent Tick\nevent Done\n"
              "question stock(item: Str): Str\n"
              "goal Outer() perform\n"
              "agent A {\n"
              "  on Tick { adopt Outer() }\n"
              "  answer stock(item: Str): Str { \"full\" }\n"
              "  plan o for Outer { let r := ask self.stock(\"oil\") "
              "within 9s else cause;\n"
              "                     emit Done{} }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena     = h.ctx.arena;
  const auto iDim = h.ctx.core.findOrMintDimension("d.intention");

  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 1u);
  // A two-arg `stock` term: one arg over the declared arity.
  const auto bad = arena.makeCell("stock");
  for (const auto arg : {"a", "b"})
    zigzag::expectWritten(
        arena.link(zigzag::rankTail(arena, bad, h.ctx.core.dims().step),
                   h.ctx.core.dims().step, DimVector::POS,
                   arena.makeCell(arg)));
  exec.deliver(arena, h.ctx.core,
               Delivery{.agent       = image->agentCell,
                        .id          = outbox[0].id,
                        .kind        = "ask",
                        .payloadCell = bad,
                        .sender      = image->agentCell});
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 2u);
  EXPECT_EQ(outbox[1].kind, "answer");
  EXPECT_EQ(outbox[1].fault, "remote_fault");

  const auto replies = VagentExecutor::route(
      arena, h.ctx.core, h.ctx.core.home(), outbox[1]);
  ASSERT_EQ(replies.size(), 1u);
  exec.deliver(arena, h.ctx.core, replies.front());
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  const auto r =
      varsRecord(arena, h.ctx.core, image->agentCell, "r", false);
  ASSERT_TRUE(r.has_value());
  const auto rv = arena.linked(*r, h.ctx.core.dims().values, DimVector::POS);
  ASSERT_TRUE(rv.has_value());
  EXPECT_EQ(arena.textOf(*rv), "remote_fault");
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  EXPECT_TRUE(std::ranges::any_of(outbox, [](const OutboxEntry &e) {
    return e.payload == "Done";
  }));
  EXPECT_TRUE(zigzag::rankAfter(arena, image->agentCell, iDim).empty());
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, StopSelfHaltsEveryLaterTurn) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image = h.lower("module demo\nevent Tick\nevent Done\n"
                             "agent A { on Tick { stop self; emit Done{} } }");
  ASSERT_TRUE(image);
  auto &arena     = h.ctx.arena;
  const auto jDim = h.ctx.core.findOrMintDimension("d.seen");

  // Turn 1 — the spine completes: stop marks destroying and raises
  // Destroy, emit still lands (statement-boundary semantics).
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 1u);
  EXPECT_EQ(outbox[0].payload, "Done");
  // [Initialize, Tick, Destroy] — the orderly stop journals its
  // percept like any other.
  EXPECT_EQ((zigzag::rankAfter(arena, image->agentCell, jDim) |
             std::ranges::to<std::vector>())
                .size(),
            3u);

  // Turn 2 — a delivered Tick is neither journaled nor dispatched:
  // the destroying agent drains nothing and lands the stop.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  EXPECT_EQ((zigzag::rankAfter(arena, image->agentCell, jDim) |
             std::ranges::to<std::vector>())
                .size(),
            3u);
  const auto rec = varsRecord(arena, h.ctx.core, image->agentCell,
                              "stopped", false);
  ASSERT_TRUE(rec);
  const auto flag =
      arena.linked(*rec, h.ctx.core.dims().values, DimVector::POS);
  ASSERT_TRUE(flag && arena.asInt64(*flag).value_or(0) != 0);
  outbox = VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  EXPECT_EQ(outbox.size(), 1u);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, StopRunsOnDestroyBeforeHalting) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\nevent Bye\n"
              "agent A {\n"
              "  on Tick { stop self }\n"
              "  on Destroy { emit Bye{} }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;

  // Turn 1 — stop raises Destroy and the drain dispatches it in the
  // same turn: §4.5's "run `on Destroy`, then stop".
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 1u);
  EXPECT_EQ(outbox[0].payload, "Bye");

  // Later turns no-op — the stop landed once Destroy ran.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  outbox = VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  EXPECT_EQ(outbox.size(), 1u);
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
  // peer rides as scalar content — N clauses from one peer cannot
  // share the peer cell's single -d.values edge.
  const auto peerCell =
      arena.linked(*peerSlot, h.ctx.core.dims().values, DimVector::POS);
  ASSERT_TRUE(peerCell.has_value());
  EXPECT_EQ(arena.asInt64(*peerCell), std::optional{std::int64_t(peer)});
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

TEST(VagentBeliefTest, PeerProvenanceSurvivesSiblingClauses) {
  ExecutorHarness h;
  const auto image =
      h.lower("module demo\nevent Tick\nagent A { on Tick { } }");
  ASSERT_TRUE(image);
  auto binds = mintMetaBindSet(h.ctx.arena, h.ctx.core, h.ctx.core.home(),
                               image->agentCell);
  ASSERT_TRUE(binds) << binds.error();
  MetaRuntime meta(h.ctx.core, h.ctx.vm, *binds);

  auto &arena     = h.ctx.arena;
  const auto peer = arena.makeCell("peer-a");

  // Two clauses sourced from the same peer. A live +d.values → peer
  // edge would make them contend for the peer cell's single
  // -d.values slot — the second write silently displacing the
  // first's provenance. Scalar content keeps both.
  const auto wetClause = meta.believe("weather", arena.makeCell("wet"),
                                      peer, 1);
  const auto dryClause = meta.believe("weather", arena.makeCell("dry"),
                                      peer, 2);
  ASSERT_TRUE(wetClause && dryClause);
  const auto sourceDim = h.ctx.core.findDimension("d.source");
  ASSERT_TRUE(sourceDim.has_value());
  for (const auto *clause : {&*wetClause, &*dryClause}) {
    const auto source = arena.linked(*clause, *sourceDim, DimVector::POS);
    ASSERT_TRUE(source.has_value());
    const auto slot =
        varsRecord(arena, h.ctx.core, *source, "peer", false);
    ASSERT_TRUE(slot.has_value());
    const auto value =
        arena.linked(*slot, h.ctx.core.dims().values, DimVector::POS);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(arena.asInt64(*value),
              std::optional{std::int64_t(peer)});
  }
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

TEST(VagentExecutorTest, InstallMintsTheFrameAndRunsLifecycleAndCapcall) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\nevent Installed\nevent Done\n"
              "goal Work() perform\n"
              "capacity Navigation { def goto(x: Int): Int }\n"
              "skill Walker(level: Int) implements Navigation {\n"
              "  var speed: Int := level\n"
              "  on install { emit Installed{} }\n"
              "  def goto(x: Int): Int { x * 2 }\n"
              "}\n"
              "agent A {\n"
              "  uses Navigation\n"
              "  on Tick { adopt Work() }\n"
              "  plan w for Work {\n"
              "    install Walker(5)\n"
              "    let r := Navigation.goto(21)\n"
              "    emit Done{}\n"
              "  }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  // One spine member per settle: Tick runs install, the two wakes run
  // the capcall and the emit.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  ASSERT_TRUE(exec(h.execution(), h.ctx));

  // §5.3: one instance on +d.skill — +d.capacity names it a
  // Navigation, and its clone master is the frame, holding the
  // skill's name and the field the install arg seeded.
  const auto skillDim  = h.ctx.core.findOrMintDimension("d.skill");
  const auto capDim    = h.ctx.core.findOrMintDimension("d.capacity");
  const auto instances = zigzag::rankAfter(arena, image->agentCell, skillDim) |
                         std::ranges::to<std::vector>();
  ASSERT_EQ(instances.size(), 1u);
  const auto capName = arena.linked(instances.front(), capDim, DimVector::POS);
  ASSERT_TRUE(capName);
  EXPECT_EQ(arena.textOf(*capName), "Navigation");
  const auto frame =
      arena.cloneMaster(instances.front(), h.ctx.core.dims().clone);
  ASSERT_TRUE(frame);
  EXPECT_EQ(arena.textOf(*frame), "Walker");
  const auto speed = varsRecord(arena, h.ctx.core, *frame, "speed", false);
  ASSERT_TRUE(speed);
  const auto speedValue =
      arena.linked(*speed, h.ctx.core.dims().values, DimVector::POS);
  ASSERT_TRUE(speedValue);
  EXPECT_EQ(arena.asInt64(*speedValue), 5);

  // `r := Navigation.goto(21)` dispatched through the instance to
  // Walker.goto — the produced value lands on the agent's vars.
  const auto r = varsRecord(arena, h.ctx.core, image->agentCell, "r", false);
  ASSERT_TRUE(r);
  const auto rv = arena.linked(*r, h.ctx.core.dims().values, DimVector::POS);
  ASSERT_TRUE(rv);
  EXPECT_EQ(arena.asInt64(*rv), 42);

  // on install's emit staged before the plan's own Done.
  const auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 2u);
  EXPECT_EQ(outbox[0].payload, "Installed");
  EXPECT_EQ(outbox[1].payload, "Done");
}

TEST(VagentExecutorTest, InstallEvictsASkillSharingTheCapacityWhole) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\nevent Removed\nevent Done\n"
              "goal Work() perform\n"
              "capacity Navigation { def goto(x: Int): Int }\n"
              "skill Slow implements Navigation {\n"
              "  on uninstall { emit Removed{} }\n"
              "  def goto(x: Int): Int { x }\n"
              "}\n"
              "skill Fast implements Navigation {\n"
              "  def goto(x: Int): Int { x * 2 }\n"
              "}\n"
              "agent A {\n"
              "  uses Navigation\n"
              "  on Tick { adopt Work() }\n"
              "  plan w for Work {\n"
              "    install Slow()\n"
              "    install Fast()\n"
              "    let r := Navigation.goto(21)\n"
              "    emit Done{}\n"
              "  }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  // install / install / capcall / emit — four members, four settles.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  ASSERT_TRUE(exec(h.execution(), h.ctx));

  // §5.3's whole-removal: Fast shares Navigation, so Slow went
  // before Fast landed — one instance remains and it answers with
  // Fast.goto's doubling, not Slow's identity.
  const auto skillDim  = h.ctx.core.findOrMintDimension("d.skill");
  const auto instances = zigzag::rankAfter(arena, image->agentCell, skillDim) |
                         std::ranges::to<std::vector>();
  ASSERT_EQ(instances.size(), 1u);
  const auto frame =
      arena.cloneMaster(instances.front(), h.ctx.core.dims().clone);
  ASSERT_TRUE(frame);
  EXPECT_EQ(arena.textOf(*frame), "Fast");
  const auto r = varsRecord(arena, h.ctx.core, image->agentCell, "r", false);
  ASSERT_TRUE(r);
  const auto rv = arena.linked(*r, h.ctx.core.dims().values, DimVector::POS);
  ASSERT_TRUE(rv);
  EXPECT_EQ(arena.asInt64(*rv), 42);
  const auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 2u);
  EXPECT_EQ(outbox[0].payload, "Removed");
  EXPECT_EQ(outbox[1].payload, "Done");
}

TEST(VagentExecutorTest, UninstallRunsTheLifecycleAndClearsTheInstances) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\nevent Installed\nevent Removed\n"
              "event Done\n"
              "goal Work() perform\n"
              "capacity Navigation { def goto(x: Int): Int }\n"
              "skill Walker implements Navigation {\n"
              "  on install { emit Installed{} }\n"
              "  on uninstall { emit Removed{} }\n"
              "  def goto(x: Int): Int { x }\n"
              "}\n"
              "agent A {\n"
              "  uses Navigation\n"
              "  on Tick { adopt Work() }\n"
              "  plan w for Work {\n"
              "    install Walker()\n"
              "    uninstall Walker\n"
              "    emit Done{}\n"
              "  }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  // install / uninstall / emit — three members, three settles.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  ASSERT_TRUE(exec(h.execution(), h.ctx));

  const auto skillDim = h.ctx.core.findOrMintDimension("d.skill");
  EXPECT_TRUE(zigzag::rankAfter(arena, image->agentCell, skillDim).empty());
  const auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 3u);
  EXPECT_EQ(outbox[0].payload, "Installed");
  EXPECT_EQ(outbox[1].payload, "Removed");
  EXPECT_EQ(outbox[2].payload, "Done");
}

TEST(VagentExecutorTest, CapcallWithoutAnInstalledSkillRefusesTheStep) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image = h.lower("module demo\nevent Tick\nevent Done\n"
                             "goal Work() perform\n"
                             "capacity Navigation { def goto(x: Int): Int }\n"
                             "agent A {\n"
                             "  uses Navigation\n"
                             "  on Tick { adopt Work() }\n"
                             "  plan w for Work {\n"
                             "    let r := Navigation.goto(21)\n"
                             "    emit Done{}\n"
                             "  }\n"
                             "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));

  // Nothing installed provides Navigation — the step refuses, the
  // intention faults, and Done never stages.
  const auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  EXPECT_TRUE(outbox.empty());
  const auto iDim   = h.ctx.core.findOrMintDimension("d.intention");
  const auto parked = zigzag::rankAfter(arena, image->agentCell, iDim) |
                      std::ranges::to<std::vector>();
  ASSERT_EQ(parked.size(), 1u);
  auto binds =
      mintMetaBindSet(arena, h.ctx.core, h.ctx.core.home(), image->agentCell);
  ASSERT_TRUE(binds);
  MetaRuntime meta(h.ctx.core, h.ctx.vm, *binds);
  EXPECT_EQ(meta.intentionField(parked.front(), "faulted"), 1);
}

TEST(VagentExecutorTest, BehaviorMemberMountsAtCreationAndUnmounts) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image = h.lower(
      "module demo\nevent Tick\nevent Remove\nevent Counted\n"
      "behavior Reporter {\n"
      "  on Tick { emit Counted{} }\n"
      "}\n"
      "agent Host {\n"
      "  behavior Reporter\n"
      "  on Remove { unmount Reporter }\n"
      "}");
  ASSERT_TRUE(image);
  auto &arena      = h.ctx.arena;
  const auto bDim  = h.ctx.core.findOrMintDimension("d.behavior");
  const auto count = [&] {
    return VagentExecutor::drainOutbox(arena, h.ctx.core,
                                     image->agentCell)
        .size();
  };

  // Turn 1 mounts the member (the $create spine) before the Tick
  // dispatches — the behavior's own clause answers it.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  const auto instances =
      zigzag::rankAfter(arena, image->agentCell, bDim) |
      std::ranges::to<std::vector>();
  ASSERT_EQ(instances.size(), 1u);
  EXPECT_EQ(arena.textOf(instances.front()), "Reporter");
  EXPECT_EQ(count(), 1u);

  // §5.7's merge runs the mounted clause on every later Tick…
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  EXPECT_EQ(count(), 2u);

  // …until unmount splices the instance: the next Tick contributes
  // nothing.
  ASSERT_TRUE(exec(h.execution("Remove"), h.ctx));
  EXPECT_TRUE(
      zigzag::rankAfter(arena, image->agentCell, bDim).empty());
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  EXPECT_EQ(count(), 2u);
}

TEST(VagentExecutorTest, MountedBehaviorContributesAPlanToTheGoal) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image = h.lower(
      "module demo\nevent Tick\nevent Helped\n"
      "goal Work() perform\n"
      "behavior Helper {\n"
      "  plan h for Work { emit Helped{} }\n"
      "}\n"
      "agent A {\n"
      "  on Tick { mount Helper() ; adopt Work() }\n"
      "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;

  // mount and adopt are two spine members of the Tick clause; the
  // adopt's selectClause already merges the mounted instance's
  // per-goal table — the agent itself declares no plan for Work.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  // The adopted intention settles its plan body on the next turn.
  ASSERT_TRUE(exec(h.execution(), h.ctx));

  const auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 1u);
  EXPECT_EQ(outbox[0].payload, "Helped");
}

TEST(VagentExecutorTest, InstallMemberRunsBeforeInitialize) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image = h.lower(
      "module demo\nevent Installed\nevent Init\n"
      "capacity Navigation { def goto(x: Int): Int }\n"
      "skill Walker(level: Int) implements Navigation {\n"
      "  var speed: Int := level\n"
      "  on install { emit Installed{} }\n"
      "  def goto(x: Int): Int { x }\n"
      "}\n"
      "agent A {\n"
      "  uses Navigation\n"
      "  install Walker(7)\n"
      "  on Initialize { emit Init{} }\n"
      "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;

  // §8.5's creation order: the $create spine installs before
  // Initialize dispatches — even on a message-less first turn.
  ASSERT_TRUE(exec(h.execution(), h.ctx));

  const auto skillDim = h.ctx.core.findOrMintDimension("d.skill");
  const auto instances =
      zigzag::rankAfter(arena, image->agentCell, skillDim) |
      std::ranges::to<std::vector>();
  ASSERT_EQ(instances.size(), 1u);
  const auto frame =
      arena.cloneMaster(instances.front(), h.ctx.core.dims().clone);
  ASSERT_TRUE(frame);
  const auto speed =
      varsRecord(arena, h.ctx.core, *frame, "speed", false);
  ASSERT_TRUE(speed);
  const auto sv =
      arena.linked(*speed, h.ctx.core.dims().values, DimVector::POS);
  ASSERT_TRUE(sv);
  EXPECT_EQ(arena.asInt64(*sv), 7);

  const auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 2u);
  EXPECT_EQ(outbox[0].payload, "Installed");
  EXPECT_EQ(outbox[1].payload, "Init");
}

TEST(VagentExecutorTest, UnmountOfAMissingBehaviorRefusesTheTurn) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image = h.lower(
      "module demo\nevent Tick\n"
      "behavior Helper { on Tick { emit Tick{} } }\n"
      "agent A {\n"
      "  on Tick { unmount Helper }\n"
      "}");
  ASSERT_TRUE(image);
  // Nothing mounted — the verb's honest no faults the turn, the same
  // contract a refused `stop` or `uninstall` keeps.
  EXPECT_FALSE(exec(h.execution("Tick"), h.ctx));
}

TEST(VagentExecutorTest, MountedBehaviorFieldsCountTicksAcrossEvents) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image = h.lower(
      "module demo\nevent Tick\n"
      "behavior Counter {\n"
      "  var ticks: Int := 0\n"
      "  on Tick { ticks := ticks + 1 }\n"
      "}\n"
      "agent Host {\n"
      "  behavior Counter\n"
      "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  auto &core  = h.ctx.core;
  for (int i = 0; i < 3; ++i)
    ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  const auto inst = arena.linked(image->agentCell,
                                 core.findOrMintDimension("d.behavior"),
                                 DimVector::POS);
  ASSERT_TRUE(inst);
  const auto ticks = varsRecord(arena, core, *inst, "ticks", false);
  ASSERT_TRUE(ticks);
  const auto value = arena.linked(*ticks, core.dims().values, DimVector::POS);
  ASSERT_TRUE(value);
  EXPECT_EQ(arena.asInt64(*value).value_or(-1), 3);
}

TEST(VagentExecutorTest, MountedBehaviorInstancesKeepSeparateFields) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image = h.lower(
      "module demo\nevent Tick\n"
      "behavior Counter(start: Int) {\n"
      "  var ticks: Int := start\n"
      "  on Tick { ticks := ticks + 1 }\n"
      "}\n"
      "agent Host {\n"
      "  behavior Counter(10)\n"
      "  behavior Counter(100)\n"
      "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  auto &core  = h.ctx.core;
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  // One Tick bumped each instance through its own $owner — Counter(10)
  // reads 11, Counter(100) reads 101. Mount order is the rank's order.
  const auto bDim = core.findOrMintDimension("d.behavior");
  const auto insts =
      zigzag::rankAfter(arena, image->agentCell, bDim) |
      std::ranges::to<std::vector>();
  ASSERT_EQ(insts.size(), 2u);
  for (const auto [inst, want] : std::vector<std::pair<CellRef, int64_t>>{
           {insts[0], 11},
           {insts[1], 101}}) {
    const auto ticks = varsRecord(arena, core, inst, "ticks", false);
    ASSERT_TRUE(ticks);
    const auto value =
        arena.linked(*ticks, core.dims().values, DimVector::POS);
    ASSERT_TRUE(value);
    EXPECT_EQ(arena.asInt64(*value).value_or(-1), want);
  }
}

TEST(VagentExecutorTest, SkillMethodReadsAndWritesFrameFields) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image = h.lower(
      "module demo\nevent Tick\nevent Go\n"
      "capacity Navigation { def goto(x: Int): Int def probe(): Int }\n"
      "skill Walker implements Navigation {\n"
      "  var speed: Int := 0\n"
      "  on install { speed := 7 }\n"
      "  def goto(x: Int): Int { speed := x; speed }\n"
      "  def probe(): Int { speed }\n"
      "}\n"
      "agent Host {\n"
      "  uses Navigation\n"
      "  install Walker\n"
      "  on Tick { let r := Navigation.probe() }\n"
      "  on Go { let r := Navigation.goto(21) }\n"
      "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  auto &core  = h.ctx.core;
  const auto r = [&]() -> std::optional<std::int64_t> {
    const auto rec = varsRecord(arena, core, image->agentCell, "r", false);
    if (!rec) return std::nullopt;
    const auto v = arena.linked(*rec, core.dims().values, DimVector::POS);
    return v ? arena.asInt64(*v) : std::nullopt;
  };
  const auto speed = [&]() -> std::optional<std::int64_t> {
    const auto inst = arena.linked(image->agentCell,
                                   core.findOrMintDimension("d.skill"),
                                   DimVector::POS);
    if (!inst) return std::nullopt;
    const auto frame = arena.cloneMaster(*inst, core.dims().clone);
    if (!frame) return std::nullopt;
    const auto rec = varsRecord(arena, core, *frame, "speed", false);
    if (!rec) return std::nullopt;
    const auto v = arena.linked(*rec, core.dims().values, DimVector::POS);
    return v ? arena.asInt64(*v) : std::nullopt;
  };
  // The first turn runs the creation spine — `install Walker` mounts
  // the frame and `on install`'s `speed := 7` lands before the Tick
  // dispatch — then probe reads the field through $owner.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  EXPECT_EQ(speed().value_or(-1), 7);
  EXPECT_EQ(r().value_or(-1), 7);
  // goto writes then reads the same field; the frame's record proves
  // the write persisted on the instance, not a frame-local copy.
  ASSERT_TRUE(exec(h.execution("Go"), h.ctx));
  EXPECT_EQ(r().value_or(-1), 21);
  EXPECT_EQ(speed().value_or(-1), 21);
}

TEST(VagentExecutorTest, EffectDoneResumesTheParkedIntentionWithTheResult) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image = h.lower(
      "module demo\nevent Tick\ngoal Work() perform\n"
      "capacity Navigation { effect def read(): Int within 3s }\n"
      "native skill Device implements Navigation\n"
      "agent A {\n"
      "  uses Navigation\n"
      "  install Device\n"
      "  on Tick { adopt Work() }\n"
      "  plan w for Work { let r := Navigation.read() }\n"
      "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  auto &core  = h.ctx.core;
  // Tick adopts Work; the effect step stages the outbox entry and
  // parks the intention on the request's correlation.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  const auto staged =
      VagentExecutor::drainOutbox(arena, core, image->agentCell);
  ASSERT_EQ(staged.size(), 1u);
  EXPECT_EQ(staged[0].kind, "effect");
  // The adapter's answer: EffectDone carrying the result, correlated
  // to the staged request — the parked wait binds it like an ask's.
  exec.deliver(arena, core,
               Delivery{.agent       = image->agentCell,
                        .percept     = "EffectDone",
                        .payloadCell = arena.makeScalarCell(std::int64_t{42}),
                        .correlation = staged[0].id});
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  const auto r = varsRecord(arena, core, image->agentCell, "r", false);
  ASSERT_TRUE(r);
  const auto rv = arena.linked(*r, core.dims().values, DimVector::POS);
  ASSERT_TRUE(rv);
  EXPECT_EQ(arena.asInt64(*rv).value_or(-1), 42);
}

TEST(VagentExecutorTest, EffectFailedRunsTheElseLadderWithTheCause) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image = h.lower(
      "module demo\nevent Tick\ngoal Work() perform\n"
      "capacity Navigation { effect def read(): Int within 3s }\n"
      "native skill Device implements Navigation\n"
      "agent A {\n"
      "  uses Navigation\n"
      "  install Device\n"
      "  on Tick { adopt Work() }\n"
      "  plan w for Work { let r := Navigation.read() else 0 }\n"
      "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  auto &core  = h.ctx.core;
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  const auto staged =
      VagentExecutor::drainOutbox(arena, core, image->agentCell);
  ASSERT_EQ(staged.size(), 1u);
  // The binding's busy refusal rides the percept's fault field — the
  // wake routes it into the else ladder as `cause`.
  exec.deliver(arena, core,
               Delivery{.agent       = image->agentCell,
                        .percept     = "EffectFailed",
                        .correlation = staged[0].id,
                        .fault       = "busy"});
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  const auto r = varsRecord(arena, core, image->agentCell, "r", false);
  ASSERT_TRUE(r);
  const auto rv = arena.linked(*r, core.dims().values, DimVector::POS);
  ASSERT_TRUE(rv);
  EXPECT_EQ(arena.asInt64(*rv).value_or(-1), 0);
}

TEST(VagentExecutorTest, EffectDeadlineStagesCancelBeforeTheLadder) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image = h.lower(
      "module demo\nevent Tick\ngoal Work() perform\n"
      "capacity Navigation { effect def read(): Int within 1s }\n"
      "native skill Device implements Navigation\n"
      "agent A {\n"
      "  uses Navigation\n"
      "  install Device\n"
      "  on Tick { adopt Work() }\n"
      "  plan w for Work { let r := Navigation.read() else 0 }\n"
      "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  auto &core  = h.ctx.core;
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  const auto staged =
      VagentExecutor::drainOutbox(arena, core, image->agentCell);
  ASSERT_EQ(staged.size(), 1u);
  EXPECT_EQ(staged[0].kind, "effect");
  // No delivery — the within bound elapses, staging cancel for the
  // binding before the else ladder runs.
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  const auto after =
      VagentExecutor::drainOutbox(arena, core, image->agentCell);
  // drainOutbox reads without consuming — the rank still holds the
  // request; the cancel is the new entry.
  ASSERT_EQ(after.size(), 2u);
  EXPECT_EQ(after[1].kind, "cancel");
  EXPECT_EQ(after[1].correlation, staged[0].id);
  const auto r = varsRecord(arena, core, image->agentCell, "r", false);
  ASSERT_TRUE(r);
  const auto rv = arena.linked(*r, core.dims().values, DimVector::POS);
  ASSERT_TRUE(rv);
  EXPECT_EQ(arena.asInt64(*rv).value_or(-1), 0);
}

TEST(VagentExecutorTest, HandlerStagedEffectsFailAsAnEvent) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image = h.lower(
      "module demo\nevent Tick\nevent Seen\n"
      "capacity Navigation { effect def read(): Int within 3s }\n"
      "native skill Device implements Navigation\n"
      "agent A {\n"
      "  uses Navigation\n"
      "  install Device\n"
      "  on Tick { Navigation.read() }\n"
      "  on EffectFailed { emit Seen{} }\n"
      "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  auto &core  = h.ctx.core;
  // The handler's bare call stages without parking — no intention
  // exists to wait, and the turn still succeeds.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  const auto staged =
      VagentExecutor::drainOutbox(arena, core, image->agentCell);
  ASSERT_EQ(staged.size(), 1u);
  EXPECT_EQ(staged[0].kind, "effect");
  // The binding's failure arrives as the EffectFailed event the spec
  // gives handler calls — ordinary dispatch, `on EffectFailed` runs.
  exec.deliver(arena, core,
               Delivery{.agent   = image->agentCell,
                        .percept     = "EffectFailed",
                        .correlation = staged[0].id,
                        .fault       = "busy"});
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  const auto outbox =
      VagentExecutor::drainOutbox(arena, core, image->agentCell);
  ASSERT_EQ(outbox.size(), 2u);
  EXPECT_EQ(outbox[1].kind, "emit");
  EXPECT_EQ(outbox[1].payload, "Seen");
}

TEST(VagentExecutorTest, StopChildAnnouncesChildStoppingToTheParent) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Boot\nevent Down\n"
              "agent A {\n"
              "  var child: Opt<Agent> := none\n"
              "  on Boot { child := spawn A(); stop child }\n"
              "  on ChildStopping [occurrence.cause == \"stop\"] { emit "
              "Down{} }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  // The stop lands in Boot's turn: the child is flagged destroying,
  // its own Destroy percept queues on the child, and §4.5's
  // announcement reaches the parent at once — the guarded handler
  // proves the cause field rode the percept.
  ASSERT_TRUE(exec(h.execution("Boot"), h.ctx));
  const auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  // The spawn's wire entry leads; [1] is the handler's Down emit.
  ASSERT_EQ(outbox.size(), 2u);
  EXPECT_EQ(outbox[0].kind, "spawn");
  EXPECT_EQ(outbox[1].kind, "emit");
  EXPECT_EQ(outbox[1].payload, "Down");
}

TEST(VagentExecutorTest, UnroutableTargetRaisesDeliveryFailedOnTheSender) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Boot\nevent Note\nevent Down\n"
              "agent A {\n"
              "  var target: Opt<Agent> := none\n"
              "  on Boot { emit Note{} for target }\n"
              "  on DeliveryFailed [occurrence.kind == \"emit\"] { emit "
              "Down{} }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  ASSERT_TRUE(exec(h.execution("Boot"), h.ctx));
  const auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 1u);
  EXPECT_EQ(outbox[0].kind, "emit");
  // No member is named "Missing" — the adapter's route comes back
  // empty, which is §7.3's failure horizon for a local send.
  EXPECT_TRUE(VagentExecutor::route(arena, h.ctx.core, h.ctx.core.home(),
                                    outbox[0])
                  .empty());
  exec.deliveryFailed(arena, h.ctx.core, outbox[0]);
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  const auto after =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(after.size(), 2u);
  EXPECT_EQ(after[1].payload, "Down");
  // §3.7's idempotence: a persistent outbox rank re-drains the same
  // id — the failure must not re-raise.
  exec.deliveryFailed(arena, h.ctx.core, outbox[0]);
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  EXPECT_EQ(VagentExecutor::drainOutbox(arena, h.ctx.core,
                                      image->agentCell)
                .size(),
            2u);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, DownOnChildRaisesChildDownAndFailsItsWaits) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Boot\nevent Lost\nevent Down\n"
              "question stock(item: Str): Str\n"
              "goal Work() perform\n"
              "agent A {\n"
              "  var child: Opt<Agent> := none\n"
              "  on Boot { child := spawn A(); adopt Work() }\n"
              "  on ChildDown { emit Down{} }\n"
              "  plan w for Work { let r := ask child.stock(\"oil\") within "
              "9s else cause; emit Lost{} }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  auto &core  = h.ctx.core;
  // Boot spawns the member and adopts the plan; the settle runs the
  // ask step, which stages the request and parks on awaitCorr +
  // awaitPeer — the member's name is what a Down matches.
  ASSERT_TRUE(exec(h.execution("Boot"), h.ctx));
  const auto staged =
      VagentExecutor::drainOutbox(arena, core, image->agentCell);
  // The spawn's wire entry leads; [1] is the ask the plan staged.
  ASSERT_EQ(staged.size(), 2u);
  EXPECT_EQ(staged[0].kind, "spawn");
  EXPECT_EQ(staged[1].kind, "ask");
  const auto childVar = varsRecord(arena, core, image->agentCell, "child", false);
  ASSERT_TRUE(childVar);
  const auto childRef = arena.linked(*childVar, core.dims().values,
                                     DimVector::POS);
  ASSERT_TRUE(childRef);
  const auto member = arena.asInt64(*childRef);
  ASSERT_TRUE(member);
  // The adapter's monitored-loss report: §"internal events"' Down
  // rung raises ChildDown and fails intentions waiting on the child —
  // both happen on the parent's next turn, not in the adapter's call.
  exec.childDown(arena, core, image->agentCell,
                 static_cast<CellRef>(*member));
  // The turn's two halves: the pending drain dispatches ChildDown
  // first, then the reconsideration walk's awaitFault rung binds the
  // else's cause — the readied spine advances next turn.
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  const auto outbox =
      VagentExecutor::drainOutbox(arena, core, image->agentCell);
  ASSERT_EQ(outbox.size(), 3u);
  EXPECT_EQ(outbox[2].payload, "Down");
  const auto r = varsRecord(arena, core, image->agentCell, "r", false);
  ASSERT_TRUE(r);
  const auto rv = arena.linked(*r, core.dims().values,
                               DimVector::POS);
  ASSERT_TRUE(rv);
  EXPECT_EQ(arena.textOf(*rv), "down");
  // The readied intention runs its next spine step — the plan's own
  // emit follows the else recovery, like every ask-else.
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  const auto resumed =
      VagentExecutor::drainOutbox(arena, core, image->agentCell);
  ASSERT_EQ(resumed.size(), 4u);
  EXPECT_EQ(resumed[3].payload, "Lost");
  // A second report for the same member is the journal's dedupe —
  // no second ChildDown dispatches.
  exec.childDown(arena, core, image->agentCell,
                 static_cast<CellRef>(*member));
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  EXPECT_EQ(
      VagentExecutor::drainOutbox(arena, core, image->agentCell).size(),
      4u);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, OrderlyStopSuppressesTheTrailingActorDown) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Boot\nevent Note\nevent Note2\n"
              "agent A {\n"
              "  var child: Opt<Agent> := none\n"
              "  on Boot { child := spawn A(); stop child }\n"
              "  on ChildStopping { emit Note{} }\n"
              "  on ChildDown { emit Note2{} }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  auto &core  = h.ctx.core;
  ASSERT_TRUE(exec(h.execution("Boot"), h.ctx));
  const auto childVar = varsRecord(arena, core, image->agentCell, "child", false);
  ASSERT_TRUE(childVar);
  const auto childRef = arena.linked(*childVar, core.dims().values,
                                     DimVector::POS);
  ASSERT_TRUE(childRef);
  const auto member = arena.asInt64(*childRef);
  ASSERT_TRUE(member);
  // §4.5: "the parent reports one ChildStopping even when the
  // runtime's ActorDown follows it" — the stop's pre-mark makes the
  // trailing report a no-op.
  exec.childDown(arena, core, image->agentCell,
                 static_cast<CellRef>(*member));
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  const auto outbox =
      VagentExecutor::drainOutbox(arena, core, image->agentCell);
  // [0] is the spawn's wire entry; the stop's Note is the only emit.
  ASSERT_EQ(outbox.size(), 2u);
  EXPECT_EQ(outbox[0].kind, "spawn");
  EXPECT_EQ(outbox[1].payload, "Note");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, RefusedSpawnFailsStagedDeliveriesAndRaisesSpawnFailed) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Boot\nevent Note\nevent "
              "Refused\nevent Lost\n"
              "agent A {\n"
              "  var child: Opt<Agent> := none\n"
              "  on Boot { child := spawn A(); emit Note{} for child }\n"
              "  on SpawnFailed { emit Refused{} }\n"
              "  on DeliveryFailed [occurrence.cause == "
              "\"spawn_refused\"] { emit Lost{} }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  auto &core  = h.ctx.core;
  ASSERT_TRUE(exec(h.execution("Boot"), h.ctx));
  const auto staged =
      VagentExecutor::drainOutbox(arena, core, image->agentCell);
  // The spawn's wire entry leads; [1] is the emit staged to the
  // member's name — the refusal fails exactly that entry.
  ASSERT_EQ(staged.size(), 2u);
  EXPECT_EQ(staged[0].kind, "spawn");
  EXPECT_EQ(staged[1].kind, "emit");
  const auto childVar = varsRecord(arena, core, image->agentCell, "child", false);
  ASSERT_TRUE(childVar);
  const auto childRef = arena.linked(*childVar, core.dims().values,
                                     DimVector::POS);
  ASSERT_TRUE(childRef);
  const auto member = arena.asInt64(*childRef);
  ASSERT_TRUE(member);
  // §8.6: the refusal raises SpawnFailed and fails the deliveries
  // staged to the member's name — the staged emit was never
  // deliverable after the refusal.
  exec.spawnFailed(arena, core, image->agentCell,
                   static_cast<CellRef>(*member));
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  const auto outbox =
      VagentExecutor::drainOutbox(arena, core, image->agentCell);
  ASSERT_EQ(outbox.size(), 4u);
  EXPECT_EQ(outbox[2].payload, "Lost");
  EXPECT_EQ(outbox[3].payload, "Refused");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, MovedRebindsStalePeerRefsAndRaisesMoved) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Boot\nevent Note\n"
              "agent A {\n"
              "  var child: Opt<Agent> := none\n"
              "  on Boot { child := spawn A() }\n"
              "  on Moved { emit Note{} }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  auto &core  = h.ctx.core;
  ASSERT_TRUE(exec(h.execution("Boot"), h.ctx));
  const auto childVar = varsRecord(arena, core, image->agentCell, "child", false);
  ASSERT_TRUE(childVar);
  const auto childRef = arena.linked(*childVar, core.dims().values,
                                     DimVector::POS);
  ASSERT_TRUE(childRef);
  const auto oldMember = arena.asInt64(*childRef);
  ASSERT_TRUE(oldMember);
  const auto name = arena.linked(
      static_cast<CellRef>(*oldMember), core.dims().name,
      DimVector::POS);
  ASSERT_TRUE(name);
  // The adapter's new reference record for the same peer name — a
  // migration's pid change minted it; the receiver's $peers update is
  // a rebind, not a name rewrite.
  const auto moved = arena.makeCell();
  zigzag::expectWritten(
      arena.link(moved, core.dims().name, DimVector::POS,
                 arena.makeCell(arena.textOf(*name))));
  exec.moved(arena, core, image->agentCell, moved);
  const auto after = arena.linked(*childVar, core.dims().values,
                                  DimVector::POS);
  ASSERT_TRUE(after);
  EXPECT_EQ(arena.asInt64(*after).value_or(-1),
            static_cast<std::int64_t>(moved));
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  const auto outbox =
      VagentExecutor::drainOutbox(arena, core, image->agentCell);
  // [0] is the spawn's wire entry; Moved's Note is the only emit.
  ASSERT_EQ(outbox.size(), 2u);
  EXPECT_EQ(outbox[0].kind, "spawn");
  EXPECT_EQ(outbox[1].payload, "Note");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, ResumedFailsEffectWaitsWithMigrated) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image = h.lower(
      "module demo\nevent Tick\ngoal Work() perform\n"
      "capacity Navigation { effect def read(): Str within 9s }\n"
      "native skill Device implements Navigation\n"
      "agent A {\n"
      "  uses Navigation\n"
      "  install Device\n"
      "  on Tick { adopt Work() }\n"
      "  plan w for Work { let r := Navigation.read() else cause }\n"
      "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  auto &core  = h.ctx.core;
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  const auto staged =
      VagentExecutor::drainOutbox(arena, core, image->agentCell);
  ASSERT_EQ(staged.size(), 1u);
  EXPECT_EQ(staged[0].kind, "effect");
  // §5.4: a resumed agent cannot follow its in-flight effects — the
  // else ladder's cause is "migrated", which the test reads off r.
  exec.resumed(arena, core, image->agentCell);
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  const auto r = varsRecord(arena, core, image->agentCell, "r", false);
  ASSERT_TRUE(r);
  const auto rv = arena.linked(*r, core.dims().values,
                               DimVector::POS);
  ASSERT_TRUE(rv);
  EXPECT_EQ(arena.textOf(*rv), "migrated");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, BehaviorGoalMemberAdoptsTheBehaviorsPlan) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\nevent Done\n"
              "behavior Sweeper {\n"
              "  goal Sweep() perform\n"
              "  plan s for Sweep { emit Done{} }\n"
              "}\n"
              "agent A {\n"
              "  behavior Sweeper()\n"
              "  on Tick { adopt Sweep() }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  // The behavior's goal member declares the shared symbol; adopt
  // resolves it and §5.7's merge finds the plan on the mounted
  // instance's own bank.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  ASSERT_TRUE(exec(h.execution(), h.ctx));
  const auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  ASSERT_EQ(outbox.size(), 1u);
  EXPECT_EQ(outbox[0].kind, "emit");
  EXPECT_EQ(outbox[0].payload, "Done");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, BeliefAssertRunsThePlusTriggerWithItsCapture) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\n"
              "predicate pos(v: Int)\n"
              "agent A {\n"
              "  var r : Int := 0\n"
              "  on +pos(?v) { r := v }\n"
              "  on Tick { believe pos(41) }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  // believe stages the pend; the same turn's drain fires the handler.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  const auto r =
      varsRecord(arena, h.ctx.core, image->agentCell, "r", false);
  ASSERT_TRUE(r);
  const auto v = arena.linked(*r, h.ctx.core.dims().values, DimVector::POS);
  ASSERT_TRUE(v);
  EXPECT_EQ(arena.asInt64(*v).value_or(0), 41);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, BeliefRetractRunsTheMinusTrigger) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\n"
              "predicate pos(v: Int)\n"
              "agent A {\n"
              "  var r : Int := 0\n"
              "  on -pos(?v) { r := v }\n"
              "  on +pos(?v) { r := 0 - v }\n"
              "  on Tick { believe pos(7); forget pos(7) }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  // The assert's "+pos" handler writes -7 first; the retract's
  // "-pos" handler overwrites with the bound value — both stages
  // land on one drain, in order.
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  const auto r =
      varsRecord(arena, h.ctx.core, image->agentCell, "r", false);
  ASSERT_TRUE(r);
  const auto v = arena.linked(*r, h.ctx.core.dims().values, DimVector::POS);
  ASSERT_TRUE(v);
  EXPECT_EQ(arena.asInt64(*v).value_or(0), 7);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, BeliefPatternUnifiesNestedAndRepeatedAndWildcard) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\n"
              "record Pair { a: Int, b: Int }\n"
              "predicate edge(p: Pair)\n"
              "predicate eq(x: Int, y: Int)\n"
              "predicate any(x: Int, y: Int)\n"
              "agent A {\n"
              "  var a : Int := 0\n"
              "  var b : Int := 0\n"
              "  var w : Int := 0\n"
              "  on +edge(Pair{a: ?x, b: ?y}) { a := x + y }\n"
              "  on +eq(?x, ?x) { b := x }\n"
              "  on +any(?_, ?v) { w := v }\n"
              "  on Tick { believe edge(Pair{a: 3, b: 4}); "
              "believe eq(9, 9); believe eq(1, 2); believe any(6, 8) }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  const auto field = [&](std::string_view name) {
    const auto rec =
        varsRecord(arena, h.ctx.core, image->agentCell, name, false);
    if (!rec) return std::int64_t{-1};
    const auto v =
        arena.linked(*rec, h.ctx.core.dims().values, DimVector::POS);
    return v ? arena.asInt64(*v).value_or(-1) : -1;
  };
  // Nested term: both captures bind — 3 + 4.
  EXPECT_EQ(field("a"), 7);
  // Repeated `?x` constrains: eq(9, 9) fires, eq(1, 2) never matches.
  EXPECT_EQ(field("b"), 9);
  // `?_` is a pure wildcard — only the second capture binds.
  EXPECT_EQ(field("w"), 8);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, BeliefTriggerGuardReadsTheCapturedVariable) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\nevent Done\n"
              "predicate val(v: Int)\n"
              "agent A {\n"
              "  on +val(?v) [v > 10] { emit Done{} }\n"
              "  on Tick { believe val(5); believe val(15) }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  const auto outbox =
      VagentExecutor::drainOutbox(arena, h.ctx.core, image->agentCell);
  // val(5) unifies but the guard refuses it — only val(15) emits.
  ASSERT_EQ(outbox.size(), 1u);
  EXPECT_EQ(outbox[0].payload, "Done");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentExecutorTest, BehaviorBeliefHandlerBindsItsOwnInstance) {
  ExecutorHarness h;
  VagentExecutor exec;
  const auto image =
      h.lower("module demo\nevent Tick\n"
              "predicate mark(v: Int)\n"
              "behavior Watch {\n"
              "  var count : Int := 0\n"
              "  on +mark(?v) { count := count + v }\n"
              "}\n"
              "agent A {\n"
              "  var hits : Int := 0\n"
              "  behavior Watch()\n"
              "  on +mark(?v) { hits := hits + 1 }\n"
              "  on Tick { believe mark(3); believe mark(4) }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  ASSERT_TRUE(exec(h.execution("Tick"), h.ctx));
  // §5.7's merge on the belief arm: the agent's own handler and the
  // mounted instance's both staged, in table order. The instance's
  // count accumulated 3 + 4 under its own $owner — not the agent's.
  const auto hits =
      varsRecord(arena, h.ctx.core, image->agentCell, "hits", false);
  ASSERT_TRUE(hits);
  const auto hv =
      arena.linked(*hits, h.ctx.core.dims().values, DimVector::POS);
  ASSERT_TRUE(hv);
  EXPECT_EQ(arena.asInt64(*hv).value_or(-1), 2);
  const auto behaviorDim = h.ctx.core.findOrMintDimension("d.behavior");
  const auto insts       = std::ranges::to<std::vector>(
      zigzag::rankAfter(arena, image->agentCell, behaviorDim));
  ASSERT_EQ(insts.size(), 1u);
  // An instance field's vars record holds the dual-linked holder —
  // the int rides the holder's own scalar bits.
  const auto count =
      varsRecord(arena, h.ctx.core, insts.front(), "count", false);
  ASSERT_TRUE(count);
  const auto cv =
      arena.linked(*count, h.ctx.core.dims().values, DimVector::POS);
  ASSERT_TRUE(cv);
  EXPECT_EQ(arena.asInt64(*cv).value_or(-1), 7);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

} // namespace
