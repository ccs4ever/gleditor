#include <gtest/gtest.h>

#include "common/xanadu/ops.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/dim_cap.hpp"

using namespace zigzag;

TEST(DimCapTest, UnauthorizedCtlLinkDenialAndAuditedExecution) {
  const std::string secret = "cluster-secret-key-42";
  DimAccessController controller(secret);

  // 1. Read-only operator token: allowed d.threads, forbidden d.ctl
  auto operatorToken = DimCap::mint("operator-alice", {"d.threads", "d.mem"},
                                    {"d.ctl"}, 0, secret);

  // d.threads is allowed
  auto resThreads = controller.verifyAccess(operatorToken, "d.threads");
  EXPECT_TRUE(resThreads.has_value());

  // d.ctl is denied
  auto resCtl = controller.verifyAccess(operatorToken, "d.ctl");
  ASSERT_FALSE(resCtl.has_value());
  EXPECT_EQ(resCtl.error(), CapError::PermissionDenied);

  // 2. Admin token with wildcard access
  auto adminToken = DimCap::mint("admin-bob", {"*"}, {}, 0, secret);

  ArenaManifold arena;

  // Missing audit context rejected
  AuditRecord badAudit{"", "", "", ""};
  auto resBad = controller.executeAuditedAction(arena, adminToken, badAudit,
                                                "kill -9 101");
  ASSERT_FALSE(resBad.has_value());
  EXPECT_EQ(resBad.error(), CapError::MissingAuditContext);

  // Valid audited action execution
  AuditRecord goodAudit{"admin-bob", "TICKET-9921", "ed25519-sig-bob", ""};
  auto resGood = controller.executeAuditedAction(arena, adminToken, goodAudit,
                                                 "kill -9 101");
  ASSERT_TRUE(resGood.has_value());

  const auto actCell = *resGood;
  EXPECT_NE(actCell, noCell);

  // Verify audit links are established in the arena
  const auto dActor  = arena.ensureDimension("d.actor");
  const auto dTicket = arena.ensureDimension("d.ticket");
  const auto dSig    = arena.ensureDimension("d.signature");

  EXPECT_TRUE(arena.linked(actCell, dActor, DimVector::POS).has_value());
  EXPECT_TRUE(arena.linked(actCell, dTicket, DimVector::POS).has_value());
  EXPECT_TRUE(arena.linked(actCell, dSig, DimVector::POS).has_value());

  // Verify audit history record
  ASSERT_EQ(controller.auditLog().size(), 1U);
  EXPECT_EQ(controller.auditLog()[0].ticketId, "TICKET-9921");
  EXPECT_EQ(controller.auditLog()[0].actionPayload, "kill -9 101");
}

TEST(DimCapTest, CircularKdumpOpsSpoolPostPanicRecovery) {
  KdumpOpsSpool nvram(3); // Small capacity of 3 entries

  // Record 4 panic breadcrumbs (1st gets overwritten in circular buffer)
  nvram.recordPanic(1001, 0xDEADBEEF, 10, "Kernel panic: NULL dereference");
  nvram.recordPanic(1002, 0xCAFEBABE, 11, "Page fault in ring0");
  nvram.recordPanic(1003, 0xFEEDFACE, 12, "Watchdog timeout");
  nvram.recordPanic(1004, 0xBAADF00D, 13, "Out of memory in kernel arena");

  // Recover post-panic trace upon simulated reboot
  const auto trace = nvram.recoverTrace();
  ASSERT_EQ(trace.size(), 3U);

  // The 3 surviving breadcrumbs must be in chronological order
  EXPECT_EQ(trace[0].timestampMs, 1002U);
  EXPECT_EQ(trace[0].faultIp, 0xCAFEBABEU);

  EXPECT_EQ(trace[1].timestampMs, 1003U);
  EXPECT_EQ(trace[1].faultIp, 0xFEEDFACEU);

  EXPECT_EQ(trace[2].timestampMs, 1004U);
  EXPECT_EQ(trace[2].faultIp, 0xBAADF00DU);
}
