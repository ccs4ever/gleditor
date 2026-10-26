/**
 * @file chronofilade.cpp
 * @brief Implementation of the Osmic Chronofilade.
 */
#include "common/xanadu/enfilade/chronofilade.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "common/xanadu/osmic_walker.hpp"
#include "common/xanadu/store.hpp"

namespace xanadu::enfilade {

namespace {

void validateXanadocBirth(const std::optional<std::uint32_t> xanadocBirth) {
  if (xanadocBirth && *xanadocBirth == 0) {
    throw std::invalid_argument(
        "xanadoc birth must be a nonzero operation index");
  }
}

} // namespace

Chronofilade::Chronofilade() {
  depth_.reserve(1024);
  up_.reserve(1024);
}

void Chronofilade::clear() {
  depth_.assign(1, std::optional<std::uint32_t>{0});
  up_.assign(1, std::array<std::uint32_t, MaxLiftingPower>{});
  indexedCount_ = 0;
  checkpoints_.clear();
  leafTransforms_.clear();
}

void Chronofilade::ensureCapacity(const std::size_t count) {
  if (depth_.size() <= count) {
    const auto newSize = std::max(count + 1, depth_.size() * 2);
    depth_.resize(newSize, std::nullopt);
    up_.resize(newSize, std::array<std::uint32_t, MaxLiftingPower>{});
  }
}

std::optional<std::uint32_t>
Chronofilade::depth(const std::uint32_t opIndex) const {
  if (opIndex >= depth_.size()) return std::nullopt;
  return depth_[opIndex];
}

std::optional<std::uint32_t>
Chronofilade::jumpAncestor(std::uint32_t idx, const std::uint32_t steps) const {
  if (idx >= depth_.size() || !depth_[idx]) return std::nullopt;
  for (std::size_t k = 0; k < MaxLiftingPower; ++k) {
    if ((steps >> k) & 1U) {
      idx = up_[idx][k];
    }
  }
  return idx;
}

std::optional<std::uint32_t>
Chronofilade::lowestCommonAncestor(std::uint32_t a, std::uint32_t b) const {
  if (a >= depth_.size() || b >= depth_.size() || !depth_[a] || !depth_[b]) {
    return std::nullopt;
  }
  if (a == b) {
    return a;
  }
  if (0 == a || 0 == b) {
    return 0;
  }

  if (*depth_[a] < *depth_[b]) {
    std::swap(a, b);
  }

  const auto diff = *depth_[a] - *depth_[b];
  for (std::size_t k = 0; k < MaxLiftingPower; ++k) {
    if ((diff >> k) & 1U) {
      a = up_[a][k];
    }
  }

  if (a == b) {
    return a;
  }

  for (int k = static_cast<int>(MaxLiftingPower) - 1; k >= 0; --k) {
    if (up_[a][static_cast<std::size_t>(k)] !=
        up_[b][static_cast<std::size_t>(k)]) {
      a = up_[a][static_cast<std::size_t>(k)];
      b = up_[b][static_cast<std::size_t>(k)];
    }
  }

  return up_[a][0];
}

void Chronofilade::recordOp(const std::uint32_t opIndex,
                            const CompactOpNode &node,
                            const MicroversionId &produces,
                            const Store &store) {
  if (opIndex == 0 || opIndex != indexedCount_ + 1) {
    throw std::invalid_argument(
        "Chronofilade operations must be recorded in index order");
  }
  const auto *const storedNode = store.segmentedOps().get(opIndex);
  const auto storedId          = store.segmentedOps().idOf(opIndex);
  if (nullptr == storedNode || !storedId || *storedId != produces ||
      storedNode->parentIndex != node.parentIndex) {
    throw std::invalid_argument(
        "Chronofilade record must match a stored operation");
  }
  const auto parent = node.parentIndex;
  if (parent >= opIndex || parent >= depth_.size() || !depth_[parent]) {
    throw std::invalid_argument(
        "Chronofilade operation parent must be a recorded earlier index");
  }
  if (*depth_[parent] == std::numeric_limits<std::uint32_t>::max()) {
    throw std::overflow_error("Chronofilade operation depth overflow");
  }
  ensureCapacity(opIndex);

  depth_[opIndex] = *depth_[parent] + 1;

  up_[opIndex][0] = parent;
  for (std::size_t k = 1; k < MaxLiftingPower; ++k) {
    const auto prev = up_[opIndex][k - 1];
    up_[opIndex][k] = (prev < up_.size()) ? up_[prev][k - 1] : 0;
  }

  ++indexedCount_;
  if (*depth_[opIndex] % CheckpointInterval == 0) {
    const auto prevCP = jumpAncestor(opIndex, CheckpointInterval);
    if (!prevCP) {
      throw std::logic_error("Recorded Chronofilade operation has no ancestor");
    }
    const auto birth =
        store.activeXanadocOnBranch(store.segmentedOps().idOf(opIndex).value());
    Version base;
    if (*prevCP > 0) {
      const auto it = checkpoints_.find(ChronoKey{*prevCP, birth});
      if (it != checkpoints_.end()) {
        base = it->second;
      }
    }

    OsmicWalker::walkPath(
        store.segmentedOps(), *prevCP, opIndex,
        [&store, &base, birth](std::uint32_t idx, const CompactOpNode &n) {
          if (birth) {
            if (idx < store.editedBirths().size() &&
                store.editedBirths()[idx] == *birth) {
              store.replay(n, base);
            }
          } else {
            if (!store.editedBirthOf(idx)) {
              store.replay(n, base);
            }
          }
        });

    checkpoints_[ChronoKey{opIndex, birth}] = base;
  }
}

void Chronofilade::indexSpool(const Store &store) {
  const auto &spool = store.segmentedOps();
  const auto count  = spool.size();
  for (std::uint32_t idx = 1; idx <= count; ++idx) {
    if (idx < depth_.size() && depth_[idx]) {
      continue;
    }
    if (const auto *const node = spool.get(idx); nullptr != node) {
      const auto id = spool.idOf(idx);
      if (!id) {
        throw std::runtime_error("Chronofilade spool operation has no ID");
      }
      recordOp(idx, *node, *id, store);
    } else {
      throw std::runtime_error("Chronofilade spool has a missing operation");
    }
  }
}

Version
Chronofilade::rebuildVersion(const std::uint32_t opIndex, const Store &store,
                             std::optional<std::uint32_t> xanadocBirth) const {
  validateXanadocBirth(xanadocBirth);
  if (0 == opIndex) {
    return Version{};
  }
  if (opIndex >= depth_.size() || !depth_[opIndex]) {
    throw std::invalid_argument("Chronofilade operation index is not recorded");
  }
  const auto d        = *depth_[opIndex];
  const auto distToCP = d % CheckpointInterval;
  const auto cp       = jumpAncestor(opIndex, distToCP);
  if (!cp) {
    throw std::logic_error("Recorded Chronofilade operation has no ancestor");
  }

  bool foundCp = false;
  Version doc;
  if (*cp > 0) {
    const auto it = checkpoints_.find(ChronoKey{*cp, xanadocBirth});
    if (it != checkpoints_.end()) {
      doc     = it->second;
      foundCp = true;
    }
  }

  if (0 == distToCP && foundCp) {
    return doc;
  }

  const auto startFrom = (!foundCp && *cp > 0) ? 0 : *cp;
  if (0 == startFrom) {
    OsmicWalker::walkAncestral(
        store.segmentedOps(), opIndex,
        [&store, &doc, xanadocBirth](std::uint32_t idx,
                                     const CompactOpNode &n) {
          if (xanadocBirth) {
            if (idx < store.editedBirths().size() &&
                store.editedBirths()[idx] == *xanadocBirth) {
              store.replay(n, doc);
            }
          } else {
            if (!store.editedBirthOf(idx)) {
              store.replay(n, doc);
            }
          }
        });
  } else {
    OsmicWalker::walkPath(store.segmentedOps(), startFrom, opIndex,
                          [&store, &doc, xanadocBirth](std::uint32_t idx,
                                                       const CompactOpNode &n) {
                            if (xanadocBirth) {
                              if (idx < store.editedBirths().size() &&
                                  store.editedBirths()[idx] == *xanadocBirth) {
                                store.replay(n, doc);
                              }
                            } else {
                              if (!store.editedBirthOf(idx)) {
                                store.replay(n, doc);
                              }
                            }
                          });
  }

  return doc;
}

bool Chronofilade::advance(
    Version &document, const std::uint32_t fromIndex,
    const std::uint32_t toIndex, const Store &store,
    const std::optional<std::uint32_t> xanadocBirth) const {
  validateXanadocBirth(xanadocBirth);
  if (fromIndex >= depth_.size() || toIndex >= depth_.size() ||
      !depth_[fromIndex] || !depth_[toIndex]) {
    return false;
  }
  if (fromIndex == toIndex) {
    return true;
  }
  const auto lca = lowestCommonAncestor(fromIndex, toIndex);
  if (!lca) return false;
  if (*lca == fromIndex) {
    return OsmicWalker::walkPath(
        store.segmentedOps(), fromIndex, toIndex,
        [&store, &document, xanadocBirth](std::uint32_t idx,
                                          const CompactOpNode &n) {
          if (xanadocBirth) {
            if (idx < store.editedBirths().size() &&
                store.editedBirths()[idx] == *xanadocBirth) {
              store.replay(n, document);
            }
          } else {
            if (!store.editedBirthOf(idx)) {
              store.replay(n, document);
            }
          }
        });
  }

  document = rebuildVersion(toIndex, store, xanadocBirth);
  return true;
}

EdlTransform Chronofilade::composePath(
    const std::uint32_t fromAncestor, const std::uint32_t toDescendant,
    const Store &store, const std::optional<std::uint32_t> xanadocBirth) const {
  validateXanadocBirth(xanadocBirth);
  if (fromAncestor >= depth_.size() || toDescendant >= depth_.size() ||
      !depth_[fromAncestor] || !depth_[toDescendant]) {
    throw std::invalid_argument("Chronofilade path index is not recorded");
  }
  const auto common = lowestCommonAncestor(fromAncestor, toDescendant);
  if (!common || *common != fromAncestor) {
    throw std::invalid_argument(
        "Chronofilade path start is not an ancestor of its destination");
  }
  if (fromAncestor == toDescendant) {
    return EdlTransform::identity(
        rebuildVersion(fromAncestor, store, xanadocBirth).length());
  }
  const auto startDoc = rebuildVersion(fromAncestor, store, xanadocBirth);
  auto transform      = EdlTransform::identity(startDoc.length());

  const bool walked = OsmicWalker::walkPath(
      store.segmentedOps(), fromAncestor, toDescendant,
      [&](std::uint32_t idx, const CompactOpNode &n) {
        if (xanadocBirth) {
          if (idx >= store.editedBirths().size() ||
              store.editedBirths()[idx] != *xanadocBirth) {
            return;
          }
        } else {
          if (store.editedBirthOf(idx)) {
            return;
          }
        }
        std::vector<PrimediaSpan> resolved;
        if (n.kind == OpKind::Transclude && n.span().empty() &&
            n.sourceOpIndex > 0) {
          const auto sourceVersion = store.segmentedOps().idOf(n.sourceOpIndex);
          if (!sourceVersion) {
            throw std::logic_error(
                "transclusion source operation has no microversion ID");
          }
          const auto srcBirth = store.activeXanadocOnBranch(*sourceVersion);
          const auto srcDoc = rebuildVersion(n.sourceOpIndex, store, srcBirth);
          resolved          = srcDoc.spansFor(n.sourceAt, n.sourceLength);
        }
        const auto stepTransform =
            EdlTransform::fromOp(n, transform.outputLength(), resolved);
        transform = EdlTransform::compose(transform, stepTransform);
      });

  if (!walked) {
    throw std::invalid_argument(
        "Chronofilade destination is not reachable from its start");
  }

  return transform;
}

bool Chronofilade::verifyAgainstFullRebuild(
    const std::uint32_t opIndex, const Store &store, const Version &fullRebuilt,
    const std::optional<std::uint32_t> xanadocBirth) const {
  validateXanadocBirth(xanadocBirth);
  if (opIndex >= depth_.size() || !depth_[opIndex]) return false;
  const auto fast = rebuildVersion(opIndex, store, xanadocBirth);
  if (fast.length() != fullRebuilt.length()) {
    return false;
  }
  if (fast.pieces() != fullRebuilt.pieces()) {
    return false;
  }
  if (fast.forcedBreaks() != fullRebuilt.forcedBreaks()) {
    return false;
  }
  return true;
}

} // namespace xanadu::enfilade
