// M0 declaration lowering tests (design/vagent-actor-executor.md §8):
// the DeclKind disposition table, module/symbol/clause emission, the
// +d.dirs/+d.agents agent path, +d.holder dispatch ownership, the
// $-reserved seeds and the executor dim filing.

#include "common/xanadu/vagent/decls.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "common/xanadu/vagent/frontend.hpp"
#include "common/xanadu/vagent/lineage.hpp"
#include "common/xanadu/vagent/records.hpp"

namespace {

using namespace xanadu::vagent;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::noCell;
using zigzag::Posward;

struct DeclHarness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};

  std::expected<LoweredImage, std::vector<Diagnostic>>
  lower(std::string_view source) {
    auto artifacts = compileArtifacts(source);
    if (!artifacts) return std::unexpected(artifacts.error());
    return lowerProgram(artifacts->program, core, vm);
  }

  std::vector<CellRef> rank(CellRef cell, DimRef dim) const {
    std::vector<CellRef> out;
    for (const auto member : zigzag::rankAfter(arena, cell, dim))
      out.push_back(member);
    return out;
  }
  std::optional<CellRef> named(CellRef owner, DimRef dim,
                               std::string_view name) const {
    for (const auto member : rank(owner, dim))
      if (arena.textOf(member) == name) return member;
    return std::nullopt;
  }
};

TEST(VagentDeclsTest, DispositionTableDisposesEveryDeclKind) {
  const DeclKind kinds[] = {
      DeclKind::Event,     DeclKind::Record,      DeclKind::Predicate,
      DeclKind::Question,  DeclKind::Capacity,    DeclKind::Skill,
      DeclKind::Behavior,  DeclKind::Agent,       DeclKind::Goal,
      DeclKind::Plan,      DeclKind::Method,      DeclKind::Answer,
      DeclKind::Handler,   DeclKind::Field,       DeclKind::Uses,
      DeclKind::Install,   DeclKind::BehaviorUse, DeclKind::Belief,
      DeclKind::Lifecycle,
  };
  ASSERT_EQ(std::size(kinds), 19U);
  int lowers = 0, stubs = 0, refuses = 0;
  for (const auto kind : kinds) switch (declDisposition(kind)) {
    case DeclDisposition::Lower:
      ++lowers;
      break;
    case DeclDisposition::Stub:
      ++stubs;
      break;
    case DeclDisposition::Refuse:
      ++refuses;
      break;
    }
  EXPECT_EQ(lowers, 10);
  EXPECT_EQ(stubs, 6);
  EXPECT_EQ(refuses, 3);

  EXPECT_EQ(declDisposition(DeclKind::Agent), DeclDisposition::Lower);
  EXPECT_EQ(declDisposition(DeclKind::Handler), DeclDisposition::Lower);
  EXPECT_EQ(declDisposition(DeclKind::Field), DeclDisposition::Lower);
  EXPECT_EQ(declDisposition(DeclKind::Uses), DeclDisposition::Refuse);
  EXPECT_EQ(declDisposition(DeclKind::Install), DeclDisposition::Refuse);
  EXPECT_EQ(declDisposition(DeclKind::BehaviorUse), DeclDisposition::Refuse);
}

TEST(VagentDeclsTest, AgentImageAssembles) {
  DeclHarness h;
  auto image = h.lower(R"(
    module demo
    event Tick
    agent A {
      goal Reach(target: Int) perform
      plan direct for Reach(target) { }
      on Tick { }
      def value(): Int { 1 }
    }
  )");
  ASSERT_TRUE(image.has_value());
  if (!image) return;
  const auto &dims = h.core.dims();

  EXPECT_EQ(image->programRoot, h.core.home());
  ASSERT_NE(image->agentCell, noCell);
  ASSERT_NE(image->directory, noCell);
  ASSERT_NE(image->ownModule, noCell);

  // Module cell rides the program root's +d.stdlib rank, content =
  // path verbatim.
  EXPECT_EQ(h.arena.textOf(image->ownModule), "demo");
  EXPECT_THAT(h.rank(image->programRoot, dims.stdlib),
              ::testing::Contains(image->ownModule));

  // cells"##/+d.dirs/+d.agents" — the agent is reachable from home
  // through the purpose-minted directory cell, not directly.
  const auto dirs   = h.core.findOrMintDimension("d.dirs");
  const auto agents = h.core.findOrMintDimension("d.agents");
  const auto holder = h.core.findOrMintDimension("d.holder");
  EXPECT_THAT(h.rank(image->programRoot, dirs),
              ::testing::ElementsAre(image->directory));
  EXPECT_THAT(h.rank(image->directory, agents),
              ::testing::ElementsAre(image->agentCell));

  // Declared symbols on the module's +d.vars rank.
  const auto reach = h.named(image->ownModule, dims.vars, "Reach");
  const auto tick  = h.named(image->ownModule, dims.vars, "Tick");
  const auto value = h.named(image->ownModule, dims.vars, "value");
  ASSERT_TRUE(reach.has_value());
  ASSERT_TRUE(tick.has_value());
  ASSERT_TRUE(value.has_value());

  // Goal/event symbols carry +d.holder → a scalar cell whose content
  // is the agent ref (one link per cell per dim means the agent's
  // -d.holder slot could only ever pair one symbol).
  const auto holderOf = [&](CellRef sym) {
    const auto cell = h.arena.linked(sym, holder, Posward);
    return cell ? h.arena.asInt64(*cell) : std::nullopt;
  };
  EXPECT_EQ(holderOf(*reach), static_cast<std::int64_t>(image->agentCell));
  EXPECT_EQ(holderOf(*tick), static_cast<std::int64_t>(image->agentCell));

  // The plan clause: +d.name/+d.grab/+d.spin on the goal's +d.clause
  // rank — the graftPlan shape — with declared body-head names.
  const auto planClauses = h.rank(*reach, dims.clause);
  ASSERT_EQ(planClauses.size(), 1U);
  const auto clause   = planClauses.front();
  const auto nameCell = h.arena.linked(clause, dims.name, Posward);
  ASSERT_TRUE(nameCell.has_value());
  EXPECT_EQ(h.arena.textOf(*nameCell), "direct");
  const auto guard = h.arena.linked(clause, dims.grab, Posward);
  const auto body  = h.arena.linked(clause, dims.spin, Posward);
  ASSERT_TRUE(guard.has_value());
  ASSERT_TRUE(body.has_value());
  const auto bodyName = h.arena.linked(*body, dims.name, Posward);
  ASSERT_TRUE(bodyName.has_value());
  EXPECT_EQ(h.arena.textOf(*bodyName), "decl:demo:direct:body");
  EXPECT_TRUE(
      h.arena.linked(*body, h.core.findOrMintDimension("d.entry"), Posward)
          .has_value());

  // The handler clause lands on the event symbol the same way.
  const auto onClauses = h.rank(*tick, dims.clause);
  ASSERT_EQ(onClauses.size(), 1U);
  const auto onName = h.arena.linked(onClauses.front(), dims.name, Posward);
  ASSERT_TRUE(onName.has_value());
  EXPECT_EQ(h.arena.textOf(*onName), "on:Tick");

  // `def` lowers a callable: the symbol's +d.values names its head.
  EXPECT_TRUE(h.arena.linked(*value, dims.values, Posward).has_value());

  // $-reserved seeds are claimed on the agent cell.
  for (const auto seed : {"$turn", "$step", "$stats"})
    EXPECT_TRUE(
        varsRecord(h.arena, h.core, image->agentCell, seed, false).has_value())
        << seed;

  // Bodies emitted ops — the image record exists.
  EXPECT_NE(image->imageRecord, noCell);
}

TEST(VagentDeclsTest, StubKindsMintSymbolsAndRecord) {
  DeclHarness h;
  auto image = h.lower(R"(
    module demo
    record S { f: Int }
    question q(x: Int): Int
    predicate reading(v: Int)
  )");
  ASSERT_TRUE(image.has_value());
  if (!image) return;
  const auto &dims = h.core.dims();
  // `q` is still a stub; `S` lowers — its field schema is the
  // signature and there is nothing deferred left to reserve.
  EXPECT_THAT(image->stubbed, ::testing::IsSupersetOf({"q"}));
  EXPECT_THAT(image->stubbed,
              ::testing::Not(::testing::Contains("S")));
  EXPECT_TRUE(h.named(image->ownModule, dims.vars, "S").has_value());
  EXPECT_TRUE(h.named(image->ownModule, dims.vars, "q").has_value());
  EXPECT_TRUE(h.named(image->ownModule, dims.vars, "reading").has_value());
}

TEST(VagentDeclsTest, CompositionDirectivesRefuse) {
  DeclHarness h;
  const auto image = h.lower("agent A { uses B }");
  ASSERT_FALSE(image.has_value());
  EXPECT_FALSE(image.error().empty());
}

TEST(VagentDeclsTest, ExecutorDimensionsFileWithTheAgentSet) {
  for (const auto name :
       {"d.agents", "d.dirs", "d.seen", "d.evidence", "d.verb", "d.holder"})
    EXPECT_TRUE(std::ranges::find(kAgentDimensionNames, name) !=
                std::ranges::end(kAgentDimensionNames))
        << name;
}

TEST(VagentDeclsTest, RefusedBodiesStillMintNamedHeads) {
  DeclHarness h;
  // `emit` is an agent statement kind the scalar Emitter refuses —
  // the head and its declared name still land, over a stub graph.
  auto image = h.lower(R"(
    module demo
    event Tick
    event Seen { v: Int }
    agent A { on Tick { emit Seen{v: 1} } }
  )");
  ASSERT_TRUE(image.has_value());
  if (!image) return;
  const auto &dims = h.core.dims();
  const auto tick  = h.named(image->ownModule, dims.vars, "Tick");
  ASSERT_TRUE(tick.has_value());
  const auto clauses = h.rank(*tick, dims.clause);
  ASSERT_EQ(clauses.size(), 1U);
  const auto body = h.arena.linked(clauses.front(), dims.spin, Posward);
  ASSERT_TRUE(body.has_value());
  const auto bodyName = h.arena.linked(*body, dims.name, Posward);
  ASSERT_TRUE(bodyName.has_value());
  EXPECT_EQ(h.arena.textOf(*bodyName), "decl:demo:on:Tick:body");
}

} // namespace
