#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "common/xanadu/vagent/runtime/agent_executor.hpp"
#include "common/xanadu/vagent/runtime/epistemic_limits.hpp"
#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/runtime_abi.hpp"

namespace {
using namespace xanadu::vagent::runtime;

TEST(VagentEpistemicAbi, DimensionsAreIdempotentAndDistinct) {
  zigzag::ArenaManifold arena;
  const auto first  = registerDimensions(arena);
  const auto size   = arena.cellCount();
  const auto second = registerDimensions(arena);
  EXPECT_EQ(first.trust, second.trust);
  EXPECT_EQ(first.epistemic, second.epistemic);
  EXPECT_NE(first.trust, first.epistemic);
  EXPECT_EQ(arena.cellCount(), size);
  EXPECT_EQ(arena.textOf(first.trust), "d.trust");
  EXPECT_EQ(arena.textOf(first.epistemic), "d.epistemic");
  EXPECT_EQ(std::ranges::count(kAgentDimensionNames, "d.trust"), 1);
  EXPECT_EQ(std::ranges::count(kAgentDimensionNames, "d.epistemic"), 1);
  EXPECT_FALSE(arena.linked(first.trust, first.epistemic, false));
}

TEST(VagentEpistemicAbi, InvalidHostLimitsAreRefusedBeforeActorCreation) {
  AgentSettings settings;
  settings.epistemic.maxSources = 0;
  EXPECT_THROW((void)AgentExecutor::runtimeOptions(settings),
               std::invalid_argument);
  settings.epistemic.maxSources    = 1;
  settings.epistemic.maxCandidates = 0;
  EXPECT_THROW((void)AgentExecutor::runtimeOptions(settings),
               std::invalid_argument);
  settings.epistemic.maxCandidates = 1;
  EXPECT_NO_THROW((void)AgentExecutor::runtimeOptions(settings));
}

TEST(VagentEpistemicAbi, ResourceSettingsRejectInvalidSignedValues) {
  const auto defaults = checkedEpistemicLimits(kDefaultEpistemicMaxSources,
                                               kDefaultEpistemicMaxCandidates);
  ASSERT_TRUE(defaults);
  EXPECT_TRUE(defaults->validate());
  EXPECT_EQ(defaults->maxSources, 128U);
  EXPECT_EQ(defaults->maxCandidates, 1024U);
  for (const auto value : {std::int64_t{0}, std::int64_t{-1},
                           std::numeric_limits<std::int64_t>::min()}) {
    EXPECT_EQ(checkedEpistemicLimits(value, 1).error(),
              EpistemicLimitError::InvalidSources);
    EXPECT_EQ(checkedEpistemicLimits(1, value).error(),
              EpistemicLimitError::InvalidCandidates);
  }
  EXPECT_TRUE(checkedEpistemicLimits(1, 1));
  EXPECT_TRUE(checkedEpistemicLimits(10000, 100000));
  EXPECT_EQ((EpistemicLimits{0, 1}.validate().error()),
            EpistemicLimitError::InvalidSources);
  EXPECT_EQ((EpistemicLimits{1, 0}.validate().error()),
            EpistemicLimitError::InvalidCandidates);
}
TEST(VagentEpistemicAbi, ScopeNestingSettingsRejectInvalidSignedValues) {
  const auto defaults = checkedEpistemicLimits(
      kDefaultEpistemicMaxSources, kDefaultEpistemicMaxCandidates,
      kDefaultEpistemicScopeDepth, kDefaultEpistemicNestedScopes);
  ASSERT_TRUE(defaults);
  EXPECT_EQ(defaults->maxScopeDepth, 32U);
  EXPECT_EQ(defaults->maxNestedScopes, 8U);
  EXPECT_EQ(EpistemicLimits{}.maxScopeDepth, defaults->maxScopeDepth);
  EXPECT_EQ(EpistemicLimits{}.maxNestedScopes, defaults->maxNestedScopes);
  for (const auto value : {std::int64_t{0}, std::int64_t{-1},
                           std::numeric_limits<std::int64_t>::min()}) {
    EXPECT_EQ(checkedEpistemicLimits(1, 1, value, 1).error(),
              EpistemicLimitError::InvalidScopeDepth);
    EXPECT_EQ(checkedEpistemicLimits(1, 1, 1, value).error(),
              EpistemicLimitError::InvalidNestedScopes);
  }
  // Source/candidate refusals keep precedence over the scope fields.
  EXPECT_EQ(checkedEpistemicLimits(0, 1, 0, 0).error(),
            EpistemicLimitError::InvalidSources);
  AgentSettings settings;
  settings.epistemic.maxScopeDepth = 0;
  EXPECT_THROW((void)AgentExecutor::runtimeOptions(settings),
               std::invalid_argument);
  settings.epistemic.maxScopeDepth   = 1;
  settings.epistemic.maxNestedScopes = 0;
  EXPECT_THROW((void)AgentExecutor::runtimeOptions(settings),
               std::invalid_argument);
}

TEST(VagentEpistemicAbi, ScopeGrantNeverExceedsTheActorGrant) {
  zigzag::vortex::mesh::ActorGrant grant;
  grant.maxInstructions = 5000;
  grant.maxCells        = 300;
  grant.maxOutputBytes  = 4096;
  EpistemicLimits limits;
  limits.maxScopeDepth   = 6;
  limits.maxNestedScopes = 2;
  const auto scoped      = scopedEvaluationGrant(grant, limits);
  ASSERT_TRUE(scoped);
  EXPECT_EQ(scoped->traversalWork, 5000U);
  EXPECT_EQ(scoped->instructions, 5000U);
  EXPECT_EQ(scoped->fields, 300U);
  EXPECT_EQ(scoped->bytes, 4096U);
  EXPECT_EQ(scoped->depth, 6U);
  EXPECT_EQ(scoped->scopes, 2U);
  grant.maxInstructions = std::numeric_limits<std::uint64_t>::max();
  EXPECT_LE(scopedEvaluationGrant(grant, limits)->instructions,
            std::numeric_limits<std::size_t>::max());
  // An exhausted dimension of the actor grant disables scopes outright.
  for (auto exhaust :
       {+[](zigzag::vortex::mesh::ActorGrant &g) { g.maxInstructions = 0; },
        +[](zigzag::vortex::mesh::ActorGrant &g) { g.maxCells = 0; },
        +[](zigzag::vortex::mesh::ActorGrant &g) { g.maxOutputBytes = 0; }}) {
    auto empty = grant;
    exhaust(empty);
    EXPECT_FALSE(scopedEvaluationGrant(empty, limits));
  }
}

TEST(VagentEpistemicAbi, ScopeGrantDoesNotOutliveTheTurn) {
  auto program = CompiledProgram::fromSource(R"(
event Ping {}
agent A {
  var count: Int := 0
  on Ping { count := count + 1 }
}
)");
  ASSERT_TRUE(program) << program.error();
  LocalHost host(std::move(*program));
  const auto pid = host.launch("A", "a");
  ASSERT_TRUE(pid);
  host.pump();
  Envelope ping;
  ping.kind = std::string(kKindEvent);
  ping.type = "Ping";
  ASSERT_TRUE(host.send(*pid, ping));
  host.pump();
  const auto *context = host.executor().context(*pid);
  ASSERT_NE(context, nullptr);
  EXPECT_FALSE(context->vm.scopedEvaluationLimits());
}
} // namespace
