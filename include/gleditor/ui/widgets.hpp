#ifndef GLEDITOR_UI_WIDGETS_HPP
#define GLEDITOR_UI_WIDGETS_HPP

#include <functional>
#include <gleditor/a11y/tree.hpp>
#include <gleditor/text/fit.hpp>
#include <gleditor/text/shaping_cache.hpp>
#include <gleditor/ui/layout.hpp>
#include <memory>
#include <string>
#include <variant>

namespace gleditor::ui {
using WidgetId = std::uint32_t;
enum class TextPurpose : unsigned char {
  Label,
  Identifier,
  Title,
  Description,
  Counter
};
enum class Tone : unsigned char { Normal, Muted, Accent };
struct Label {
  std::string text;
  TextPurpose purpose{TextPurpose::Label};
};
struct Button {
  std::string text, action;
  bool enabled{true};
};
struct ButtonFlow {};
struct Tab {
  WidgetId id{};
  std::string text, action;
  bool enabled{true};
};
struct Tabs {
  std::vector<Tab> tabs;
  std::size_t selected{};
};
struct ListRow {
  WidgetId id{};
  std::string text, action;
  bool enabled{true};
};
struct List {
  std::vector<ListRow> rows;
  float scrollPx{};
  float rowHeightPx{};
  std::size_t overscan{1};
  [[nodiscard]] std::pair<std::size_t, std::size_t>
  visibleRange(float viewportHeight, float resolvedRowHeight) const;
};
struct Card {
  std::string title, description;
};
struct Badge {
  std::string text;
  Tone tone{Tone::Accent};
};
struct Stepper {
  std::string label, action;
  double value{}, minimum{}, maximum{100}, step{1};
  bool change(int direction);
};
struct TextField {
  std::string value, placeholder, action;
  std::size_t caret{};
  float scrollPx{};
  void insert(std::string_view);
  bool key(Key, KeyMods = KeyMods::None);
};
struct Scrubber {
  std::string label, action;
  double value{}, minimum{}, maximum{1};
  double keyboardStep{0.01};
  bool setFraction(double);
  [[nodiscard]] double fraction() const;
};
struct Tooltip {
  std::string text;
};
struct Panel {
  std::string title;
};
struct Modal {
  std::string title;
};
enum class DockSide : unsigned char { Left, Right, Top, Bottom };
struct Dock {
  std::string title;
  DockSide side{DockSide::Left};
};
using WidgetModel =
    std::variant<Label, Button, ButtonFlow, Tabs, List, Card, Badge, Stepper,
                 TextField, Scrubber, Tooltip, Panel, Modal, Dock>;
struct Widget {
  WidgetId id{};
  WidgetModel model{Label{}};
  std::vector<Widget> children;
  /// Zero dimensions choose content size. Positive dimensions are logical
  /// units.
  Size preferred;
  FontRole fontRole{FontRole::Label};
  Tone tone{Tone::Normal};
  std::uint16_t maxLines{3};
  bool defaultAction{};
};
struct WidgetVisual {
  WidgetId id{}, ownerId{};
  std::uint16_t pickingId{};
  std::string text, accessibleLabel, value, action, fontDescription;
  FontRole fontRole{FontRole::Label};
  Tone tone{Tone::Normal};
  text::FittedText fitted;
  text::FontFacePtr font;
  a11y::Role accessibilityRole{a11y::Role::Label};
  bool background{}, selected{}, interactive{}, textInput{};
  int stepDirection{};
  std::size_t itemIndex{};
  float textOffsetPx{};
  double fraction{};
  bool scrubber{};
  std::optional<float> caretOffsetPx;
};
struct WidgetScene {
  LayoutResult layout;
  std::vector<WidgetVisual> visuals;
  std::shared_ptr<const std::vector<WidgetId>> pickingTargets;
  [[nodiscard]] const WidgetVisual *find(WidgetId) const;
  [[nodiscard]] std::optional<WidgetId> resolvePickingId(std::uint32_t) const;
};
/// The same pixel boxes are used by drawing, hit testing, and accessibility.
/// IDs must be nonzero and unique, including tabs and list rows.
[[nodiscard]] WidgetScene layoutWidgets(const Widget &, Rect pixelBounds,
                                        const UiMetrics &, const Theme &,
                                        text::ShapingCache &);
struct WidgetAction {
  WidgetId id{};
  std::string action, value;
  std::size_t itemIndex{};
};
} // namespace gleditor::ui
#endif
