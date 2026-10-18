#pragma once

namespace gleditor::ui {

enum class InputModality : unsigned char {
  Pointer,
  Touch,
};

enum class ButtonShapeKind : unsigned char {
  Pill,
  Micro,
  Chamfered,
  SoftRect,
  Ghost,
};

struct ButtonMetrics {
  float heightCompactPx{28.0F};
  float heightStandardPx{32.0F};
  float heightTouchPx{44.0F};
  float radiusMicroPx{2.5F}; // Architectural micro-radii preserving text space
  float radiusPillShare{0.5F};
  float radiusChamferPx{2.0F};
  float radiusSoftPx{4.0F};
  float paddingHorizontalEm{0.75F};
  float paddingVerticalEm{0.25F};
  float seamWidthPx{1.0F};
  float virtualHitTargetMinPx{36.0F};

  [[nodiscard]] constexpr float height(InputModality modality,
                                       bool compact) const noexcept {
    if (modality == InputModality::Touch) {
      return heightTouchPx;
    }
    return compact ? heightCompactPx : heightStandardPx;
  }

  [[nodiscard]] constexpr float cornerRadius(ButtonShapeKind shape,
                                             float h) const noexcept {
    switch (shape) {
    case ButtonShapeKind::Pill:
      return radiusPillShare * h;
    case ButtonShapeKind::Micro:
      return radiusMicroPx;
    case ButtonShapeKind::Chamfered:
      return radiusChamferPx;
    case ButtonShapeKind::SoftRect:
      return radiusSoftPx;
    case ButtonShapeKind::Ghost:
      return radiusMicroPx;
    }
    return 0.0F;
  }

  constexpr ButtonMetrics &setHeightCompactPx(float val) noexcept {
    heightCompactPx = val;
    return *this;
  }
  constexpr ButtonMetrics &setHeightStandardPx(float val) noexcept {
    heightStandardPx = val;
    return *this;
  }
  constexpr ButtonMetrics &setHeightTouchPx(float val) noexcept {
    heightTouchPx = val;
    return *this;
  }
  constexpr ButtonMetrics &setRadiusMicroPx(float val) noexcept {
    radiusMicroPx = val;
    return *this;
  }
  constexpr ButtonMetrics &setRadiusPillShare(float val) noexcept {
    radiusPillShare = val;
    return *this;
  }
  constexpr ButtonMetrics &setRadiusChamferPx(float val) noexcept {
    radiusChamferPx = val;
    return *this;
  }
  constexpr ButtonMetrics &setRadiusSoftPx(float val) noexcept {
    radiusSoftPx = val;
    return *this;
  }
  constexpr ButtonMetrics &setPaddingHorizontalEm(float val) noexcept {
    paddingHorizontalEm = val;
    return *this;
  }
  constexpr ButtonMetrics &setPaddingVerticalEm(float val) noexcept {
    paddingVerticalEm = val;
    return *this;
  }
  constexpr ButtonMetrics &setSeamWidthPx(float val) noexcept {
    seamWidthPx = val;
    return *this;
  }
  constexpr ButtonMetrics &setVirtualHitTargetMinPx(float val) noexcept {
    virtualHitTargetMinPx = val;
    return *this;
  }

  bool operator==(const ButtonMetrics &) const = default;
};

} // namespace gleditor::ui
