#pragma once

namespace gleditor::ui {

/// Logical Canvas coordinates, with Y upwards. Zero extent is an empty box.
struct Rect {
  float left{};
  float bottom{};
  float width{};
  float height{};
};

} // namespace gleditor::ui
