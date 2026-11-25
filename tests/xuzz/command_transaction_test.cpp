/**
 * @file command_transaction_test.cpp
 * @brief Unit tests for Command Transaction Mailbox and Promise Cells.
 */
#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "common/xanadu/vortex/payload_resolver.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/command_transaction.hpp"
#include "common/xanadu/zigzag/crdt_zz.hpp"

namespace {

using namespace zigzag;
using namespace xanadu::vortex;

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

TEST(CommandTransactionTest, ThrowingSyncHandlerPublishesErrorReply) {
  ArenaManifold arena;
  const CellRef target = arena.makeCell("proc:throw");

  CommandMailbox mailbox;
  // A throwing handler must not escape with the promise cell still
  // "pending": the sync path reports the same ERROR reply the staged
  // adapter path produces for a throwing handler.
  const CellRef promiseCell = mailbox.submitTransaction(
      arena, target, "explode", [](std::string_view) -> CommandResult {
        throw std::runtime_error("boom");
      });
  ASSERT_NE(promiseCell, noCell);

  const auto reply = mailbox.queryPromise(arena, promiseCell);
  ASSERT_TRUE(reply.has_value());
  EXPECT_FALSE(reply->success);
  EXPECT_NE(reply->response.find("handler threw"), std::string::npos);
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

// -- the async half: beginTransaction/publishResult -------------------------

TEST(CommandTransactionTest, BeginLeavesPendingUntilPublishResult) {
  ArenaManifold arena;
  const CellRef target = arena.makeCell("proc:77");

  CommandMailbox mailbox;
  const CellRef promise = mailbox.beginTransaction(arena, target, "status");
  ASSERT_NE(promise, noCell);
  EXPECT_EQ(arena.textOf(promise), "pending");

  mailbox.publishResult(arena, promise,
                        CommandResult{.success = true, .response = "running"});
  const auto reply = mailbox.queryPromise(arena, promise);
  ASSERT_TRUE(reply.has_value());
  EXPECT_TRUE(reply->success);
  EXPECT_EQ(reply->response, "running");
  EXPECT_TRUE(crdt::CRDTEngine::verifyManifoldInvariants(arena));
}

// -- CommandMailboxAdapter: the §6 request-cell migration --------------------

namespace {

struct CapturingSink final : CompletionSink {
  /// @p resolver mirrors the service sink: the terminal event retains the
  /// payload so the bytes outlive the worker lease `execute`'s guard
  /// releases on return. Pass the same PayloadResolver the BackendContext
  /// carries.
  explicit CapturingSink(PayloadResolver *resolver = nullptr)
      : payloads(resolver) {}
  void progress(PrivateRequestToken, IoProgress) noexcept override {}
  void
  terminalCandidate(PrivateRequestToken, BackendResult result,
                    std::optional<PayloadLease> payload) noexcept override {
    candidate = std::move(result);
    if (payload && payloads) {
      if (auto retained = payloads->retain(payload->range)) {
        payloadLease = *retained;
      }
    }
  }
  void releaseAcknowledged(BackendReleaseAck ack) noexcept override {
    released = ack;
  }
  PayloadResolver *payloads;
  std::optional<BackendResult> candidate;
  std::optional<PayloadLease> payloadLease;
  std::optional<BackendReleaseAck> released;
};

BackendContext makeContext(PayloadResolver &payloads) {
  return BackendContext{.stop = {}, .payloads = payloads, .budget = {1 << 20}};
}

CellRequest requestFor(AdapterKey key, std::uint32_t specification) {
  return CellRequest{.key           = key,
                     .capability    = CapabilityCell{},
                     .specification = specification};
}

constexpr AdapterKey mailboxKey{30, 1};

} // namespace

TEST(CommandMailboxAdapterTest,
     StagePrepareExecutePublishRoundTripsThroughResultOf) {
  CommandMailboxAdapter adapter;
  CommandMailbox mailbox;
  ArenaManifold arena;
  const CellRef target  = arena.makeCell("proc:5");
  const CellRef promise = mailbox.beginTransaction(arena, target, "ping");
  ASSERT_NE(promise, noCell);

  const auto key =
      adapter.stage("ping", [](std::string_view verb) -> CommandResult {
        return CommandResult{.success = true,
                             .response =
                                 std::string("pong:") + std::string(verb)};
      });
  OwnerContext owner;
  PayloadResolver payloads;

  const auto prepared = adapter.prepare(requestFor(mailboxKey, key), owner);
  ASSERT_TRUE(prepared.has_value());

  CapturingSink sink{&payloads};
  auto context = makeContext(payloads);
  adapter.execute(BackendCommand{{1}, *prepared}, context, sink);
  ASSERT_TRUE(sink.candidate.has_value());
  ASSERT_TRUE(sink.released.has_value());
  EXPECT_TRUE(std::holds_alternative<Complete>(sink.candidate->outcome));

  const auto published = adapter.publish(
      NativeCompletion{{1}, *sink.candidate, {}, *sink.released}, owner);
  ASSERT_TRUE(published.has_value());

  const auto result =
      CommandMailboxAdapter::resultOf(published->outcome, payloads);
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.response, "pong:ping");

  // The owner-side half: the decoded result fills the promise cell.
  mailbox.publishResult(arena, promise, result);
  const auto reply = mailbox.queryPromise(arena, promise);
  ASSERT_TRUE(reply.has_value());
  EXPECT_TRUE(reply->success);
  EXPECT_EQ(reply->response, "pong:ping");
}

TEST(CommandMailboxAdapterTest, PrepareRefusesAnUnknownStagingKey) {
  CommandMailboxAdapter adapter;
  OwnerContext owner;
  const auto prepared = adapter.prepare(requestFor(mailboxKey, 77), owner);
  ASSERT_FALSE(prepared.has_value());
  EXPECT_EQ(prepared.error().code, IoErrorCode::InvalidRequest);
}

TEST(CommandMailboxAdapterTest, StagedEntriesAreOneShot) {
  CommandMailboxAdapter adapter;
  OwnerContext owner;
  PayloadResolver payloads;
  auto context = makeContext(payloads);

  const auto key      = adapter.stage("once", [](std::string_view) {
    return CommandResult{.success = true, .response = "ran"};
  });
  const auto prepared = adapter.prepare(requestFor(mailboxKey, key), owner);
  ASSERT_TRUE(prepared.has_value());

  CapturingSink first{&payloads};
  adapter.execute(BackendCommand{{1}, *prepared}, context, first);
  ASSERT_TRUE(first.candidate.has_value());
  EXPECT_TRUE(std::holds_alternative<Complete>(first.candidate->outcome));

  // A replayed command can never re-invoke the handler: the staged entry
  // was consumed by the first worker turn.
  CapturingSink replay{&payloads};
  adapter.execute(BackendCommand{{2}, *prepared}, context, replay);
  ASSERT_TRUE(replay.candidate.has_value());
  const auto *failed = std::get_if<Failed>(&replay.candidate->outcome);
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->reason.code, IoErrorCode::InvalidRequest);
}

TEST(CommandMailboxAdapterTest, ResultOfDecodesFailuresWithoutThrowing) {
  PayloadResolver payloads;
  const auto failedOutcome =
      IoOutcome{Failed{{IoErrorCode::BackendFailure, 3}, {}, {}}};
  const auto result = CommandMailboxAdapter::resultOf(failedOutcome, payloads);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.response.find("Failed"), std::string::npos);

  // A Complete carrying no resolvable payload range decodes to a failed
  // CommandResult, never a throw or a phantom success.
  const auto complete = IoOutcome{
      Complete{{}, std::monostate{}, IoProgress{.phase = IoPhase::Terminal}}};
  const auto empty = CommandMailboxAdapter::resultOf(complete, payloads);
  EXPECT_FALSE(empty.success);
}

} // namespace
