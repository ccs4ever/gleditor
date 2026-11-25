#ifndef XANADU_VORTEX_FILE_ENDPOINT_ADAPTER_HPP
#define XANADU_VORTEX_FILE_ENDPOINT_ADAPTER_HPP

/**
 * @file file_endpoint_adapter.hpp
 * @brief `FileEndpointAdapter` for Vortex I/O M5
 *        (design/vortex-io-implementation-plan.md §7.2, §8.5, §13 M5 row).
 *
 * The `d.io.kind` "file" endpoint adapter: ONE instance serves every file
 * endpoint opened through `IoService::admitOpen`, registered under both
 * `fileOpenAdapterKey` (opens) and `fileDataAdapterKey` (reads/writes).
 * `FileSourceAdapter` alone cannot serve that path -- its constructor
 * opens one path synchronously, `prepare` rejects a zero specification,
 * and its auto stage keys collide with the dense `CellRef` space a real
 * `IoCellStore` demands -- so this adapter is the locator dispatcher:
 *
 * - `prepare`/`execute` on an open command run `::open` on the scheduler
 *   worker (plan §7.2's "all opens off evaluation/UI threads"), then wrap
 *   the descriptor in a per-locator `FileSourceAdapter`.
 * - `stageAt` forwards a bounded `FileOpSpec` to the `FileSourceAdapter`
 *   that owns the locator, under the caller-chosen key (the minted spec
 *   cell's ref -- `ArenaIoCellStore::freezeRequest` refuses OwnerCells with
 *   no arena incarnation, the `StagedLensSourceAdapter::stageAt` pattern).
 * - `execute` on a data command routes the staged key back to that file's
 *   adapter, which performs the `pread`/`pwrite` and the observation.
 *
 * Endpoint-to-locator binding is the caller/staged-spec contract, the same
 * trust model M4's staged adapters use: the capability cell proves WHICH
 * endpoint admitted the request, and the staged spec names WHICH opened
 * file it addresses. A spec naming a locator this adapter never opened is
 * refused at `stageAt`.
 */

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "common/xanadu/vortex/file_source_adapter.hpp"
#include "common/xanadu/vortex/io_protocol.hpp"

namespace zigzag {
class ArenaManifold;
}

namespace xanadu::vortex {

/// The `d.io.kind` "file" registration (plan §8.5): `sourceKind` is this
/// adapter family's id; `operation` distinguishes the open half from the
/// data half of the same instance. Both keys must be registered to the
/// same `FileEndpointAdapter` -- see the class comment.
inline constexpr std::uint32_t fileSourceKind = 4;
inline constexpr AdapterKey fileOpenAdapterKey{fileSourceKind, 1};
inline constexpr AdapterKey fileDataAdapterKey{fileSourceKind, 2};

/// A staged data op on a file endpoint: the locator the open minted plus
/// the bounded file operation itself.
struct FileEndpointOp {
  std::string locator;
  FileOpSpec op;
};

/// Spec-cell text tags `makeFileRequestDecoder` understands: the staged op
/// itself lives in `FileEndpointAdapter::stageAt`, so the cell text only
/// selects the operation's direction and byte bounds.
inline constexpr std::string_view fileReadSpecTag   = "io.file.read";
inline constexpr std::string_view fileWriteSpecTag  = "io.file.write";
inline constexpr std::string_view fileStableSpecTag = "io.file.stable";

class FileEndpointAdapter final : public IoAdapter {
public:
  ~FileEndpointAdapter() override = default;

  // -- owner-thread staging for data ops on an opened locator -------------

  /// Stage a read/write/stable-read against the file @p spec.locator names.
  /// The file must already be open (its `admitOpen` completed): `stageAt`
  /// refuses a locator this adapter has not opened. @p key is the minted
  /// spec cell's ref -- see the class comment for why it must be a live
  /// arena cell.
  [[nodiscard]] std::expected<std::uint32_t, std::string>
  stageAt(std::uint32_t key, FileEndpointOp spec);

  /// Owner-thread: drop a staged op that will never reach `execute` (the
  /// submission was refused, or the caller abandoned it).
  void abandon(std::uint32_t key);

  /// `IoAdapter` hook: the service calls this with the request's
  /// `specification` when a submission is refused or its request reaches a
  /// terminal state without `execute` consuming the staged entry.
  void abandonStaged(OwnerCell specification) noexcept override;

  // -- IoAdapter ----------------------------------------------------------

  auto prepare(const CellRequest &request, OwnerContext &context)
      -> std::expected<PreparedOperation, IoErrorInfo> override;
  void execute(const BackendCommand &command, BackendContext &context,
               CompletionSink &sink) noexcept override;
  auto publish(const NativeCompletion &completion, OwnerContext &context)
      -> std::expected<OwnerPublication, IoErrorInfo> override;

  // -- owner/test accessors ----------------------------------------------

  /// The opened file's declared capabilities, or nullopt when @p locator
  /// was never opened through this adapter (or its open failed).
  [[nodiscard]] auto capabilities(std::string_view locator) const
      -> std::optional<FileOpenCapabilities>;
  [[nodiscard]] auto openedCount() const -> std::size_t;
  /// The per-locator `FileSourceAdapter` for adapter-level tests and
  /// owners that need its observation/decode-lease views; nullptr when the
  /// locator was never opened through this adapter.
  [[nodiscard]] auto fileFor(std::string_view locator) -> FileSourceAdapter *;

private:
  mutable std::mutex mutex_;
  /// locator -> opened file. Populated in `execute` (worker thread) when an
  /// open operation succeeds; read under `mutex_` everywhere else. Shared
  /// ownership so a delegated `prepare`/`execute`/`abandon` keeps the
  /// adapter alive even if a re-open replaces the map entry mid-call.
  std::map<std::string, std::shared_ptr<FileSourceAdapter>> files_;
  /// staged spec key -> locator, so `execute`/`abandonStaged` can route a
  /// data op to the file that owns it. Entries live from `stageAt` until
  /// `execute` consumes them or `abandon`/`abandonStaged` drops them.
  std::unordered_map<std::uint32_t, std::string> stagedLocator_;
};

/// Build the `RequestDecoder` an `ArenaIoCellStore` needs for file
/// endpoints opened through `FileEndpointAdapter` registered at @p
/// dataKey (`fileDataAdapterKey`). The spec cell's text selects the
/// operation -- `fileReadSpecTag`/`fileWriteSpecTag`/`fileStableSpecTag` --
/// while the staged op itself lives in `FileEndpointAdapter::stageAt`.
/// Reads may retain up to @p payloadBoundBytes; writes declare none.
[[nodiscard]] std::function<
    std::expected<CellRequest, IoErrorInfo>(CapabilityCell, OwnerCell)>
makeFileRequestDecoder(zigzag::ArenaManifold &arena, AdapterKey dataKey,
                       std::size_t payloadBoundBytes);

} // namespace xanadu::vortex

#endif
