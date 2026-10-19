/**
 * @file command_transaction.cpp
 * @brief Implementation of Command Transaction Mailbox & Promise Cells.
 */
#include "common/xanadu/zigzag/command_transaction.hpp"

#include <format>

namespace zigzag {

CellRef CommandMailbox::submitTransaction(
    ArenaManifold &manifold, const CellRef targetCell,
    const std::string_view verb,
    const std::function<CommandResult(std::string_view)> &handler) {
  // Ensure dimensions exist
  const DimRef dTarget = manifold.ensureDimension("d.target");
  const DimRef dAction = manifold.ensureDimension("d.action");
  const DimRef dReply  = manifold.ensureDimension("d.reply");

  // Mint transaction cell
  const CellRef txCell = manifold.makeCell("tx");

  // Mint action cell holding the command string
  const CellRef actionCell = manifold.makeCell(verb);

  // Mint promise cell
  const CellRef promiseCell = manifold.makeCell("pending");

  // Link transaction edges
  expectWritten(manifold.link(txCell, dTarget, DimVector::POS, targetCell));
  expectWritten(manifold.link(txCell, dAction, DimVector::POS, actionCell));
  expectWritten(manifold.link(txCell, dReply, DimVector::POS, promiseCell));

  // Invoke handler synchronously
  CommandResult result{.success = false, .response = "Unhandled action"};
  if (handler) {
    result = handler(verb);
  }

  // Populate promise cell with result
  const std::string statusStr = result.success ? "OK" : "ERROR";
  const std::string replyText =
      std::format("{}: {}", statusStr, result.response);

  // Overwrite promiseCell content
  const auto replySpan = manifold.intern(replyText);
  const std::array<xanadu::PrimediaSpan, 1> spans{replySpan};
  expectWritten(manifold.setContent(promiseCell, spans));

  return promiseCell;
}

std::optional<CommandResult>
CommandMailbox::queryPromise(const ArenaManifold &manifold,
                             const CellRef promiseCell) const {
  if (!manifold.contains(promiseCell)) {
    return std::nullopt;
  }

  const std::string text = manifold.textOf(promiseCell);
  if (text == "pending") {
    return CommandResult{.success = false, .response = "Pending"};
  }

  if (text.starts_with("OK: ")) {
    return CommandResult{.success = true, .response = text.substr(4)};
  }
  if (text.starts_with("ERROR: ")) {
    return CommandResult{.success = false, .response = text.substr(7)};
  }

  return CommandResult{.success = false, .response = text};
}

} // namespace zigzag
