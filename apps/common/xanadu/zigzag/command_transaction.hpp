/**
 * @file command_transaction.hpp
 * @brief Command Transaction Mailbox & Promise Cells for ZigZag Control Ranks.
 *
 * Implements the non-colliding Command Transaction pattern replacing shared
 * multi-writer d.ctl ranks (Invariant A6).
 *
 * Mechanism:
 * - Client mints a private Transaction cell T.
 * - T links to the target cell (+d.target).
 * - T links to the command action string (+d.action).
 * - T links to a private Promise cell (+d.reply).
 * - The receiving lens evaluates the action synchronously or asynchronously,
 *   writing "OK" or "Error(errstr)" into the Promise cell.
 *
 * `submitTransaction` keeps its original inline-handler contract for
 * existing callers. `CommandMailboxAdapter` (design/vortex-io-implementation-
 * plan.md §6, §13 M4 row: "command mailbox ... migrated from inline handler
 * invocation to request cells and later owner publication") is the async
 * path: `prepare` stages a handler+verb pair on the owner thread, `execute`
 * runs the handler off-owner (never touching `ArenaManifold`), and
 * `publish` -- back on the owner -- is what actually writes the promise
 * cell's content, through `CommandMailbox::publishResult`. The transaction/
 * promise cell minting and the handler's own CommandResult contract are
 * unchanged; only when the handler runs and when the promise cell is
 * written move off the inline synchronous path.
 */
#ifndef ZIGZAG_COMMAND_TRANSACTION_HPP
#define ZIGZAG_COMMAND_TRANSACTION_HPP

#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/xanadu/vortex/io_protocol.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace zigzag {

struct CommandResult {
  bool success{false};
  std::string response;
};

class CommandMailbox {
public:
  CommandMailbox() = default;

  /**
   * @brief Submit a command transaction directed at @p targetCell.
   *
   * Mints transaction and promise cells, establishes directional links,
   * invokes @p handler, and populates the promise cell with the outcome.
   *
   * @return CellRef of the Promise cell containing the reply.
   */
  CellRef submitTransaction(
      ArenaManifold &manifold, CellRef targetCell, std::string_view verb,
      const std::function<CommandResult(std::string_view)> &handler);

  /**
   * @brief Read the outcome from a Promise cell.
   */
  [[nodiscard]] std::optional<CommandResult>
  queryPromise(const ArenaManifold &manifold, CellRef promiseCell) const;

  /**
   * @brief The async entry point: mint transaction/promise cells and the
   *        directional links @ref submitTransaction does, but invoke no
   *        handler -- the promise cell is left "pending" for
   *        @ref publishResult to fill once the caller's
   *        `CommandMailboxAdapter` submission completes.
   */
  CellRef beginTransaction(ArenaManifold &manifold, CellRef targetCell,
                           std::string_view verb);

  /**
   * @brief Owner-side: write a handler outcome into a promise cell
   *        @ref beginTransaction minted. Called from the owner thread after
   *        `IoService` delivers the terminal outcome of a
   *        `CommandMailboxAdapter` submission -- the one place the async
   *        path still touches `ArenaManifold`.
   */
  void publishResult(ArenaManifold &manifold, CellRef promiseCell,
                     const CommandResult &result) const;
};

/**
 * @brief Migrates `CommandMailbox` from inline handler invocation to the
 *        `IoAdapter::prepare/execute/publish` path (design/vortex-io-
 *        implementation-plan.md §6, §13 M4 row). `stage` records one
 *        verb+handler pair on the owner thread; `prepare` looks it up by the
 *        request's `specification` key; `execute` invokes the handler
 *        off-owner (the handler itself must not touch `ArenaManifold` --
 *        the same worker-ownership rule every other M4 adapter observes);
 *        `publish` is a pure outcome forward. The caller is responsible for
 *        calling `CommandMailbox::publishResult` with that outcome once
 *        `IoService` delivers it, mirroring how `ProjectionSession`'s
 *        staged refresh/commit leaves arena writes to the owner.
 */
class CommandMailboxAdapter final : public xanadu::vortex::IoAdapter {
public:
  [[nodiscard]] std::uint32_t
  stage(std::string verb,
        std::function<CommandResult(std::string_view)> handler);

  auto prepare(const xanadu::vortex::CellRequest &request,
               xanadu::vortex::OwnerContext &)
      -> std::expected<xanadu::vortex::PreparedOperation,
                       xanadu::vortex::IoErrorInfo> override;
  void execute(const xanadu::vortex::BackendCommand &command,
               xanadu::vortex::BackendContext &context,
               xanadu::vortex::CompletionSink &sink) noexcept override;
  auto publish(const xanadu::vortex::NativeCompletion &completion,
               xanadu::vortex::OwnerContext &)
      -> std::expected<xanadu::vortex::OwnerPublication,
                       xanadu::vortex::IoErrorInfo> override;

  /// Decode a terminal `Complete` outcome from this adapter back into a
  /// `CommandResult`, for `CommandMailbox::publishResult`. Any other
  /// outcome (Failed/Cancelled/...) decodes to a failed CommandResult
  /// naming the outcome, never a thrown exception or logical failure.
  [[nodiscard]] static CommandResult
  resultOf(const xanadu::vortex::IoOutcome &outcome);

private:
  struct Staged {
    std::string verb;
    std::function<CommandResult(std::string_view)> handler;
  };
  std::mutex mutex_;
  std::unordered_map<std::uint32_t, Staged> staged_;
  std::uint32_t nextKey_{1};
};

} // namespace zigzag

#endif // ZIGZAG_COMMAND_TRANSACTION_HPP
