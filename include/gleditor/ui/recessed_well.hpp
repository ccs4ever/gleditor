#pragma once

#include <glm/ext/vector_float4.hpp>

namespace gleditor::ui {

struct RecessedWellStyle {
  glm::vec4 backgroundFill{0.08F, 0.09F, 0.12F, 1.0F};
  glm::vec4 borderCrease{0.0F, 0.0F, 0.0F, 0.55F};
  glm::vec4 borderRim{0.30F, 0.33F, 0.38F, 0.45F};
  glm::vec4 focusAura{1.00F, 0.84F, 0.00F, 0.20F};
  float cornerRadiusPx{3.0F};
  float creaseThicknessPx{1.5F};

  constexpr RecessedWellStyle &
  setBackgroundFill(const glm::vec4 &val) noexcept {
    backgroundFill = val;
    return *this;
  }
  constexpr RecessedWellStyle &setBorderCrease(const glm::vec4 &val) noexcept {
    borderCrease = val;
    return *this;
  }
  constexpr RecessedWellStyle &setBorderRim(const glm::vec4 &val) noexcept {
    borderRim = val;
    return *this;
  }
  constexpr RecessedWellStyle &setFocusAura(const glm::vec4 &val) noexcept {
    focusAura = val;
    return *this;
  }
  constexpr RecessedWellStyle &setCornerRadiusPx(float val) noexcept {
    cornerRadiusPx = val;
    return *this;
  }
  constexpr RecessedWellStyle &setCreaseThicknessPx(float val) noexcept {
    creaseThicknessPx = val;
    return *this;
  }

  bool operator==(const RecessedWellStyle &) const = default;
};

} // namespace gleditor::ui
