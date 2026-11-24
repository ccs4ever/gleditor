/**
 * @file command_transaction.cpp
 * @brief Implementation of Command Transaction Mailbox & Promise Cells.
 */
#include "common/xanadu/zigzag/command_transaction.hpp"

#include <cstring>
#include <exception>
#include <format>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace zigzag {

namespace {

/// The reply text both paths write into the promise cell: the synchronous
/// `publishResult` and the async adapter's `Complete` payload spell a
/// `CommandResult` identically, so `queryPromise`/`resultOf` read either.
std::string replyTextOf(const CommandResult &result) {
  return std::format("{}: {}", result.success ? "OK" : "ERROR",
                     result.response);
}

std::vector<std::byte> bytesOf(std::string_view text) {
  std::vector<std::byte> out(text.size());
  std::memcpy(out.data(), text.data(), text.size());
  return out;
}

std::string_view outcomeName(const xanadu::vortex::IoOutcome &outcome) {
  return std::visit(
      []<typename T>(const T &) -> std::string_view {
        using U     = std::remove_cvref_t<T>;
        namespace v = xanadu::vortex;
        if constexpr (std::is_same_v<U, v::Complete>)
          return "Complete";
        else if constexpr (std::is_same_v<U, v::EndOfStream>)
          return "EndOfStream";
        else if constexpr (std::is_same_v<U, v::Partial>)
          return "Partial";
        else if constexpr (std::is_same_v<U, v::Failed>)
          return "Failed";
        else if constexpr (std::is_same_v<U, v::Cancelled>)
          return "Cancelled";
        else if constexpr (std::is_same_v<U, v::TimedOut>)
          return "TimedOut";
        else if constexpr (std::is_same_v<U, v::Overflow>)
          return "Overflow";
        else if constexpr (std::is_same_v<U, v::MessageTooLarge>)
          return "MessageTooLarge";
        else if constexpr (std::is_same_v<U, v::Stale>)
          return "Stale";
        else if constexpr (std::is_same_v<U, v::Conflict>)
          return "Conflict";
        else
          return "Unknown";
      },
      outcome);
}

} // namespace

CellRef CommandMailbox::submitTransaction(
    ArenaManifold &manifold, const CellRef targetCell,
    const std::string_view verb,
    const std::function<CommandResult(std::string_view)> &handler) {
  const CellRef promiseCell = beginTransaction(manifold, targetCell, verb);

  // Invoke handler synchronously
  CommandResult result{.success = false, .response = "Unhandled action"};
  if (handler) {
    result = handler(verb);
  }
  publishResult(manifold, promiseCell, result);
  return promiseCell;
}

CellRef CommandMailbox::beginTransaction(ArenaManifold &manifold,
                                         const CellRef targetCell,
                                         const std::string_view verb) {
  // Ensure dimensions exist
  const DimRef dTarget = manifold.ensureDimension("d.target");
  const DimRef dAction = manifold.ensureDimension("d.action");
  const DimRef dReply  = manifold.ensureDimension("d.reply");

  // Mint transaction cell
  const CellRef txCell = manifold.makeCell("tx");

  // Mint action cell holding the command string
  const CellRef actionCell = manifold.makeCell(verb);

  // Mint promise cell; "pending" is what queryPromise reports until
  // publishResult (sync path or staged IoService publication) fills it.
  const CellRef promiseCell = manifold.makeCell("pending");

  // Link transaction edges
  expectWritten(manifold.link(txCell, dTarget, DimVector::POS, targetCell));
  expectWritten(manifold.link(txCell, dAction, DimVector::POS, actionCell));
  expectWritten(manifold.link(txCell, dReply, DimVector::POS, promiseCell));

  return promiseCell;
}

void CommandMailbox::publishResult(ArenaManifold &manifold,
                                   const CellRef promiseCell,
                                   const CommandResult &result) const {
  const auto replySpan = manifold.intern(replyTextOf(result));
  const std::array<xanadu::PrimediaSpan, 1> spans{replySpan};
  expectWritten(manifold.setContent(promiseCell, spans));
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

// -- CommandMailboxAdapter ----------------------------------------------------

std::uint32_t CommandMailboxAdapter::stage(
    std::string verb, std::function<CommandResult(std::string_view)> handler) {
  const std::lock_guard lock(mutex_);
  const auto key = nextKey_++;
  staged_[key]   = Staged{std::move(verb), std::move(handler)};
  return key;
}

auto CommandMailboxAdapter::prepare(const xanadu::vortex::CellRequest &request,
                                    xanadu::vortex::OwnerContext &)
    -> std::expected<xanadu::vortex::PreparedOperation,
                     xanadu::vortex::IoErrorInfo> {
  const auto key = static_cast<std::uint32_t>(request.specification);
  {
    const std::lock_guard lock(mutex_);
    if (!staged_.contains(key))
      return std::unexpected(xanadu::vortex::IoErrorInfo{
          xanadu::vortex::IoErrorCode::InvalidRequest});
  }
  // Only the staging key crosses to the worker -- the handler and verb
  // stay owner-side until execute re-looks them up, matching every other
  // staged adapter's cell-free PreparedOperation.
  std::vector<std::byte> bytes(sizeof(key));
  std::memcpy(bytes.data(), &key, sizeof(key));
  return xanadu::vortex::PreparedOperation{request.key, std::move(bytes)};
}

void CommandMailboxAdapter::execute(
    const xanadu::vortex::BackendCommand &command,
    xanadu::vortex::BackendContext &,
    xanadu::vortex::CompletionSink &sink) noexcept {
  std::uint32_t key{};
  if (command.operation.bytes.size() >= sizeof(key))
    std::memcpy(&key, command.operation.bytes.data(), sizeof(key));

  // One-shot: the staged entry is consumed by the worker turn that runs
  // it, so a replayed command can never re-invoke the handler.
  Staged staged;
  bool found = false;
  {
    const std::lock_guard lock(mutex_);
    const auto it = staged_.find(key);
    if (it != staged_.end()) {
      staged = std::move(it->second);
      staged_.erase(it);
      found = true;
    }
  }

  if (!found) {
    sink.terminalCandidate(
        command.token,
        xanadu::vortex::BackendResult{
            xanadu::vortex::Failed{
                {xanadu::vortex::IoErrorCode::InvalidRequest, 0}, {}, {}},
            {}},
        {});
    sink.releaseAcknowledged(
        {xanadu::vortex::PrivateReleaseToken{command.token}});
    return;
  }

  // The handler runs off-owner and must not touch ArenaManifold; its
  // CommandResult is frozen into the Complete payload the owner later
  // decodes with resultOf and publishes through publishResult.
  CommandResult result{.success = false, .response = "Unhandled action"};
  try {
    if (staged.handler) {
      result = staged.handler(staged.verb);
    }
  } catch (const std::exception &e) {
    result =
        CommandResult{.success  = false,
                      .response = std::string("handler threw: ") + e.what()};
  } catch (...) {
    result = CommandResult{.success = false, .response = "handler threw"};
  }

  sink.terminalCandidate(
      command.token,
      xanadu::vortex::BackendResult{
          xanadu::vortex::Complete{
              {},
              bytesOf(replyTextOf(result)),
              xanadu::vortex::IoProgress{
                  .phase = xanadu::vortex::IoPhase::Terminal}},
          {}},
      {});
  sink.releaseAcknowledged(
      {xanadu::vortex::PrivateReleaseToken{command.token}});
}

auto CommandMailboxAdapter::publish(
    const xanadu::vortex::NativeCompletion &completion,
    xanadu::vortex::OwnerContext &)
    -> std::expected<xanadu::vortex::OwnerPublication,
                     xanadu::vortex::IoErrorInfo> {
  return xanadu::vortex::OwnerPublication{completion.result.outcome};
}

CommandResult
CommandMailboxAdapter::resultOf(const xanadu::vortex::IoOutcome &outcome) {
  const auto *complete = std::get_if<xanadu::vortex::Complete>(&outcome);
  if (complete == nullptr) {
    // A non-Complete terminal outcome is a failed command, never a
    // logical failure -- the promise cell still gets an ERROR reply.
    return CommandResult{.success  = false,
                         .response = std::string("io outcome ") +
                                     std::string(outcomeName(outcome))};
  }
  const auto *bytes = std::get_if<std::vector<std::byte>>(&complete->value);
  if (bytes == nullptr) {
    return CommandResult{.success = false, .response = "empty command result"};
  }
  std::string text;
  text.reserve(bytes->size());
  for (const auto b : *bytes) {
    text.push_back(static_cast<char>(b));
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
