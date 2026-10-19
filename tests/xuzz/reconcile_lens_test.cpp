#include <gtest/gtest.h>

#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/reconcile_lens.hpp"

using namespace zigzag;

TEST(ReconcileLensTest, DeclarativeReconciliationAndZeroLeakHypertimeRollback) {
  auto mockDriver = std::make_shared<MockOSDriver>();
  ReconcileLens lens(mockDriver);

  // Snapshot 1 (Historical baseline): only 1 service
  std::vector<ServiceConfig> snap1 = {{.name     = "auth-svc",
                                       .command  = "/bin/auth",
                                       .port     = 8080,
                                       .replicas = 1}};

  // Snapshot 2 (Expanded rollout): 3 services
  std::vector<ServiceConfig> snap2 = {
      {.name = "auth-svc", .command = "/bin/auth", .port = 8080, .replicas = 1},
      {.name     = "db-proxy",
       .command  = "/bin/dbproxy",
       .port     = 5432,
       .replicas = 1},
      {.name     = "metrics-exporter",
       .command  = "/bin/exporter",
       .port     = 9100,
       .replicas = 1}};

  // 1. Initial reconciliation to Snap 1
  lens.reconcile(snap1);
  EXPECT_EQ(mockDriver->processCount(), 1U);
  EXPECT_EQ(mockDriver->portCount(), 1U);
  EXPECT_TRUE(mockDriver->hasPort(8080));

  // 2. Advance to Snap 2 (3 services)
  lens.reconcile(snap2);
  EXPECT_EQ(mockDriver->processCount(), 3U);
  EXPECT_EQ(mockDriver->portCount(), 3U);
  EXPECT_TRUE(mockDriver->hasPort(8080));
  EXPECT_TRUE(mockDriver->hasPort(5432));
  EXPECT_TRUE(mockDriver->hasPort(9100));

  // 3. Hypertime rollback backward to Snap 1:
  // Must terminate the 2 newer services and unbind their ports with zero leaks!
  const auto unwoundCount = lens.rollbackTo(snap1);
  EXPECT_EQ(unwoundCount, 2U);

  // Verify only auth-svc is alive, no leaked processes or ports
  EXPECT_EQ(mockDriver->processCount(), 1U);
  EXPECT_EQ(mockDriver->portCount(), 1U);
  EXPECT_TRUE(mockDriver->hasPort(8080));
  EXPECT_FALSE(mockDriver->hasPort(5432));
  EXPECT_FALSE(mockDriver->hasPort(9100));
}

TEST(ReconcileLensTest, CryptographicCanaryBarrierRollout) {
  auto mockDriver = std::make_shared<MockOSDriver>();
  ReconcileLens lens(mockDriver);

  // Invalid canary token rejected
  EXPECT_FALSE(lens.advanceCanaryPhase("unsigned-token-123"));

  // Valid cryptographic token accepted
  EXPECT_TRUE(lens.advanceCanaryPhase("canary-ok-stage-1-ed25519-sig"));
  EXPECT_TRUE(lens.advanceCanaryPhase("canary-ok-stage-2-ed25519-sig"));
}
