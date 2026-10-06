#pragma once

#include <cmath>
#include <cstdint>

namespace gleditor::ui {

/// Logical text/container bounds in Canvas coordinates: pixels, Y upwards.
/// Diagnostic metadata only; setting these neither clips nor fits text.
struct TextBounds {
  float left{};
  float bottom{};
  float width{};
  float height{};

  [[nodiscard]] bool contains(const TextBounds &child) const {
    const auto valid = [](const TextBounds &box) {
      return std::isfinite(box.left) && std::isfinite(box.bottom) &&
             std::isfinite(box.width) && std::isfinite(box.height) &&
             box.width >= 0.0F && box.height >= 0.0F;
    };
    return valid(*this) && valid(child) && child.left >= left &&
           child.bottom >= bottom && child.left + child.width <= left + width &&
           child.bottom + child.height <= bottom + height;
  }
};

/// Reports geometry and picking identity through ui.layout at debug level.
/// No source text, font description or user content enters the diagnostic.
/// Returns true when the logical run lies outside its declared parent.
bool reportTextOverflow(const TextBounds &run, const TextBounds &parent,
                        std::uint32_t tagKind, std::uint32_t tagIndex);

} // namespace gleditor::ui
