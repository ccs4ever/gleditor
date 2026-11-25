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
 *   the descriptor in a `FileSourceAdapter` registered under a fresh
 *   `FileOpenId` -- one per open, never per path, so a second open of the
 *   same path (after a replace, say) is a different file and cannot
 *   redirect the first's operations.
 * - `stageAt` forwards a bounded `FileOpSpec` to the `FileSourceAdapter`
 *   that @p open names, under the caller-chosen key (the minted spec
 *   cell's ref -- `ArenaIoCellStore::freezeRequest` refuses OwnerCells with
 *   no arena incarnation, the `StagedLensSourceAdapter::stageAt` pattern).
 * - `execute` on a data command routes the staged key back to that file's
 *   adapter, which performs the `pread`/`pwrite` and the observation.
 * - `release` is the close path: it drops the open, abandons its staged
 *   ops, and lets the descriptor close once the last in-flight operation
 *   holding the file lets go.
 *
 * Endpoint-to-open binding is the caller/staged-spec contract, the same
 * trust model M4's staged adapters use: the capability cell proves WHICH
 * endpoint admitted the request, and the staged spec names WHICH opened
 * file it addresses. A spec naming an open this adapter does not hold is
 * refused at `stageAt`. `openIdsFor` lists a locator's live opens in
 * completion order; await each open before starting the next of the same
 * locator so the newest id is the one just completed.
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
#include <vector>

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

/// Identity of one completed open of a file endpoint. Minted per open, so
/// two opens of the same path are two ids.
struct FileOpenId {
  std::uint64_t value{};
  friend auto operator<=>(const FileOpenId &, const FileOpenId &) = default;
  friend bool operator==(const FileOpenId &, const FileOpenId &)  = default;
};

/// A staged data op on a file endpoint: the open it addresses plus the
/// bounded file operation itself.
struct FileEndpointOp {
  FileOpenId open;
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

  /// Stage a read/write/stable-read against the open @p spec.open names.
  /// The open must be live (its `admitOpen` completed and it was not
  /// released). @p key is the minted spec cell's ref -- see the class
  /// comment for why it must be a live arena cell -- and must not already
  /// name a live staged op on this adapter.
  [[nodiscard]] std::expected<StageKey, std::string>
  stageAt(StageKey key, FileEndpointOp spec);

  /// Owner-thread: drop a staged op that will never reach `execute` (the
  /// submission was refused, or the caller abandoned it).
  void abandon(StageKey key);

  /// Close path: forget @p open and abandon its staged ops. Operations
  /// already executing keep the file alive until they finish; ones not yet
  /// started fail as InvalidRequest. Refuses an id that is not live.
  [[nodiscard]] std::expected<FileEndpointAdapter *, std::string>
  release(FileOpenId open);

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

  /// The opened file's declared capabilities, or nullopt when @p open is
  /// not live.
  [[nodiscard]] auto capabilities(FileOpenId open) const
      -> std::optional<FileOpenCapabilities>;
  [[nodiscard]] auto openedCount() const -> std::size_t;
  /// Live opens of @p locator, oldest completion first.
  [[nodiscard]] auto openIdsFor(std::string_view locator) const
      -> std::vector<FileOpenId>;
  /// The `FileSourceAdapter` of a live open, for adapter-level tests and
  /// owners that need its observation/decode-lease views; null when
  /// @p open is not live. Shared so the caller's use cannot outlive it.
  [[nodiscard]] auto fileFor(FileOpenId open)
      -> std::shared_ptr<FileSourceAdapter>;

private:
  struct OpenFile {
    std::string locator;
    std::shared_ptr<FileSourceAdapter> file;
  };

  mutable std::mutex mutex_;
  /// open id -> opened file. Populated in `execute` (worker thread) when an
  /// open operation succeeds; read under `mutex_` everywhere else. Shared
  /// ownership so a delegated `prepare`/`execute`/`abandon` keeps the
  /// adapter (and its descriptor) alive even if `release` drops the entry
  /// mid-call.
  std::map<FileOpenId, OpenFile> files_;
  FileOpenId nextOpenId_{1};
  /// staged spec key -> the open that owns it, so `execute`/`abandonStaged`
  /// can route a data op to its file. Entries live from `stageAt` until
  /// `execute` consumes them or `abandon`/`release` drops them.
  std::unordered_map<StageKey, FileOpenId> stagedOpen_;
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
