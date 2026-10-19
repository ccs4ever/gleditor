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
 */
#ifndef ZIGZAG_COMMAND_TRANSACTION_HPP
#define ZIGZAG_COMMAND_TRANSACTION_HPP

#include <functional>
#include <optional>
#include <string>
#include <string_view>

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
};

} // namespace zigzag

#endif // ZIGZAG_COMMAND_TRANSACTION_HPP
