/**
 * @file anthology.hpp
 * @brief Cross-store ranks: the anthology (§5.9).
 *
 * A rank whose members live in other people's documents or in the local
 * document. Each authored entry occurrence is a local cell that cites an
 * interned foreign placeholder (via d.member) and carries its own snapshot pin
 * (via d.member-state). Members are traversed along d.anthology.
 */
#ifndef XANADU_ANTHOLOGY_HPP
#define XANADU_ANTHOLOGY_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gleditor/cpp26.hpp>
#include <gleditor/ranges.hpp>

#include "common/xanadu/extern_ref.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace zigzag {
class ArenaManifold;
} // namespace zigzag

namespace xanadu {

class Store;
struct Scroll;

/**
 * @brief Whether an anthology rank item is a local cell or a foreign citation
 * entry.
 */
enum class AnthologyMemberKind : std::uint8_t {
  Local,        ///< Ordinary local cell on the rank
  ForeignEntry, ///< Citation entry pointing to an extern placeholder & state
                ///< descriptor
};

/**
 * @brief Resolution and display status of an anthology member (§5.9 §5).
 */
enum class AnthologyDisplayStatus : std::uint8_t {
  Local,          ///< Native cell in the local store
  Resolved,       ///< Foreign member resolved and available
  NotFetched,     ///< Foreign document or descriptor content not yet available
  Absent,         ///< Definitively absent in foreign store history
  Unintelligible, ///< Target resolves to a non-MakeCell or malformed op
};

[[nodiscard]] constexpr std::string_view
toString(const AnthologyDisplayStatus status) noexcept {
  switch (status) {
  case AnthologyDisplayStatus::Local:
    return "local";
  case AnthologyDisplayStatus::Resolved:
    return "resolved";
  case AnthologyDisplayStatus::NotFetched:
    return "not fetched";
  case AnthologyDisplayStatus::Absent:
    return "absent";
  case AnthologyDisplayStatus::Unintelligible:
    return "unintelligible";
  }
  return "unknown";
}

/**
 * @brief Decoded member occurrence on an anthology rank (§5.9).
 */
struct AnthologyMember {
  zigzag::CellRef cell{zigzag::noCell}; ///< The cell on the d.anthology rank
  AnthologyMemberKind kind{AnthologyMemberKind::Local};
  zigzag::CellRef placeholderCell{
      zigzag::noCell}; ///< If ForeignEntry, cell linked on d.member
  zigzag::CellRef stateCell{
      zigzag::noCell}; ///< If ForeignEntry, cell linked on d.member-state
  std::optional<ExternOpRef>
      externRef; ///< If ForeignEntry, target from placeholder
  std::optional<GlobalDocumentState>
      pinnedState;   ///< If ForeignEntry, descriptor from stateCell
  std::string label; ///< Cell's direct text content

  [[nodiscard]] bool isForeign() const noexcept {
    return kind == AnthologyMemberKind::ForeignEntry;
  }
  [[nodiscard]] bool isLocal() const noexcept {
    return kind == AnthologyMemberKind::Local;
  }

  bool operator==(const AnthologyMember &) const = default;
};

/**
 * @brief Resolved view of an anthology member against foreign stores and arena
 * (§5.9 §5, §5.7).
 */
struct ResolvedAnthologyMember {
  AnthologyMember member;
  AnthologyDisplayStatus status{AnthologyDisplayStatus::NotFetched};
  zigzag::CellRef resolvedForeignCell{
      zigzag::noCell};                        ///< cell inside foreign store
  zigzag::CellRef arenaProxy{zigzag::noCell}; ///< proxy cell in ArenaManifold
};

/**
 * @brief Reads one anthology rank member at @p cell.
 */
[[nodiscard]] std::optional<AnthologyMember>
readAnthologyMember(const zigzag::Manifold &manifold, const Store &store,
                    zigzag::CellRef cell);

/**
 * @brief Collects all members of an anthology rank starting from @p root or
 *        its posward d.anthology neighbors.
 */
[[nodiscard]] std::vector<AnthologyMember>
anthologyMembers(const zigzag::Manifold &manifold, const Store &store,
                 zigzag::CellRef root);

/**
 * @brief Functional traversal over all members of an anthology rank.
 */
void forEachAnthologyMember(
    const zigzag::Manifold &manifold, const Store &store, zigzag::CellRef root,
    gleditor::cpp26::function_ref<bool(const AnthologyMember &)> visitor);

/**
 * @brief Resolves an external anthology member against an opened foreign store
 * (§5.9 §5).
 */
[[nodiscard]] ResolvedAnthologyMember
resolveAnthologyMember(const Store &localStore, const AnthologyMember &member,
                       const Store &foreignStore, const Scroll &sealedAs,
                       const zigzag::Manifold *foreignFold = nullptr);

/**
 * @brief Resolves an external anthology member into an ArenaManifold, minting
 * its proxy (§5.7, §5.9).
 */
[[nodiscard]] ResolvedAnthologyMember
resolveAnthologyInArena(zigzag::ArenaManifold &arena, const Store &localStore,
                        const AnthologyMember &member,
                        std::uint32_t foreignSpaceId, const Scroll &sealedAs);

} // namespace xanadu

#endif // XANADU_ANTHOLOGY_HPP
