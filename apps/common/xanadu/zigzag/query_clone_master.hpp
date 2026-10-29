#ifndef COMMON_XANADU_ZIGZAG_QUERY_CLONE_MASTER_HPP
#define COMMON_XANADU_ZIGZAG_QUERY_CLONE_MASTER_HPP

#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace zigzag {

/// Query-time master lookup may resolve foreign proxies and charges each hop.
std::optional<CellRef> queryCloneMaster(ArenaManifold &arena, CellRef cell,
                                        DimRef cloneDimension);

} // namespace zigzag
#endif
