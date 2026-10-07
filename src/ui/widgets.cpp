#include <gleditor/ui/widgets.hpp>

#include <algorithm>
#include <cmath>
#include <fribidi.h>
#include <graphemebreak.h>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace gleditor::ui {
namespace {
std::vector<char> graphemes(const std::string &text) {
  static const bool initialized = [] {
    init_graphemebreak();
    return true;
  }();
  static_cast<void>(initialized);
  std::vector<char> boundaries(text.size());
  if (!text.empty())
    set_graphemebreaks_utf8(reinterpret_cast<const utf8_t *>(text.data()),
                            text.size(), nullptr, boundaries.data());
  return boundaries;
}
bool boundary(const std::vector<char> &breaks, std::size_t at) {
  return at == 0 || at == breaks.size() ||
         breaks[at - 1] == GRAPHEMEBREAK_BREAK;
}
std::size_t previous(const std::string &text, std::size_t at) {
  const auto breaks = graphemes(text);
  at                = std::min(at, text.size());
  if (at == 0) return 0;
  do {
    --at;
  } while (!boundary(breaks, at));
  return at;
}
std::size_t next(const std::string &text, std::size_t at) {
  const auto breaks = graphemes(text);
  at                = std::min(at, text.size());
  if (at == text.size()) return at;
  do {
    ++at;
  } while (!boundary(breaks, at));
  return at;
}
std::size_t aligned(const std::string &text, std::size_t at) {
  const auto breaks = graphemes(text);
  at                = std::min(at, text.size());
  while (!boundary(breaks, at)) --at;
  return at;
}
bool validUtf8(std::string_view value) {
  std::size_t i = 0;
  while (i < value.size()) {
    const auto lead       = static_cast<unsigned char>(value[i++]);
    unsigned continuation = 0;
    std::uint32_t scalar = 0, minimum = 0;
    if (lead < 0x80U) continue;
    if (lead >= 0xC2U && lead <= 0xDFU) {
      continuation = 1;
      scalar       = lead & 0x1FU;
      minimum      = 0x80U;
    } else if (lead >= 0xE0U && lead <= 0xEFU) {
      continuation = 2;
      scalar       = lead & 0xFU;
      minimum      = 0x800U;
    } else if (lead >= 0xF0U && lead <= 0xF4U) {
      continuation = 3;
      scalar       = lead & 7U;
      minimum      = 0x10000U;
    } else
      return false;
    for (unsigned n = 0; n < continuation; ++n) {
      if (i == value.size()) return false;
      const auto byte = static_cast<unsigned char>(value[i++]);
      if ((byte & 0xC0U) != 0x80U) return false;
      scalar = (scalar << 6U) | (byte & 0x3FU);
    }
    if (scalar < minimum || scalar > 0x10FFFFU ||
        (scalar >= 0xD800U && scalar <= 0xDFFFU))
      return false;
  }
  return true;
}
float caretPosition(const text::FittedText &fitted, const std::string &source,
                    std::size_t caret) {
  if (source.empty() || fitted.shaping.glyphs.empty()) return 0;
  std::vector<FriBidiChar> characters;
  std::vector<std::size_t> offsets;
  for (std::size_t i = 0; i < source.size();) {
    offsets.push_back(i);
    const auto lead    = static_cast<unsigned char>(source[i++]);
    FriBidiChar scalar = lead;
    unsigned extra     = 0;
    if (lead >= 0xF0U) {
      scalar = lead & 7U;
      extra  = 3;
    } else if (lead >= 0xE0U) {
      scalar = lead & 15U;
      extra  = 2;
    } else if (lead >= 0xC0U) {
      scalar = lead & 31U;
      extra  = 1;
    }
    while (extra-- > 0)
      scalar = (scalar << 6U) | (static_cast<unsigned char>(source[i++]) & 63U);
    characters.push_back(scalar);
  }
  const auto count = static_cast<FriBidiStrIndex>(characters.size());
  std::vector<FriBidiCharType> types(characters.size());
  std::vector<FriBidiBracketType> brackets(characters.size());
  std::vector<FriBidiLevel> levels(characters.size());
  fribidi_get_bidi_types(characters.data(), count, types.data());
  fribidi_get_bracket_types(characters.data(), count, types.data(),
                            brackets.data());
  FriBidiParType direction = FRIBIDI_PAR_ON;
  if (!fribidi_get_par_embedding_levels_ex(types.data(), brackets.data(), count,
                                           &direction, levels.data()))
    throw std::runtime_error("Cannot resolve text field direction");
  const auto &glyphs = fitted.shaping.glyphs;
  // At a direction boundary, use the leading edge of the following source
  // cluster. Interior ligature carets divide its advance by source characters.
  for (bool includeTrailing : {false, true}) {
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
      const auto &glyph   = glyphs[i];
      const auto &cluster = fitted.shaping.clusters[glyph.clusterIndex];
      const auto begin    = static_cast<std::size_t>(cluster.byteStart);
      const auto end      = begin + cluster.byteLength;
      if (caret < begin || caret > end || (!includeTrailing && caret == end))
        continue;
      const auto first =
          std::lower_bound(offsets.begin(), offsets.end(), begin);
      const auto characterIndex =
          static_cast<std::size_t>(first - offsets.begin());
      const auto before = static_cast<std::size_t>(
          std::lower_bound(first, offsets.end(), caret) - first);
      const auto width = i + 1 < glyphs.size()
                             ? glyphs[i + 1].clusterLeft - glyph.clusterLeft
                             : fitted.widthPx - glyph.clusterLeft;
      const auto share =
          cluster.charCount
              ? std::clamp(static_cast<float>(before) /
                               static_cast<float>(cluster.charCount),
                           0.0F, 1.0F)
              : 0;
      const bool rtl =
          characterIndex < levels.size() && (levels[characterIndex] & 1U);
      return glyph.clusterLeft + width * (rtl ? 1 - share : share);
    }
  }
  return 0;
}
void validateRange(double minimum, double maximum, double value) {
  if (!std::isfinite(minimum) || !std::isfinite(maximum) ||
      !std::isfinite(value) || minimum > maximum)
    throw std::invalid_argument("Invalid widget numeric range");
}
} // namespace
std::pair<std::size_t, std::size_t> List::visibleRange(float viewport,
                                                       float height) const {
  if (!std::isfinite(viewport) || !std::isfinite(height) ||
      !std::isfinite(scrollPx) || viewport < 0 || height <= 0)
    throw std::invalid_argument("Invalid list viewport");
  if (viewport == 0 || rows.empty()) return {0, 0};
  const auto maxScroll =
      std::max(0.0, static_cast<double>(rows.size()) * height - viewport);
  const auto offset = std::clamp(static_cast<double>(scrollPx), 0.0, maxScroll);
  const auto first  = static_cast<std::size_t>(std::floor(offset / height));
  const auto last   = std::min(rows.size(), static_cast<std::size_t>(std::ceil(
                                              (offset + viewport) / height)));
  return {first > overscan ? first - overscan : 0,
          overscan > rows.size() - last ? rows.size() : last + overscan};
}
bool Stepper::change(int direction) {
  validateRange(minimum, maximum, value);
  if (!std::isfinite(step) || step <= 0)
    throw std::invalid_argument("Invalid step");
  const auto old = value;
  value          = std::clamp(value + (direction < 0   ? -step
                                       : direction > 0 ? step
                                                       : 0),
                              minimum, maximum);
  return old != value;
}
void TextField::insert(std::string_view text) {
  if (!validUtf8(value) || !validUtf8(text))
    throw std::invalid_argument("Text input must be UTF-8");
  caret = aligned(value, caret);
  value.insert(caret, text);
  caret += text.size();
}
bool TextField::key(Key key, KeyMods) {
  if (!validUtf8(value))
    throw std::invalid_argument("Text input must be UTF-8");
  caret = aligned(value, caret);
  switch (key) {
  case Key::Left:
    caret = previous(value, caret);
    return true;
  case Key::Right:
    caret = next(value, caret);
    return true;
  case Key::Home:
    caret = 0;
    return true;
  case Key::End:
    caret = value.size();
    return true;
  case Key::Backspace: {
    const auto from = previous(value, caret);
    value.erase(from, caret - from);
    caret = from;
    return true;
  }
  case Key::Delete:
    value.erase(caret, next(value, caret) - caret);
    return true;
  default:
    return false;
  }
}
double Scrubber::fraction() const {
  validateRange(minimum, maximum, value);
  if (minimum == maximum) return 0;
  const auto clamped = std::clamp(value, minimum, maximum);
  const auto extent  = maximum - minimum;
  if (std::isfinite(extent)) return (clamped - minimum) / extent;
  const auto scale = std::max(std::abs(minimum), std::abs(maximum));
  return (clamped / scale - minimum / scale) /
         (maximum / scale - minimum / scale);
}
bool Scrubber::setFraction(double amount) {
  validateRange(minimum, maximum, value);
  if (!std::isfinite(amount))
    throw std::invalid_argument("Invalid scrubber fraction");
  const auto old = value;
  value          = std::lerp(minimum, maximum, std::clamp(amount, 0.0, 1.0));
  return old != value;
}
const WidgetVisual *WidgetScene::find(WidgetId id) const {
  const auto it = std::ranges::find(visuals, id, &WidgetVisual::id);
  return it == visuals.end() ? nullptr : &*it;
}
std::optional<WidgetId>
WidgetScene::resolvePickingId(std::uint32_t pickingId) const {
  if (!pickingTargets || pickingId == 0 || pickingId > pickingTargets->size())
    return std::nullopt;
  return (*pickingTargets)[pickingId - 1];
}
namespace {
class WidgetBuilder {
public:
  WidgetBuilder(const Widget &root, const UiMetrics &metrics,
                const Theme &theme, text::ShapingCache &cache)
      : metrics_(metrics), theme_(theme), cache_(cache) {
    collect(root);
  }
  WidgetScene build(const Widget &root, Rect bounds) {
    scene_.layout.bounds = bounds;
    const auto size      = natural(root, bounds.width);
    if (root.preferred.width > 0)
      bounds.width = std::min(bounds.width, metrics_.px(root.preferred.width));
    if (root.preferred.height > 0)
      bounds.height =
          std::min(bounds.height, metrics_.px(root.preferred.height));
    if (std::holds_alternative<Modal>(root.model)) {
      const auto safe = scene_.layout.bounds;
      bounds.left     = safe.left + (safe.width - bounds.width) * 0.5F;
      bounds.bottom   = safe.bottom + (safe.height - bounds.height) * 0.5F;
    } else if (const auto *dock = std::get_if<Dock>(&root.model)) {
      if (dock->side == DockSide::Right)
        bounds.left += scene_.layout.bounds.width - bounds.width;
      if (dock->side == DockSide::Top)
        bounds.bottom += scene_.layout.bounds.height - bounds.height;
    } else if (!container(root) && root.preferred.height <= 0) {
      const auto top = bounds.bottom + bounds.height;
      bounds.height  = std::min(bounds.height, size.height);
      bounds.bottom  = top - bounds.height;
    }
    place(root, clampToSafeArea(metrics_.rounded(bounds), scene_.layout.bounds),
          0);
    auto targets = std::make_shared<std::vector<WidgetId>>();
    targets->reserve(scene_.visuals.size());
    for (const auto &visual : scene_.visuals) targets->push_back(visual.id);
    scene_.pickingTargets = std::move(targets);
    return std::move(scene_);
  }

private:
  const UiMetrics &metrics_;
  const Theme &theme_;
  text::ShapingCache &cache_;
  WidgetScene scene_;
  std::unordered_set<WidgetId> ids_;
  WidgetId largest_{};
  void reserve(WidgetId id) {
    if (id == 0 || !ids_.insert(id).second)
      throw std::invalid_argument("Widget IDs must be nonzero and unique");
    largest_ = std::max(largest_, id);
  }
  void collect(const Widget &widget) {
    reserve(widget.id);
    if (!std::isfinite(widget.preferred.width) ||
        !std::isfinite(widget.preferred.height) || widget.preferred.width < 0 ||
        widget.preferred.height < 0)
      throw std::invalid_argument("Invalid widget preferred size");
    if (const auto *tabs = std::get_if<Tabs>(&widget.model))
      for (const auto &tab : tabs->tabs) reserve(tab.id);
    if (const auto *list = std::get_if<List>(&widget.model))
      for (const auto &row : list->rows) reserve(row.id);
    for (const auto &child : widget.children) collect(child);
  }
  WidgetId generated() {
    if (largest_ == std::numeric_limits<WidgetId>::max())
      throw std::length_error("Widget ID space exhausted");
    return ++largest_;
  }
  static bool container(const Widget &w) {
    return std::holds_alternative<PositionedPanel>(w.model) ||
           std::holds_alternative<Panel>(w.model) ||
           std::holds_alternative<Modal>(w.model) ||
           std::holds_alternative<Dock>(w.model) ||
           std::holds_alternative<ButtonFlow>(w.model);
  }
  text::FontFacePtr font(FontRole role) {
    return text::FontManager::instance().getFont(
        metrics_.fontDescription(role, theme_));
  }
  float pad(FontRole role) {
    return font(role)->metrics().lineHeight * theme_.paddingEm;
  }
  float gap() {
    return font(FontRole::Label)->metrics().lineHeight * theme_.gapEm;
  }
  float touch() const {
    return std::max(theme_.type.minTouchPx,
                    metrics_.px(theme_.type.minTouchPx));
  }
  float line(FontRole role) {
    return font(role)->metrics().lineHeight * (1 + theme_.type.lineGapEm);
  }
  Size measure(std::string_view text, FontRole role) {
    const auto &fitted = cache_.fitted(text, font(role), {});
    return {fitted.widthPx, fitted.heightPx};
  }
  Size natural(const Widget &w, float width) {
    const auto p = pad(w.fontRole);
    Size size{width, line(w.fontRole) + p * 2};
    if (const auto *label = std::get_if<Label>(&w.model)) {
      size = measure(label->text, w.fontRole);
      if (label->purpose == TextPurpose::Title)
        size.height = line(FontRole::Title) * 2;
      if (label->purpose == TextPurpose::Description)
        size.height = line(w.fontRole) * w.maxLines;
    } else if (const auto *button = std::get_if<Button>(&w.model)) {
      size = measure(button->text, w.fontRole);
      size.width += 2 * p;
      size.height = std::max(touch(), size.height + 2 * p);
    } else if (const auto *badge = std::get_if<Badge>(&w.model)) {
      size = measure(badge->text, FontRole::Caption);
      size.width += 2 * p;
      size.height += p;
    } else if (std::holds_alternative<Card>(w.model))
      size.height =
          line(FontRole::Title) * 2 + line(FontRole::Body) * w.maxLines + 3 * p;
    else if (std::holds_alternative<Tooltip>(w.model))
      size.height = line(FontRole::Caption) * w.maxLines + 2 * p;
    else if (const auto *list = std::get_if<List>(&w.model))
      size.height = std::min(static_cast<float>(list->rows.size()),
                             static_cast<float>(w.maxLines)) *
                    rowHeight(w, *list);
    else if (const auto *positioned = std::get_if<PositionedPanel>(&w.model)) {
      if (positioned->childBounds.size() != w.children.size())
        throw std::invalid_argument(
            "Positioned panel bounds must match children");
      size.height = 2 * p;
      for (const auto &bounds : positioned->childBounds)
        size.height = std::max(
            size.height, metrics_.px(bounds.bottom + bounds.height) + 2 * p);
    } else if (container(w)) {
      size.height    = p * 2;
      float rowWidth = 0, rowHeight = 0;
      for (const auto &child : w.children) {
        const auto childSize = natural(child, std::max(0.0F, width - p * 2));
        if (std::holds_alternative<ButtonFlow>(w.model)) {
          if (rowWidth > 0 &&
              rowWidth + gap() + childSize.width > width - p * 2) {
            size.height += rowHeight + gap();
            rowWidth  = 0;
            rowHeight = 0;
          }
          rowWidth += (rowWidth > 0 ? gap() : 0) + childSize.width;
          rowHeight = std::max(rowHeight, childSize.height);
        } else
          size.height += childSize.height + gap();
      }
      size.height += rowHeight;
      if (!heading(w).empty()) size.height += line(FontRole::Title) * 2 + gap();
    }
    if (w.preferred.width > 0) size.width = metrics_.px(w.preferred.width);
    if (w.preferred.height > 0) size.height = metrics_.px(w.preferred.height);
    return size;
  }
  static std::string heading(const Widget &w) {
    if (const auto *p = std::get_if<Panel>(&w.model)) return p->title;
    if (const auto *p = std::get_if<Modal>(&w.model)) return p->title;
    if (const auto *p = std::get_if<Dock>(&w.model)) return p->title;
    return {};
  }
  float rowHeight(const Widget &w, const List &list) {
    if (!std::isfinite(list.rowHeightPx) || list.rowHeightPx < 0)
      throw std::invalid_argument("Invalid list row height");
    return list.rowHeightPx > 0
               ? list.rowHeightPx
               : std::max(touch(), line(w.fontRole) + pad(w.fontRole));
  }
  LayoutBox box(WidgetId id, WidgetId parent, Rect rect, float padding,
                bool focusable, bool enabled) {
    const auto original = rect;
    rect                = clampToSafeArea(metrics_.rounded(rect), original);
    const auto p        = std::max(
        0.0F, std::min({padding, rect.width * .5F, rect.height * .5F}));
    LayoutBox result{id,
                     parent,
                     rect,
                     metrics_.rounded({rect.left + p, rect.bottom + p,
                                       std::max(0.0F, rect.width - 2 * p),
                                       std::max(0.0F, rect.height - 2 * p)}),
                     focusable,
                     enabled};
    result.contentRect = clampToSafeArea(result.contentRect, rect);
    scene_.layout.boxes.push_back(result);
    if (focusable && enabled && rect.width > 0 && rect.height > 0)
      scene_.layout.focusOrder.push_back(id);
    return result;
  }
  void visual(const Widget &w, LayoutBox geometry, std::string text,
              TextPurpose purpose, a11y::Role role, bool background = false,
              std::string action = {}, WidgetId owner = 0) {
    if (scene_.visuals.size() >= std::numeric_limits<std::uint16_t>::max())
      throw std::length_error("Widget scene exceeds 65535 picking targets");
    WidgetVisual item;
    item.pickingId = static_cast<std::uint16_t>(scene_.visuals.size() + 1);
    item.id        = geometry.id;
    item.ownerId   = owner ? owner : w.id;
    item.text      = text;
    item.accessibleLabel = text;
    item.action          = std::move(action);
    item.fontRole        = w.fontRole;
    item.tone            = w.tone;
    if (purpose == TextPurpose::Title) item.fontRole = FontRole::Title;
    if (std::holds_alternative<Badge>(w.model) ||
        std::holds_alternative<Tooltip>(w.model))
      item.fontRole = FontRole::Caption;
    item.fontDescription   = metrics_.fontDescription(item.fontRole, theme_);
    item.font              = font(item.fontRole);
    item.background        = background;
    item.accessibilityRole = role;
    item.interactive       = geometry.focusable;
    text::TextFit constraints{.maxWidthPx  = geometry.contentRect.width,
                              .maxHeightPx = geometry.contentRect.height};
    if (purpose == TextPurpose::Counter) {
      constraints.maxWidthPx = 0;
      constraints.overflow   = text::Overflow::Clip;
    }
    if (purpose == TextPurpose::Identifier)
      constraints.at = text::EllipsisAt::Middle;
    if (purpose == TextPurpose::Title || purpose == TextPurpose::Description) {
      constraints.overflow = text::Overflow::Wrap;
      constraints.maxLines = purpose == TextPurpose::Title ? 2 : w.maxLines;
    }
    if (geometry.contentRect.width > 0 && geometry.contentRect.height > 0 &&
        !text.empty())
      item.fitted = cache_.fitted(text, item.font, constraints);
    scene_.visuals.push_back(std::move(item));
  }
  void place(const Widget &w, Rect rect, WidgetId parent) {
    const bool button       = std::holds_alternative<Button>(w.model);
    const bool field        = std::holds_alternative<TextField>(w.model);
    const bool scrubber     = std::holds_alternative<Scrubber>(w.model);
    const auto *buttonModel = std::get_if<Button>(&w.model);
    const bool enabled      = !buttonModel || buttonModel->enabled;
    const auto p = std::holds_alternative<Label>(w.model) ? 0 : pad(w.fontRole);
    auto geometry =
        box(w.id, parent, rect, p, button || field || scrubber, enabled);
    scene_.layout.boxes.back().textInput     = field;
    scene_.layout.boxes.back().defaultAction = w.defaultAction;
    if (parent && button) scene_.layout.boxes.back().focusGroup = parent;

    if (const auto *label = std::get_if<Label>(&w.model))
      visual(w, geometry, label->text, label->purpose, a11y::Role::Label);
    else if (buttonModel) {
      visual(w, geometry, buttonModel->text, TextPurpose::Label,
             a11y::Role::Button, true, buttonModel->action);
      if (!buttonModel->accessibleLabel.empty())
        scene_.visuals.back().accessibleLabel = buttonModel->accessibleLabel;
    } else if (const auto *badge = std::get_if<Badge>(&w.model)) {
      visual(w, geometry, badge->text, TextPurpose::Label, a11y::Role::Label,
             true);
      scene_.visuals.back().tone = badge->tone;
    } else if (const auto *tooltip = std::get_if<Tooltip>(&w.model))
      visual(w, geometry, tooltip->text, TextPurpose::Description,
             a11y::Role::Label, true);
    else if (const auto *input = std::get_if<TextField>(&w.model)) {
      if (!validUtf8(input->value))
        throw std::invalid_argument("Text field must contain UTF-8");
      visual(w, geometry,
             input->value.empty() ? input->placeholder : input->value,
             TextPurpose::Label, a11y::Role::TextInput, true, input->action);
      auto &item           = scene_.visuals.back();
      item.textInput       = true;
      item.value           = input->value;
      item.accessibleLabel = input->placeholder;
      const auto source =
          input->value.empty() ? input->placeholder : input->value;
      item.fitted = cache_.fitted(
          source, item.font, text::TextFit{.overflow = text::Overflow::Clip});
      const auto caret  = aligned(input->value, input->caret);
      const auto prefix = caretPosition(item.fitted, input->value, caret);
      const auto maxScroll =
          std::max(0.0F, item.fitted.widthPx - geometry.contentRect.width);
      item.textOffsetPx = std::clamp(input->scrollPx, 0.0F, maxScroll);
      if (prefix < item.textOffsetPx) item.textOffsetPx = prefix;
      if (prefix > item.textOffsetPx + geometry.contentRect.width)
        item.textOffsetPx = prefix - geometry.contentRect.width;
      item.caretOffsetPx = std::clamp(prefix - item.textOffsetPx, 0.0F,
                                      geometry.contentRect.width);
    } else if (const auto *control = std::get_if<Scrubber>(&w.model)) {
      visual(w, geometry, control->label, TextPurpose::Label, a11y::Role::Group,
             true, control->action);
      scene_.visuals.back().fraction = control->fraction();
      scene_.visuals.back().scrubber = true;
      scene_.visuals.back().value    = std::to_string(control->value);
    } else if (const auto *step = std::get_if<Stepper>(&w.model)) {
      validateRange(step->minimum, step->maximum, step->value);
      visual(w, geometry, {}, TextPurpose::Label, a11y::Role::Group, true);
      scene_.visuals.back().accessibleLabel = step->label;
      const auto labelId = generated(), minusId = generated();
      const auto valueId = generated(), plusId = generated();
      const auto valueWidth =
          measure(std::to_string(step->value), FontRole::Mono).width;
      const std::array<LayoutItem, 4> items{
          {{.id        = labelId,
            .intrinsic = {measure(step->label, FontRole::Label).width, touch()},
            .grow      = 1},
           {.id        = minusId,
            .intrinsic = {touch(), touch()},
            .focusable = true,
            .enabled   = step->value > step->minimum,
            .paddingPx = p},
           {.id        = valueId,
            .intrinsic = {valueWidth, touch()},
            .minimum   = {valueWidth, 0}},
           {.id        = plusId,
            .intrinsic = {touch(), touch()},
            .focusable = true,
            .enabled   = step->value < step->maximum,
            .paddingPx = p}}};
      const auto placed = stack(geometry.contentRect, items,
                                {.axis     = Axis::Horizontal,
                                 .gap      = gap(),
                                 .align    = Align::Stretch,
                                 .parentId = w.id});
      scene_.layout.append(placed);
      for (auto &entry : scene_.layout.boxes)
        if (entry.parentId == w.id) entry.focusGroup = w.id;
      for (std::size_t i = 0; i < 4; ++i) {
        auto copy       = w;
        copy.fontRole   = i == 2 ? FontRole::Mono : FontRole::Label;
        const auto text = i == 0   ? step->label
                          : i == 1 ? "−"
                          : i == 3 ? "+"
                                   : std::to_string(step->value);
        visual(copy, placed.boxes[i], text,
               i == 2 ? TextPurpose::Counter : TextPurpose::Label,
               i == 1 || i == 3 ? a11y::Role::Button : a11y::Role::Label,
               i == 1 || i == 3, step->action);
        scene_.visuals.back().stepDirection = i == 1 ? -1 : i == 3 ? 1 : 0;
      }
    } else if (const auto *tabs = std::get_if<Tabs>(&w.model)) {
      visual(w, geometry, {}, TextPurpose::Label, a11y::Role::Group);
      std::vector<LayoutItem> items;
      for (const auto &tab : tabs->tabs) {
        auto size = measure(tab.text, w.fontRole);
        size.width += 2 * p;
        size.height = touch();
        items.push_back({.id        = tab.id,
                         .intrinsic = size,
                         .grow      = 1,
                         .focusable = true,
                         .enabled   = tab.enabled,
                         .paddingPx = p});
      }
      auto placed = stack(geometry.contentRect, items,
                          {.axis     = Axis::Horizontal,
                           .align    = Align::Stretch,
                           .parentId = w.id});
      scene_.layout.append(placed);
      for (auto &entry : scene_.layout.boxes)
        if (entry.parentId == w.id) entry.focusGroup = w.id;
      for (std::size_t i = 0; i < placed.boxes.size(); ++i) {
        visual(w, placed.boxes[i], tabs->tabs[i].text, TextPurpose::Label,
               a11y::Role::Button, true, tabs->tabs[i].action);
        scene_.visuals.back().selected  = i == tabs->selected;
        scene_.visuals.back().itemIndex = i;
      }
    } else if (const auto *list = std::get_if<List>(&w.model)) {
      visual(w, geometry, {}, TextPurpose::Label, a11y::Role::List);
      const auto height = rowHeight(w, *list);
      const auto [first, last] =
          list->visibleRange(geometry.contentRect.height, height);
      const auto offset = std::clamp(
          list->scrollPx, 0.0F,
          std::max(0.0F, static_cast<float>(list->rows.size()) * height -
                             geometry.contentRect.height));
      for (auto i = first; i < last; ++i) {
        Rect row{geometry.contentRect.left,
                 geometry.contentRect.bottom + geometry.contentRect.height -
                     (static_cast<float>(i) + 1) * height + offset,
                 geometry.contentRect.width, height};
        const auto bottom = std::max(row.bottom, geometry.contentRect.bottom);
        const auto top =
            std::min(row.bottom + row.height,
                     geometry.contentRect.bottom + geometry.contentRect.height);
        if (top <= bottom) continue;
        // Hide unreadable edge rows when the viewport can fit a text line.
        // Tiny viewports retain row bounds for visible selection feedback.
        const auto textHeight = font(w.fontRole)->metrics().lineHeight + p;
        if (top - bottom < textHeight &&
            geometry.contentRect.height >= textHeight)
          continue;
        row.bottom       = bottom;
        row.height       = top - bottom;
        const auto entry = box(list->rows[i].id, w.id, row, p * .5F, true,
                               list->rows[i].enabled);
        scene_.layout.boxes.back().focusGroup = w.id;
        visual(w, entry, list->rows[i].text, list->rows[i].purpose,
               a11y::Role::ListItem, true, list->rows[i].action);
        scene_.visuals.back().itemIndex = i;
      }
    } else if (const auto *card = std::get_if<Card>(&w.model)) {
      visual(w, geometry, {}, TextPurpose::Label, a11y::Role::Group, true);
      scene_.visuals.back().accessibleLabel = card->title;
      scene_.visuals.back().value           = card->description;
      const auto titleId = generated(), descriptionId = generated();
      const std::array<LayoutItem, 2> items{
          {{.id        = titleId,
            .intrinsic = {geometry.contentRect.width,
                          line(FontRole::Title) * 2}},
           {.id        = descriptionId,
            .intrinsic = {geometry.contentRect.width,
                          line(FontRole::Body) * w.maxLines},
            .grow      = 1}}};
      auto placed =
          stack(geometry.contentRect, items, {.gap = gap(), .parentId = w.id});
      scene_.layout.append(placed);
      for (auto &entry : scene_.layout.boxes)
        if (entry.parentId == w.id) entry.focusGroup = w.id;
      visual(w, placed.boxes[0], card->title, TextPurpose::Title,
             a11y::Role::Label);
      auto copy     = w;
      copy.fontRole = FontRole::Body;
      visual(copy, placed.boxes[1], card->description, TextPurpose::Description,
             a11y::Role::Label);
    } else if (const auto *positioned =
                   std::get_if<PositionedPanel>(&w.model)) {
      if (positioned->childBounds.size() != w.children.size())
        throw std::invalid_argument(
            "Positioned panel bounds must match children");
      visual(w, geometry, {}, TextPurpose::Label, a11y::Role::Group);
      for (std::size_t i = 0; i < w.children.size(); ++i) {
        const auto &bounds = positioned->childBounds[i];
        if (!std::isfinite(bounds.left) || !std::isfinite(bounds.bottom) ||
            !std::isfinite(bounds.width) || !std::isfinite(bounds.height) ||
            bounds.width < 0 || bounds.height < 0)
          throw std::invalid_argument("Invalid positioned child bounds");
        const auto &area = geometry.contentRect;
        const Rect proposed{area.left + metrics_.px(bounds.left),
                            area.bottom + metrics_.px(bounds.bottom),
                            metrics_.px(bounds.width),
                            metrics_.px(bounds.height)};
        const auto left   = std::max(area.left, proposed.left);
        const auto bottom = std::max(area.bottom, proposed.bottom);
        const auto right =
            std::min(area.left + area.width, proposed.left + proposed.width);
        const auto top = std::min(area.bottom + area.height,
                                  proposed.bottom + proposed.height);
        if (right <= left || top <= bottom) continue;
        place(w.children[i], {left, bottom, right - left, top - bottom}, w.id);
      }
    } else if (container(w)) {
      const auto title = heading(w);
      visual(w, geometry, title, TextPurpose::Label, a11y::Role::Group,
             !std::holds_alternative<ButtonFlow>(w.model));
      // The container's accessible name is drawn by its dedicated heading box.
      scene_.visuals.back().text.clear();
      scene_.visuals.back().fitted = {};
      std::vector<LayoutItem> items;
      WidgetId titleId = 0;
      if (!title.empty()) {
        titleId = generated();
        items.push_back({.id        = titleId,
                         .intrinsic = {geometry.contentRect.width,
                                       line(FontRole::Title) * 2}});
      }
      for (const auto &child : w.children)
        items.push_back(
            {.id        = child.id,
             .intrinsic = natural(child, geometry.contentRect.width)});
      auto placed =
          std::holds_alternative<ButtonFlow>(w.model)
              ? flow(geometry.contentRect, items,
                     {.gap = gap(), .lineGap = gap(), .parentId = w.id})
              : stack(geometry.contentRect, items,
                      {.gap = gap(), .parentId = w.id});
      std::size_t index = 0;
      if (titleId) {
        scene_.layout.boxes.push_back(placed.boxes[0]);
        visual(w, placed.boxes[0], title, TextPurpose::Title,
               a11y::Role::Label);
        ++index;
      }
      for (const auto &child : w.children)
        place(child, placed.boxes[index++].rect, w.id);
    }
  }
};
} // namespace
WidgetScene layoutWidgets(const Widget &root, Rect bounds,
                          const UiMetrics &metrics, const Theme &theme,
                          text::ShapingCache &cache) {
  if (!std::isfinite(bounds.left) || !std::isfinite(bounds.bottom) ||
      !std::isfinite(bounds.width) || !std::isfinite(bounds.height) ||
      bounds.width < 0 || bounds.height < 0)
    throw std::invalid_argument("Invalid widget bounds");
  return WidgetBuilder(root, metrics, theme, cache)
      .build(root, clampToSafeArea(bounds, metrics.pixelSafeArea()));
}
} // namespace gleditor::ui
