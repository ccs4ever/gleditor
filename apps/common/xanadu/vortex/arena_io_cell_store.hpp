#ifndef XANADU_VORTEX_ARENA_IO_CELL_STORE_HPP
#define XANADU_VORTEX_ARENA_IO_CELL_STORE_HPP

#include <expected>
#include <functional>

#include "common/xanadu/vortex/io_service.hpp"

namespace xanadu::vortex {

/// Owner-thread bridge from the service protocol to one ephemeral arena.
/// The decoder reads the owner's request cell and freezes its validated,
/// cell-free command description before the scheduler can see it.
class ArenaIoCellStore final : public IoCellStore {
public:
  /// The decoder must enforce the capability's narrowed rights and
  /// consistency claims for each operation it freezes. Capability bits are
  /// adapter-defined; the bridge cannot infer read/write policy from an
  /// AdapterKey without that adapter's mapping.
  using RequestDecoder = std::function<std::expected<CellRequest, IoErrorInfo>(
      CapabilityCell, OwnerCell)>;
  using CloseDecoder   = std::function<std::expected<CellRequest, IoErrorInfo>(
      EndpointCell, CloseMode)>;
  struct OpenGrant {
    // Policy authority; the minted capability is narrowed to the requested
    // masks, never widened to these grant masks.
    std::uint64_t rights{};
    std::uint64_t consistency{};
  };
  using OpenAuthorizer =
      std::function<std::expected<OpenGrant, IoErrorInfo>(const OpenSpec &)>;

  ArenaIoCellStore(zigzag::ArenaManifold &arena, IoCellSchema schema,
                   PayloadResolver &payloads, RequestDecoder decoder,
                   CloseDecoder closeDecoder     = {},
                   OpenAuthorizer openAuthorizer = {});

  auto freezeRequest(CapabilityCell capability, OwnerCell specification)
      -> std::expected<CellRequest, IoErrorInfo> override;
  auto freezeClose(EndpointCell endpoint, CloseMode mode)
      -> std::expected<CellRequest, IoErrorInfo> override;
  auto authorizeOpen(const OpenSpec &spec)
      -> std::expected<void, IoErrorInfo> override;
  auto mintRequest(const CellRequest &request)
      -> std::expected<RequestCell, IoErrorInfo> override;
  auto mintRefusal(IoErrorInfo error) -> OutcomeCell override;
  auto publishTerminal(RequestCell request, const IoOutcome &outcome,
                       const IoProgress &progress,
                       const std::optional<PayloadLease> &payload)
      -> std::expected<OutcomeCell, IoErrorInfo> override;
  [[nodiscard]] auto isOutcomeCurrent(OutcomeCell outcome) const noexcept
      -> bool override;
  [[nodiscard]] auto outcomeFor(const RequestCell request)
      -> std::optional<OutcomeCell> override;
  auto mintOpenedEndpoint(const OpenSpec &spec)
      -> std::expected<OpenedEndpoint, IoErrorInfo> override;
  auto mintWaiter(RequestCell request, OwnerWaiterRegistration registration)
      -> WaiterCell override;
  void retireWaiter(WaiterCell waiter) noexcept override;

private:
  auto openGrant(const OpenSpec &spec) -> std::expected<OpenGrant, IoErrorInfo>;

  zigzag::ArenaManifold &arena_;
  IoCellSchema schema_;
  PayloadResolver &payloads_;
  RequestDecoder decoder_;
  CloseDecoder closeDecoder_;
  OpenAuthorizer openAuthorizer_;
};

} // namespace xanadu::vortex

#endif
