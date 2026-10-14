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
/// Between a view's name and its sub-view's in a row: the whole is what the
/// closed combo reads, so a sub-view row says whose it is.
constexpr std::string_view kSubviewJoin = ": ";

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
      offer(view.name + std::string(kSubviewJoin) + subview.name,
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

namespace {

/// "Label: a, b, c." -- nothing at all for a list with nothing in it.
[[nodiscard]] std::string listed(const std::string_view label,
                                 const std::vector<std::string> &names) {
  if (names.empty()) {
    return {};
  }
  std::string out = std::string(label) + ": ";
  for (std::size_t i = 0; i < names.size(); ++i) {
    out += names[i];
    out += i + 1 < names.size() ? ", " : ".";
  }
  return out;
}

} // namespace

std::string bindingNote(const BindingChoices &choices,
                        const std::string_view reason) {
  std::string note;
  const auto add = [&note](const std::string &part) {
    if (!part.empty()) {
      note += note.empty() ? "" : " ";
      note += part;
    }
  };
  add(std::string(reason));
  add(listed("Points", choices.points));
  add(listed("Dimensions", choices.dimensions));
  add(listed("Groups", choices.groups));
  return note;
}

void openBindingCommand(gleditor::Form &form, BindingChoices choices,
                        BindingAttempt attempt, std::string typed,
                        const std::string_view reason) {
  gleditor::Form::Field field;
  field.label    = "Bind";
  field.hint     = "a point, then what it shows: x d.across";
  field.value    = std::move(typed);
  field.required = true;
  field.kind     = gleditor::Form::Kind::Text;
  auto note      = bindingNote(choices, reason);
  std::vector<gleditor::Form::Field> fields{std::move(field)};
  form.open("Bind a dimension", std::move(note), std::move(fields),
            [&form, choices = std::move(choices), attempt = std::move(attempt)](
                const std::vector<gleditor::Form::Field> &answers) {
              if (answers.empty() || !attempt) {
                return;
              }
              auto text = answers.front().answer();
              // A refusal is not the end of the reader's sentence: the form
              // comes back with what was typed and why it did not work, rather
              // than closing on a message they must reopen it to read. The
              // binding is made on another thread, so the answer arrives when
              // it arrives.
              attempt(text, [&form, choices, attempt,
                             text](const std::optional<std::string> &refusal) {
                if (refusal) {
                  openBindingCommand(form, choices, attempt, text, *refusal);
                }
              });
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
  // A sub-view is the view's own: its id means nothing to another view, so it
  // is kept under the kind that offered it.
  if (!choice.subview.empty()) {
    head = ensureSetting(layout, head,
                         sliceSubviewSettingSpec(sliceId, choice.presentation));
    head = setSetting(layout, head,
                      settings::sliceSubviewKey(sliceId, choice.presentation),
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
