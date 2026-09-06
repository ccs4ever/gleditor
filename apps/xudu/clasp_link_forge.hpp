/**
 * @file clasp_link_forge.hpp
 * @brief Tripartite Clasp Assembly Bench widget for N x M asymmetric
 * hyperlinking.
 */
#ifndef XUDU_CLASP_LINK_FORGE_HPP
#define XUDU_CLASP_LINK_FORGE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include <gleditor/canvas.hpp>
#include <gleditor/render/types.hpp>

#include "core/ops.hpp"
#include "core/pouch_zone.hpp"
#include "session.hpp"

struct RenderState;

namespace xudu {

/**
 * @class LinkForgeWidget
 * @brief Tripartite link creation control: Homestead (Left), Relation Nexus,
 * Toward (Right).
 */
class LinkForgeWidget {
public:
  static constexpr std::uint32_t kTagClaspLeftDrop     = 7010U;
  static constexpr std::uint32_t kTagClaspRightDrop    = 7011U;
  static constexpr std::uint32_t kTagClaspTypeSelector = 7012U;
  static constexpr std::uint32_t kTagClaspTierSelector = 7013U;
  static constexpr std::uint32_t kTagClaspForgeButton  = 7014U;
  static constexpr std::uint32_t kTagClaspClearLeft    = 7015U;
  static constexpr std::uint32_t kTagClaspClearRight   = 7016U;

  LinkForgeWidget();

  void setGeometry(float x, float y, float width, float height) noexcept;
  void draw(gleditor::Canvas &canvas, RenderState &state);
  bool picked(std::uint32_t tag, Session &session,
              std::uint32_t activeDocIndex);

  void dropLeft(PouchItem item);
  void dropRight(PouchItem item);
  void clearLeft() noexcept;
  void clearRight() noexcept;

  void cycleType() noexcept;
  void cycleTier() noexcept;

  void setLinkType(const LinkType type) noexcept { selectedType_ = type; }
  [[nodiscard]] LinkType linkType() const noexcept { return selectedType_; }

  void setProminenceTier(const ProminenceTier tier) noexcept {
    selectedTier_ = tier;
  }
  [[nodiscard]] ProminenceTier prominenceTier() const noexcept {
    return selectedTier_;
  }

  [[nodiscard]] bool canForge() const noexcept {
    return !leftSpans_.empty() && !rightSpans_.empty();
  }
  bool forge(Session &session, std::uint32_t activeDocIndex);

  [[nodiscard]] bool containsLeft(float screenX, float screenY) const noexcept;
  [[nodiscard]] bool containsRight(float screenX, float screenY) const noexcept;

  [[nodiscard]] const std::vector<PouchItem> &leftSpans() const noexcept {
    return leftSpans_;
  }
  [[nodiscard]] const std::vector<PouchItem> &rightSpans() const noexcept {
    return rightSpans_;
  }

  [[nodiscard]] float x() const noexcept { return x_; }
  [[nodiscard]] float y() const noexcept { return y_; }
  [[nodiscard]] float width() const noexcept { return width_; }
  [[nodiscard]] float height() const noexcept { return height_; }

private:
  float x_{0.0F};
  float y_{0.0F};
  float width_{0.0F};
  float height_{0.0F};

  float leftX_{0.0F};
  float leftY_{0.0F};
  float leftW_{0.0F};
  float leftH_{0.0F};

  float rightX_{0.0F};
  float rightY_{0.0F};
  float rightW_{0.0F};
  float rightH_{0.0F};

  LinkType selectedType_{LinkType::Comment};
  ProminenceTier selectedTier_{ProminenceTier::Author};

  std::vector<PouchItem> leftSpans_;
  std::vector<PouchItem> rightSpans_;
};

} // namespace xudu

#endif // XUDU_CLASP_LINK_FORGE_HPP
