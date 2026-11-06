// The adapter seam (actor-executor's greenfield half): VagentAdapter
// drives VagentExecutor turns the way MeshActorRuntime's callbacks
// hand them down — mesh pids translated through the member's durable
// `pid` record, ActorDown mailbox messages becoming childDown, each
// committed turn ending in the §3.7 outbox drain.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "../../apps/common/xanadu/vagent/adapter.hpp"
#include "../../apps/common/xanadu/vagent/decls.hpp"
#include "../../apps/common/xanadu/vagent/frontend.hpp"
#include "../../apps/common/xanadu/vagent/records.hpp"
#include "../../apps/common/xanadu/store.hpp"

namespace {

using namespace xanadu::vagent;
using xanadu::identity::ActorPid;
using xanadu::identity::MessageType;
using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;
using zigzag::vortex::WorkerContext;
using zigzag::vortex::mesh::ActorExecution;
using zigzag::vortex::mesh::ActorGenesis;
using zigzag::vortex::mesh::ActorGrant;
using zigzag::vortex::mesh::ActorMessage;

struct AdapterHarness {
  WorkerContext ctx;
  VagentAdapter adapter;

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

  ActorExecution execution(std::optional<ActorMessage> message =
                               std::nullopt) {
    ActorExecution exec;
    exec.pid   = ActorPid{"worker", "agent", 1, 1};
    exec.grant = ActorGrant{.programRef      = "sha256:p",
                            .baseRef         = "sha256:b",
                            .maxInstructions = 100'000,
                            .maxCells        = 10'000};
    exec.message = std::move(message);
    exec.cursor =
        ctx.vm.spawnCursor(ctx.vm.mintOpcode(OpcodeKind::Halt), "actor");
    return exec;
  }

  ActorMessage send(std::string value) {
    ActorMessage msg;
    msg.type      = MessageType::ActorSend;
    msg.requestId = "req-1";
    msg.value     = std::move(value);
    return msg;
  }

  std::vector<std::string> journalOf(CellRef agent) {
    const auto seen = ctx.core.findOrMintDimension("d.seen");
    std::vector<std::string> names;
    for (const CellRef cell :
         zigzag::rankAfter(ctx.arena, agent, seen))
      names.emplace_back(ctx.arena.textOf(cell));
    return names;
  }

  std::optional<CellRef> memberCell(CellRef agent,
                                  std::string_view varName) {
    const auto rec = varsRecord(ctx.arena, ctx.core, agent, varName, false);
    if (!rec) return std::nullopt;
    const auto ref = ctx.arena.linked(*rec, ctx.core.dims().values,
                                    DimVector::POS);
    if (!ref) return std::nullopt;
    const auto member = ctx.arena.asInt64(*ref);
    if (!member) return std::nullopt;
    return static_cast<CellRef>(*member);
  }
};

TEST(VagentAdapterTest, NotedPidResolvesBackToItsMember) {
  AdapterHarness h;
  const auto image =
      h.lower("module demo\nevent Boot\nagent A {\n"
              "  var child: Opt<Agent> := none\n"
              "  on Boot { child := spawn A() }\n"
              "}");
  ASSERT_TRUE(image);
  ASSERT_TRUE(h.adapter(h.execution(h.send("Boot")), h.ctx));
  const auto member = h.memberCell(image->agentCell, "child");
  ASSERT_TRUE(member);

  const ActorPid meshPid{"node7", "contentid", 3, 41};
  // A mesh spawnSequence is the runtime's global counter — it is not
  // a member index, so before the note the pid resolves to nothing.
  EXPECT_FALSE(h.adapter.memberForPid(h.ctx, meshPid).has_value());
  h.adapter.noteMemberPid(h.ctx, *member, meshPid);
  const auto found = h.adapter.memberForPid(h.ctx, meshPid);
  ASSERT_TRUE(found);
  EXPECT_EQ(*found, *member);
  EXPECT_EQ(h.adapter.memberIndex(h.ctx, *member), 1u);
  // The declared agent is member 0; the spawned member shares its
  // declared +d.name — the pid record is what tells the two apart.
  EXPECT_EQ(h.adapter.memberNamed(h.ctx, "A"), image->agentCell);
  // A rewritten incarnation resolves through the new key only.
  const ActorPid moved{"node8", "contentid", 4, 42};
  h.adapter.noteMemberPid(h.ctx, *member, moved);
  EXPECT_FALSE(h.adapter.memberForPid(h.ctx, meshPid).has_value());
  ASSERT_TRUE(h.adapter.memberForPid(h.ctx, moved));
  EXPECT_EQ(*h.adapter.memberForPid(h.ctx, moved), *member);
}

TEST(VagentAdapterTest, ActorDownBecomesChildDownAndFailsTheWait) {
  AdapterHarness h;
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
  ASSERT_TRUE(h.adapter(h.execution(h.send("Boot")), h.ctx));
  const auto member = h.memberCell(image->agentCell, "child");
  ASSERT_TRUE(member);
  const ActorPid childPid{"node3", "childid", 2, 7};
  h.adapter.noteMemberPid(h.ctx, *member, childPid);
  // Once any member is noted, harness pids stop resolving — the turn
  // arrives with the mesh pid genesis recorded for this actor.
  const ActorPid parentPid{"worker", "agentid", 1, 5};
  h.adapter.noteMemberPid(h.ctx, image->agentCell, parentPid);

  // The mesh delivers the monitored member's ActorDown as a mailbox
  // message — the adapter consumes it, raises ChildDown, and the same
  // turn's settle faults the ask parked on the child's name.
  ActorMessage down;
  down.type      = MessageType::ActorDown;
  down.requestId = "mon-1";
  down.source    = childPid;
  down.value     = "ActorDown";
  auto turnDown = h.execution(down);
  turnDown.pid  = parentPid;
  ASSERT_TRUE(h.adapter(turnDown, h.ctx));
  const auto journal = h.journalOf(image->agentCell);
  EXPECT_THAT(journal, ::testing::Contains("ChildDown"));
  // The raw message never dispatches as a generic event — ChildDown
  // is the only loss percept the journal holds.
  EXPECT_THAT(journal,
              ::testing::Not(::testing::Contains("ActorDown")));
  // The faulted wait's else ran with cause "down"; the continuation
  // needs its own turn to emit Lost, and the handler's Down rides the
  // same drain — journal order is Initialize, ChildDown, Down, Lost.
  auto turnWake = h.execution();
  turnWake.pid  = parentPid;
  ASSERT_TRUE(h.adapter(turnWake, h.ctx));
  const auto after = h.journalOf(image->agentCell);
  EXPECT_THAT(after, ::testing::Contains("Down"));
  EXPECT_THAT(after, ::testing::Contains("Lost"));
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentAdapterTest, CommittedTurnDrainsTheOutbox) {
  AdapterHarness h;
  const auto image =
      h.lower("module demo\nevent Tick\nagent A {\n"
              "  on Initialize { emit Tick{} }\n"
              "}");
  ASSERT_TRUE(image);
  // One adapter turn: the Initialize handler stages Tick, the
  // post-commit drain routes it broadcast — back onto the sender's
  // own journal like every other member's.
  ASSERT_TRUE(h.adapter(h.execution(), h.ctx));
  EXPECT_THAT(h.journalOf(image->agentCell),
              ::testing::Contains("Tick"));
  // The staged record is delivered, not re-staged: the drain is
  // read-only and deliver's +d.delivered dedupe makes a second turn
  // stage nothing new — the journal stays Initialize + Tick.
  ASSERT_TRUE(h.adapter(h.execution(), h.ctx));
  EXPECT_EQ(h.journalOf(image->agentCell).size(), 2u);
}

TEST(VagentAdapterTest, UnroutableTargetFaultsTheSender) {
  AdapterHarness h;
  const auto image =
      h.lower("module demo\nevent Ping\nagent A {\n"
              "  var target: Opt<Agent> := none\n"
              "  on Initialize { emit Ping{} for target }\n"
              "}");
  ASSERT_TRUE(image);
  ASSERT_TRUE(h.adapter(h.execution(), h.ctx));
  const auto journal = h.journalOf(image->agentCell);
  EXPECT_THAT(journal, ::testing::Contains("DeliveryFailed"));
  EXPECT_THAT(journal, ::testing::Not(::testing::Contains("Ping")));
  EXPECT_EQ(h.ctx.arena.outstandingMarks(), 0u);
}

TEST(VagentAdapterTest, GenesisRewritesPidAndFailsEffectWaits) {
  AdapterHarness h;
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
  ASSERT_TRUE(h.adapter(h.execution(h.send("Tick")), h.ctx));
  const auto staged =
      VagentExecutor::drainOutbox(arena, core, image->agentCell);
  ASSERT_EQ(staged.size(), 1u);
  EXPECT_EQ(staged[0].kind, "effect");

  // A genesis carrying a checkpoint means the actor reincarnated:
  // §5.4's effect waits cannot follow the device, so the parked
  // intention takes the else ladder with cause "migrated" — and the
  // member's durable pid record follows the new incarnation.
  xanadu::Store transported;
  ActorGenesis genesis;
  genesis.pid         = ActorPid{"node9", "reincarnation", 5, 88};
  genesis.transported = &transported;
  ASSERT_TRUE(h.adapter.genesis(genesis, h.ctx));
  ASSERT_TRUE(h.adapter.memberForPid(h.ctx, genesis.pid));
  EXPECT_EQ(*h.adapter.memberForPid(h.ctx, genesis.pid),
            image->agentCell);
  auto turnWake = h.execution();
  turnWake.pid  = genesis.pid;
  ASSERT_TRUE(h.adapter(turnWake, h.ctx));
  const auto r = varsRecord(arena, core, image->agentCell, "r", false);
  ASSERT_TRUE(r);
  const auto rv =
      arena.linked(*r, core.dims().values, DimVector::POS);
  ASSERT_TRUE(rv);
  EXPECT_EQ(arena.textOf(*rv), "migrated");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentAdapterTest, FirstSpawnGenesisOnlyNotesThePid) {
  AdapterHarness h;
  const auto image =
      h.lower("module demo\nagent A { }");
  ASSERT_TRUE(image);
  // transport=null means first spawn — the incarnation's pid lands
  // durably without the migrated-agent mark.
  ActorGenesis genesis;
  genesis.pid = ActorPid{"node1", "firstspawn", 1, 3};
  ASSERT_TRUE(h.adapter.genesis(genesis, h.ctx));
  ASSERT_TRUE(h.adapter.memberForPid(h.ctx, genesis.pid));
  EXPECT_EQ(*h.adapter.memberForPid(h.ctx, genesis.pid),
            image->agentCell);
}

TEST(VagentAdapterTest, SpawnRefusedFailsTheMemberAndItsDeliveries) {
  AdapterHarness h;
  const auto image =
      h.lower("module demo\nevent Boot\nevent Wave\n"
              "agent A {\n"
              "  var child: Opt<Agent> := none\n"
              "  on Boot { child := spawn A() in inner; emit Wave{} in "
              "inner for child }\n"
              "}");
  ASSERT_TRUE(image);
  ASSERT_TRUE(h.adapter(h.execution(h.send("Boot")), h.ctx));
  const auto member = h.memberCell(image->agentCell, "child");
  ASSERT_TRUE(member);
  // The mesh answered the spawn request with no pid — §8.6 raises
  // SpawnFailed on the parent and fails deliveries the refused name
  // still stages.
  h.adapter.spawnRefused(h.ctx, image->agentCell, *member);
  const auto journal = h.journalOf(image->agentCell);
  EXPECT_THAT(journal, ::testing::Contains("SpawnFailed"));
  EXPECT_THAT(journal, ::testing::Contains("DeliveryFailed"));
}

TEST(VagentAdapterTest, MemberReboundUpdatesStaleReferences) {
  AdapterHarness h;
  const auto image =
      h.lower("module demo\nevent Boot\n"
              "agent A {\n"
              "  var child: Opt<Agent> := none\n"
              "  on Boot { child := spawn A() }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  auto &core  = h.ctx.core;
  ASSERT_TRUE(h.adapter(h.execution(h.send("Boot")), h.ctx));
  const auto member = h.memberCell(image->agentCell, "child");
  ASSERT_TRUE(member);
  // The adapter's replacement reference for the migrated member —
  // same +d.name, fresh cell — rebinds the var still holding the
  // stale one, then Moved dispatches. (Opt vars that merely copied
  // the binding keep their opt node; only direct member refs rebind.)
  const auto replacement = arena.makeCell();
  zigzag::expectWritten(
      arena.link(replacement, core.dims().name, DimVector::POS,
                 arena.makeCell("A")));
  h.adapter.memberRebound(h.ctx, image->agentCell, replacement);
  const auto rebound = h.memberCell(image->agentCell, "child");
  ASSERT_TRUE(rebound);
  EXPECT_EQ(*rebound, replacement);
  EXPECT_THAT(h.journalOf(image->agentCell),
              ::testing::Contains("Moved"));
}

} // namespace
