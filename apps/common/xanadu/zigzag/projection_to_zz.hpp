/**
 * @file projection_to_zz.hpp
 * @brief projectionToZz: a live ProjectionSession's arena as a
 *        ZzStructureDocument -- the adoptDocument half that puts a
 *        projected source into xuzz's slice view.
 */
#ifndef COMMON_XANADU_ZIGZAG_PROJECTION_TO_ZZ_HPP
#define COMMON_XANADU_ZIGZAG_PROJECTION_TO_ZZ_HPP

#include <string_view>

#include "common/xanadu/zigzag/zzstructure.hpp"

namespace zigzag {

class ArenaManifold;
class ProjectionSession;
class ZzCore;

/// Snapshot the session's arena into a slice DTO. Cell ids are the
/// cells' refs (birthOp indexes); every rank link lands in
/// Cell::dimensions under the dimension's own name; the focus is the
/// session's first root; the view binds d.contains on X and d.seq on Y
/// so the projected containment tree reads as a navigable slice.
[[nodiscard]] ZzStructureDocument projectionToZz(std::string_view label,
                                                 ProjectionSession &session,
                                                 ArenaManifold &arena);

} // namespace zigzag

#endif // COMMON_XANADU_ZIGZAG_PROJECTION_TO_ZZ_HPP
