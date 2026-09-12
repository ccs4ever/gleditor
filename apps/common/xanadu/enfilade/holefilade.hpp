/**
 * @file holefilade.hpp
 * @brief The True Holefilade & Transcopyright Settlement Ledger.
 *
 * Implements Frontier 4 of the Grand Enfilade architecture:
 * A 1D interval B-enfilade managing authorial withholding, cryptographic
 * revocations, legal takedowns, and Transcopyright paywalls across permascroll
 * coordinates. Paired with an append-only Merkle micropayment settlement
 * ledger for succinct peer-wire verification.
 */
#ifndef XANADU_ENFILADE_HOLEFILADE_HPP
#define XANADU_ENFILADE_HOLEFILADE_HPP

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/xanadu/enfilade/crum_node.hpp"
#include "common/xanadu/identity/identity_layout.hpp"
#include "common/xanadu/merkle_ledger.hpp"
#include "common/xanadu/scroll.hpp"
#include "common/xanadu/spool.hpp"

namespace xanadu::enfilade {

struct HoleWid;

/**
 * @struct HoleDsp
 * @brief Relative coordinate displacement monoid along the permascroll axis.
 */
struct HoleDsp {
  std::int64_t deltaOffset{0};

  [[nodiscard]] HoleDsp compose(const HoleDsp &other) const noexcept {
    return HoleDsp{deltaOffset + other.deltaOffset};
  }

  [[nodiscard]] bool isIdentity() const noexcept { return 0 == deltaOffset; }

  [[nodiscard]] HoleWid act(const HoleWid &w) const noexcept;

  bool operator==(const HoleDsp &) const = default;
};

/**
 * @struct HoleWid
 * @brief Subtree aggregate summary width monoid tracking status bytes and
 *        micropayment liabilities.
 */
struct HoleWid {
  std::uint64_t minOffset{std::numeric_limits<std::uint64_t>::max()};
  std::uint64_t maxOffset{0};
  std::uint64_t clearBytes{0};
  std::uint64_t withheldBytes{0};
  std::uint64_t revokedBytes{0};
  std::uint64_t takedownBytes{0};
  std::uint64_t lockedBytes{0};
  std::uint64_t unsealedBytes{0};
  std::uint32_t holeMask{0};       ///< Bitmask of (1 << HoleReason)
  std::uint64_t microcentsOwed{0}; ///< Nano-xu liability for locked spans
  std::uint32_t spanCount{0};      ///< Number of distinct status spans

  [[nodiscard]] bool isEmpty() const noexcept {
    return minOffset >= maxOffset || 0 == spanCount;
  }

  [[nodiscard]] HoleWid combine(const HoleWid &other) const noexcept {
    if (isEmpty()) {
      return other;
    }
    if (other.isEmpty()) {
      return *this;
    }
    return HoleWid{
        .minOffset      = std::min(minOffset, other.minOffset),
        .maxOffset      = std::max(maxOffset, other.maxOffset),
        .clearBytes     = clearBytes + other.clearBytes,
        .withheldBytes  = withheldBytes + other.withheldBytes,
        .revokedBytes   = revokedBytes + other.revokedBytes,
        .takedownBytes  = takedownBytes + other.takedownBytes,
        .lockedBytes    = lockedBytes + other.lockedBytes,
        .unsealedBytes  = unsealedBytes + other.unsealedBytes,
        .holeMask       = holeMask | other.holeMask,
        .microcentsOwed = microcentsOwed + other.microcentsOwed,
        .spanCount      = spanCount + other.spanCount,
    };
  }

  [[nodiscard]] bool overlaps(const std::uint64_t start,
                              const std::uint64_t end) const noexcept {
    if (isEmpty() || start >= end) {
      return false;
    }
    return !(end <= minOffset || start >= maxOffset);
  }

  bool operator==(const HoleWid &) const = default;
};

inline HoleWid HoleDsp::act(const HoleWid &w) const noexcept {
  if (w.isEmpty() || 0 == deltaOffset) {
    return w;
  }
  const auto newMin =
      (deltaOffset >= 0)
          ? (w.minOffset + static_cast<std::uint64_t>(deltaOffset))
          : (w.minOffset > static_cast<std::uint64_t>(-deltaOffset)
                 ? w.minOffset - static_cast<std::uint64_t>(-deltaOffset)
                 : 0ULL);
  const auto newMax =
      (deltaOffset >= 0)
          ? (w.maxOffset + static_cast<std::uint64_t>(deltaOffset))
          : (w.maxOffset > static_cast<std::uint64_t>(-deltaOffset)
                 ? w.maxOffset - static_cast<std::uint64_t>(-deltaOffset)
                 : 0ULL);
  auto res      = w;
  res.minOffset = newMin;
  res.maxOffset = newMax;
  return res;
}

static_assert(DisplacementMonoid<HoleDsp>);
static_assert(WidthMonoid<HoleWid>);
static_assert(EnfiladeAction<HoleDsp, HoleWid>);

/**
 * @enum PermascrollSpanState
 * @brief High-level classification of a permascroll span.
 */
enum class PermascrollSpanState : std::uint8_t {
  Clear = 0, ///< Cleartext available
  Hole  = 1, ///< Withheld, revoked, takedown, or paywalled
};

/**
 * @struct HoleSpanEntry
 * @brief Leaf interval entry describing a contiguous span in a scroll.
 */
struct HoleSpanEntry {
  std::uint64_t start{0};
  std::uint64_t length{0};
  PermascrollSpanState state{PermascrollSpanState::Clear};
  HoleReason reason{HoleReason::Withheld};
  std::uint16_t flags{0};
  std::uint32_t priceAtomicUnits{0}; ///< Nano-xu per span or per byte
  bool flatFee{true};
  identity::Fingerprint authorWallet{};
  std::array<std::uint8_t, 32> keyId{};
  std::array<std::uint8_t, 32> contentCommitment{};

  [[nodiscard]] std::uint64_t end() const noexcept { return start + length; }
  [[nodiscard]] bool isHole() const noexcept {
    return PermascrollSpanState::Hole == state;
  }
  [[nodiscard]] bool isClear() const noexcept {
    return PermascrollSpanState::Clear == state;
  }

  [[nodiscard]] std::uint64_t
  computeCost(const std::uint64_t subLength) const noexcept {
    if (PermascrollSpanState::Hole != state ||
        HoleReason::TranscopyrightLock != reason) {
      return 0;
    }
    return flatFee ? priceAtomicUnits
                   : (static_cast<std::uint64_t>(priceAtomicUnits) * subLength);
  }

  bool operator==(const HoleSpanEntry &) const = default;
};

/**
 * @struct HoleSlice
 * @brief Result of decomposing an arbitrary query span into atomic status
 *        slices.
 */
struct HoleSlice {
  std::uint64_t start{0};
  std::uint64_t length{0};
  PermascrollSpanState state{PermascrollSpanState::Clear};
  HoleReason reason{HoleReason::Withheld};
  std::uint64_t costAtomicUnits{0};
  bool requiresPayment{false};
  identity::Fingerprint authorWallet{};
  std::array<std::uint8_t, 32> keyId{};
  std::array<std::uint8_t, 32> contentCommitment{};

  [[nodiscard]] std::uint64_t end() const noexcept { return start + length; }
  [[nodiscard]] bool isClear() const noexcept {
    return PermascrollSpanState::Clear == state;
  }
  [[nodiscard]] bool isHole() const noexcept {
    return PermascrollSpanState::Hole == state;
  }

  bool operator==(const HoleSlice &) const = default;
};

/**
 * @struct HoleCrum
 * @brief Routing node in the Holefilade B-enfilade tree (branching factor B =
 * 16).
 */
struct alignas(64) HoleCrum {
  static constexpr std::size_t BranchingFactor = 16;

  HoleDsp dsp{};
  HoleWid wid{};
  std::uint32_t firstChild{0};
  std::uint16_t childCount{0};
  std::uint32_t firstEntry{0};
  std::uint16_t entryCount{0};
  std::uint32_t parentIndex{0};
  bool isLeaf{true};
  std::uint8_t padding[15]{0};
};

/**
 * @class ScrollHolefilade
 * @brief 1D interval B-enfilade for a single permascroll.
 */
class ScrollHolefilade {
public:
  ScrollHolefilade() = default;

  static ScrollHolefilade
  buildFromEntries(const std::vector<HoleSpanEntry> &entries);

  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] const HoleWid &metrics() const noexcept {
    return crums_.empty() ? emptyWid_ : crums_[rootIndex_].wid;
  }

  /**
   * @brief Decompose an arbitrary span into contiguous cleartext and hole
   * slices in O(log N + K).
   */
  [[nodiscard]] std::vector<HoleSlice>
  decomposeSpan(std::uint64_t queryStart, std::uint64_t queryLength) const;

  /**
   * @brief Query aggregate status metrics over a specific span range in O(log N
   * + K).
   */
  [[nodiscard]] HoleWid statusOf(std::uint64_t queryStart,
                                 std::uint64_t queryLength) const;

  /**
   * @brief Calculate total micropayment cost to unlock paywalled spans in O(log
   * N + K).
   */
  [[nodiscard]] std::uint64_t microcentsOwed(std::uint64_t queryStart,
                                             std::uint64_t queryLength) const;

  /**
   * @brief Ruling R9 verification: validates decomposition against sequential
   * scans.
   */
  [[nodiscard]] bool verifyAgainstLinearScan(std::uint64_t queryStart,
                                             std::uint64_t queryLength) const;

  [[nodiscard]] const std::vector<HoleSpanEntry> &entries() const noexcept {
    return entries_;
  }

private:
  std::vector<HoleCrum> crums_;
  std::vector<HoleSpanEntry> entries_;
  std::size_t rootIndex_{0};

  static inline const HoleWid emptyWid_{};

  void buildTree();
  void decomposeRecursive(std::size_t crumIdx, std::uint64_t qStart,
                          std::uint64_t qEnd,
                          std::vector<HoleSlice> &out) const;
};

/**
 * @class Holefilade
 * @brief Universal multi-scroll router for permascroll hole and transcopyright
 * indexing.
 */
class Holefilade {
public:
  Holefilade() = default;

  void indexSpan(ScrollId scroll, const HoleSpanEntry &entry);
  void indexHoleRecord(ScrollId scroll, const PublishedHoleRecord &record);
  void indexScrollSegments(ScrollId scroll,
                           const std::vector<ScrollSegment> &segments);

  [[nodiscard]] std::vector<HoleSlice>
  decomposeSpan(const PrimediaSpan &span) const;

  [[nodiscard]] HoleWid statusOf(const PrimediaSpan &span) const;

  [[nodiscard]] std::uint64_t microcentsOwed(const PrimediaSpan &span) const;

  [[nodiscard]] bool hasScroll(ScrollId scroll) const noexcept {
    return scrolls_.find(scroll) != scrolls_.end();
  }

  [[nodiscard]] const ScrollHolefilade *
  getScroll(ScrollId scroll) const noexcept {
    const auto it = scrolls_.find(scroll);
    return it != scrolls_.end() ? &it->second : nullptr;
  }

private:
  std::unordered_map<ScrollId, ScrollHolefilade> scrolls_;
  std::unordered_map<ScrollId, std::vector<HoleSpanEntry>> rawEntries_;

  void rebuildScroll(ScrollId scroll);
};

/**
 * @struct PaymentReceipt
 * @brief An authenticated micropayment receipt for a transcopyright unlock.
 */
struct PaymentReceipt {
  std::uint64_t receiptId{0};
  ScrollId scroll{0};
  std::uint64_t offset{0};
  std::uint64_t length{0};
  std::uint64_t amountAtomicUnits{0}; ///< Nano-xu settled
  identity::Fingerprint payerWallet{};
  identity::Fingerprint payeeWallet{};
  std::array<std::uint8_t, 32> keyId{};
  std::uint64_t timestamp{0};
  std::string signature{}; ///< Ed25519 signature
  std::uint64_t sequence{0};

  [[nodiscard]] std::string canonicalForm() const;
  [[nodiscard]] std::array<std::uint8_t, 32> leafHash() const;
  [[nodiscard]] std::string leafHashHex() const;

  bool operator==(const PaymentReceipt &) const = default;
};

/**
 * @class SettlementLedger
 * @brief Append-only Merkle settlement ledger for Transcopyright micropayments.
 */
class SettlementLedger {
public:
  SettlementLedger();
  ~SettlementLedger();

  SettlementLedger(const SettlementLedger &)            = delete;
  SettlementLedger &operator=(const SettlementLedger &) = delete;
  SettlementLedger(SettlementLedger &&) noexcept;
  SettlementLedger &operator=(SettlementLedger &&) noexcept;

  /**
   * @brief Append a verified micropayment receipt into the Merkle ledger.
   * @return Sequence number of the recorded receipt.
   */
  std::uint64_t recordReceipt(PaymentReceipt receipt);

  /**
   * @brief Generate a succinct Merkle audit path for a receipt in O(log M).
   */
  [[nodiscard]] std::optional<MerkleProof>
  generateProof(std::size_t receiptIndex) const;

  /**
   * @brief Verify that an audit proof authenticates a receipt against a
   * published Merkle root.
   */
  [[nodiscard]] static bool
  verifyReceiptProof(const PaymentReceipt &receipt, const MerkleProof &proof,
                     const std::array<std::uint8_t, 32> &expectedRoot);

  [[nodiscard]] std::size_t receiptCount() const noexcept {
    return receipts_.size();
  }
  [[nodiscard]] std::uint64_t totalSettledNanoXu() const noexcept {
    return totalSettled_;
  }
  [[nodiscard]] std::uint64_t
  totalSettledForAuthor(const identity::Fingerprint &author) const noexcept;
  [[nodiscard]] std::uint64_t
  totalSettledForScroll(ScrollId scroll) const noexcept;

  [[nodiscard]] std::array<std::uint8_t, 32> rootHash() const;
  [[nodiscard]] std::string rootHashHex() const;

  [[nodiscard]] const std::vector<PaymentReceipt> &receipts() const noexcept {
    return receipts_;
  }

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::vector<PaymentReceipt> receipts_;
  std::uint64_t totalSettled_{0};
  std::unordered_map<std::string, std::uint64_t> authorTotals_;
  std::unordered_map<ScrollId, std::uint64_t> scrollTotals_;
};

} // namespace xanadu::enfilade

#endif // XANADU_ENFILADE_HOLEFILADE_HPP
