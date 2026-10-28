#ifndef COMMON_XANADU_VQL_CLONE_MASTER_HPP
#define COMMON_XANADU_VQL_CLONE_MASTER_HPP

#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {

/// Query-time master lookup may resolve foreign proxies and charges each hop.
std::optional<zigzag::CellRef> queryCloneMaster(zigzag::ArenaManifold &arena,
                                                zigzag::CellRef cell,
                                                zigzag::DimRef cloneDimension);

} // namespace xanadu::vql
#endif
