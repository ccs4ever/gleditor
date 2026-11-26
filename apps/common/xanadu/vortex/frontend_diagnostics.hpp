#ifndef XANADU_VORTEX_FRONTEND_DIAGNOSTICS_HPP
#define XANADU_VORTEX_FRONTEND_DIAGNOSTICS_HPP

/**
 * @file frontend_diagnostics.hpp
 * @brief The shared registry of frontend I/O diagnostic codes (plan §10, §14).
 *
 * A diagnostic code is a stable, greppable string, never an enum ordinal: it
 * is what a compiler diagnostic or a runner's `Failed` result names, and a
 * language's tests and documentation quote it. Host-side by nature (a
 * registry of names the frontends share), so it is C++ and not Vortex.
 */

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xanadu::vortex {

/// Which frontend owns a code. `Shared` codes are common to every frontend
/// and carry the `IO_` prefix; the rest carry their language's prefix.
enum class FrontendId : std::uint8_t {
  Shared,
  Vql,
  Vpl,
  Vprolog,
  Vlace,
  Vicon,
  Vagent,
};

[[nodiscard]] std::string_view toString(FrontendId frontend) noexcept;
/// The prefix every code owned by @p frontend must start with.
[[nodiscard]] std::string_view codePrefix(FrontendId frontend) noexcept;

/// The built-in codes. Shared:
namespace frontend_codes {
/// Message/frame (or any adapter-gated) mode requested before its gate.
inline constexpr std::string_view unsupportedMode = "IO_UNSUPPORTED_MODE";
/// `await` was handed a refused submission; only `Admitted` may be awaited.
inline constexpr std::string_view awaitNotAdmitted = "IO_AWAIT_NOT_ADMITTED";
/// A payload was dereferenced before the outcome's terminal tag was checked,
/// or the tag does not carry one.
inline constexpr std::string_view payloadBeforeStatus =
    "IO_PAYLOAD_BEFORE_STATUS";
/// Submit/cancel/capture reached from a pure context (renderer, projection,
/// pure predicate, arithmetic, presentation).
inline constexpr std::string_view effectForbiddenContext =
    "IO_EFFECT_FORBIDDEN_CONTEXT";
/// An advertised async path would have blocked the evaluation thread.
inline constexpr std::string_view syncFallbackForbidden =
    "IO_SYNC_FALLBACK_FORBIDDEN";
/// A parked token no longer names a live activation or waiter.
inline constexpr std::string_view tokenStale = "IO_TOKEN_STALE";
/// The arena refused to mint or link the managed submission value.
inline constexpr std::string_view cellRefused = "IO_CELL_REFUSED";
/// The language-owned codes of §10 and §14.
inline constexpr std::string_view vqlQueryContext = "VQL_IO_QUERY_CONTEXT";
/// A script action's capability, offset or bound is not a usable value; the
/// script stops before anything is submitted.
inline constexpr std::string_view vqlEffectArgument = "VQL_IO_EFFECT_ARGUMENT";
/// A synchronous executor was asked to run a script holding an I/O action. The
/// action compiles to bytecode but can park, and there is no blocking
/// fallback, so such a script runs only through begin/resume (the runner).
inline constexpr std::string_view vqlEffectNeedsRunner =
    "VQL_IO_EFFECT_NEEDS_RUNNER";
/// A front end was asked to make durable rows that an I/O action produced.
/// Input is transient unless explicitly captured, and no capture route exists
/// for those rows yet, so the persistence is refused rather than made without
/// a capture decision or provenance.
inline constexpr std::string_view vqlPersistNeedsCapture =
    "VQL_IO_PERSIST_NEEDS_CAPTURE";
/// A query segment of a script failed (the QueryError category and message
/// ride in the diagnostic's message).
inline constexpr std::string_view vqlScriptFailed = "VQL_SCRIPT_FAILED";
/// A parked script outlived its overall deadline. The host abandoned it; the
/// request it awaited was asked to stop, and no row is produced.
inline constexpr std::string_view vqlScriptTimeout = "VQL_SCRIPT_TIMEOUT";
inline constexpr std::string_view vplShapeMismatch = "VPL_IO_SHAPE_MISMATCH";
inline constexpr std::string_view prologEffectContext =
    "PROLOG_IO_EFFECT_CONTEXT";
inline constexpr std::string_view vlaceAsyncUnavailable =
    "VLACE_ASYNC_UNAVAILABLE";
inline constexpr std::string_view vagentActorSuspendForbidden =
    "VAGENT_ACTOR_IO_SUSPEND_FORBIDDEN";
} // namespace frontend_codes

/// What a runner's `Failed` result and a compiler diagnostic carry.
struct FrontendDiagnostic {
  std::string code;
  std::string message;
  /// A frontend position (VPL element, script action index); optional.
  std::optional<std::size_t> position;
  friend bool operator==(const FrontendDiagnostic &,
                         const FrontendDiagnostic &) = default;
};

enum class DiagnosticRegistryError : std::uint8_t {
  MalformedCode, ///< not [A-Z][A-Z0-9_]*
  WrongPrefix,   ///< the code does not start with its owner's prefix
  Duplicate,     ///< already registered
  Unregistered,  ///< diagnose() of a code nobody registered
};

[[nodiscard]] std::string_view toString(DiagnosticRegistryError error) noexcept;

class FrontendDiagnosticRegistry {
public:
  struct Entry {
    FrontendId owner{};
    std::string code;
    std::string summary;
  };

  /// A registry seeded with the shared codes and the §10/§14 language codes.
  [[nodiscard]] static auto withBuiltins() -> FrontendDiagnosticRegistry;

  /// Language milestones register theirs here; duplicates are refused so two
  /// languages can never share a code with different meanings.
  [[nodiscard]] auto registerCode(FrontendId owner, std::string_view code,
                                  std::string_view summary)
      -> std::expected<void, DiagnosticRegistryError>;
  [[nodiscard]] auto find(std::string_view code) const -> std::optional<Entry>;
  [[nodiscard]] auto entries() const -> std::vector<Entry>;
  /// A diagnostic for a registered code; an unregistered code is refused so a
  /// typo cannot reach a user as an unstable string.
  [[nodiscard]] auto diagnose(std::string_view code, std::string message,
                              std::optional<std::size_t> position = {}) const
      -> std::expected<FrontendDiagnostic, DiagnosticRegistryError>;

private:
  std::map<std::string, Entry, std::less<>> entries_;
};

} // namespace xanadu::vortex

#endif
