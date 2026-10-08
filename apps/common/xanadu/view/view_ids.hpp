/**
 * @file view_ids.hpp
 * @brief The two plain numbers every layer of the view framework names.
 *
 * design/view-system.md §8.2 and §8.3 declare these beside ViewManifold and
 * ViewAxisSet, but the layout records (§8.4) carry both and must not depend
 * on either class: a raster, a presenter or a third-party view reads records
 * with no view space in sight. So they live here, once, and the headers that
 * own the concepts include this one.
 */
#ifndef COMMON_XANADU_VIEW_VIEW_IDS_HPP
#define COMMON_XANADU_VIEW_VIEW_IDS_HPP

#include <cstdint>

namespace xanadu::view {

/// A generation of the derived arena (§6.5). A toss increments it; binding
/// cells are generation 0 for the placement's life, derived ones count from 1.
using ViewEpoch = std::uint64_t;

/// A binding point's place on the d.axes rank (§8.3). Not a cap.
using ViewAxisId = std::uint32_t;

} // namespace xanadu::view

#endif // COMMON_XANADU_VIEW_VIEW_IDS_HPP
