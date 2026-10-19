#include <chrono>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "common/xanadu/enfilade/treefilade.hpp"

using namespace xanadu::enfilade;

TEST(TreefiladeTest, FiftyThousandNodeAggregationAndSubMillisecondP99) {
  Treefilade fleet;

  const std::size_t kHostCount           = 50000;
  const std::vector<std::string> regions = {"us-east", "us-west", "eu-central",
                                            "ap-southeast"};
  const std::vector<std::string> zones   = {"zone-a", "zone-b", "zone-c"};

  // Insert 50,000 metrics across 4 regions and 3 zones
  // 99% of hosts have 10.0ms latency, 1% (tail) have 200.0ms latency
  for (std::size_t i = 0; i < kHostCount; ++i) {
    const auto &region     = regions[i % regions.size()];
    const auto &zone       = zones[(i / regions.size()) % zones.size()];
    const std::string host = "node-" + std::to_string(i);

    const double latency =
        (i % 50 == 49) ? 200.0 : (10.0 + (static_cast<double>(i % 10) * 0.1));
    fleet.recordMetric(region, zone, host, latency, AnomalyNone);
  }

  EXPECT_EQ(fleet.totalMetrics(), kHostCount);

  // Time O(1) p99 evaluation on root crum
  const auto t0    = std::chrono::steady_clock::now();
  const double p99 = fleet.fleetQuantile(0.99);
  const auto t1    = std::chrono::steady_clock::now();
  const auto durationUs =
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

  // Root crum evaluation must be instantaneous (< 1000 microseconds)
  EXPECT_LT(durationUs, 1000);

  // p99 should be in the tail bucket (~200ms)
  EXPECT_GE(p99, 150.0);
  EXPECT_LE(p99, 250.0);
}

TEST(TreefiladeTest, ReactiveAnomalyDetectionAndAlertDispatch) {
  Treefilade fleet;

  // Normal hosts
  fleet.recordMetric("us-east", "zone-a", "node-1", 12.0, AnomalyNone);
  fleet.recordMetric("us-east", "zone-a", "node-2", 14.0, AnomalyNone);
  fleet.recordMetric("us-west", "zone-b", "node-3", 15.0, AnomalyNone);

  EXPECT_FALSE(fleet.hasFleetAnomaly(AnomalyHighLatency));
  EXPECT_FALSE(fleet.hasFleetAnomaly(AnomalyErrorSpike));

  // Inject anomaly on specific node
  fleet.recordMetric("us-east", "zone-a", "node-bad", 500.0,
                     AnomalyHighLatency | AnomalyErrorSpike);

  EXPECT_TRUE(fleet.hasFleetAnomaly(AnomalyHighLatency));
  EXPECT_TRUE(fleet.hasFleetAnomaly(AnomalyErrorSpike));
  EXPECT_FALSE(fleet.hasFleetAnomaly(AnomalySaturation));

  std::vector<std::string> alerts;
  fleet.collectAlerts(alerts);

  ASSERT_EQ(alerts.size(), 1U);
  EXPECT_TRUE(alerts[0].contains("us-east/zone-a/node-bad"));
  EXPECT_TRUE(alerts[0].contains("HighLatency"));
  EXPECT_TRUE(alerts[0].contains("ErrorSpike"));
}
