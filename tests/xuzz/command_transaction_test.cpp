/**
 * @file command_transaction_test.cpp
 * @brief Unit tests for Command Transaction Mailbox and Promise Cells.
 */
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/command_transaction.hpp"
#include "common/xanadu/zigzag/crdt_zz.hpp"

namespace {

using namespace zigzag;

TEST(CommandTransactionTest, SubmitAndResolveSuccess) {
  ArenaManifold arena;
  const CellRef target = arena.makeCell("proc:1234");

  CommandMailbox mailbox;
  const auto handler = [](std::string_view verb) -> CommandResult {
    if (verb == "pause") {
      return CommandResult{.success = true, .response = "Process 1234 paused"};
    }
    return CommandResult{.success = false, .response = "Unknown verb"};
  };

  const CellRef promiseCell =
      mailbox.submitTransaction(arena, target, "pause", handler);
  ASSERT_NE(promiseCell, noCell);

  const auto reply = mailbox.queryPromise(arena, promiseCell);
  ASSERT_TRUE(reply.has_value());
  EXPECT_TRUE(reply->success);
  EXPECT_EQ(reply->response, "Process 1234 paused");

  EXPECT_TRUE(crdt::CRDTEngine::verifyManifoldInvariants(arena));
}

TEST(CommandTransactionTest, SubmitAndResolveFailure) {
  ArenaManifold arena;
  const CellRef target = arena.makeCell("proc:9999");

  CommandMailbox mailbox;
  const auto handler = [](std::string_view) -> CommandResult {
    return CommandResult{.success = false, .response = "PID 9999 not found"};
  };

  const CellRef promiseCell =
      mailbox.submitTransaction(arena, target, "kill", handler);
  ASSERT_NE(promiseCell, noCell);

  const auto reply = mailbox.queryPromise(arena, promiseCell);
  ASSERT_TRUE(reply.has_value());
  EXPECT_FALSE(reply->success);
  EXPECT_EQ(reply->response, "PID 9999 not found");

  EXPECT_TRUE(crdt::CRDTEngine::verifyManifoldInvariants(arena));
}

TEST(CommandTransactionTest, MultipleConcurrentTransactionsNoCollision) {
  ArenaManifold arena;
  const CellRef target = arena.makeCell("proc:500");

  CommandMailbox mailbox;
  std::vector<CellRef> promises;

  for (int i = 0; i < 10; ++i) {
    const std::string cmd = "metric_" + std::to_string(i);
    const CellRef p =
        mailbox.submitTransaction(arena, target, cmd, [i](std::string_view) {
          return CommandResult{.success  = true,
                               .response = "Ack " + std::to_string(i)};
        });
    promises.push_back(p);
  }

  for (int i = 0; i < 10; ++i) {
    const auto reply = mailbox.queryPromise(arena, promises[i]);
    ASSERT_TRUE(reply.has_value());
    EXPECT_TRUE(reply->success);
    EXPECT_EQ(reply->response, "Ack " + std::to_string(i));
  }

  EXPECT_TRUE(crdt::CRDTEngine::verifyManifoldInvariants(arena));
}

} // namespace
