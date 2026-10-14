/**
 * @file presentation_palette.cpp
 * @brief Which presentation shows the slice, and the typed binding command.
 */
#include "presentation_palette.hpp"

#include <algorithm>
#include <utility>

#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"

namespace xanadu::view {

namespace {

/// Between a view's kind and a sub-view's id in a row's value.
constexpr char kSubviewMark = '#';
/// How far a sub-view row is set in under its view's.
constexpr std::string_view kSubviewIndent = "    ";

} // namespace

std::string paletteValue(const PaletteChoice &choice) {
  if (choice.subview.empty()) {
    return choice.presentation;
  }
  return choice.presentation + kSubviewMark + choice.subview;
}

PaletteChoice paletteChoiceOf(const std::string_view value) {
  const auto mark = value.find(kSubviewMark);
  if (std::string_view::npos == mark) {
    return {.presentation = std::string(value), .subview = {}};
  }
  return {.presentation = std::string(value.substr(0, mark)),
          .subview      = std::string(value.substr(mark + 1))};
}

gleditor::Form::Field
slicePresentationChoice(const ViewRegistry &registry,
                        const std::string_view current,
                        const std::string_view currentSubview) {
  gleditor::Form::Field field;
  field.label         = "Slice view";
  field.kind          = gleditor::Form::Kind::Choice;
  field.submitOnEnter = true;
  bool chosen         = false;
  const auto offer    = [&field, &chosen](std::string name, std::string value,
                                       std::string description,
                                       const bool isCurrent) {
    if (isCurrent && !chosen) {
      field.chosen = field.options.size();
      chosen       = true;
    }
    field.options.push_back(std::move(name));
    field.optionValues.push_back(std::move(value));
    field.optionDescriptions.push_back(std::move(description));
  };
  offer("ZigZag visualizer", std::string(settings::kSlicePresentationLegacy),
        "Today's presentation, with the omnibar, the palette and editing",
        settings::kSlicePresentationLegacy == current);
  for (const auto &view : registry.views()) {
    if (ViewSubject::Slice != view.subject) {
      continue;
    }
    const bool showing = view.kind == current;
    // A reader on a sub-view finds that row chosen, the more exact of the two.
    const bool onSubview =
        showing &&
        std::ranges::contains(view.subviews, currentSubview, &SubviewSpec::id);
    offer(view.name, view.kind, view.description, showing && !onSubview);
    for (const auto &subview : view.subviews) {
      offer(std::string(kSubviewIndent) + subview.name,
            paletteValue({.presentation = view.kind, .subview = subview.id}),
            "A sub-view of " + view.name,
            onSubview && subview.id == currentSubview);
    }
  }
  return field;
}

void openSlicePresentationPalette(
    gleditor::Form &form, const ViewRegistry &registry,
    const std::string_view current, const std::string_view currentSubview,
    std::function<void(const PaletteChoice &)> chosen) {
  std::vector<gleditor::Form::Field> fields{
      slicePresentationChoice(registry, current, currentSubview)};
  form.open("Views", "Choose how the slice is shown", std::move(fields),
            [chosen = std::move(chosen)](
                const std::vector<gleditor::Form::Field> &answers) {
              if (!answers.empty() && chosen) {
                chosen(paletteChoiceOf(answers.front().answer()));
              }
            });
}

void openBindingCommand(gleditor::Form &form,
                        std::function<void(std::string)> typed) {
  gleditor::Form::Field field;
  field.label    = "Bind";
  field.hint     = "a point, then what it shows: x d.across";
  field.required = true;
  field.kind     = gleditor::Form::Kind::Text;
  std::vector<gleditor::Form::Field> fields{std::move(field)};
  form.open("Bind a dimension",
            "Name a binding point and a dimension or group of this slice",
            std::move(fields),
            [typed = std::move(typed)](
                const std::vector<gleditor::Form::Field> &answers) {
              if (!answers.empty() && typed) {
                typed(answers.front().answer());
              }
            });
}

MicroversionId recordSlicePresentation(Store &layout,
                                       const std::string_view sliceId,
                                       const PaletteChoice &choice) {
  const auto before = layout.primaryCurrentVersion();
  auto head =
      ensureSetting(layout, before, slicePresentationSettingSpec(sliceId));
  head = setSetting(layout, head, settings::slicePresentationKey(sliceId),
                    choice.presentation);
  if (!choice.subview.empty()) {
    head = ensureSetting(layout, head, sliceSubviewSettingSpec(sliceId));
    head = setSetting(layout, head, settings::sliceSubviewKey(sliceId),
                      choice.subview);
  }
  return head;
}

std::string steppedPresentation(const std::vector<std::string> &kinds,
                                const std::string_view current, const int by) {
  if (kinds.empty()) {
    return std::string(current);
  }
  const auto found = std::ranges::find(kinds, current);
  const auto size  = static_cast<long long>(kinds.size());
  const long long from =
      found == kinds.end() ? 0 : static_cast<long long>(found - kinds.begin());
  const auto next = (from + by) % size;
  return kinds[static_cast<std::size_t>(next < 0 ? next + size : next)];
}

} // namespace xanadu::view
