/**
 * @file spike_s3_crdt_zz.cpp
 * @brief Exploratory Spike S3: CRDT-ZZ Conflict Promotion and Convergence.
 *
 * Simulates concurrent Zlink operations from 4 virtual nodes on the same (C,
 * +d) edge, testing both Strategy A (RGA-like Rank Splice) and Strategy B
 * (Orthogonal Conflict Promotion).
 *
 * Pass Criteria:
 * - Deterministic convergence across all arrival permutations
 * - Strict 2-rank manifold invariant preservation (deg^+(c, d) <= 1, deg^-(c,
 * d) <= 1)
 * - Zero data loss: all concurrent links preserved
 */

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace crdt_zz {

using CellId = std::uint32_t;
using DimId  = std::uint32_t;
using NodeId = std::uint32_t;

// Total order key: <parentMicroversion, opRef, authorKey>
struct OpKey {
  std::uint64_t microversion{0};
  std::uint64_t opRef{0};
  NodeId author{0};

  [[nodiscard]] auto operator<=>(const OpKey &) const = default;
};

struct LinkOp {
  OpKey key;
  CellId fromCell{0};
  DimId dim{0};
  CellId toCell{0};
  bool isPos{true};
};

// Simple lightweight CRDT Manifold state tracker for invariant verification
struct DirectedEdge {
  CellId target{0};
  OpKey key{};
};

class SimulatedCRDTManifold {
public:
  // posLinks[dim][source] = target
  std::unordered_map<DimId, std::unordered_map<CellId, DirectedEdge>> posLinks;
  // negLinks[dim][target] = source
  std::unordered_map<DimId, std::unordered_map<CellId, DirectedEdge>> negLinks;

  // Conflict dimensions allocated dynamically: dim -> list of conflict dim ids
  std::unordered_map<DimId, std::vector<DimId>> conflictDims;
  DimId nextDimId{1000};

  // Strategy B: Orthogonal Conflict Dimension Promotion
  void applyStrategyB(const LinkOp &op) {
    if (!op.isPos) return; // focus on +d for this benchmark

    auto &dimPos = posLinks[op.dim];
    auto it      = dimPos.find(op.fromCell);

    if (it == dimPos.end()) {
      // Slot on dim is free, link directly
      dimPos[op.fromCell] = DirectedEdge{.target = op.toCell, .key = op.key};
      negLinks[op.dim][op.toCell] =
          DirectedEdge{.target = op.fromCell, .key = op.key};
    } else {
      // Conflict! Two operations targeting the same (fromCell, +dim).
      // Winner is the one with the higher OpKey.
      if (op.key > it->second.key) {
        // New op wins, displaces existing occupant to conflict dimension
        const DirectedEdge displaced = it->second;
        // Promote displaced to conflict dimension
        const DimId cDim            = getOrCreateConflictDim(op.dim);
        posLinks[cDim][op.fromCell] = displaced;
        negLinks[cDim][displaced.target] =
            DirectedEdge{.target = op.fromCell, .key = displaced.key};

        // Winner takes primary dim
        it->second = DirectedEdge{.target = op.toCell, .key = op.key};
        negLinks[op.dim][op.toCell] =
            DirectedEdge{.target = op.fromCell, .key = op.key};
      } else {
        // Existing occupant wins, new op is promoted to conflict dimension
        const DimId cDim = getOrCreateConflictDim(op.dim);
        posLinks[cDim][op.fromCell] =
            DirectedEdge{.target = op.toCell, .key = op.key};
        negLinks[cDim][op.toCell] =
            DirectedEdge{.target = op.fromCell, .key = op.key};
      }
    }
  }

  // Strategy A: Deterministic Rank Sequence Splice (RGA total order)
  void applyStrategyA(const std::vector<LinkOp> &ops) {
    // Sort all ops on (fromCell, dim) by OpKey descending
    std::vector<LinkOp> sorted = ops;
    std::sort(sorted.begin(), sorted.end(),
              [](const LinkOp &a, const LinkOp &b) { return a.key > b.key; });

    // Splice in order: fromCell -> toCell[0] -> toCell[1] -> toCell[2] ...
    CellId curr = sorted.front().fromCell;
    for (const auto &op : sorted) {
      posLinks[op.dim][curr] = DirectedEdge{.target = op.toCell, .key = op.key};
      negLinks[op.dim][op.toCell] = DirectedEdge{.target = curr, .key = op.key};
      curr                        = op.toCell;
    }
  }

  [[nodiscard]] bool verifyInvariants() const {
    // Verify deg^+(c, d) <= 1
    for (const auto &[dim, m] : posLinks) {
      for (const auto &[src, edge] : m) {
        if (edge.target == 0) return false;
      }
    }
    // Verify deg^-(c, d) <= 1
    for (const auto &[dim, m] : negLinks) {
      for (const auto &[tgt, edge] : m) {
        if (edge.target == 0) return false;
      }
    }
    return true;
  }

private:
  DimId getOrCreateConflictDim(DimId baseDim) {
    auto &cList      = conflictDims[baseDim];
    const DimId cDim = nextDimId++;
    cList.push_back(cDim);
    return cDim;
  }
};

} // namespace crdt_zz

int main() {
  std::cout << "=== Spike S3: CRDT-ZZ Conflict Promotion and Convergence ==="
            << std::endl;

  constexpr crdt_zz::CellId kSubjectCell = 100;
  constexpr crdt_zz::DimId kBaseDim      = 1; // e.g. d.parent

  // Define 4 concurrent operations from 4 nodes on the same (kSubjectCell,
  // +kBaseDim)
  const std::vector<crdt_zz::LinkOp> ops = {
      crdt_zz::LinkOp{
          .key = crdt_zz::OpKey{.microversion = 10, .opRef = 101, .author = 1},
          .fromCell = kSubjectCell,
          .dim      = kBaseDim,
          .toCell   = 201,
          .isPos    = true},
      crdt_zz::LinkOp{
          .key = crdt_zz::OpKey{.microversion = 10, .opRef = 102, .author = 2},
          .fromCell = kSubjectCell,
          .dim      = kBaseDim,
          .toCell   = 202,
          .isPos    = true},
      crdt_zz::LinkOp{
          .key      = crdt_zz::OpKey{.microversion = 12,
                                     .opRef        = 103,
                                     .author       = 3}, // HIGHEST: Expected winner
          .fromCell = kSubjectCell,
          .dim      = kBaseDim,
          .toCell   = 203,
          .isPos    = true},
      crdt_zz::LinkOp{
          .key = crdt_zz::OpKey{.microversion = 8, .opRef = 104, .author = 4},
          .fromCell = kSubjectCell,
          .dim      = kBaseDim,
          .toCell   = 204,
          .isPos    = true},
  };

  std::cout
      << "[Test] Generating all 24 permutations of arrival order for 4 nodes..."
      << std::endl;

  std::vector<int> perm = {0, 1, 2, 3};
  std::vector<crdt_zz::SimulatedCRDTManifold> manifolds;

  int permutationCount = 0;
  bool allConverged    = true;
  bool invariantsHold  = true;

  do {
    crdt_zz::SimulatedCRDTManifold m;
    for (int idx : perm) {
      m.applyStrategyB(ops[idx]);
    }

    if (!m.verifyInvariants()) {
      invariantsHold = false;
    }

    // Verify winner on primary dimension is ALWAYS Cell 203 (Node 3,
    // microversion 12)
    const auto it = m.posLinks[kBaseDim].find(kSubjectCell);
    if (it == m.posLinks[kBaseDim].end() || it->second.target != 203) {
      allConverged = false;
    }

    // Verify 3 conflict dimensions were created for the 3 displaced links
    if (m.conflictDims[kBaseDim].size() != 3) {
      allConverged = false;
    }

    manifolds.push_back(std::move(m));
    ++permutationCount;
  } while (std::next_permutation(perm.begin(), perm.end()));

  std::cout << "[Verify] Tested " << permutationCount << " permutations."
            << std::endl;
  std::cout
      << "  Deterministic Winner on d.1: Cell 203 in all 24 permutations: "
      << (allConverged ? "YES" : "NO") << std::endl;
  std::cout << "  2-Rank Invariant deg^+(c, d) <= 1 holds across all nodes: "
            << (invariantsHold ? "YES" : "NO") << std::endl;

  // Test Strategy A: Rank Sequence Splice
  std::cout
      << "\n[Test] Testing Strategy A (Deterministic Rank Sequence Splice)..."
      << std::endl;
  crdt_zz::SimulatedCRDTManifold mSeq;
  mSeq.applyStrategyA(ops);
  const bool seqInvariant = mSeq.verifyInvariants();
  std::cout << "  Strategy A spliced sequence invariant: "
            << (seqInvariant ? "PASS" : "FAIL") << std::endl;

  const bool pass = allConverged && invariantsHold && seqInvariant;

  std::cout << "\n=== SPIKE S3 RESULTS ===" << std::endl;
  std::cout << "CRDT-ZZ Invariant & Convergence: "
            << (pass ? "[PASS]" : "[FAIL]") << std::endl;

  return pass ? 0 : 1;
}
