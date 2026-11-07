/**
 * @file epistemic_translate_registry.hpp
 * @brief The structures P19 owns in an agent's graph, declared once per area
 *        (P19 T1c).
 *
 * A structure that a later slice adds without a translation must fail a test,
 * so the registry is the single list the translation routine, the parity
 * tests and `tools/check-translator-coverage.py` all read. An entry says where
 * the structure lives, which ABI constants it covers, what a resume and an
 * upgrade do with it, and carries a `populate` hook that builds one sample of
 * it on a live profile. The ids are authored here: the ABI headers hold bare
 * field names, so there is nothing to parse them from.
 *
 * Each area lists its structures in its own array below, and
 * `allOwnedStructures()` concatenates them. A slice that adds an area adds an
 * array and one line in the concatenation.
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_TRANSLATE_REGISTRY_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_TRANSLATE_REGISTRY_HPP

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "common/xanadu/vagent/runtime/wire.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vagent::runtime {

/// What a transfer does with a structure. `Derived` means the new program
/// re-mints it and the translator only compares it; `Dropped` is allowed only
/// with a reason text.
enum class Fate : std::uint8_t { Carried, Derived, Dropped };

/// A live profile of an executor's arena, the dimensions the samples hang on
/// and, for a structure that lives on a support, one of that profile's.
struct PopulateContext {
  zigzag::ArenaManifold &arena;
  ValueDims vd;
  zigzag::DimRef clone, epistemic;
  zigzag::CellRef profile;
  std::optional<zigzag::CellRef> support;
};

struct OwnedStructure {
  std::string_view id;
  std::string_view dimension;
  /// "agent header", "agent rank", "profile field", "profile list", "profile
  /// rank", "stream field" (inside a profile's `streams`), "support rank",
  /// "record field" or "predicate master".
  std::string_view where;
  /// The ABI constants (by their C++ names, spelled in the array that
  /// declares them) this entry covers.
  std::span<const std::string_view> fields;
  Fate resume;
  Fate upgrade;
  std::string_view dropReason;
  /// The shape version that introduced it.
  std::uint32_t since;
  void (*populate)(const PopulateContext &);
};

/// A dimension that is deliberately not P19 state, with the reason. The
/// coverage script refuses a dimension that is in neither list.
struct NotOwnedDimension {
  std::string_view dimension;
  std::string_view reason;
};

/// Host memory that a swap keeps or loses, so the user guide and the tests
/// read one source (design 5.4).
struct HostStateRow {
  std::string_view name;
  std::string_view upgrade;
  std::string_view resume;
};

[[nodiscard]] std::span<const OwnedStructure> allOwnedStructures() noexcept;
[[nodiscard]] std::span<const NotOwnedDimension> notOwnedDimensions() noexcept;
[[nodiscard]] std::span<const HostStateRow> hostState() noexcept;

/// Whether a top-level name of a profile or an observation record is a field
/// some entry covers; anything else refuses the transfer.
[[nodiscard]] bool ownsProfileField(std::string_view name) noexcept;
[[nodiscard]] bool ownsRecordField(std::string_view name) noexcept;
/// Whether a dimension a profile, a view or a record links along is one the
/// registry owns or one of the four value dimensions.
[[nodiscard]] bool ownsDimension(std::string_view name) noexcept;

} // namespace xanadu::vagent::runtime

#endif
