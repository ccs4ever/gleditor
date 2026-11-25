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
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "common/xanadu/vortex/arena_io_cell_store.hpp"
#include "common/xanadu/vortex/file_endpoint_adapter.hpp"
#include "common/xanadu/vortex/frontend_bridge.hpp"
#include "common/xanadu/vortex/frontend_diagnostics.hpp"
#include "common/xanadu/vortex/io_host.hpp"
#include "common/xanadu/vortex/io_scheduler.hpp"
#include "common/xanadu/vortex/io_service.hpp"

namespace xanadu::vortex {

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
  static constexpr std::chrono::milliseconds defaultOpenDeadline{5000};

  std::size_t workers{defaultWorkers};
  std::size_t outstanding{defaultOutstanding};
  std::size_t requests{defaultRequests};
  std::size_t budgetBytes{defaultBudgetBytes};
  std::size_t endpointRequests{defaultEndpointRequests};
  std::size_t payloadBytes{defaultPayloadBytes};
  std::chrono::milliseconds openDeadline{defaultOpenDeadline};
};

/// Why a read could not be staged. Nothing was submitted in any of them.
enum class StageReadError : std::uint8_t {
  NotAFileCapability, ///< the capability's endpoint is not an open file
  OutOfBounds,        ///< offset + length overflows, or exceeds the bound
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
  [[nodiscard]] auto beginOpen(std::string_view path)
      -> std::expected<RequestCell, FrontendDiagnostic>;
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

  /// Publish completions and deliver wakes; never blocks.
  auto pump(const std::size_t budget) -> std::size_t {
    return host_.pumpIo(budget);
  }

  [[nodiscard]] auto bridge() -> FrontendBridge & { return bridge_; }
  [[nodiscard]] auto host() -> IoHost & { return host_; }
  [[nodiscard]] auto service() -> IoService & { return *service_; }
  [[nodiscard]] auto payloads() -> PayloadResolver & { return payloads_; }
  [[nodiscard]] auto diagnostics() const -> const FrontendDiagnosticRegistry & {
    return diagnostics_;
  }
  [[nodiscard]] auto schema() const -> const IoCellSchema & { return schema_; }
  [[nodiscard]] auto adapter() -> FileEndpointAdapter & { return adapter_; }

private:
  zigzag::ArenaManifold &arena_;
  FileReadHostOptions options_;
  IoCellSchema schema_;
  PayloadResolver payloads_;
  FileEndpointAdapter adapter_;
  ArenaIoCellStore cells_;
  std::unique_ptr<IoService> service_;
  IoHost host_;
  FrontendDiagnosticRegistry diagnostics_;
  FrontendBridge bridge_;
};

} // namespace xanadu::vortex

#endif
