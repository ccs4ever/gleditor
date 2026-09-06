/**
 * @file transcopyright_logic.hpp
 * @brief Core logic for Transcopyright micropayments, Permascroll Holes, and
 * Wireframe streaming.
 */
#ifndef XUDU_TRANSCOPYRIGHT_LOGIC_HPP
#define XUDU_TRANSCOPYRIGHT_LOGIC_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "resolver.hpp"
#include "scroll.hpp"
#include "transcopyright_crypto.hpp"
#include "version.hpp"

namespace xudu {

class Store;

/**
 * @struct HoleSpanInfo
 * @brief Rich metadata describing an active withheld or transcopyright-locked
 * span in an open view.
 */
struct HoleSpanInfo {
  std::size_t docIndex{0};
  std::size_t storeIndex{0};
  PrimediaSpan span{};
  std::uint32_t charStart{0};
  std::uint32_t charEnd{0};
  std::uint32_t charOffset{0};
  std::uint32_t length{0};
  HoleReason reason{HoleReason::Withheld};
  std::uint32_t color{kWithheldColour};
  std::string reasonLabel{"WITHHELD"};
  std::optional<TranscopyrightDescriptor> transcopyright{std::nullopt};

  [[nodiscard]] bool isLocked() const noexcept {
    return reason == HoleReason::TranscopyrightLock &&
           transcopyright.has_value();
  }

  [[nodiscard]] bool isWithheld() const noexcept {
    return reason != HoleReason::TranscopyrightLock;
  }

  [[nodiscard]] const std::optional<TranscopyrightDescriptor> &
  lockDescriptor() const noexcept {
    return transcopyright;
  }

  [[nodiscard]] std::string badgeText() const {
    if (isLocked() && transcopyright) {
      const auto &tc   = *transcopyright;
      const auto count = (length > 0) ? length : 1U;
      const auto cost  = tc.computeCost(count);
      return "🔒 Transcopyright: " + std::to_string(cost) + " " +
             tc.currencySymbol + " | Unlock ✦";
    }
    return "[" + reasonLabel + "]";
  }
};

/**
 * @struct WireframeProgress
 * @brief State telemetry for a document currently materializing from the
 * BitTorrent swarm.
 */
struct WireframeProgress {
  std::size_t docIndex{0};
  std::string title;
  std::string infoHash;
  std::uint32_t piecesFetched{0};
  std::uint32_t totalPieces{1};
  float shimmerPhase{0.0F};

  [[nodiscard]] bool isComplete() const noexcept {
    return piecesFetched >= totalPieces && totalPieces > 0;
  }

  [[nodiscard]] float progressFraction() const noexcept {
    if (totalPieces == 0) return 1.0F;
    return std::clamp(static_cast<float>(piecesFetched) /
                          static_cast<float>(totalPieces),
                      0.0F, 1.0F);
  }

  [[nodiscard]] float fraction() const noexcept { return progressFraction(); }

  [[nodiscard]] std::uint32_t percent() const noexcept {
    return static_cast<std::uint32_t>(progressFraction() * 100.0F);
  }

  [[nodiscard]] std::string statusLine() const {
    return "[" + std::to_string(piecesFetched) + "/" +
           std::to_string(totalPieces) + " pieces | " +
           std::to_string(percent()) + "% materializing]";
  }

  [[nodiscard]] std::string progressStatusText() const {
    return "⟳ Swarm Materializing: " + std::to_string(piecesFetched) + "/" +
           std::to_string(totalPieces) + " pieces (" +
           std::to_string(percent()) + "%)";
  }
};

/**
 * @class TranscopyrightLogic
 * @brief Pure engine utilities for classifying holes, computing pricing, and
 * resolving test keys.
 */
class TranscopyrightLogic {
public:
  [[nodiscard]] static std::string reasonLabel(HoleReason reason) noexcept;

  [[nodiscard]] static std::uint32_t
  colorForReason(HoleReason reason) noexcept {
    return colourForHole(reason);
  }

  [[nodiscard]] static std::string
  formatCost(const TranscopyrightDescriptor &desc,
             std::uint64_t byteCount) noexcept;

  [[nodiscard]] static std::string formatCost(std::uint64_t cost,
                                              std::string_view symbol);

  /// Generate deterministic test key ID
  [[nodiscard]] static std::array<std::uint8_t, 32>
  testKeyId(std::string_view seed) noexcept;

  /// Register a Content Encryption Key into the test/in-memory key vault
  static void registerTestCek(const std::array<std::uint8_t, 32> &keyId,
                              const crypto::Key32 &cek);

  /// Look up a previously registered test CEK
  [[nodiscard]] static std::optional<crypto::Key32>
  lookupTestCek(const std::array<std::uint8_t, 32> &keyId);

  /// Deterministically derive a valid CEK for testing/sample documents
  [[nodiscard]] static crypto::Key32
  deriveDeterministicTestCek(const std::array<std::uint8_t, 32> &keyId);

  /// Inspect a Store's version and find all active holes and locked spans
  [[nodiscard]] static std::vector<HoleSpanInfo>
  inspectHoles(const Store &st, const Version &version,
               std::size_t docIndex = 0, std::size_t storeIndex = 0);
};

} // namespace xudu

#endif // XUDU_TRANSCOPYRIGHT_LOGIC_HPP
