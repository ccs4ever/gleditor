#ifndef XANADU_VORTEX_FILE_READ_HOST_HPP
#define XANADU_VORTEX_FILE_READ_HOST_HPP

/**
 * @file file_read_host.hpp
 * @brief The assembled asynchronous file-read stack a program embeds so a
 *        language can submit and await bounded reads (plan §7.2, §10, M7).
 *
 * Language-neutral: it wires the M4/M5 pieces -- `FileEndpointAdapter`,
 * `ArenaIoCellStore`, `BoundedIoScheduler`, `IoService`, `IoHost` and
 * `FrontendBridge` -- over one arena and one VM, and offers the three things
 * a frontend needs from them: open a file without blocking, stage a bounded
 * read as a request specification, and pump. Opens and reads run on the
 * scheduler's workers; this class never reads a descriptor on the caller's
 * thread. It is C++ because it is OS-facing wiring (worker threads, file
 * descriptors) over the host seam; no language policy lives here.
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vortex/arena_io_cell_store.hpp"
#include "common/xanadu/vortex/capture_host.hpp"
#include "common/xanadu/vortex/file_endpoint_adapter.hpp"
#include "common/xanadu/vortex/frontend_bridge.hpp"
#include "common/xanadu/vortex/frontend_diagnostics.hpp"
#include "common/xanadu/vortex/io_host.hpp"
#include "common/xanadu/vortex/io_scheduler.hpp"
#include "common/xanadu/vortex/io_service.hpp"

namespace xanadu::vortex {

/// The host's write policy: given the CANONICAL locator of an open that asks
/// to change a file (symlinks and `..` already resolved by the host, so the
/// policy sees the path the kernel would), the subset of
/// `fileRightsMutating` it may be opened with. Empty (the default) and a zero
/// return both deny. Authority to write belongs to the invoking human's
/// program, never to a script (design/vortex-io-m7-vql-c.md §4.1).
using WriteGrant = std::function<std::uint64_t(std::string_view)>;

/// A `WriteGrant` that allows @p rights to any file strictly beneath one of
/// @p roots and nothing elsewhere. Roots are canonicalised once, here, so a
/// symlinked root names its target; a root that cannot be canonicalised
/// grants nothing. Containment is by path component, so `/data/out` does not
/// admit `/data/outside`.
[[nodiscard]] auto
writeRootGrant(const std::vector<std::filesystem::path> &roots,
               std::uint64_t rights = fileRightsMutating) -> WriteGrant;

/// Defaults for an embedded file-read stack. They are the bounds the M5
/// adapter tests run under: a handful of in-flight requests, a few MiB of
/// command, completion and payload budget, and one request per endpoint at a
/// time per slot. A program with a configuration source overrides them.
struct FileReadHostOptions {
  static constexpr std::size_t defaultWorkers          = 2;
  static constexpr std::size_t defaultOutstanding      = 8;
  static constexpr std::size_t defaultRequests         = 16;
  static constexpr std::size_t defaultBudgetBytes      = 1U << 22;
  static constexpr std::size_t defaultEndpointRequests = 8;
  /// What one read may retain; also the bound a read's byte count must meet.
  static constexpr std::size_t defaultPayloadBytes = 1U << 20;
  /// What one write may carry: the same bound as a read's payload, so a
  /// range read can be written back whole. The bytes are charged to the
  /// service's command budget at admission, not just to this bound.
  static constexpr std::size_t defaultWriteBytes = defaultPayloadBytes;
  static constexpr std::chrono::milliseconds defaultOpenDeadline{5000};

  std::size_t workers{defaultWorkers};
  std::size_t outstanding{defaultOutstanding};
  std::size_t requests{defaultRequests};
  std::size_t budgetBytes{defaultBudgetBytes};
  std::size_t endpointRequests{defaultEndpointRequests};
  std::size_t payloadBytes{defaultPayloadBytes};
  std::size_t writeBytes{defaultWriteBytes};
  /// What a script's named outcomes may retain between them (M7-VQL-c §12.1):
  /// the physical chunks their payload leases pin. Unset means `budgetBytes`,
  /// so retention never exceeds what the service itself would have held for
  /// the same requests and adds no new ceiling.
  std::optional<std::size_t> retainedPayloadBytes;
  std::chrono::milliseconds openDeadline{defaultOpenDeadline};
  /// Empty denies every open that asks to write, append or create: the safe
  /// default, so a host with no flag set can change no file.
  WriteGrant writeGrant;
  /// The clock request deadlines are read against; empty is the steady clock.
  /// A test injects one so a deadline expires on demand.
  IoClock clock;
};

/// Why a read, write, append or sync could not be staged. Nothing was
/// submitted in any of them.
enum class StageReadError : std::uint8_t {
  NotAFileCapability, ///< the capability's endpoint is not an open file
  OutOfBounds,        ///< offset + length overflows, or exceeds the bound
                      ///< (a read's payload bound, a write's `writeBytes`)
  Refused,            ///< the adapter refused the staged operation
};

[[nodiscard]] std::string_view toString(StageReadError error) noexcept;

class FileReadHost final {
public:
  /// @p vm must be the VM the owning language runs its cursors on; one host
  /// per VM, because the IoHost installs the VM's await handler.
  FileReadHost(zigzag::vortex::VortexVM &vm, zigzag::ArenaManifold &arena,
               FileReadHostOptions options = {});
  ~FileReadHost();

  FileReadHost(const FileReadHost &)            = delete;
  FileReadHost &operator=(const FileReadHost &) = delete;

  /// Admit an asynchronous open of @p path for reading. The open runs on a
  /// worker; poll `openedCapability` while pumping. Refused (a malformed or
  /// unauthorised open) is a diagnostic: nothing was started.
  [[nodiscard]] auto beginOpen(std::string_view path,
                               std::uint64_t rights = fileRightRead)
      -> std::expected<RequestCell, FrontendDiagnostic>;
  /// The open request of @p path as a specification, with this host's
  /// default open deadline on its clock. A language that wants the open as
  /// its own managed submission value admits it through `bridge()`.
  /// @p rights beyond read are requests the host's `writeGrant` still has to
  /// approve at admission; for them the locator is the canonical path, so
  /// the worker opens exactly the path the grant judged.
  [[nodiscard]] auto openSpec(std::string_view path,
                              std::uint64_t rights = fileRightRead) const
      -> OpenSpec;
  /// The capability an open outcome minted, when it completed with one.
  [[nodiscard]] auto capabilityOf(OutcomeCell open) const
      -> std::optional<CapabilityCell>;
  /// Close the endpoint behind @p capability through the file close path; the
  /// close is its own admitted request, never run on the caller's thread. A
  /// capability that names no open file is refused as a diagnostic.
  [[nodiscard]] auto closeCapability(CapabilityCell capability)
      -> std::expected<SubmissionCell, FrontendDiagnostic>;
  /// The capability the open minted: nullopt while the open is in flight, a
  /// diagnostic when it ended in any non-Complete outcome (the file is
  /// missing, not a regular file, ...), and the capability when it opened.
  [[nodiscard]] auto openedCapability(RequestCell open)
      -> std::optional<std::expected<CapabilityCell, FrontendDiagnostic>>;

  /// Stage a bounded read on @p capability and mint the request
  /// specification cell a submit takes. Owner thread; starts no I/O.
  [[nodiscard]] auto stageRead(CapabilityCell capability, std::uint64_t offset,
                               std::uint64_t length)
      -> std::expected<zigzag::CellRef, StageReadError>;

  /// Stage a positional write of @p bytes at @p offset. The bytes are frozen
  /// here, so later changes to the caller's buffer cannot reach the file.
  /// Owner thread; starts no I/O. Refused `OutOfBounds` past `writeBytes`.
  [[nodiscard]] auto stageWrite(CapabilityCell capability, std::uint64_t offset,
                                std::vector<std::byte> bytes)
      -> std::expected<zigzag::CellRef, StageReadError>;
  /// Stage an append of @p bytes (the capability must be an append open).
  [[nodiscard]] auto stageAppend(CapabilityCell capability,
                                 std::vector<std::byte> bytes)
      -> std::expected<zigzag::CellRef, StageReadError>;
  /// Stage a durable sync (`fdatasync`) of the capability's file; the write
  /// lane runs it after every earlier write on the endpoint.
  [[nodiscard]] auto stageSync(CapabilityCell capability)
      -> std::expected<zigzag::CellRef, StageReadError>;

  /// Publish completions and deliver wakes; never blocks. Also finishes the
  /// reaping that `reapWhenEnded` took over.
  auto pump(std::size_t budget) -> std::size_t;

  /// Take over the cleanup of a close a departing owner left running (a
  /// runner destroyed with a file still open and its worker busy): when
  /// @p closeRequest has ended, its graph and then @p endpoint's graph are
  /// freed on a later `pump`. The request is the caller's to have orphaned,
  /// never cancelled; nothing can name either graph once the owner is gone,
  /// so nothing else would free them. Owner thread.
  void reapWhenEnded(RequestCell closeRequest, EndpointCell endpoint);

  /// The retention budget a script runner charges its owned records to.
  [[nodiscard]] auto retainedPayloadBytes() const noexcept -> std::size_t {
    return options_.retainedPayloadBytes.value_or(options_.budgetBytes);
  }

  [[nodiscard]] auto bridge() -> FrontendBridge & { return bridge_; }
  [[nodiscard]] auto host() -> IoHost & { return host_; }
  [[nodiscard]] auto service() -> IoService & { return *service_; }
  [[nodiscard]] auto payloads() -> PayloadResolver & { return payloads_; }
  [[nodiscard]] auto diagnostics() const -> const FrontendDiagnosticRegistry & {
    return diagnostics_;
  }
  [[nodiscard]] auto schema() const -> const IoCellSchema & { return schema_; }
  [[nodiscard]] auto arena() -> zigzag::ArenaManifold & { return arena_; }

  /// Build the capture stack over this host's service, arena and payloads
  /// (M7-VQL-c §13, §18 C3): the coordinator's append adapter is registered
  /// with the service and its spec cells join this host's request decoder. The
  /// host owns the capture host so that it is destroyed after the service,
  /// whose workers run the coordinator's adapter until they stop. @p sink and
  /// @p publisher are the caller's and must outlive this host. Calling it
  /// twice replaces nothing: the first stack stays and is returned.
  auto attachCapture(CaptureByteSink &sink, CapturePublisher *publisher,
                     CaptureHostOptions options) -> CaptureHost &;
  /// The capture stack, when one was attached.
  [[nodiscard]] auto capture() noexcept -> CaptureHost * {
    return capture_.get();
  }
  [[nodiscard]] auto adapter() -> FileEndpointAdapter & { return adapter_; }

private:
  zigzag::ArenaManifold &arena_;
  FileReadHostOptions options_;
  IoCellSchema schema_;
  PayloadResolver payloads_;
  FileEndpointAdapter adapter_;
  ArenaIoCellStore cells_;
  /// Declared before the service so it is destroyed after it: the service's
  /// destructor joins the workers that execute the coordinator's adapter.
  std::unique_ptr<CaptureHost> capture_;
  std::unique_ptr<IoService> service_;
  IoHost host_;
  FrontendDiagnosticRegistry diagnostics_;
  FrontendBridge bridge_;
  struct DeferredReap {
    RequestCell request;
    EndpointCell endpoint;
  };
  std::vector<DeferredReap> deferredReaps_;

  /// Mint the spec cell tagged @p tag and stage @p op under it on the file
  /// @p capability names; shared by every stage* above.
  [[nodiscard]] auto stageOp(CapabilityCell capability, std::string_view tag,
                             FileOpSpec op)
      -> std::expected<zigzag::CellRef, StageReadError>;
};

} // namespace xanadu::vortex

#endif
