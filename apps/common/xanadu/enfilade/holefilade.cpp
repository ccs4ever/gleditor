/**
 * @file holefilade.cpp
 * @brief Implementation of the True Holefilade & Transcopyright Settlement
 * Ledger.
 */
#include "common/xanadu/enfilade/holefilade.hpp"

#include <libtorrent/hasher.hpp>
#include <merklecpp.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace xanadu::enfilade {

namespace {

constexpr char kInteriorDomain = '\x01';

void sha256_receipt(const merkle::HashT<32> &l, const merkle::HashT<32> &r,
                    merkle::HashT<32> &out) {
  libtorrent::hasher256 h;
  h.update(&kInteriorDomain, 1);
  h.update(reinterpret_cast<const char *>(l.bytes), 32);
  h.update(reinterpret_cast<const char *>(r.bytes), 32);
  const auto dig = h.final();
  std::memcpy(out.bytes, dig.data(), 32);
}

using ReceiptTree = merkle::TreeT<32, sha256_receipt>;
using ReceiptPath = merkle::PathT<32, sha256_receipt>;

[[nodiscard]] HoleWid computeWidForEntries(const HoleSpanEntry *entries,
                                           const std::size_t count) noexcept {
  HoleWid w{};
  for (std::size_t i = 0; i < count; ++i) {
    const auto &e = entries[i];
    w.minOffset   = std::min(w.minOffset, e.start);
    w.maxOffset   = std::max(w.maxOffset, e.end());
    w.spanCount += 1;

    if (e.isClear()) {
      w.clearBytes += e.length;
    } else {
      w.holeMask |= (1U << static_cast<std::uint8_t>(e.reason));
      switch (e.reason) {
      case HoleReason::Withheld:
        w.withheldBytes += e.length;
        break;
      case HoleReason::Revoked:
        w.revokedBytes += e.length;
        break;
      case HoleReason::Takedown:
        w.takedownBytes += e.length;
        break;
      case HoleReason::TranscopyrightLock:
        w.lockedBytes += e.length;
        w.microcentsOwed += e.computeCost(e.length);
        break;
      case HoleReason::Unsealed:
        w.unsealedBytes += e.length;
        break;
      }
    }
  }
  return w;
}

} // namespace

// ============================================================================
// PaymentReceipt
// ============================================================================

std::string PaymentReceipt::canonicalForm() const {
  std::ostringstream oss;
  oss << "receipt:" << receiptId << "\n"
      << "scroll:" << scroll << "\n"
      << "offset:" << offset << "\n"
      << "length:" << length << "\n"
      << "amount:" << amountAtomicUnits << "\n"
      << "payer:" << payerWallet.view() << "\n"
      << "payee:" << payeeWallet.view() << "\n"
      << "keyId:";
  for (const auto b : keyId) {
    oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
  }
  oss << "\n"
      << "timestamp:" << timestamp << "\n"
      << "sequence:" << sequence << "\n"
      << "sig:" << signature;
  return oss.str();
}

std::array<std::uint8_t, 32> PaymentReceipt::leafHash() const {
  const auto canon           = canonicalForm();
  constexpr char kLeafDomain = '\x00';
  libtorrent::hasher256 h;
  h.update(&kLeafDomain, 1);
  h.update(canon.data(), canon.size());
  const auto dig = h.final();

  std::array<std::uint8_t, 32> out{};
  std::memcpy(out.data(), dig.data(), 32);
  return out;
}

std::string PaymentReceipt::leafHashHex() const {
  const auto h = leafHash();
  std::ostringstream oss;
  for (const auto b : h) {
    oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
  }
  return oss.str();
}

// ============================================================================
// SettlementLedger
// ============================================================================

struct SettlementLedger::Impl {
  ReceiptTree tree;
};

SettlementLedger::SettlementLedger() : impl_(std::make_unique<Impl>()) {}
SettlementLedger::~SettlementLedger()                            = default;
SettlementLedger::SettlementLedger(SettlementLedger &&) noexcept = default;
SettlementLedger &
SettlementLedger::operator=(SettlementLedger &&) noexcept = default;

std::uint64_t SettlementLedger::recordReceipt(PaymentReceipt receipt) {
  receipt.sequence = receipts_.size();
  if (0 == receipt.receiptId) {
    receipt.receiptId = receipt.sequence + 1;
  }

  const auto h = receipt.leafHash();
  const merkle::HashT<32> mHash(h.data());
  impl_->tree.insert(mHash);

  totalSettled_ += receipt.amountAtomicUnits;
  authorTotals_[receipt.payeeWallet.toString()] += receipt.amountAtomicUnits;
  scrollTotals_[receipt.scroll] += receipt.amountAtomicUnits;

  receipts_.push_back(receipt);
  return receipt.sequence;
}

std::optional<MerkleProof>
SettlementLedger::generateProof(const std::size_t receiptIndex) const {
  if (receiptIndex >= receipts_.size()) {
    return std::nullopt;
  }

  const auto path = impl_->tree.path(receiptIndex);
  if (!path) {
    return std::nullopt;
  }

  MerkleProof proof;
  proof.leafIndex = receiptIndex;
  proof.maxIndex  = receipts_.size() - 1;
  proof.leafHash  = receipts_[receiptIndex].leafHash();
  proof.rootHash  = rootHash();

  for (const auto &elem : *path) {
    MerkleProof::Element el;
    std::memcpy(el.hash.data(), elem.hash.bytes, 32);
    el.isLeft = (ReceiptPath::PATH_LEFT == elem.direction);
    proof.path.push_back(el);
  }

  return proof;
}

bool SettlementLedger::verifyReceiptProof(
    const PaymentReceipt &receipt, const MerkleProof &proof,
    const std::array<std::uint8_t, 32> &expectedRoot) {
  if (receipt.leafHash() != proof.leafHash) {
    return false;
  }
  return proof.verify(expectedRoot);
}

std::uint64_t SettlementLedger::totalSettledForAuthor(
    const identity::Fingerprint &author) const noexcept {
  const auto it = authorTotals_.find(author.toString());
  return it != authorTotals_.end() ? it->second : 0ULL;
}

std::uint64_t
SettlementLedger::totalSettledForScroll(const ScrollId scroll) const noexcept {
  const auto it = scrollTotals_.find(scroll);
  return it != scrollTotals_.end() ? it->second : 0ULL;
}

std::array<std::uint8_t, 32> SettlementLedger::rootHash() const {
  std::array<std::uint8_t, 32> out{};
  if (0 == impl_->tree.size()) {
    return out;
  }
  const auto r = impl_->tree.root();
  std::memcpy(out.data(), r.bytes, 32);
  return out;
}

std::string SettlementLedger::rootHashHex() const {
  const auto r = rootHash();
  std::ostringstream oss;
  for (const auto b : r) {
    oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
  }
  return oss.str();
}

// ============================================================================
// ScrollHolefilade
// ============================================================================

ScrollHolefilade
ScrollHolefilade::buildFromEntries(const std::vector<HoleSpanEntry> &entries) {
  ScrollHolefilade filade;
  filade.entries_ = entries;
  std::sort(filade.entries_.begin(), filade.entries_.end(),
            [](const HoleSpanEntry &a, const HoleSpanEntry &b) {
              return a.start < b.start;
            });
  filade.buildTree();
  return filade;
}

void ScrollHolefilade::buildTree() {
  crums_.clear();
  rootIndex_ = 0;
  if (entries_.empty()) {
    return;
  }

  constexpr std::size_t B = HoleCrum::BranchingFactor;
  std::vector<std::size_t> currentLevel;

  // 1. Leaf level
  for (std::size_t i = 0; i < entries_.size(); i += B) {
    const auto count =
        static_cast<std::uint16_t>(std::min(B, entries_.size() - i));
    const auto idx = crums_.size();

    HoleCrum crum;
    crum.firstEntry  = static_cast<std::uint32_t>(i);
    crum.entryCount  = count;
    crum.firstChild  = 0;
    crum.childCount  = 0;
    crum.parentIndex = 0;
    crum.isLeaf      = true;
    crum.wid         = computeWidForEntries(&entries_[i], count);

    crums_.push_back(crum);
    currentLevel.push_back(idx);
  }

  // 2. Interior levels
  while (currentLevel.size() > 1) {
    std::vector<std::size_t> nextLevel;
    for (std::size_t i = 0; i < currentLevel.size(); i += B) {
      const auto childCount =
          static_cast<std::uint16_t>(std::min(B, currentLevel.size() - i));
      const auto parentIdx  = crums_.size();
      const auto firstChild = static_cast<std::uint32_t>(currentLevel[i]);

      HoleCrum parentCrum;
      parentCrum.firstEntry  = 0;
      parentCrum.entryCount  = 0;
      parentCrum.firstChild  = firstChild;
      parentCrum.childCount  = childCount;
      parentCrum.parentIndex = 0;
      parentCrum.isLeaf      = false;

      HoleWid parentWid{};
      for (std::size_t c = 0; c < childCount; ++c) {
        const auto chIdx          = currentLevel[i + c];
        crums_[chIdx].parentIndex = static_cast<std::uint32_t>(parentIdx);
        parentWid                 = parentWid.combine(crums_[chIdx].wid);
      }
      parentCrum.wid = parentWid;

      crums_.push_back(parentCrum);
      nextLevel.push_back(parentIdx);
    }
    currentLevel = std::move(nextLevel);
  }

  rootIndex_ = currentLevel.empty() ? 0 : currentLevel.front();
}

void ScrollHolefilade::decomposeRecursive(const std::size_t crumIdx,
                                          const std::uint64_t qStart,
                                          const std::uint64_t qEnd,
                                          std::vector<HoleSlice> &out) const {
  if (crumIdx >= crums_.size()) {
    return;
  }
  const auto &crum = crums_[crumIdx];
  if (!crum.wid.overlaps(qStart, qEnd)) {
    return;
  }

  if (crum.isLeaf) {
    for (std::size_t i = 0; i < crum.entryCount; ++i) {
      const auto &e = entries_[crum.firstEntry + i];
      if (e.start >= qEnd || e.end() <= qStart) {
        continue;
      }
      const auto sStart = std::max(e.start, qStart);
      const auto sEnd   = std::min(e.end(), qEnd);
      const auto sLen   = sEnd - sStart;

      out.push_back(HoleSlice{
          .start           = sStart,
          .length          = sLen,
          .state           = e.state,
          .reason          = e.reason,
          .costAtomicUnits = e.computeCost(sLen),
          .requiresPayment =
              (e.isHole() && HoleReason::TranscopyrightLock == e.reason),
          .authorWallet      = e.authorWallet,
          .keyId             = e.keyId,
          .contentCommitment = e.contentCommitment,
      });
    }
    return;
  }

  for (std::size_t c = 0; c < crum.childCount; ++c) {
    decomposeRecursive(crum.firstChild + c, qStart, qEnd, out);
  }
}

std::vector<HoleSlice>
ScrollHolefilade::decomposeSpan(const std::uint64_t queryStart,
                                const std::uint64_t queryLength) const {
  if (0 == queryLength) {
    return {};
  }
  const auto queryEnd = queryStart + queryLength;

  std::vector<HoleSlice> rawSlices;
  if (!crums_.empty()) {
    decomposeRecursive(rootIndex_, queryStart, queryEnd, rawSlices);
  }

  std::sort(
      rawSlices.begin(), rawSlices.end(),
      [](const HoleSlice &a, const HoleSlice &b) { return a.start < b.start; });

  // Reconcile gaps with Clear slices to ensure contiguous, full coverage of
  // query
  std::vector<HoleSlice> reconciled;
  auto currentOffset = queryStart;

  for (const auto &s : rawSlices) {
    if (s.start > currentOffset) {
      // Gap before entry is cleartext
      reconciled.push_back(HoleSlice{
          .start           = currentOffset,
          .length          = s.start - currentOffset,
          .state           = PermascrollSpanState::Clear,
          .reason          = HoleReason::Withheld,
          .costAtomicUnits = 0,
          .requiresPayment = false,
      });
      currentOffset = s.start;
    }

    if (s.end() > currentOffset) {
      const auto sStart = currentOffset;
      const auto sEnd   = std::min(s.end(), queryEnd);
      const auto sLen   = sEnd - sStart;

      auto slice            = s;
      slice.start           = sStart;
      slice.length          = sLen;
      slice.costAtomicUnits = (s.length > 0)
                                  ? (s.costAtomicUnits * sLen / s.length)
                                  : s.costAtomicUnits;
      reconciled.push_back(slice);
      currentOffset = sEnd;
    }
  }

  if (currentOffset < queryEnd) {
    reconciled.push_back(HoleSlice{
        .start           = currentOffset,
        .length          = queryEnd - currentOffset,
        .state           = PermascrollSpanState::Clear,
        .reason          = HoleReason::Withheld,
        .costAtomicUnits = 0,
        .requiresPayment = false,
    });
  }

  return reconciled;
}

HoleWid ScrollHolefilade::statusOf(const std::uint64_t queryStart,
                                   const std::uint64_t queryLength) const {
  const auto slices = decomposeSpan(queryStart, queryLength);
  HoleWid w{};

  for (const auto &s : slices) {
    w.minOffset = std::min(w.minOffset, s.start);
    w.maxOffset = std::max(w.maxOffset, s.end());
    w.spanCount += 1;

    if (s.isClear()) {
      w.clearBytes += s.length;
    } else {
      w.holeMask |= (1U << static_cast<std::uint8_t>(s.reason));
      switch (s.reason) {
      case HoleReason::Withheld:
        w.withheldBytes += s.length;
        break;
      case HoleReason::Revoked:
        w.revokedBytes += s.length;
        break;
      case HoleReason::Takedown:
        w.takedownBytes += s.length;
        break;
      case HoleReason::TranscopyrightLock:
        w.lockedBytes += s.length;
        w.microcentsOwed += s.costAtomicUnits;
        break;
      case HoleReason::Unsealed:
        w.unsealedBytes += s.length;
        break;
      }
    }
  }

  return w;
}

std::uint64_t
ScrollHolefilade::microcentsOwed(const std::uint64_t queryStart,
                                 const std::uint64_t queryLength) const {
  return statusOf(queryStart, queryLength).microcentsOwed;
}

bool ScrollHolefilade::verifyAgainstLinearScan(
    const std::uint64_t queryStart, const std::uint64_t queryLength) const {
  const auto enfiladeSlices = decomposeSpan(queryStart, queryLength);
  const auto qEnd           = queryStart + queryLength;

  // Linear scan across entries_
  std::vector<HoleSlice> linearRaw;
  for (const auto &e : entries_) {
    if (e.start >= qEnd || e.end() <= queryStart) {
      continue;
    }
    const auto sStart = std::max(e.start, queryStart);
    const auto sEnd   = std::min(e.end(), qEnd);
    const auto sLen   = sEnd - sStart;

    linearRaw.push_back(HoleSlice{
        .start           = sStart,
        .length          = sLen,
        .state           = e.state,
        .reason          = e.reason,
        .costAtomicUnits = e.computeCost(sLen),
        .requiresPayment =
            (e.isHole() && HoleReason::TranscopyrightLock == e.reason),
        .authorWallet      = e.authorWallet,
        .keyId             = e.keyId,
        .contentCommitment = e.contentCommitment,
    });
  }

  std::sort(
      linearRaw.begin(), linearRaw.end(),
      [](const HoleSlice &a, const HoleSlice &b) { return a.start < b.start; });

  std::vector<HoleSlice> linearReconciled;
  auto currentOffset = queryStart;
  for (const auto &s : linearRaw) {
    if (s.start > currentOffset) {
      linearReconciled.push_back(HoleSlice{
          .start           = currentOffset,
          .length          = s.start - currentOffset,
          .state           = PermascrollSpanState::Clear,
          .reason          = HoleReason::Withheld,
          .costAtomicUnits = 0,
          .requiresPayment = false,
      });
      currentOffset = s.start;
    }
    if (s.end() > currentOffset) {
      const auto sStart = currentOffset;
      const auto sEnd   = std::min(s.end(), qEnd);
      const auto sLen   = sEnd - sStart;

      auto slice            = s;
      slice.start           = sStart;
      slice.length          = sLen;
      slice.costAtomicUnits = (s.length > 0)
                                  ? (s.costAtomicUnits * sLen / s.length)
                                  : s.costAtomicUnits;
      linearReconciled.push_back(slice);
      currentOffset = sEnd;
    }
  }
  if (currentOffset < qEnd) {
    linearReconciled.push_back(HoleSlice{
        .start           = currentOffset,
        .length          = qEnd - currentOffset,
        .state           = PermascrollSpanState::Clear,
        .reason          = HoleReason::Withheld,
        .costAtomicUnits = 0,
        .requiresPayment = false,
    });
  }

  if (enfiladeSlices.size() != linearReconciled.size()) {
    return false;
  }
  for (std::size_t i = 0; i < enfiladeSlices.size(); ++i) {
    if (enfiladeSlices[i] != linearReconciled[i]) {
      return false;
    }
  }

  return true;
}

// ============================================================================
// Holefilade (Multi-Scroll Router)
// ============================================================================

void Holefilade::indexSpan(const ScrollId scroll, const HoleSpanEntry &entry) {
  rawEntries_[scroll].push_back(entry);
  rebuildScroll(scroll);
}

void Holefilade::indexHoleRecord(const ScrollId scroll,
                                 const PublishedHoleRecord &record) {
  HoleSpanEntry entry{
      .start             = record.at,
      .length            = record.length,
      .state             = PermascrollSpanState::Hole,
      .reason            = record.reason,
      .flags             = 0,
      .priceAtomicUnits  = 0,
      .flatFee           = true,
      .authorWallet      = {},
      .keyId             = {},
      .contentCommitment = record.contentCommitment,
  };

  if (record.transcopyright.has_value()) {
    entry.priceAtomicUnits =
        static_cast<std::uint32_t>(record.transcopyright->priceAtomicUnits);
    entry.flatFee      = record.transcopyright->flatFee;
    entry.authorWallet = record.transcopyright->authorWallet;
    entry.keyId        = record.transcopyright->keyId;
  }

  indexSpan(scroll, entry);
}

void Holefilade::indexScrollSegments(
    const ScrollId scroll, const std::vector<ScrollSegment> &segments) {
  for (const auto &seg : segments) {
    HoleSpanEntry entry{
        .start  = seg.at,
        .length = seg.length,
        .state  = seg.holeRecord ? PermascrollSpanState::Hole
                                 : PermascrollSpanState::Clear,
        .reason =
            seg.holeRecord ? seg.holeRecord->reason : HoleReason::Withheld,
        .flags   = 0,
        .flatFee = true,
    };
    if (seg.holeRecord && seg.holeRecord->transcopyright.has_value()) {
      entry.priceAtomicUnits = static_cast<std::uint32_t>(
          seg.holeRecord->transcopyright->priceAtomicUnits);
      entry.flatFee      = seg.holeRecord->transcopyright->flatFee;
      entry.authorWallet = seg.holeRecord->transcopyright->authorWallet;
      entry.keyId        = seg.holeRecord->transcopyright->keyId;
    }
    rawEntries_[scroll].push_back(entry);
  }
  rebuildScroll(scroll);
}

void Holefilade::rebuildScroll(const ScrollId scroll) {
  scrolls_[scroll] = ScrollHolefilade::buildFromEntries(rawEntries_[scroll]);
}

std::vector<HoleSlice>
Holefilade::decomposeSpan(const PrimediaSpan &span) const {
  const auto it = scrolls_.find(span.scroll);
  if (it == scrolls_.end()) {
    // Unindexed scroll is treated as completely cleartext
    return {HoleSlice{
        .start           = span.start,
        .length          = span.length,
        .state           = PermascrollSpanState::Clear,
        .reason          = HoleReason::Withheld,
        .costAtomicUnits = 0,
        .requiresPayment = false,
    }};
  }
  return it->second.decomposeSpan(span.start, span.length);
}

HoleWid Holefilade::statusOf(const PrimediaSpan &span) const {
  const auto it = scrolls_.find(span.scroll);
  if (it == scrolls_.end()) {
    return HoleWid{
        .minOffset  = span.start,
        .maxOffset  = span.end(),
        .clearBytes = span.length,
        .spanCount  = 1,
    };
  }
  return it->second.statusOf(span.start, span.length);
}

std::uint64_t Holefilade::microcentsOwed(const PrimediaSpan &span) const {
  const auto it = scrolls_.find(span.scroll);
  if (it == scrolls_.end()) {
    return 0ULL;
  }
  return it->second.microcentsOwed(span.start, span.length);
}

} // namespace xanadu::enfilade
