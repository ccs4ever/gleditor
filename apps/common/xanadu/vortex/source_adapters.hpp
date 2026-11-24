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
#include <mutex>
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
  /// calling `IoService::submit`.
  [[nodiscard]] std::uint32_t stage(StreamOpSpec spec);

  auto prepare(const CellRequest &request, OwnerContext &context)
      -> std::expected<PreparedOperation, IoErrorInfo> final;
  auto publish(const NativeCompletion &completion, OwnerContext &context)
      -> std::expected<OwnerPublication, IoErrorInfo> final;

protected:
  /// Off-owner transfer. Implementations must not block unboundedly; honor
  /// `context.stop`. Never touches cells or `OwnerContext`.
  virtual auto perform(const StreamOpSpec &spec, BackendContext &context)
      -> BackendResult = 0;
  /// 0 means "no generation tracked"; the base class then skips both the
  /// prepare-time Stale check and leaves Conflict detection to `perform`.
  [[nodiscard]] virtual std::uint64_t currentGeneration() const noexcept {
    return 0;
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
  auto perform(const StreamOpSpec &spec, BackendContext &context)
      -> BackendResult override;
  [[nodiscard]] std::uint64_t currentGeneration() const noexcept override;

private:
  mutable std::mutex stateMutex_;
  std::vector<std::byte> content_;
  std::uint64_t generation_{1};
};

/// Runs one external process per staged read and captures bounded stdout
/// bytes. `d.io.kind` "process". There is no stable content generation for
/// a spawned process, so generation checks are disabled (every staged
/// operation spawns its own process instance).
class ProcessIoAdapter final : public StagedByteSourceAdapter {
public:
  /// `command` is passed to the shell (`popen`), matching the host-I/O
  /// pattern already used by Vlace's lowering (lower_io.cpp); this adapter
  /// moves that transfer onto the Vortex I/O worker path instead of the
  /// evaluation thread.
  explicit ProcessIoAdapter(std::string command);

protected:
  auto perform(const StreamOpSpec &spec, BackendContext &context)
      -> BackendResult override;

private:
  std::string command_;
};

/// A loopback network source over a caller-provided, already-connected
/// socket file descriptor (e.g. one end of `socketpair(2)`). `d.io.kind`
/// "net". The adapter does not own connection setup; it performs bounded
/// `read`/`write` against the descriptor it is given.
class NetIoAdapter final : public StagedByteSourceAdapter {
public:
  explicit NetIoAdapter(int fd) noexcept : fd_(fd) {}

protected:
  auto perform(const StreamOpSpec &spec, BackendContext &context)
      -> BackendResult override;

private:
  int fd_;
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

private:
  IoAdapter &delegate_;
};

} // namespace xanadu::vortex

#endif
