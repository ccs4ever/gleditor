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

  // Invoke handler synchronously. A throwing handler must not escape with
  // the promise cell still "pending" -- it publishes the same ERROR reply
  // the staged adapter path reports for a throwing handler.
  CommandResult result{.success = false, .response = "Unhandled action"};
  if (handler) {
    try {
      result = handler(verb);
    } catch (const std::exception &e) {
      result =
          CommandResult{.success  = false,
                        .response = std::string("handler threw: ") + e.what()};
    } catch (...) {
      result = CommandResult{.success = false, .response = "handler threw"};
    }
  }
  publishResult(manifold, promiseCell, result);
  return promiseCell;
}

CellRef CommandMailbox::beginTransaction(ArenaManifold &manifold,
                                         const CellRef targetCell,
                                         const std::string_view verb) {
  // The three mints and three links are one logical step: a mid-sequence
  // failure (expectWritten throws, or the arena refuses) must roll the
  // whole mint back rather than leak a partial transaction graph.
  const auto mark = manifold.mark();
  try {
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

    if (!manifold.discard(mark)) {
      throw std::runtime_error("beginTransaction: arena refused mark discard");
    }
    return promiseCell;
  } catch (...) {
    static_cast<void>(manifold.release(mark));
    throw;
  }
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

namespace {

/// Live staged commands per adapter; a full table refuses new stages
/// (key 0) rather than evicting an entry an in-flight request still names.
constexpr std::size_t maxStagedCommands = 4096;

xanadu::vortex::BackendResult
mailboxFailed(xanadu::vortex::IoErrorInfo reason) {
  return xanadu::vortex::BackendResult{
      xanadu::vortex::Failed{reason,
                             {},
                             xanadu::vortex::IoProgress{
                                 .phase = xanadu::vortex::IoPhase::Terminal}},
      {}};
}

} // namespace

std::uint32_t CommandMailboxAdapter::stage(
    std::string verb, std::function<CommandResult(std::string_view)> handler) {
  const std::lock_guard lock(mutex_);
  if (staged_.size() >= maxStagedCommands) return 0;
  // Skip 0 (the table-full sentinel) and any key a wrapped counter could
  // revisit while still live.
  auto key = nextKey_++;
  while (key == 0 || staged_.contains(key)) key = nextKey_++;
  staged_[key] = Staged{std::move(verb), std::move(handler)};
  return key;
}

void CommandMailboxAdapter::abandon(const std::uint32_t key) {
  const std::lock_guard lock(mutex_);
  staged_.erase(key);
}

void CommandMailboxAdapter::abandonStaged(
    const xanadu::vortex::OwnerCell specification) noexcept {
  abandon(static_cast<std::uint32_t>(specification));
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
    xanadu::vortex::BackendContext &context,
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

  // Everything that can throw -- the handler, payload registration --
  // stays inside this try: execute is noexcept, so an escaped exception
  // would terminate the worker.
  xanadu::vortex::BackendResult backend;
  std::optional<xanadu::vortex::PayloadLease> payloadLease;
  // A PayloadLease dropped on any path -- the catch below, an early exit --
  // must still release its resolver lease; terminalCandidate's own release
  // of the same id on the success path makes the guard's a no-op there.
  const xanadu::vortex::PayloadLeaseGuard payloadGuard(context.payloads,
                                                       payloadLease);
  try {
    if (!found) {
      backend = mailboxFailed({xanadu::vortex::IoErrorCode::InvalidRequest, 0});
    } else {
      // The handler runs off-owner and must not touch ArenaManifold; its
      // CommandResult freezes into reply bytes that publish through the
      // payload channel -- the outcome names the retained range and the
      // lease reaches terminalCandidate, so publishTerminal can bind it.
      CommandResult result{.success = false, .response = "Unhandled action"};
      try {
        if (staged.handler) {
          result = staged.handler(staged.verb);
        }
      } catch (const std::exception &e) {
        result = CommandResult{.success = false,
                               .response =
                                   std::string("handler threw: ") + e.what()};
      } catch (...) {
        result = CommandResult{.success = false, .response = "handler threw"};
      }

      const auto reply = bytesOf(replyTextOf(result));
      const auto lease =
          xanadu::vortex::publishTransientBytes(context.payloads, reply);
      if (!lease) {
        backend =
            mailboxFailed({xanadu::vortex::IoErrorCode::BackendFailure, 0});
      } else {
        payloadLease = *lease;
        backend      = xanadu::vortex::BackendResult{
            xanadu::vortex::Complete{
                lease->range, std::monostate{},
                xanadu::vortex::IoProgress{
                         .phase = xanadu::vortex::IoPhase::Terminal}},
                 {}};
      }
    }
  } catch (...) {
    backend = mailboxFailed({xanadu::vortex::IoErrorCode::BackendFailure, 0});
  }

  sink.terminalCandidate(command.token, std::move(backend), payloadLease);
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
CommandMailboxAdapter::resultFromReply(const std::string_view text) {
  if (text.starts_with("OK: ")) {
    return CommandResult{.success  = true,
                         .response = std::string(text.substr(4))};
  }
  if (text.starts_with("ERROR: ")) {
    return CommandResult{.success  = false,
                         .response = std::string(text.substr(7))};
  }
  return CommandResult{.success = false, .response = std::string(text)};
}

CommandResult
CommandMailboxAdapter::resultOf(const xanadu::vortex::IoOutcome &outcome,
                                xanadu::vortex::PayloadResolver &payloads) {
  const auto *complete = std::get_if<xanadu::vortex::Complete>(&outcome);
  if (complete == nullptr) {
    // A non-Complete terminal outcome is a failed command, never a
    // logical failure -- the promise cell still gets an ERROR reply.
    return CommandResult{.success  = false,
                         .response = std::string("io outcome ") +
                                     std::string(outcomeName(outcome))};
  }
  if (!complete->payload) {
    return CommandResult{.success = false, .response = "empty command result"};
  }
  // The reply text is the retained payload range; resolve it through the
  // shared resolver (the source stays registered for exactly this read).
  auto lease = payloads.resolveTransient(*complete->payload);
  if (!lease) {
    return CommandResult{.success  = false,
                         .response = "unresolved command payload"};
  }
  const auto bytes =
      payloads.read(*lease, 0, static_cast<std::size_t>(lease->range.length));
  payloads.release(lease->lease);
  if (!bytes) {
    return CommandResult{.success  = false,
                         .response = "unreadable command payload"};
  }
  std::string text;
  text.reserve(bytes->size());
  for (const auto b : *bytes) {
    text.push_back(static_cast<char>(b));
  }
  return resultFromReply(text);
}

} // namespace zigzag
