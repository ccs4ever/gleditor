/**
 * @file readable_scale.hpp
 * @brief How much a slice embedded beside a page is enlarged so its text can
 *        be read, shared by every slice presentation.
 */
#ifndef COMMON_UI_READABLE_SCALE_HPP
#define COMMON_UI_READABLE_SCALE_HPP

#include <algorithm>
#include <cmath>

namespace xanadu::ui {

/// Quarter steps, so a zoom re-lays a presentation a few times rather than
/// every frame.
inline constexpr float kReadableScaleSteps = 4.0F;

/**
 * @brief The enlargement that brings a line of text drawn @p lineOnScreenPx
 *        tall up to @p wantedPx: never below 1, in quarter steps.
 */
[[nodiscard]] inline float readableScaleFor(const float lineOnScreenPx,
                                            const float wantedPx) noexcept {
  if (lineOnScreenPx <= 0.0F || wantedPx <= 0.0F) {
    return 1.0F;
  }
  return std::max(1.0F,
                  std::ceil(wantedPx / lineOnScreenPx * kReadableScaleSteps) /
                      kReadableScaleSteps);
}

} // namespace xanadu::ui

#endif // COMMON_UI_READABLE_SCALE_HPP
