#ifndef XANADU_VORTEX_FRONTEND_TOKEN_HPP
#define XANADU_VORTEX_FRONTEND_TOKEN_HPP

/**
 * @file frontend_token.hpp
 * @brief The language-neutral carrier a frontend runner returns while an
 *        await is Pending (plan §10): FrontendRunResult, FrontendToken and the
 *        runner interface IoHost routes a wakeup to.
 *
 * All of it is owner-thread host state, never a language value: a token holds
 * native handles and ids (request incarnation, waiter, ContinuationId) that
 * must not appear in a cell a language can read. It is C++ because it is the
 * seam between the host's pump and each language's own activation type.
 */

#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <variant>

#include "common/xanadu/vortex/frontend_diagnostics.hpp"
#include "common/xanadu/vortex/io_protocol.hpp"

namespace xanadu::vortex {

/// A language's suspended activation (VQL script action, VPL executor, Prolog
/// query driver, VLace IO continuation, VIcon generator cursor, VAgent plan).
/// The bridge never looks inside; the owning runner downcasts its own.
class FrontendActivation {
public:
  FrontendActivation()                                      = default;
  FrontendActivation(const FrontendActivation &)            = delete;
  FrontendActivation &operator=(const FrontendActivation &) = delete;
  virtual ~FrontendActivation()                             = default;
};

struct FrontendToken {
  std::shared_ptr<FrontendActivation> activation;
  RequestCell request;
  WaiterCell waiter;
  ContinuationId continuation{};
  /// VPL element, script action index, Prolog goal ordinal; frontend-owned.
  std::optional<std::size_t> position;
};

/// Why a parked token is being handed back to its runner.
enum class FrontendWake : std::uint8_t {
  /// The request is terminal and the VM cursor has been resumed: the runner
  /// steps the same activation, which re-awaits the same request and gets
  /// its outcome without submitting again.
  Resumable,
  /// The cursor or request went stale before delivery; the runner reports
  /// `IO_TOKEN_STALE` (or unwinds) instead of resuming.
  Abandoned,
};

/// What IoHost routes a wakeup to. Called on the owner thread from `pumpIo`,
/// after the host's own bookkeeping is consistent: the runner may re-await
/// (and so park again) from inside the call, but must not call `pumpIo`.
class FrontendRunner {
public:
  virtual void wake(FrontendToken token, FrontendWake why) = 0;
  virtual ~FrontendRunner()                                = default;
};

/// `Completed{value} | Parked{FrontendToken} | Failed{FrontendDiagnostic}`.
/// Pending is a runner state, never a value of the language: a runner that
/// parks returns Parked, and a terminal outcome of any tag is Completed.
template <typename Value> struct FrontendRunResult {
  struct Completed {
    Value value;
  };
  struct Parked {
    FrontendToken token;
  };
  struct Failed {
    FrontendDiagnostic diagnostic;
  };
  std::variant<Completed, Parked, Failed> value;

  [[nodiscard]] static auto completed(Value v) -> FrontendRunResult {
    return {Completed{std::move(v)}};
  }
  [[nodiscard]] static auto parked(FrontendToken token) -> FrontendRunResult {
    return {Parked{std::move(token)}};
  }
  [[nodiscard]] static auto failed(FrontendDiagnostic diagnostic)
      -> FrontendRunResult {
    return {Failed{std::move(diagnostic)}};
  }
};

} // namespace xanadu::vortex

#endif
