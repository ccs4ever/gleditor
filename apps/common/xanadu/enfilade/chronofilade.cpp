/**
 * @file chronofilade.cpp
 * @brief Implementation of the Osmic Chronofilade.
 */
#include "common/xanadu/enfilade/chronofilade.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "common/xanadu/store.hpp"

namespace xanadu::enfilade {

Chronofilade::Chronofilade() {
  depth_.reserve(1024);
  up_.reserve(1024);
}

void Chronofilade::clear() {
  depth_.assign(1, 0);
  up_.assign(1, std::array<std::uint32_t, MaxLiftingPower>{});
  checkpoints_.clear();
  leafTransforms_.clear();
}

void Chronofilade::ensureCapacity(const std::size_t count) {
  if (depth_.size() <= count) {
    const auto newSize = std::max(count + 1, depth_.size() * 2);
    depth_.resize(newSize, 0);
    up_.resize(newSize, std::array<std::uint32_t, MaxLiftingPower>{});
  }
}

std::uint32_t Chronofilade::depth(const std::uint32_t opIndex) const {
  if (opIndex < depth_.size()) {
    return depth_[opIndex];
  }
  return 0;
}

std::uint32_t Chronofilade::jumpAncestor(std::uint32_t idx,
                                         const std::uint32_t steps) const {
  for (std::size_t k = 0; k < MaxLiftingPower; ++k) {
    if ((steps >> k) & 1U) {
      if (idx >= up_.size()) {
        return 0;
      }
      idx = up_[idx][k];
    }
  }
  return idx;
}

std::uint32_t Chronofilade::lowestCommonAncestor(std::uint32_t a,
                                                 std::uint32_t b) const {
  if (a == b) {
    return a;
  }
  if (0 == a || 0 == b) {
    return 0;
  }
  if (a >= depth_.size() || b >= depth_.size()) {
    return 0;
  }

  if (depth_[a] < depth_[b]) {
    std::swap(a, b);
  }

  const auto diff = depth_[a] - depth_[b];
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
  static_cast<void>(produces);
  ensureCapacity(opIndex);

  const auto parent = node.parentIndex;
  const auto pDepth = (parent < depth_.size()) ? depth_[parent] : 0;
  depth_[opIndex]   = pDepth + 1;

  up_[opIndex][0] = parent;
  for (std::size_t k = 1; k < MaxLiftingPower; ++k) {
    const auto prev = up_[opIndex][k - 1];
    up_[opIndex][k] = (prev < up_.size()) ? up_[prev][k - 1] : 0;
  }

  if (depth_[opIndex] % CheckpointInterval == 0) {
    const auto prevCP = jumpAncestor(opIndex, CheckpointInterval);
    Version base;
    if (prevCP > 0) {
      const auto it = checkpoints_.find(prevCP);
      if (it != checkpoints_.end()) {
        base = it->second;
      }
    }

    std::vector<std::uint32_t> path;
    path.reserve(CheckpointInterval);
    auto curr = opIndex;
    while (curr != prevCP && curr > 0) {
      path.push_back(curr);
      curr = (curr < up_.size()) ? up_[curr][0] : 0;
    }
    std::reverse(path.begin(), path.end());

    for (const auto idx : path) {
      if (const auto *const n = store.getCompactOp(idx); nullptr != n) {
        store.replay(*n, base);
      }
    }

    checkpoints_[opIndex] = base;
  }
}

void Chronofilade::indexSpool(const Store &store) {
  const auto &spool = store.segmentedOps();
  const auto count  = spool.size();
  for (std::uint32_t idx = 1; idx <= count; ++idx) {
    if (idx < depth_.size() && depth_[idx] > 0) {
      continue;
    }
    if (const auto *const node = spool.get(idx); nullptr != node) {
      recordOp(idx, *node, MicroversionId{}, store);
    }
  }
}

Version Chronofilade::rebuildVersion(const std::uint32_t opIndex,
                                     const Store &store) const {
  if (0 == opIndex) {
    return Version{};
  }
  if (opIndex >= depth_.size()) {
    return Version{};
  }

  const auto d        = depth_[opIndex];
  const auto distToCP = d % CheckpointInterval;
  const auto cp       = jumpAncestor(opIndex, distToCP);

  Version doc;
  if (cp > 0) {
    const auto it = checkpoints_.find(cp);
    if (it != checkpoints_.end()) {
      doc = it->second;
    }
  }

  if (0 == distToCP) {
    return doc;
  }

  std::vector<std::uint32_t> path;
  path.reserve(distToCP);
  auto curr = opIndex;
  while (curr != cp && curr > 0) {
    path.push_back(curr);
    curr = (curr < up_.size()) ? up_[curr][0] : 0;
  }
  std::reverse(path.begin(), path.end());

  for (const auto idx : path) {
    if (const auto *const n = store.getCompactOp(idx); nullptr != n) {
      store.replay(*n, doc);
    }
  }

  return doc;
}

bool Chronofilade::advance(Version &document, const std::uint32_t fromIndex,
                           const std::uint32_t toIndex,
                           const Store &store) const {
  if (fromIndex == toIndex) {
    return true;
  }
  const auto lca = lowestCommonAncestor(fromIndex, toIndex);
  if (lca == fromIndex) {
    std::vector<std::uint32_t> path;
    auto curr = toIndex;
    while (curr != fromIndex && curr > 0) {
      path.push_back(curr);
      curr = (curr < up_.size()) ? up_[curr][0] : 0;
    }
    std::reverse(path.begin(), path.end());
    for (const auto idx : path) {
      if (const auto *const n = store.getCompactOp(idx); nullptr != n) {
        store.replay(*n, document);
      }
    }
    return true;
  }

  document = rebuildVersion(toIndex, store);
  return true;
}

EdlTransform Chronofilade::composePath(const std::uint32_t fromAncestor,
                                       const std::uint32_t toDescendant,
                                       const Store &store) const {
  if (fromAncestor == toDescendant || 0 == toDescendant) {
    return EdlTransform::identity(0);
  }

  std::vector<std::uint32_t> path;
  auto curr = toDescendant;
  while (curr != fromAncestor && curr > 0) {
    path.push_back(curr);
    curr = (curr < up_.size()) ? up_[curr][0] : 0;
  }
  if (curr != fromAncestor) {
    return EdlTransform::identity(0);
  }
  std::reverse(path.begin(), path.end());

  const auto startDoc = rebuildVersion(fromAncestor, store);
  auto transform      = EdlTransform::identity(startDoc.length());

  for (const auto idx : path) {
    if (const auto *const n = store.getCompactOp(idx); nullptr != n) {
      std::vector<PrimediaSpan> resolved;
      if (n->kind == OpKind::Transclude && n->span().empty() &&
          n->sourceOpIndex > 0) {
        const auto srcDoc = rebuildVersion(n->sourceOpIndex, store);
        resolved          = srcDoc.spansFor(n->sourceAt, n->sourceLength);
      }
      const auto stepTransform =
          EdlTransform::fromOp(*n, transform.outputLength(), resolved);
      transform = EdlTransform::compose(transform, stepTransform);
    }
  }
  return transform;
}

bool Chronofilade::verifyAgainstFullRebuild(const std::uint32_t opIndex,
                                            const Store &store,
                                            const Version &fullRebuilt) const {
  const auto fast = rebuildVersion(opIndex, store);
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
