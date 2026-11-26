#include "common/xanadu/vortex/frontend_diagnostics.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

namespace xanadu::vortex {
namespace {

constexpr std::array<std::string_view, 7> frontendNames{
    "shared", "vql", "vpl", "vprolog", "vlace", "vicon", "vagent"};
constexpr std::array<std::string_view, 7> prefixes{
    "IO_", "VQL_", "VPL_", "PROLOG_", "VLACE_", "VICON_", "VAGENT_"};

bool wellFormed(const std::string_view code) {
  return !code.empty() && code.front() >= 'A' && code.front() <= 'Z' &&
         std::ranges::all_of(code, [](const char c) {
           return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
         });
}

struct Builtin {
  FrontendId owner;
  std::string_view code;
  std::string_view summary;
};

constexpr std::array builtins{
    Builtin{FrontendId::Shared, frontend_codes::unsupportedMode,
            "message or frame mode requested before its adapter gate"},
    Builtin{FrontendId::Shared, frontend_codes::awaitNotAdmitted,
            "await was given a refused submission"},
    Builtin{FrontendId::Shared, frontend_codes::payloadBeforeStatus,
            "payload accessed without a payload-bearing terminal status"},
    Builtin{FrontendId::Shared, frontend_codes::effectForbiddenContext,
            "submit, cancel or capture from a pure context"},
    Builtin{FrontendId::Shared, frontend_codes::syncFallbackForbidden,
            "an advertised async path would block the evaluation thread"},
    Builtin{FrontendId::Shared, frontend_codes::tokenStale,
            "parked token no longer names a live activation or waiter"},
    Builtin{FrontendId::Shared, frontend_codes::cellRefused,
            "the arena refused to mint or link the managed submission value"},
    Builtin{FrontendId::Vql, frontend_codes::vqlQueryContext,
            "I/O effect inside a row or query expression"},
    Builtin{FrontendId::Vql, frontend_codes::vqlEffectArgument,
            "script I/O action given an unusable capability, offset or bound"},
    Builtin{FrontendId::Vql, frontend_codes::vqlWriteSource,
            "a write's byte source is neither a text cell nor an outcome that "
            "exposes a payload"},
    Builtin{FrontendId::Vql, frontend_codes::vqlEffectNeedsRunner,
            "script I/O action can park and runs only through a runner"},
    Builtin{FrontendId::Vql, frontend_codes::vqlPersistNeedsCapture,
            "rows produced by an I/O action cannot be made durable without an "
            "explicit capture"},
    Builtin{FrontendId::Vql, frontend_codes::vqlScriptFailed,
            "a query segment of a script failed"},
    Builtin{FrontendId::Vql, frontend_codes::vqlScriptTimeout,
            "a parked script outlived its overall deadline and was abandoned"},
    Builtin{FrontendId::Vpl, frontend_codes::vplShapeMismatch,
            "unequal shapes given to a vectorized submit"},
    Builtin{FrontendId::Vprolog, frontend_codes::prologEffectContext,
            "I/O effect inside a clause or under speculative choice"},
    Builtin{FrontendId::Vlace, frontend_codes::vlaceAsyncUnavailable,
            "path not yet migrated to the async IO runner"},
    Builtin{FrontendId::Vagent, frontend_codes::vagentActorSuspendForbidden,
            "actor turn attempted to suspend on I/O"},
};

} // namespace

std::string_view toString(const FrontendId frontend) noexcept {
  return frontendNames[static_cast<std::size_t>(frontend)];
}

std::string_view codePrefix(const FrontendId frontend) noexcept {
  return prefixes[static_cast<std::size_t>(frontend)];
}

std::string_view toString(const DiagnosticRegistryError error) noexcept {
  switch (error) {
  case DiagnosticRegistryError::MalformedCode:
    return "MalformedCode";
  case DiagnosticRegistryError::WrongPrefix:
    return "WrongPrefix";
  case DiagnosticRegistryError::Duplicate:
    return "Duplicate";
  case DiagnosticRegistryError::Unregistered:
    return "Unregistered";
  }
  return "Unknown";
}

auto FrontendDiagnosticRegistry::withBuiltins() -> FrontendDiagnosticRegistry {
  FrontendDiagnosticRegistry registry;
  for (const auto &entry : builtins) {
    // The table above is the only source and satisfies every rule.
    static_cast<void>(
        registry.registerCode(entry.owner, entry.code, entry.summary));
  }
  return registry;
}

auto FrontendDiagnosticRegistry::registerCode(const FrontendId owner,
                                              const std::string_view code,
                                              const std::string_view summary)
    -> std::expected<void, DiagnosticRegistryError> {
  if (!wellFormed(code)) {
    return std::unexpected{DiagnosticRegistryError::MalformedCode};
  }
  if (!code.starts_with(codePrefix(owner))) {
    return std::unexpected{DiagnosticRegistryError::WrongPrefix};
  }
  const auto [at, inserted] = entries_.try_emplace(
      std::string{code}, Entry{owner, std::string{code}, std::string{summary}});
  static_cast<void>(at);
  if (!inserted) return std::unexpected{DiagnosticRegistryError::Duplicate};
  return {};
}

auto FrontendDiagnosticRegistry::find(const std::string_view code) const
    -> std::optional<Entry> {
  const auto it = entries_.find(code);
  if (it == entries_.end()) return std::nullopt;
  return it->second;
}

auto FrontendDiagnosticRegistry::entries() const -> std::vector<Entry> {
  std::vector<Entry> all;
  all.reserve(entries_.size());
  for (const auto &[code, entry] : entries_) {
    static_cast<void>(code);
    all.push_back(entry);
  }
  return all;
}

auto FrontendDiagnosticRegistry::diagnose(
    const std::string_view code, std::string message,
    const std::optional<std::size_t> position) const
    -> std::expected<FrontendDiagnostic, DiagnosticRegistryError> {
  if (!entries_.contains(code)) {
    return std::unexpected{DiagnosticRegistryError::Unregistered};
  }
  return FrontendDiagnostic{std::string{code}, std::move(message), position};
}

} // namespace xanadu::vortex
