/**
 * @file presentation_palette.cpp
 * @brief Which presentation shows the slice.
 */
#include "presentation_palette.hpp"

#include <utility>

#include "common/xanadu/system_docs.hpp"

namespace xanadu::view {

gleditor::Form::Field slicePresentationChoice(const ViewRegistry &registry,
                                              const std::string_view current) {
  gleditor::Form::Field field;
  field.label         = "Slice view";
  field.kind          = gleditor::Form::Kind::Choice;
  field.submitOnEnter = true;
  const auto offer    = [&field, current](std::string name, std::string value,
                                       std::string description) {
    if (value == current) {
      field.chosen = field.options.size();
    }
    field.options.push_back(std::move(name));
    field.optionValues.push_back(std::move(value));
    field.optionDescriptions.push_back(std::move(description));
  };
  offer("ZigZag visualizer", std::string(settings::kSlicePresentationLegacy),
        "Today's presentation, with the omnibar, the palette and editing");
  for (const auto &view : registry.views()) {
    if (ViewSubject::Slice == view.subject) {
      offer(view.name, view.kind, view.description);
    }
  }
  return field;
}

void openSlicePresentationPalette(gleditor::Form &form,
                                  const ViewRegistry &registry,
                                  const std::string_view current,
                                  std::function<void(std::string)> chosen) {
  std::vector<gleditor::Form::Field> fields{
      slicePresentationChoice(registry, current)};
  form.open("Views", "Choose how the slice is shown", std::move(fields),
            [chosen = std::move(chosen)](
                const std::vector<gleditor::Form::Field> &answers) {
              if (!answers.empty() && chosen) {
                chosen(answers.front().answer());
              }
            });
}

} // namespace xanadu::view
