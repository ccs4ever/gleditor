/**
 * @file presentation_palette.hpp
 * @brief The view palette's first form: which presentation shows the slice
 *        (view-system.md §11.7, §12.1 view.palette; plan §4.1 M2, §4.6 U2).
 *
 * Lists today's visualizer and every slice view in the registry, built-in or
 * third-party alike, so a view is reachable the day it is installed (V-R1).
 * U5a turns this into the palette proper, with sub-views under each view.
 */
#ifndef COMMON_UI_VIEW_PRESENTATION_PALETTE_HPP
#define COMMON_UI_VIEW_PRESENTATION_PALETTE_HPP

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <gleditor/form.hpp>

#include "common/xanadu/view/view.hpp"

namespace xanadu::view {

/// The palette's one field: today's visualizer, then each slice view of
/// @p registry in palette order, with @p current chosen. Each option's
/// value is what the per-slice presentation setting holds.
[[nodiscard]] gleditor::Form::Field
slicePresentationChoice(const ViewRegistry &registry, std::string_view current);

/// Open the palette in @p form; @p chosen is called, on the event thread,
/// with the value of what the reader picked.
void openSlicePresentationPalette(gleditor::Form &form,
                                  const ViewRegistry &registry,
                                  std::string_view current,
                                  std::function<void(std::string)> chosen);

} // namespace xanadu::view

#endif // COMMON_UI_VIEW_PRESENTATION_PALETTE_HPP
