/**
 * @file projection_session.cpp
 * @brief Implementation of the source-agnostic projection framework.
 */
#include "common/xanadu/zigzag/projection_session.hpp"

#include <algorithm>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace zigzag {

ProjectionSession::ProjectionSession(std::unique_ptr<ProjectionLens> lens,
                                     zigzag::vortex::VortexCore &core)
    : lens_(std::move(lens)), core_(core) {
  project();
}

std::optional<CellRef>
ProjectionSession::cellFor(const ExternalKey &key) const noexcept {
  const auto it = keyToCell_.find(key);
  if (it == keyToCell_.end()) return std::nullopt;
  return it->second;
}

std::optional<ExternalKey>
ProjectionSession::keyFor(CellRef cell) const noexcept {
  const auto it = cellToKey_.find(cell);
  if (it == cellToKey_.end()) return std::nullopt;
  return it->second;
}

void ProjectionSession::project() {
  auto &arena = core_.arena();

  // 1. Mint every declared dimension by its canonical name, so a query that
  //    names the same string resolves the same rank (§4.3.1).
  //    findOrMintDimension is what MultiStoreCoordinator::resolveDimension
  //    falls through to, so the minted DimRef is exactly what VQL/Vlog will
  //    resolve.
  for (const auto &[name, generic] : lens_->dimensions()) {
    (void)generic; // the enum is a C++-side token; the string is the contract
    (void)core_.findOrMintDimension(name);
  }

  // 2. Mint a cell per node, recording the identity map both ways.
  lens_->forEachNode([&](const SourceNode &node) {
    CellRef cell = noCell;
    std::visit(
        [&](const auto &c) {
          using T = std::remove_cvref_t<decltype(c)>;
          if constexpr (std::is_same_v<T, std::monostate>) {
            cell = arena.makeCell();
          } else if constexpr (std::is_same_v<T, std::string>) {
            cell = arena.makeCell(std::string_view{c});
          } else if constexpr (std::is_same_v<T, double>) {
            cell = arena.makeScalarCell(c);
          } else if constexpr (std::is_same_v<T, bool>) {
            cell = arena.makeScalarCell(c);
          } else if constexpr (std::is_same_v<T, std::int64_t>) {
            cell = arena.makeScalarCell(c);
          }
        },
        node.content);
    if (cell == noCell) return;
    keyToCell_.emplace(node.key, cell);
    cellToKey_.emplace(cell, node.key);

    // Attach attribute ranks: one child cell per attribute, holding the value
    // text, linked posward on the named rank (the d.role/d.mime pattern).
    for (const auto &[rank, value] : node.attributes) {
      DimRef dim   = core_.findOrMintDimension(rank);
      CellRef attr = arena.makeCell(std::string_view{value});
      (void)arena.link(cell, dim, Posward, std::optional<CellRef>(attr));
    }
  });

  // 3. Link every edge, resolving both ends through the map, and record which
  //    keys are "contained" -- reached as the posward target of a containment
  //    OR a sequence edge -- so a root is a node nothing points into. (Siblings
  //    chain on d.seq, so only the first child is a containment target; the
  //    rest are sequence targets, and none of them are roots.)
  std::unordered_set<ExternalKey> containedTargets;
  lens_->forEachEdge([&](const SourceEdge &edge) {
    const auto from = cellFor(edge.from);
    const auto to   = cellFor(edge.to);
    if (!from || !to) return; // an edge naming an unprojected node is skipped
    DimRef dim = core_.findOrMintDimension(edge.dimension);
    (void)arena.link(*from, dim, edge.dir, std::optional<CellRef>(*to));
    const bool structural =
        edge.dimension == genericDimName(GenericDim::Containment) ||
        edge.dimension == genericDimName(GenericDim::Sequence);
    if (structural && edge.dir == Posward) {
      containedTargets.insert(edge.to);
    }
  });

  // 4. Roots: projected nodes that are not the posward target of any
  //    containment edge, in mint order (deterministic given a deterministic
  //    lens). A flat source with no containment makes every node a root.
  std::vector<std::pair<CellRef, ExternalKey>> ordered;
  ordered.reserve(cellToKey_.size());
  for (const auto &[cell, key] : cellToKey_) ordered.emplace_back(cell, key);
  std::ranges::sort(
      ordered, [](const auto &a, const auto &b) { return a.first < b.first; });
  for (const auto &[cell, key] : ordered) {
    if (!containedTargets.contains(key)) roots_.push_back(cell);
  }
}

} // namespace zigzag
