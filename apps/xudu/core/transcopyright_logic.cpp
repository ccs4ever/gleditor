/**
 * @file transcopyright_logic.cpp
 * @brief Implementation of pure transcopyright and hole reasoning logic.
 */
#include "transcopyright_logic.hpp"

#include <cstring>
#include <mutex>
#include <unordered_map>

#include "store.hpp"

namespace xudu {

namespace {

struct ArrayHash {
  std::size_t
  operator()(const std::array<std::uint8_t, 32> &arr) const noexcept {
    std::size_t seed = 0;
    for (std::size_t i = 0; i < 32; i += sizeof(std::size_t)) {
      std::size_t val = 0;
      std::memcpy(&val, arr.data() + i, sizeof(std::size_t));
      seed ^= val + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }
    return seed;
  }
};

std::mutex s_keyVaultMutex;
std::unordered_map<std::array<std::uint8_t, 32>, crypto::Key32, ArrayHash>
    s_keyVault;

} // namespace

std::string TranscopyrightLogic::reasonLabel(const HoleReason reason) noexcept {
  switch (reason) {
  case HoleReason::Withheld:
    return "WITHHELD";
  case HoleReason::Revoked:
    return "REVOKED";
  case HoleReason::Takedown:
    return "TAKEDOWN";
  case HoleReason::TranscopyrightLock:
    return "PAYWALL";
  case HoleReason::Unsealed:
    return "UNSEALED";
  }
  return "UNKNOWN";
}

std::string
TranscopyrightLogic::formatCost(const TranscopyrightDescriptor &desc,
                                const std::uint64_t byteCount) noexcept {
  const auto cost = desc.computeCost(byteCount);
  return formatCost(cost, desc.currencySymbol);
}

std::string TranscopyrightLogic::formatCost(const std::uint64_t cost,
                                            const std::string_view symbol) {
  if (symbol == "XU" && cost >= 1000000000ULL) {
    if (cost % 1000000000ULL == 0) {
      return std::to_string(cost / 1000000000ULL) + " " + std::string(symbol);
    }
    const double xu = static_cast<double>(cost) / 1000000000.0;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f %.*s", xu,
                  static_cast<int>(symbol.size()), symbol.data());
    return std::string(buf);
  }
  return std::to_string(cost) + " " + std::string(symbol);
}

std::array<std::uint8_t, 32>
TranscopyrightLogic::testKeyId(const std::string_view seed) noexcept {
  std::array<std::uint8_t, 32> keyId{};
  for (std::size_t i = 0; i < 32; ++i) {
    keyId[i] = static_cast<std::uint8_t>(
        (i < seed.size() ? static_cast<std::uint8_t>(seed[i]) : 0x5AU) ^
        (i * 13U));
  }
  return keyId;
}

void TranscopyrightLogic::registerTestCek(
    const std::array<std::uint8_t, 32> &keyId, const crypto::Key32 &cek) {
  std::lock_guard<std::mutex> lock(s_keyVaultMutex);
  s_keyVault[keyId] = cek;
}

std::optional<crypto::Key32>
TranscopyrightLogic::lookupTestCek(const std::array<std::uint8_t, 32> &keyId) {
  std::lock_guard<std::mutex> lock(s_keyVaultMutex);
  const auto it = s_keyVault.find(keyId);
  if (it != s_keyVault.end()) {
    return it->second;
  }
  return std::nullopt;
}

crypto::Key32 TranscopyrightLogic::deriveDeterministicTestCek(
    const std::array<std::uint8_t, 32> &keyId) {
  // Check explicit registration first
  if (const auto found = lookupTestCek(keyId)) {
    return *found;
  }

  // Deterministic fallback derivation
  crypto::Key32 cek{};
  for (std::size_t i = 0; i < 32; ++i) {
    cek[i] = static_cast<std::uint8_t>(keyId[i] ^ 0xA5U ^ (i * 7U));
  }
  return cek;
}

std::vector<HoleSpanInfo>
TranscopyrightLogic::inspectHoles(const Store &st, const Version &version,
                                  const std::size_t docIndex,
                                  const std::size_t storeIndex) {
  std::vector<HoleSpanInfo> out;

  for (const auto &piece : version.pieces()) {
    if (piece.empty() || breakMarkerScroll == piece.scroll ||
        vocabularyScroll == piece.scroll) {
      continue;
    }

    const auto overlapping =
        st.segmentsOverlapping(piece.scroll, piece.start, piece.length);
    for (const auto &segment : overlapping) {
      if (!segment.isWithheld()) {
        continue;
      }
      const auto holeStartInScroll = std::max(piece.start, segment.at);
      const auto holeEndInScroll   = std::min(piece.end(), segment.end());
      if (holeEndInScroll <= holeStartInScroll) {
        continue;
      }
      const auto holeLen = holeEndInScroll - holeStartInScroll;
      const PrimediaSpan holeSpan{piece.scroll, holeStartInScroll, holeLen};

      const auto res = st.resolve(holeSpan);
      if (res.status != ResolutionStatus::WithheldRedacted &&
          res.status != ResolutionStatus::TranscopyrightLocked) {
        continue;
      }

      const auto reason =
          res.holeRecord
              ? res.holeRecord->reason
              : (segment.holeRecord
                     ? segment.holeRecord->reason
                     : (res.status == ResolutionStatus::TranscopyrightLocked
                            ? HoleReason::TranscopyrightLock
                            : HoleReason::Withheld));
      const auto colour =
          (res.status == ResolutionStatus::TranscopyrightLocked ||
           reason == HoleReason::TranscopyrightLock)
              ? kTranscopyrightColour
              : colourForHole(reason);

      const auto lockDescriptor =
          res.lockInfo.has_value()
              ? res.lockInfo
              : (segment.holeRecord ? segment.holeRecord->transcopyright
                                    : std::nullopt);

      for (const auto &extent : version.occurrencesOf(piece)) {
        const auto docCharStart =
            extent.start +
            static_cast<std::uint32_t>(holeStartInScroll - piece.start);
        const auto docCharEnd =
            docCharStart + static_cast<std::uint32_t>(holeLen);

        HoleSpanInfo info;
        info.docIndex       = docIndex;
        info.storeIndex     = storeIndex;
        info.span           = holeSpan;
        info.charStart      = docCharStart;
        info.charEnd        = docCharEnd;
        info.charOffset     = docCharStart;
        info.length         = static_cast<std::uint32_t>(holeLen);
        info.reason         = reason;
        info.color          = colour;
        info.reasonLabel    = reasonLabel(reason);
        info.transcopyright = lockDescriptor;
        out.push_back(std::move(info));
      }
    }
  }

  return out;
}

} // namespace xudu
