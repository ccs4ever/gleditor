/**
 * @file raster.hpp
 * @brief Layout records drawn as a character grid.
 *
 * design/view-system.md §8.7. What `xuzz --raster` prints, generalised to any
 * view: vquery, vpl and vprolog show a result slice through a slice view with
 * it, and golden layouts (§16.2) are stored as rasters beside their numbers so
 * a reviewer can read a layout change as a picture.
 *
 * The projection is orthographic down the local z axis, fitted to the grid:
 * the placed things' extent in x and y fills the columns and rows. Nearer
 * things (larger z) are drawn first and keep their cells; edges and then
 * frames fill only the cells left empty, so a box is never crossed by a line
 * or a frame drawn behind it. The same records always give the same grid.
 */
#ifndef COMMON_XANADU_VIEW_RASTER_HPP
#define COMMON_XANADU_VIEW_RASTER_HPP

#include <cstdint>
#include <string>

#include <gleditor/cpp26.hpp>

#include "common/xanadu/view/view_records.hpp"

namespace xanadu::view {

struct RasterOptions {
  std::uint32_t columns{80}, rows{24};
  bool edges{true};
};

/// Draws the records orthographically onto a character grid, nearest first.
/// @p text gives a subject's words; only its first lines that fit are drawn.
/// Rows are separated by newlines and carry no trailing blanks.
[[nodiscard]] std::string
rasterise(const LayoutSink &layout, const RasterOptions &options,
          gleditor::cpp26::function_ref<std::string(SubjectId)> text);

} // namespace xanadu::view

#endif // COMMON_XANADU_VIEW_RASTER_HPP
