/**
 * @file epistemic_shape.hpp
 * @brief The shape version of an agent's P19 state and the one guard that
 *        decides whether the executor may carry it (P19 T1b1).
 *
 * Shape 1 is the layout of the profiles, their histories, the policy masters
 * and the header that names it. The header is a three-cell record at the head
 * of the agent cell's `d.epistemic` rank, minted at compile time only for a
 * program with an arbitrated predicate. It is compared by number and never
 * copied: the new program's instantiation already holds its own.
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_SHAPE_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_SHAPE_HPP

#include <cstdint>
#include <optional>
#include <set>
#include <string_view>

#include "common/xanadu/vagent/runtime/epistemic_preflight.hpp"
#include "common/xanadu/vagent/runtime/epistemic_refusal.hpp"
#include "common/xanadu/vagent/runtime/upgrade_schema.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vagent::runtime {

/// Any slice that renames a field, changes a field's kind, moves a rank or
/// changes a meaning bumps this and deletes the old reader, so state saved
/// under the old number refuses by number.
inline constexpr std::int64_t kEpistemicShapeVersion = 1;
inline constexpr std::string_view kShapeHeaderName   = "EpistemicShape";
inline constexpr std::string_view kShapeVersionField = "version";

struct EpistemicStateProbe {
  /// The header's version, absent when there is no header or it is unreadable.
  std::optional<std::int64_t> shape;
  /// Anything beyond the header: a profile (a tombstone included) or another
  /// cell on `d.epistemic`.
  bool populated{false};
};

/// Read-only, so the pre-flight can ask it of a received image.
[[nodiscard]] EpistemicStateProbe
probeEpistemicState(const zigzag::ArenaManifold &arena, zigzag::CellRef agent,
                    const OwnedGraphDims &dims);

/// The shapes a translator is registered for. T1c replaces the single seam
/// with per-shape entries; until then a registered seam carries shape 1.
class TranslatorRegistry {
public:
  [[nodiscard]] static TranslatorRegistry of(bool seamRegistered) {
    TranslatorRegistry registry;
    if (seamRegistered) registry.shapes_.insert(kEpistemicShapeVersion);
    return registry;
  }
  [[nodiscard]] bool translates(std::int64_t shape) const {
    return shapes_.contains(shape);
  }

private:
  std::set<std::int64_t> shapes_;
};

/// The single decision whether an agent holds state the executor may carry.
/// It never reads the header's cells as state: an arbitrating agent with no
/// source yet is translatable under any registry.
[[nodiscard]] bool
hasTranslatableEpistemicState(const EpistemicStateProbe &probe,
                              const TranslatorRegistry &registry) noexcept;

/// The shape the program's own instantiation will mint a header for: the
/// compiler mints it exactly when a predicate is arbitrated.
[[nodiscard]] std::optional<std::int64_t>
programShape(const UpgradeSchema &schema);

/// `UnknownShapeVersion` when state is populated and the saved shape is not
/// the program's. A program that has dropped every arbitrated predicate is
/// left to the removal check, which names the predicate.
[[nodiscard]] std::optional<TransferRefusal>
checkShape(const EpistemicStateProbe &saved,
           std::optional<std::int64_t> current, TransferRoute route);

} // namespace xanadu::vagent::runtime

#endif
