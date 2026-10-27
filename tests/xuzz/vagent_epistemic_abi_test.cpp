#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "common/xanadu/vagent/runtime/agent_executor.hpp"
#include "common/xanadu/vagent/runtime/epistemic_limits.hpp"
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
} // namespace
