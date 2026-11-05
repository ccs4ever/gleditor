#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>

#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/vagent/cluster/cluster_node.hpp"
#include "common/xanadu/vagent/runtime/agent_executor.hpp"
#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace xanadu::vagent::runtime {

TEST(VagentSettingsTest, DefaultFallbacksFromEmptyStore) {
  Store store;
  const auto settings = AgentSettings::fromStore(store);

  EXPECT_EQ(settings.beliefEventDepth, 16U);
  EXPECT_EQ(settings.conditionFuel, 1000U);
  EXPECT_EQ(settings.maintainBackoff, std::chrono::milliseconds{30000});
  EXPECT_EQ(settings.sendRetries, 5U);
  EXPECT_EQ(settings.sendRetryInterval, std::chrono::milliseconds{1000});
  EXPECT_DOUBLE_EQ(settings.renewFraction, 0.8);
  EXPECT_EQ(settings.readyPerMessage, 4U);
  EXPECT_EQ(settings.compactEvery, 64U);
  EXPECT_EQ(settings.valueOperations, 65536U);
  EXPECT_EQ(settings.spawnStamp, "vagent-local-spawn");

  EXPECT_EQ(settings.epistemic.maxSources, 128U);
  EXPECT_EQ(settings.epistemic.maxCandidates, 1024U);
  EXPECT_EQ(settings.epistemic.maxScopeDepth, 32U);
  EXPECT_EQ(settings.epistemic.maxNestedScopes, 8U);
  EXPECT_EQ(settings.epistemic.history, 4U);
  EXPECT_EQ(settings.epistemic.dedup, 16U);
  EXPECT_EQ(settings.epistemic.keysPerSource, 4U);
  EXPECT_EQ(settings.epistemic.ingressQueue, 64U);
  EXPECT_EQ(settings.epistemic.ingressQueuePerSource, 8U);
  EXPECT_EQ(settings.epistemic.outcomeLog, 64U);
  EXPECT_EQ(settings.epistemic.cells, 24576U);
  EXPECT_EQ(settings.epistemic.ingestPerWake, 4U);
  EXPECT_EQ(settings.epistemic.arenaSlots, std::size_t{1} << 30);
  EXPECT_EQ(settings.epistemic.healthHalfLife, 32);
  EXPECT_DOUBLE_EQ(settings.epistemic.suspectFraction, 0.25);
  EXPECT_EQ(settings.epistemic.suspectMinEvents, 8);
  EXPECT_EQ(settings.epistemic.healthReplayRing, 8);
  EXPECT_EQ(settings.epistemic.healthSequenceGap, 1024);

  const auto hostOptions = LocalHost::Options::fromStore(store);
  EXPECT_EQ(hostOptions.settings, settings);
  EXPECT_EQ(hostOptions.maxInstructions, 10'000'000U);

  const auto clusterOptions = cluster::ClusterNode::Options::fromStore(store);
  EXPECT_EQ(clusterOptions.hostOptions.settings, settings);
  EXPECT_EQ(clusterOptions.hostOptions.maxInstructions, 10'000'000U);
}

TEST(VagentSettingsTest, DefaultFallbacksFromInitializedStore) {
  Store store;
  store.setSystem(true);
  initializeSystemStore(store, SystemDocKind::Settings);

  const auto settings = AgentSettings::fromStore(store);
  EXPECT_EQ(settings, AgentSettings{});

  const auto hostOptions = LocalHost::Options::fromStore(store);
  EXPECT_EQ(hostOptions.settings, AgentSettings{});
  EXPECT_EQ(hostOptions.maxInstructions, 10'000'000U);

  const auto clusterOptions = cluster::ClusterNode::Options::fromStore(store);
  EXPECT_EQ(clusterOptions.hostOptions.settings, AgentSettings{});
  EXPECT_EQ(clusterOptions.hostOptions.maxInstructions, 10'000'000U);
}

TEST(VagentSettingsTest, DefaultFallbacksFromManifold) {
  Store store;
  store.setSystem(true);
  initializeSystemStore(store, SystemDocKind::Settings);

  const auto manifold = store.rebuildManifold(store.primaryCurrentVersion());
  const auto settings =
      AgentSettings::fromManifold(manifold, store.homeCell(), &store);
  EXPECT_EQ(settings, AgentSettings{});

  const auto hostOptions =
      LocalHost::Options::fromManifold(manifold, store.homeCell(), &store);
  EXPECT_EQ(hostOptions.settings, AgentSettings{});
  EXPECT_EQ(hostOptions.maxInstructions, 10'000'000U);

  const auto clusterOptions = cluster::ClusterNode::Options::fromManifold(
      manifold, store.homeCell(), &store);
  EXPECT_EQ(clusterOptions.hostOptions.settings, AgentSettings{});
  EXPECT_EQ(clusterOptions.hostOptions.maxInstructions, 10'000'000U);
}

TEST(VagentSettingsTest, DefaultFallbacksFromEmptyManifold) {
  zigzag::Manifold emptyManifold;
  const auto settings = AgentSettings::fromManifold(emptyManifold);
  EXPECT_EQ(settings, AgentSettings{});

  const auto hostOptions = LocalHost::Options::fromManifold(emptyManifold);
  EXPECT_EQ(hostOptions.settings, AgentSettings{});
  EXPECT_EQ(hostOptions.maxInstructions, 10'000'000U);

  const auto clusterOptions =
      cluster::ClusterNode::Options::fromManifold(emptyManifold);
  EXPECT_EQ(clusterOptions.hostOptions.settings, AgentSettings{});
  EXPECT_EQ(clusterOptions.hostOptions.maxInstructions, 10'000'000U);
}

TEST(VagentSettingsTest, CustomizedValuesFromStoreAndManifold) {
  Store store;
  store.setSystem(true);
  initializeSystemStore(store, SystemDocKind::Settings);

  auto v = store.primaryCurrentVersion();
  v = setSetting(store, v, settings::kVagentBeliefEventDepth, std::int64_t{32});
  v = setSetting(store, v, settings::kVagentConditionFuel, std::int64_t{5000});
  v = setSetting(store, v, settings::kVagentMaintainBackoffMs,
                 std::int64_t{12000});
  v = setSetting(store, v, settings::kVagentSendRetries, std::int64_t{9});
  v = setSetting(store, v, settings::kVagentSendRetryIntervalMs,
                 std::int64_t{3500});
  v = setSetting(store, v, settings::kVagentRenewFraction, 0.65);
  v = setSetting(store, v, settings::kVagentReadyPerMessage, std::int64_t{7});
  v = setSetting(store, v, settings::kVagentCompactEvery, std::int64_t{128});
  v = setSetting(store, v, settings::kVagentValueOperations,
                 std::int64_t{100000});
  v = setSetting(store, v, settings::kVagentSpawnStamp,
                 std::string{"custom-spawn-stamp"});
  v = setSetting(store, v, settings::kVagentMaxInstructions,
                 std::int64_t{25'000'000});
  v = setSetting(store, v, settings::kVagentMaxSources, std::int64_t{256});
  v = setSetting(store, v, settings::kVagentMaxCandidates, std::int64_t{2048});
  v = setSetting(store, v, settings::kVagentScopeDepth, std::int64_t{64});
  v = setSetting(store, v, settings::kVagentNestedScopes, std::int64_t{16});
  v = setSetting(store, v, settings::kVagentEpistemicHistory, std::int64_t{8});
  v = setSetting(store, v, settings::kVagentEpistemicDedup, std::int64_t{32});
  v = setSetting(store, v, settings::kVagentEpistemicKeysPerSource,
                 std::int64_t{10});
  v = setSetting(store, v, settings::kVagentEpistemicIngressQueue,
                 std::int64_t{128});
  v = setSetting(store, v, settings::kVagentEpistemicIngressQueuePerSource,
                 std::int64_t{16});
  v = setSetting(store, v, settings::kVagentEpistemicOutcomeLog,
                 std::int64_t{128});
  v = setSetting(store, v, settings::kVagentEpistemicCells,
                 std::int64_t{30000});
  v = setSetting(store, v, settings::kVagentEpistemicIngestPerWake,
                 std::int64_t{8});
  v = setSetting(store, v, settings::kVagentEpistemicArenaSlots,
                 std::int64_t{1'000'000});

  store.repointCurrentVersion(v);

  // Load from store
  const auto s = AgentSettings::fromStore(store);

  EXPECT_EQ(s.beliefEventDepth, 32U);
  EXPECT_EQ(s.conditionFuel, 5000U);
  EXPECT_EQ(s.maintainBackoff, std::chrono::milliseconds{12000});
  EXPECT_EQ(s.sendRetries, 9U);
  EXPECT_EQ(s.sendRetryInterval, std::chrono::milliseconds{3500});
  EXPECT_DOUBLE_EQ(s.renewFraction, 0.65);
  EXPECT_EQ(s.readyPerMessage, 7U);
  EXPECT_EQ(s.compactEvery, 128U);
  EXPECT_EQ(s.valueOperations, 100000U);
  EXPECT_EQ(s.spawnStamp, "custom-spawn-stamp");

  EXPECT_EQ(s.epistemic.maxSources, 256U);
  EXPECT_EQ(s.epistemic.maxCandidates, 2048U);
  EXPECT_EQ(s.epistemic.maxScopeDepth, 64U);
  EXPECT_EQ(s.epistemic.maxNestedScopes, 16U);
  EXPECT_EQ(s.epistemic.history, 8U);
  EXPECT_EQ(s.epistemic.dedup, 32U);
  EXPECT_EQ(s.epistemic.keysPerSource, 10U);
  EXPECT_EQ(s.epistemic.ingressQueue, 128U);
  EXPECT_EQ(s.epistemic.ingressQueuePerSource, 16U);
  EXPECT_EQ(s.epistemic.outcomeLog, 128U);
  EXPECT_EQ(s.epistemic.cells, 30000U);
  EXPECT_EQ(s.epistemic.ingestPerWake, 8U);
  EXPECT_EQ(s.epistemic.arenaSlots, 1'000'000U);

  const auto hostOpts = LocalHost::Options::fromStore(store);
  EXPECT_EQ(hostOpts.settings, s);
  EXPECT_EQ(hostOpts.maxInstructions, 25'000'000U);

  const auto clusterOpts = cluster::ClusterNode::Options::fromStore(store);
  EXPECT_EQ(clusterOpts.hostOptions.settings, s);
  EXPECT_EQ(clusterOpts.hostOptions.maxInstructions, 25'000'000U);

  // Load from manifold
  const auto manifold = store.rebuildManifold(v);
  const auto sm =
      AgentSettings::fromManifold(manifold, store.homeCell(), &store);
  EXPECT_EQ(sm, s);

  const auto hostOptsM =
      LocalHost::Options::fromManifold(manifold, store.homeCell(), &store);
  EXPECT_EQ(hostOptsM.settings, s);
  EXPECT_EQ(hostOptsM.maxInstructions, 25'000'000U);

  const auto clusterOptsM = cluster::ClusterNode::Options::fromManifold(
      manifold, store.homeCell(), &store);
  EXPECT_EQ(clusterOptsM.hostOptions.settings, s);
  EXPECT_EQ(clusterOptsM.hostOptions.maxInstructions, 25'000'000U);
}

TEST(VagentSettingsTest, BackwardsCompatibleSecondsConversion) {
  Store store;
  store.setSystem(true);
  initializeSystemStore(store, SystemDocKind::Settings);

  auto v = store.primaryCurrentVersion();
  v      = setSetting(store, v, settings::kVagentMaintainBackoff, 45.0);
  v      = setSetting(store, v, settings::kVagentSendRetryInterval, 2.5);
  store.repointCurrentVersion(v);

  const auto settings = AgentSettings::fromStore(store);
  EXPECT_EQ(settings.maintainBackoff, std::chrono::milliseconds{45000});
  EXPECT_EQ(settings.sendRetryInterval, std::chrono::milliseconds{2500});
}

TEST(VagentSettingsTest, InvalidValuesFallBackToDefaults) {
  Store store;
  store.setSystem(true);
  initializeSystemStore(store, SystemDocKind::Settings);

  auto v = store.primaryCurrentVersion();
  // Non-positive depth and fuel
  v = setSetting(store, v, settings::kVagentBeliefEventDepth,
                 std::int64_t{-10});
  v = setSetting(store, v, settings::kVagentConditionFuel, std::int64_t{0});
  // Out-of-bounds fraction
  v = setSetting(store, v, settings::kVagentRenewFraction, 1.5);
  // Non-positive instruction limit
  v = setSetting(store, v, settings::kVagentMaxInstructions,
                 std::int64_t{-500});
  // Inconsistent epistemic bounds: per-source queue larger than total queue
  v = setSetting(store, v, settings::kVagentEpistemicIngressQueue,
                 std::int64_t{10});
  v = setSetting(store, v, settings::kVagentEpistemicIngressQueuePerSource,
                 std::int64_t{50});
  store.repointCurrentVersion(v);

  const auto settings = AgentSettings::fromStore(store);
  // Default values should be preserved when stored values are invalid
  EXPECT_EQ(settings.beliefEventDepth, 16U);
  EXPECT_EQ(settings.conditionFuel, 1000U);
  EXPECT_DOUBLE_EQ(settings.renewFraction, 0.8);
  EXPECT_EQ(settings.epistemic, EpistemicLimits{});

  const auto hostOpts = LocalHost::Options::fromStore(store);
  EXPECT_EQ(hostOpts.maxInstructions, 10'000'000U);
}

namespace {
/// A settings store holding @p apply's changes over the defaults.
template <typename Apply> AgentSettings healthSettings(Apply &&apply) {
  Store store;
  store.setSystem(true);
  initializeSystemStore(store, SystemDocKind::Settings);
  auto v = store.primaryCurrentVersion();
  v      = apply(store, v);
  store.repointCurrentVersion(v);
  return AgentSettings::fromStore(store);
}
} // namespace

TEST(VagentSettingsTest, TheAdmissionPauseLoadsWithinItsRange) {
  EXPECT_EQ(AgentSettings{}.admissionPause, std::chrono::milliseconds(30000));
  const auto load = [](std::int64_t ms) {
    return healthSettings([ms](Store &store, auto v) {
      return setSetting(store, v, settings::kVagentAdmissionPauseMs, ms);
    });
  };
  EXPECT_EQ(load(5000).admissionPause, std::chrono::milliseconds(5000));
  EXPECT_EQ(load(1000).admissionPause, std::chrono::milliseconds(1000));
  EXPECT_EQ(load(600000).admissionPause, std::chrono::milliseconds(600000));
  // Out of range keeps the default rather than closing doors for ever or
  // never.
  EXPECT_EQ(load(999).admissionPause, std::chrono::milliseconds(30000));
  EXPECT_EQ(load(600001).admissionPause, std::chrono::milliseconds(30000));
}

TEST(VagentSettingsTest, HealthSettingsLoadEachField) {
  const auto s = healthSettings([](Store &store, auto v) {
    v = setSetting(store, v, settings::kVagentEpistemicHealthHalfLife,
                   std::int64_t{64});
    v = setSetting(store, v, settings::kVagentEpistemicSuspectFraction, 0.4);
    v = setSetting(store, v, settings::kVagentEpistemicSuspectMinEvents,
                   std::int64_t{20});
    v = setSetting(store, v, settings::kVagentEpistemicHealthReplayRing,
                   std::int64_t{16});
    v = setSetting(store, v, settings::kVagentEpistemicHealthSequenceGap,
                   std::int64_t{4096});
    return v;
  });
  EXPECT_EQ(s.epistemic.healthHalfLife, 64);
  EXPECT_DOUBLE_EQ(s.epistemic.suspectFraction, 0.4);
  EXPECT_EQ(s.epistemic.suspectMinEvents, 20);
  EXPECT_EQ(s.epistemic.healthReplayRing, 16);
  EXPECT_EQ(s.epistemic.healthSequenceGap, 4096);
}

TEST(VagentSettingsTest,
     AnInvalidHealthSettingKeepsTheDefaultsForTheWholeBlock) {
  // A half-life past its domain, a fraction outside (0, 1], a min_events
  // above 90% of the weight bound at H = 4 (floor(0.9 / (1 - b)) = 5), a ring
  // or a gap past its bound: each also resets the unrelated max_sources,
  // which is the whole-block fallback of decision F30(A).
  const auto bad = [](auto apply) {
    return healthSettings([&](Store &store, auto v) {
      v = setSetting(store, v, settings::kVagentMaxSources, std::int64_t{256});
      return apply(store, v);
    });
  };
  const auto check = [&](const AgentSettings &s, const char *what) {
    EXPECT_EQ(s.epistemic, EpistemicLimits{}) << what;
  };
  check(bad([](Store &store, auto v) {
          return setSetting(store, v, settings::kVagentEpistemicHealthHalfLife,
                            std::int64_t{2'000'000});
        }),
        "half-life");
  check(bad([](Store &store, auto v) {
          return setSetting(store, v, settings::kVagentEpistemicSuspectFraction,
                            1.5);
        }),
        "fraction above 1");
  check(bad([](Store &store, auto v) {
          return setSetting(store, v, settings::kVagentEpistemicSuspectFraction,
                            0.0);
        }),
        "fraction of 0");
  check(bad([](Store &store, auto v) {
          v = setSetting(store, v, settings::kVagentEpistemicHealthHalfLife,
                         std::int64_t{4});
          return setSetting(store, v,
                            settings::kVagentEpistemicSuspectMinEvents,
                            std::int64_t{8});
        }),
        "min_events above the weight bound");
  check(bad([](Store &store, auto v) {
          return setSetting(store, v,
                            settings::kVagentEpistemicHealthReplayRing,
                            std::int64_t{5000});
        }),
        "ring");
  check(bad([](Store &store, auto v) {
          return setSetting(store, v,
                            settings::kVagentEpistemicHealthSequenceGap,
                            std::int64_t{1} << 40);
        }),
        "gap");
  // A non-positive Int is skipped before validate(), so it neither warns nor
  // resets the block: only that field keeps its default.
  const auto skipped = bad([](Store &store, auto v) {
    return setSetting(store, v, settings::kVagentEpistemicHealthHalfLife,
                      std::int64_t{-5});
  });
  EXPECT_EQ(skipped.epistemic.healthHalfLife, 32);
  EXPECT_EQ(skipped.epistemic.maxSources, 256U);
}

TEST(VagentSettingsTest, AHalfLifeBelowTheQueueShareIsValidAndWarns) {
  // The relation H >= ingress_queue_per_source is a construction-time WARN,
  // not a refusal (R3-2): the setting loads.
  const auto s = healthSettings([](Store &store, auto v) {
    v = setSetting(store, v, settings::kVagentEpistemicIngressQueue,
                   std::int64_t{64});
    v = setSetting(store, v, settings::kVagentEpistemicIngressQueuePerSource,
                   std::int64_t{16});
    return setSetting(store, v, settings::kVagentEpistemicHealthHalfLife,
                      std::int64_t{8});
  });
  EXPECT_EQ(s.epistemic.healthHalfLife, 8);
  EXPECT_EQ(s.epistemic.ingressQueuePerSource, 16U);
  EXPECT_TRUE(s.epistemic.validate());
  // Both numbers the warning prints: the order error is 2^(16/8) = 4.
  EXPECT_DOUBLE_EQ(std::exp2(16.0 / 8.0), 4.0);
}

} // namespace xanadu::vagent::runtime
