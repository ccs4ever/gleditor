/**
 * @file presentation_palette.hpp
 * @brief The view palette: which presentation shows the slice, and under each
 *        view its sub-views (view-system.md §11.7, §12.1 view.palette; plan
 *        §4.1 M2, §4.6 U2 and U5a).
 *
 * Lists today's visualizer and every slice view in the registry, built-in or
 * third-party alike, so a view is reachable the day it is installed (V-R1),
 * and under each view that has them its sub-views, each named "View:
 * Sub-view" so the closed combo says whose it is. A choice is
 * the value of a row: the view's kind, which keeps the sub-view the slice
 * already shows, or "kind#id" which names one.
 */
#ifndef COMMON_UI_VIEW_PRESENTATION_PALETTE_HPP
#define COMMON_UI_VIEW_PRESENTATION_PALETTE_HPP

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gleditor/form.hpp>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/view/view.hpp"
#include "common/xanadu/view/view_commands.hpp"

namespace xanadu {
class Store;
} // namespace xanadu

namespace xanadu::view {

/// What a row of the palette chooses.
struct PaletteChoice {
  /// settings::kSlicePresentationLegacy or a view's kind.
  std::string presentation;
  /// A sub-view's id; empty keeps the one the slice shows (a cycle of views
  /// has no sub-view to choose). The palette's own row for a view names its
  /// default, see withDefaultSubview().
  std::string subview;

  bool operator==(const PaletteChoice &) const = default;
};

/// The row value that names @p choice, and the choice a row value names.
[[nodiscard]] std::string paletteValue(const PaletteChoice &choice);
[[nodiscard]] PaletteChoice paletteChoiceOf(std::string_view value);

/// @p choice with the default sub-view of its view named when it names none:
/// choosing a view's own row is choosing the view as it opens, not as the
/// last sub-view left it. A choice of a view with no sub-views, or of the
/// visualizer, is as it was.
[[nodiscard]] PaletteChoice withDefaultSubview(const ViewRegistry &registry,
                                               PaletteChoice choice);

/// The palette's one field: today's visualizer, then each slice view of
/// @p registry in palette order with its sub-views beneath, with @p current
/// chosen, at @p currentSubview when that names one of its sub-views. Each
/// option's value is paletteValue()'s.
[[nodiscard]] gleditor::Form::Field
slicePresentationChoice(const ViewRegistry &registry, std::string_view current,
                        std::string_view currentSubview = {});

/// Open the palette in @p form; @p chosen is called, on the event thread,
/// with what the reader picked.
void openSlicePresentationPalette(
    gleditor::Form &form, const ViewRegistry &registry,
    std::string_view current, std::string_view currentSubview,
    std::function<void(const PaletteChoice &)> chosen);

/// Made with a line the reader accepted: it calls the second argument, from
/// any thread and when it knows, with nothing for a binding made or the reason
/// for one that was not.
using BindingResult = std::function<void(const std::optional<std::string> &)>;
using BindingAttempt =
    std::function<void(const std::string &typed, BindingResult done)>;

/// The words under the binding dialog's field: @p reason first when there is
/// one, then the points, the dimensions and the groups a reader can name.
[[nodiscard]] std::string bindingNote(const BindingChoices &choices,
                                      std::string_view reason = {});

/// The hint under the binding field: its form, with an example from the first
/// point and the first dimension (or group) @p choices lists.
[[nodiscard]] std::string bindingExample(const BindingChoices &choices);

/// The binding command: one line, a point and what it shows ('x d.across'),
/// with what can be named listed under it. @p attempt is called with the line
/// the reader accepted; when it gives a reason the dialog comes back up, with
/// the line as typed and the reason shown (@p typed and @p reason are how it
/// comes back).
void openBindingCommand(gleditor::Form &form, BindingChoices choices,
                        BindingAttempt attempt, std::string typed = {},
                        std::string_view reason = {});

/// Record @p choice as the presentation of the slice @p sliceId in the
/// system://layout store @p layout, and its sub-view when it names one.
/// @return the version to make current, unchanged when nothing changed.
[[nodiscard]] MicroversionId
recordSlicePresentation(Store &layout, std::string_view sliceId,
                        const PaletteChoice &choice);

/// The row after (@p by 1) or before (-1) the one showing @p current among
/// the presentations @p kinds, wrapping; the next presentation for
/// view.cycle.
[[nodiscard]] std::string
steppedPresentation(const std::vector<std::string> &kinds,
                    std::string_view current, int by);

} // namespace xanadu::view

#endif // COMMON_UI_VIEW_PRESENTATION_PALETTE_HPP
