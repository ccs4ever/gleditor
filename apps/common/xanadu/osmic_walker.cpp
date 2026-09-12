/**
 * @file osmic_walker.cpp
 * @brief Implementation of non-template methods for OsmicWalker.
 */
#include "common/xanadu/osmic_walker.hpp"

namespace xanadu {

std::vector<std::uint32_t> OsmicWalker::path(const SegmentedOpsSpool &spool,
                                             const std::uint32_t fromAncestor,
                                             const std::uint32_t toDescendant) {
  std::vector<std::uint32_t> result;
  walkPath(spool, fromAncestor, toDescendant,
           [&result](const std::uint32_t idx, const CompactOpNode &) {
             result.push_back(idx);
           });
  return result;
}

std::vector<std::uint32_t>
OsmicWalker::ancestralPath(const SegmentedOpsSpool &spool,
                           const std::uint32_t targetIndex) {
  return path(spool, 0, targetIndex);
}

bool OsmicWalker::isAncestor(const SegmentedOpsSpool &spool,
                             const std::uint32_t potentialAncestor,
                             const std::uint32_t descendant) {
  if (potentialAncestor == descendant) {
    return true;
  }
  if (0 == descendant || potentialAncestor > descendant) {
    return false;
  }
  auto curr = descendant;
  while (curr > potentialAncestor && curr > 0) {
    const auto *const node = spool.get(curr);
    if (nullptr == node) {
      return false;
    }
    curr = node->parentIndex;
  }
  return curr == potentialAncestor;
}

} // namespace xanadu
