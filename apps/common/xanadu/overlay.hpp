/**
 * @file overlay.hpp
 * @brief Plural structure maps: overlays without a second writer (§5.12).
 *
 * Implements Section 5.12 of structure-hyperop-vision.md:
 * - OverlayClaimState & OverlayCandidate DTOs.
 * - Persistent overlay authoring helpers (declareOverlayTarget,
 *   authorOverlayClaim, authorOverlayBreak, sealOverlayRelease, rebaseOverlay).
 * - OverlayComposition: two-slot arbitration, candidate provenance,
 *   target snapshot validation, reader trust graph governance.
 * - Discovery rendezvous target generator.
 */
#ifndef COMMON_XANADU_OVERLAY_HPP
#define COMMON_XANADU_OVERLAY_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/extern_ref.hpp"
#include "common/xanadu/link_discovery.hpp"
#include "common/xanadu/link_package.hpp"
#include "common/xanadu/microversion.hpp"
#include "common/xanadu/mutable_link.hpp"
#include "common/xanadu/ops.hpp"
#include "common/xanadu/publication.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"
#include "common/xanadu/zigzag/manifold.hpp"
#include <gleditor/cpp26.hpp>
#include <gleditor/ranges.hpp>

namespace xanadu {

/**
 * @brief Evaluation state of an overlay claim (§5.12 §4).
 */
enum class OverlayClaimState : std::uint8_t {
  Applied,  ///< Won arbitration and materialised into arena view
  Shadowed, ///< Lost arbitration to intrinsic base link or higher-prominence
            ///< claim
  SlotConflict,     ///< One slot was free, but reciprocal opposite slot was
                    ///< occupied
  TargetNotFetched, ///< Target document snapshot bytes were not found/fetched
  TargetAbsent,     ///< Target cell does not exist in target store
  OutsideSnapshot,  ///< Claim references an operation minted after the declared
                    ///< snapshot
  Unintelligible,   ///< Operation was corrupted or not a valid SetLink
};

/**
 * @brief An inspectable alternative structure claim (§5.12 §4).
 */
struct OverlayCandidate {
  zigzag::CellRef claimHandle{zigzag::noCell};
  zigzag::CellRef from{zigzag::noCell};
  zigzag::DimRef dimension{zigzag::noCell};
  zigzag::DimVector direction{zigzag::DimVector::POS};
  zigzag::CellRef to{zigzag::noCell};
  ProminenceTier tier{ProminenceTier::Public};
  std::int64_t score{0};
  OverlayClaimState state{OverlayClaimState::Unintelligible};
  std::uint32_t claimOpIndex{0};
  std::string publicationKey;
  MicroversionId claimVersion;

  [[nodiscard]] bool isBreak() const noexcept { return to == zigzag::noCell; }
  [[nodiscard]] bool isApplied() const noexcept {
    return state == OverlayClaimState::Applied;
  }
};

/**
 * @brief An attached overlay store and its publication provenance (§5.12 §4).
 */
struct OverlayAttachment {
  const Store *store{nullptr};
  const Scroll *sealedAs{nullptr};
  PublicKey publisher;
  std::string publicationKey;
  std::int64_t sequence{0};
  std::optional<ProminenceTier> explicitTier;
  zigzag::CellRef releaseCell{zigzag::noCell};
  std::vector<GlobalLink> classicLinks;
};

/**
 * @brief Target descriptor declared on d.overlay-targets (§5.12 §3).
 */
struct DeclaredTarget {
  zigzag::CellRef targetCell{zigzag::noCell};
  GlobalDocumentState state;
};

// -- Authoring Helpers (§5.12 §2, §3, §8) -----------------------------------

struct OverlayTargetResult {
  MicroversionId version;
  zigzag::CellRef targetCell{zigzag::noCell};
  zigzag::CellRef releaseCell{zigzag::noCell};
};

struct OverlayClaimResult {
  MicroversionId version;
  zigzag::CellRef handleCell{zigzag::noCell};
  zigzag::CellRef releaseCell{zigzag::noCell};
};

struct OverlayReleaseResult {
  MicroversionId version;
  zigzag::CellRef releaseCell{zigzag::noCell};
};

/**
 * @brief Declare a target document snapshot on d.overlay-targets (§5.12 §3).
 */
[[nodiscard]] OverlayTargetResult
declareOverlayTarget(Store &store, const MicroversionId &parent,
                     const GlobalDocumentState &targetState,
                     zigzag::CellRef releaseCell = zigzag::noCell);

[[nodiscard]] OverlayTargetResult
declareOverlayTarget(Store &store, const MicroversionId &parent,
                     zigzag::CellRef releaseCell,
                     const GlobalDocumentState &targetState);

/**
 * @brief Author an overlay SetLink claim and file its handle on
 * d.overlay-claims (§5.12 §2).
 */
[[nodiscard]] OverlayClaimResult authorOverlayClaim(
    Store &store, const MicroversionId &parent, zigzag::CellRef from,
    zigzag::DimRef dim, zigzag::DimVector dir, zigzag::CellRef to,
    zigzag::CellRef releaseCell = zigzag::noCell, std::string_view label = {});

[[nodiscard]] OverlayClaimResult
authorOverlayClaim(Store &store, const MicroversionId &parent,
                   zigzag::CellRef releaseCell, zigzag::CellRef from,
                   zigzag::DimRef dim, zigzag::DimVector dir,
                   zigzag::CellRef to, std::string_view label = {});

/**
 * @brief Author an explicit break (to == noCell) and file its handle on
 * d.overlay-claims (§5.12 §2).
 */
[[nodiscard]] OverlayClaimResult authorOverlayBreak(
    Store &store, const MicroversionId &parent, zigzag::CellRef from,
    zigzag::DimRef dim, zigzag::DimVector dir,
    zigzag::CellRef releaseCell = zigzag::noCell, std::string_view label = {});

[[nodiscard]] OverlayClaimResult
authorOverlayBreak(Store &store, const MicroversionId &parent,
                   zigzag::CellRef releaseCell, zigzag::CellRef from,
                   zigzag::DimRef dim, zigzag::DimVector dir,
                   std::string_view label = {});

/**
 * @brief Seal an overlay release by touching releaseCell last (§5.12 §2).
 */
[[nodiscard]] OverlayReleaseResult
sealOverlayRelease(Store &store, const MicroversionId &parent,
                   zigzag::CellRef releaseCell, std::string_view label = {});

struct RebaseResult {
  MicroversionId version;
  zigzag::CellRef newReleaseCell{zigzag::noCell};
  std::vector<OverlayCandidate> conflictingClaims;
};

/**
 * @brief Rebase an overlay onto a newer target state (§5.12 §8).
 */
[[nodiscard]] RebaseResult
rebaseOverlay(Store &overlayStore, const MicroversionId &parent,
              zigzag::CellRef releaseCell, const Store &targetStore,
              const GlobalDocumentState &newTargetState,
              std::string_view newReleaseLabel = {});

/**
 * @brief Compute the DHT rendezvous target for overlays on @p targetScrollKey
 * (§5.12 §9).
 */
[[nodiscard]] DhtTarget
overlayRendezvousTarget(std::string_view targetScrollKey);

// -- Plural Composition & Arbitration (§5.12 §4, §5, §6) --------------------

class OverlayComposition {
public:
  struct TargetSpec {
    std::uint32_t spaceId{0};
    GlobalDocumentState state;
    const Store *store{nullptr};
    const zigzag::Manifold *manifold{nullptr};
    PublicKey author;
    std::string scrollKey;
  };

  OverlayComposition() = default;

  // Fluent chainable configuration
  OverlayComposition &withTarget(TargetSpec spec) &;
  OverlayComposition &&withTarget(TargetSpec spec) &&;

  OverlayComposition &withOverlay(OverlayAttachment overlay) &;
  OverlayComposition &&withOverlay(OverlayAttachment overlay) &&;

  OverlayComposition &withCurators(std::set<PublicKey> followedCurators) &;
  OverlayComposition &&withCurators(std::set<PublicKey> followedCurators) &&;

  OverlayComposition &withMaxPublicOverlays(std::size_t maxCount) &;
  OverlayComposition &&withMaxPublicOverlays(std::size_t maxCount) &&;

  OverlayComposition &withClassicLink(GlobalLink link) &;
  OverlayComposition &&withClassicLink(GlobalLink link) &&;

  /// Execute composition and arbitrate two-slot links
  OverlayComposition &compose() &;
  OverlayComposition &&compose() &&;

  // Monoidal combination
  OverlayComposition &merge(const OverlayComposition &other);
  friend OverlayComposition operator+(OverlayComposition a,
                                      const OverlayComposition &b) {
    a.merge(b);
    return a;
  }

  // Functional query API
  [[nodiscard]] const zigzag::ArenaManifold &view() const noexcept {
    return arena_;
  }
  [[nodiscard]] zigzag::ArenaManifold &view() noexcept { return arena_; }

  [[nodiscard]] std::span<const OverlayCandidate>
  candidates(zigzag::CellRef from, zigzag::DimRef dim,
             zigzag::DimVector dir) const noexcept;

  [[nodiscard]] std::optional<zigzag::CellRef>
  effectiveLink(zigzag::CellRef from, zigzag::DimRef dim,
                zigzag::DimVector dir) const noexcept;

  [[nodiscard]] const std::vector<OverlayCandidate> &
  allCandidates() const noexcept {
    return allCandidates_;
  }

  [[nodiscard]] const std::vector<GlobalLink> &classicLinks() const noexcept {
    return classicLinks_;
  }

  [[nodiscard]] std::vector<GlobalLink>
  classicLinksTouching(const GlobalSpan &span) const;

  [[nodiscard]] const std::vector<TargetSpec> &targets() const noexcept {
    return targets_;
  }

  [[nodiscard]] const std::vector<OverlayAttachment> &
  overlays() const noexcept {
    return overlays_;
  }

private:
  struct SlotKey {
    zigzag::CellRef cell{zigzag::noCell};
    zigzag::DimRef dim{zigzag::noCell};
    zigzag::DimVector dir{zigzag::DimVector::POS};
    bool operator==(const SlotKey &) const  = default;
    auto operator<=>(const SlotKey &) const = default;
  };

  zigzag::ArenaManifold arena_;
  std::vector<TargetSpec> targets_;
  std::vector<OverlayAttachment> overlays_;
  std::set<PublicKey> followedCurators_;
  std::size_t maxPublicOverlays_{10};

  std::map<SlotKey, std::vector<OverlayCandidate>> candidateMap_;
  std::vector<OverlayCandidate> allCandidates_;
  std::set<SlotKey> occupiedSlots_;
  std::vector<GlobalLink> classicLinks_;
};

} // namespace xanadu

#endif // COMMON_XANADU_OVERLAY_HPP
