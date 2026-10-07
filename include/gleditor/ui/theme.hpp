#ifndef GLEDITOR_UI_THEME_HPP
#define GLEDITOR_UI_THEME_HPP

#include <array>
#include <cstddef>
#include <gleditor/color.hpp>
#include <glm/ext/vector_float4.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace gleditor::ui {
[[nodiscard]] inline std::uint32_t rgba(const glm::vec4 &colour) {
  return color::packRgba(colour.r, colour.g, colour.b, colour.a);
}
inline constexpr float kSafeMarginShare = 0.05F;

enum class FontRole : unsigned char { Caption, Label, Body, Title, Mono };
inline constexpr std::size_t kFontRoleCount =
    static_cast<std::size_t>(FontRole::Mono) + 1;
inline constexpr std::array<std::string_view, kFontRoleCount> kFontRoleNames{
    "caption", "label", "body", "title", "mono"};
struct FontSpec {
  std::string family{"Sans"};
  float points{12.0F};
  bool operator==(const FontSpec &) const = default;
};
struct TypeScale {
  float minFontPx{9.0F};
  float minTouchPx{44.0F};
  float lineGapEm{0.25F};
  bool operator==(const TypeScale &) const = default;
};
struct ThemeColours {
  glm::vec4 surface{0.12F, 0.14F, 0.18F, 1.0F};
  std::optional<glm::vec4> buttonSurface;
  glm::vec4 text{0.95F, 0.95F, 0.96F, 1.0F};
  glm::vec4 muted{0.65F, 0.68F, 0.72F, 1.0F};
  glm::vec4 accent{0.25F, 0.7F, 0.95F, 1.0F};
  glm::vec4 border{0.3F, 0.33F, 0.38F, 1.0F};
  glm::vec4 disabled{0.42F, 0.44F, 0.48F, 1.0F};
  bool operator==(const ThemeColours &) const = default;
};
struct Theme {
  std::array<FontSpec, kFontRoleCount> fonts{{{"Sans", 10.0F},
                                              {"Sans", 12.0F},
                                              {"Sans", 12.0F},
                                              {"Sans Bold", 18.0F},
                                              {"Monospace", 12.0F}}};
  ThemeColours colours;
  TypeScale type;
  float paddingEm{0.75F};
  float gapEm{0.5F};
  [[nodiscard]] const FontSpec &font(FontRole role) const {
    return fonts.at(static_cast<std::size_t>(role));
  }
  bool operator==(const Theme &) const = default;
};
[[nodiscard]] inline const Theme &defaultTheme() {
  static const Theme theme;
  return theme;
}
} // namespace gleditor::ui
#endif
