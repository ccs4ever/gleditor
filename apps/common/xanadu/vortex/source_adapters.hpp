#ifndef XANADU_VORTEX_SOURCE_ADAPTERS_HPP
#define XANADU_VORTEX_SOURCE_ADAPTERS_HPP

/**
 * @file source_adapters.hpp
 * @brief Registered `IoAdapter` implementations for Vortex I/O M4
 *        (design/vortex-io-implementation-plan.md §6, §13 M4 row):
 *        an in-memory/stream source, a child-process source, a loopback
 *        network source, and a router that delegates to another adapter
 *        without resubmitting.
 *
 * Every adapter here stages its bounded native argument (`StreamOpSpec`) on
 * the owner thread when the caller asks for a read/write, keyed by a small
 * integer the caller embeds in the request's `specification` cell. `prepare`
 * looks that spec up, performs the one pre-admission generation check it can
 * do synchronously, and encodes only the staging key (never a `CellRef` or
 * pointer) into `PreparedOperation::bytes`. `execute` runs off the owner
 * thread, re-validates generation against the adapter's live source state --
 * catching a source mutated between admission and execution -- and performs
 * the actual transfer. `publish` is a pure forward of the worker's outcome;
 * all cell minting remains in `IoService`/`IoCellStore`.
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/xanadu/vortex/io_protocol.hpp"

namespace xanadu::vortex {

enum class StreamOpKind : std::uint8_t { Read, Write };

/// A bounded, owner-frozen operation argument. `expectedGeneration == 0`
/// disables the generation check (process/net sources with no coherent
/// "current content" concept use this).
struct StreamOpSpec {
  StreamOpKind kind{StreamOpKind::Read};
  std::uint64_t offset{};
  /// Read bound in bytes. 0 requests zero bytes: every adapter completes
  /// trivially with an empty `Complete` rather than inventing a default
  /// chunk the spec never named or reporting `EndOfStream` for a request
  /// that asked for nothing.
  std::uint64_t length{};
  std::vector<std::byte> writeBytes;
  std::uint64_t expectedGeneration{};
};

/// Shared staging/dispatch for byte-oriented adapters. A subclass supplies
/// `perform` (the actual transfer, run off-owner) and, optionally,
/// `currentGeneration` (enables the Stale pre-admission / Conflict
/// in-flight checks of design/vortex-io-redesign.md §8.2's source-version
/// rule).
class StagedByteSourceAdapter : public IoAdapter {
public:
  ~StagedByteSourceAdapter() override = default;

  /// Owner-thread: stage one operation's bounded arguments and return the
  /// key to place in the request's `specification` cell/value before
  /// calling `IoService::submit`. Returns 0 when the staging table is full
  /// (bounded at 4096 live specs); a request staged under key 0 then fails
  /// `prepare` as InvalidRequest. Keys are never recycled within the
  /// adapter's lifetime; the 32-bit wrap is a documented M4 limit.
  [[nodiscard]] std::uint32_t stage(StreamOpSpec spec);

  /// Owner-thread: drop a staged spec that will never reach `execute`
  /// (the submission was refused, or the caller abandoned it). Bounds the
  /// staging table against stage-without-submit leaks; a no-op for unknown
  /// or already-consumed keys.
  void abandon(std::uint32_t key);

  /// `IoAdapter` hook: the service calls this with the request's
  /// `specification` when a submission is refused or its request reaches a
  /// terminal state without `execute` consuming the staged entry.
  void abandonStaged(OwnerCell specification) noexcept override;

  auto prepare(const CellRequest &request, OwnerContext &context)
      -> std::expected<PreparedOperation, IoErrorInfo> final;
  auto publish(const NativeCompletion &completion, OwnerContext &context)
      -> std::expected<OwnerPublication, IoErrorInfo> final;

protected:
  /// Off-owner transfer. Implementations must not block unboundedly; honor
  /// `context.stop`. Never touches cells or `OwnerContext`.
  ///
  /// Byte output goes through the payload channel, not `IoResultValue`:
  /// publish produced bytes with `publishTransientBytes(context.payloads,
  /// ...)`, put `lease->range` in the outcome's payload field, and assign
  /// the lease to @p payload so `execute` can hand it to
  /// `CompletionSink::terminalCandidate`. Leave @p payload disengaged for
  /// outcomes that carry no bytes.
  virtual auto perform(const StreamOpSpec &spec, BackendContext &context,
                       std::optional<PayloadLease> &payload)
      -> BackendResult = 0;
  /// 0 means "no generation tracked"; the base class then refuses a
  /// nonzero `expectedGeneration` at prepare (it cannot verify one) and
  /// leaves Conflict detection to `perform`.
  [[nodiscard]] virtual std::uint64_t currentGeneration() const noexcept {
    return 0;
  }
  /// Optional owner-thread pre-admission check on the staged spec, run by
  /// `prepare` after the generation gate. Return an IoErrorInfo to refuse
  /// the request before admission; nullopt accepts.
  [[nodiscard]] virtual std::optional<IoErrorInfo>
  validateSpec(const StreamOpSpec &) const {
    return std::nullopt;
  }

private:
  void execute(const BackendCommand &command, BackendContext &context,
               CompletionSink &sink) noexcept final;

  mutable std::mutex mutex_;
  std::unordered_map<std::uint32_t, StreamOpSpec> staged_;
  std::uint32_t nextKey_{1};
};

/// An in-memory byte buffer source: `d.io.kind` "stream". Bumping its
/// generation (via `setContent`) models a concurrent writer; a staged
/// operation whose `expectedGeneration` no longer matches is refused
/// (Stale, before admission) or completes Conflict (if the source moved
/// between a successful `prepare` and `execute`).
class InMemoryStreamAdapter final : public StagedByteSourceAdapter {
public:
  /// Replace the backing content and bump the generation. Thread-safe; the
  /// owner calls this to simulate (or really perform) an out-of-band write.
  void setContent(std::vector<std::byte> bytes);
  [[nodiscard]] std::uint64_t generation() const noexcept;
  [[nodiscard]] std::vector<std::byte> content() const;

protected:
  auto perform(const StreamOpSpec &spec, BackendContext &context,
               std::optional<PayloadLease> &payload) -> BackendResult override;
  [[nodiscard]] std::uint64_t currentGeneration() const noexcept override;

private:
  mutable std::mutex stateMutex_;
  std::vector<std::byte> content_;
  std::uint64_t generation_{1};
};

/// Runs one external process per staged read and captures bounded stdout
/// bytes. `d.io.kind` "process". There is no stable content generation for
/// a spawned process, so generation checks are disabled (every staged
/// operation spawns its own process instance) and nonzero offsets are
/// refused at prepare. Known M4 limit: `popen` output cannot be killed
/// mid-read, so `context.stop` is honored only between `fread` chunks and
/// a cancelled read may still run the child to completion.
class ProcessIoAdapter final : public StagedByteSourceAdapter {
public:
  /// `command` is passed to the shell (`popen`), matching the host-I/O
  /// pattern already used by Vlace's lowering (lower_io.cpp); this adapter
  /// moves that transfer onto the Vortex I/O worker path instead of the
  /// evaluation thread.
  ///
  /// Only `StreamOpKind::Read` is supported: a spawned process has no
  /// seekable offset and no stdin channel, so `prepare` refuses any other
  /// kind as Unsupported before admission.
  ///
  /// Documented limit: the child can block the worker indefinitely -- a
  /// command that never exits and never closes its stdout keeps
  /// `pclose`/`read` waiting, and `context.stop` only ends the read loop,
  /// not a blocked `pclose`. Callers needing a hard bound must wrap the
  /// command in a timeout of its own (e.g. `timeout(1)`).
  explicit ProcessIoAdapter(std::string command);

protected:
  auto perform(const StreamOpSpec &spec, BackendContext &context,
               std::optional<PayloadLease> &payload) -> BackendResult override;
  [[nodiscard]] std::optional<IoErrorInfo>
  validateSpec(const StreamOpSpec &spec) const override;

private:
  std::string command_;
};

/// A loopback network source over a caller-provided, already-connected
/// socket file descriptor (e.g. one end of `socketpair(2)`). `d.io.kind`
/// "net". The adapter does not own connection setup or the descriptor's
/// lifetime -- the caller keeps the fd open until every staged operation
/// has terminated. All operations on one adapter instance serialize on a
/// single-flight mutex because they share that one descriptor; run
/// concurrent streams on separate adapter instances (one fd each).
class NetIoAdapter final : public StagedByteSourceAdapter {
public:
  explicit NetIoAdapter(int fd) noexcept : fd_(fd) {}

protected:
  auto perform(const StreamOpSpec &spec, BackendContext &context,
               std::optional<PayloadLease> &payload) -> BackendResult override;

private:
  int fd_;
  std::mutex ioMutex_;
};

/// Delegates every call to an underlying adapter unchanged, preserving the
/// original request/source identity (design/vortex-io-implementation-plan.md
/// §6: "Router/Union adapters may delegate a prepared immutable operation
/// ... they do not resubmit"). Registering a `RouterSourceAdapter` at a
/// second `AdapterKey` composes two source views over one backend without
/// `IoService` ever admitting more than one request per caller submission.
class RouterSourceAdapter final : public IoAdapter {
public:
  explicit RouterSourceAdapter(IoAdapter &delegate) noexcept
      : delegate_(delegate) {}

  auto prepare(const CellRequest &request, OwnerContext &context)
      -> std::expected<PreparedOperation, IoErrorInfo> override {
    return delegate_.prepare(request, context);
  }
  void execute(const BackendCommand &command, BackendContext &context,
               CompletionSink &sink) noexcept override {
    delegate_.execute(command, context, sink);
  }
  auto publish(const NativeCompletion &completion, OwnerContext &context)
      -> std::expected<OwnerPublication, IoErrorInfo> override {
    return delegate_.publish(completion, context);
  }
  void abandonStaged(OwnerCell specification) noexcept override {
    delegate_.abandonStaged(specification);
  }

private:
  IoAdapter &delegate_;
};

/// Composes several member adapters behind one `AdapterKey` registration,
/// preserving original request/source identity the way `RouterSourceAdapter`
/// does for a single delegate (design/vortex-io-implementation-plan.md §6:
/// "Router/Union adapters may delegate a prepared immutable operation ...
/// they do not resubmit"). `select` picks exactly one member index per
/// request on the owner thread during `prepare`; that same index rides along
/// in `PreparedOperation::bytes` (ahead of the member's own encoding) so
/// `execute`/`publish` dispatch to the same member without re-running
/// `select` or touching any other member -- one caller submission still
/// produces exactly one `prepare`/`execute`/`publish` call, on one member.
class UnionSourceAdapter final : public IoAdapter {
public:
  using Selector = std::function<std::size_t(const CellRequest &)>;

  UnionSourceAdapter(std::vector<IoAdapter *> members, Selector select)
      : members_(std::move(members)), select_(std::move(select)) {}

  auto prepare(const CellRequest &request, OwnerContext &context)
      -> std::expected<PreparedOperation, IoErrorInfo> override;
  void execute(const BackendCommand &command, BackendContext &context,
               CompletionSink &sink) noexcept override;
  auto publish(const NativeCompletion &completion, OwnerContext &context)
      -> std::expected<OwnerPublication, IoErrorInfo> override;
  /// Forwards the abandonment to the member `prepare` routed this
  /// specification to (recorded in stagedRoute_). A spec that never
  /// prepared through the union -- a pre-prepare refusal, or one staged on
  /// a member directly -- still broadcasts to every member: the documented
  /// M4 limit, since members share one 32-bit staged-key space and a
  /// same-key entry staged on a *different* member for an unrelated
  /// in-flight request could be dropped early. It then fails
  /// `prepare`/`execute` as InvalidRequest, a typed refusal, never silent
  /// corruption.
  void abandonStaged(OwnerCell specification) noexcept override;

private:
  std::vector<IoAdapter *> members_;
  Selector select_;
  mutable std::mutex routeMutex_;
  std::unordered_map<std::uint64_t, std::size_t> route_;
  /// specification key -> member index, recorded at prepare() so
  /// abandonStaged can forward precisely; erased on first abandon.
  std::unordered_map<std::uint32_t, std::size_t> stagedRoute_;
};

} // namespace xanadu::vortex

#endif
