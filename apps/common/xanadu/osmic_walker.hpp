/**
 * @file osmic_walker.hpp
 * @brief Unified hypertime DAG traversal and monoidal replay products.
 *
 * Replaces redundant backward-trace-and-reverse loops across Store, Manifold,
 * and Chronofilade with a single zero-allocation forward walker.
 *
 * Silicon & Hardware Guarantees:
 * - Stack-Buffered Optimization (SBO): paths <= 64 operations (covering all
 *   Chronofilade checkpoint intervals) allocate zero heap memory.
 * - Inlined C++23 Concepts: OsmicVisitor and OsmicFolder eliminate virtual
 *   dispatch, allowing the CPU to prefetch contiguous 64-byte CompactOpNodes
 *   at bus speed.
 * - In-place forward iteration: reverses indices during stack/heap traversal
 *   without an explicit std::reverse pass.
 */
#ifndef XANADU_OSMIC_WALKER_HPP
#define XANADU_OSMIC_WALKER_HPP

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/xanadu/compact_op.hpp"
#include "common/xanadu/segmented_ops_spool.hpp"

namespace xanadu {

/**
 * @brief Concept for a callable that visits an operation in hypertime.
 *
 * Can return void (unconditional continuation) or bool (false signals early
 * termination).
 */
template <typename F>
concept OsmicVisitor =
    requires(F f, std::uint32_t opIndex, const CompactOpNode &node) {
      f(opIndex, node);
    };

/**
 * @brief Concept for a left-fold accumulator that mutates a target state.
 */
template <typename F, typename State>
concept OsmicFolder =
    requires(F f, State &state, std::uint32_t opIndex,
             const CompactOpNode &node) { f(state, opIndex, node); };

/**
 * @class OsmicWalker
 * @brief High-performance, zero-allocation walker over the OSMIC operations
 * DAG.
 */
class OsmicWalker {
public:
  /// Standard stack buffer capacity for small-buffer optimization (256 bytes).
  static constexpr std::size_t StackBufferSize = 64;

  /**
   * @brief Walk the forward ancestral path from @p fromAncestor to
   *        @p toDescendant, invoking @p visitor on each step.
   *
   * @tparam NodeAccessor Callable returning `const CompactOpNode*` for an
   * index.
   * @tparam Visitor Callable satisfying OsmicVisitor.
   * @return true if the path was valid and walked completely; false if
   *         fromAncestor is not an ancestor of toDescendant or a node was
   *         unreadable.
   */
  template <typename NodeAccessor, OsmicVisitor Visitor>
  static bool walkPathCustom(const std::uint32_t fromAncestor,
                             const std::uint32_t toDescendant,
                             NodeAccessor &&getNode, Visitor &&visitor) {
    if (fromAncestor == toDescendant || 0 == toDescendant) {
      return true;
    }

    std::array<std::uint32_t, StackBufferSize> stackBuf{};
    std::size_t count = 0;
    std::vector<std::uint32_t> heapBuf;

    auto curr = toDescendant;
    while (curr != fromAncestor && curr > 0) {
      if (count < StackBufferSize) {
        stackBuf[count++] = curr;
      } else {
        if (heapBuf.empty()) {
          heapBuf.reserve(StackBufferSize * 2);
          heapBuf.insert(heapBuf.end(), stackBuf.begin(), stackBuf.end());
        }
        heapBuf.push_back(curr);
        count++;
      }

      const auto *const node = getNode(curr);
      if (nullptr == node) {
        return false;
      }
      curr = node->parentIndex;
    }

    if (curr != fromAncestor) {
      return false; // fromAncestor is not on the ancestral line of toDescendant
    }

    // Iterate backwards through the recorded indices to visit them in forward
    // chronological order (ancestor -> descendant) without an explicit reverse
    // pass.
    if (heapBuf.empty()) {
      for (std::size_t i = count; i > 0; --i) {
        const auto idx         = stackBuf[i - 1];
        const auto *const node = getNode(idx);
        if (nullptr != node) {
          if constexpr (std::is_same_v<
                            std::invoke_result_t<Visitor, std::uint32_t,
                                                 const CompactOpNode &>,
                            bool>) {
            if (!visitor(idx, *node)) {
              return false;
            }
          } else {
            visitor(idx, *node);
          }
        }
      }
    } else {
      for (auto it = heapBuf.rbegin(); it != heapBuf.rend(); ++it) {
        const auto idx         = *it;
        const auto *const node = getNode(idx);
        if (nullptr != node) {
          if constexpr (std::is_same_v<
                            std::invoke_result_t<Visitor, std::uint32_t,
                                                 const CompactOpNode &>,
                            bool>) {
            if (!visitor(idx, *node)) {
              return false;
            }
          } else {
            visitor(idx, *node);
          }
        }
      }
    }

    return true;
  }

  /**
   * @brief Walk the forward path from @p fromAncestor to @p toDescendant in
   *        @p spool.
   */
  template <OsmicVisitor Visitor>
  static bool walkPath(const SegmentedOpsSpool &spool,
                       const std::uint32_t fromAncestor,
                       const std::uint32_t toDescendant, Visitor &&visitor) {
    return walkPathCustom(
        fromAncestor, toDescendant,
        [&spool](const std::uint32_t idx) { return spool.get(idx); },
        std::forward<Visitor>(visitor));
  }

  /**
   * @brief Walk the full ancestral path from State 0 to @p targetIndex.
   */
  template <OsmicVisitor Visitor>
  static bool walkAncestral(const SegmentedOpsSpool &spool,
                            const std::uint32_t targetIndex,
                            Visitor &&visitor) {
    return walkPath(spool, 0, targetIndex, std::forward<Visitor>(visitor));
  }

  /**
   * @brief Fold operations from @p fromAncestor to @p toDescendant onto
   *        @p state.
   */
  template <typename State, OsmicFolder<State> Folder>
  static bool
  fold(const SegmentedOpsSpool &spool, const std::uint32_t fromAncestor,
       const std::uint32_t toDescendant, State &state, Folder &&folder) {
    return walkPath(
        spool, fromAncestor, toDescendant,
        [&state, &folder](const std::uint32_t idx, const CompactOpNode &node) {
          folder(state, idx, node);
        });
  }

  /**
   * @brief Fold operations from State 0 to @p targetIndex onto @p state.
   */
  template <typename State, OsmicFolder<State> Folder>
  static bool foldAncestral(const SegmentedOpsSpool &spool,
                            const std::uint32_t targetIndex, State &state,
                            Folder &&folder) {
    return fold(spool, 0, targetIndex, state, std::forward<Folder>(folder));
  }

  /**
   * @brief Collect all op indices on the forward path from @p fromAncestor to
   *        @p toDescendant into a vector.
   */
  static std::vector<std::uint32_t> path(const SegmentedOpsSpool &spool,
                                         std::uint32_t fromAncestor,
                                         std::uint32_t toDescendant);

  /**
   * @brief Collect all op indices on the ancestral path from State 0 to
   *        @p targetIndex.
   */
  static std::vector<std::uint32_t>
  ancestralPath(const SegmentedOpsSpool &spool, std::uint32_t targetIndex);

  /**
   * @brief Check whether @p potentialAncestor is an ancestor of @p descendant
   *        in zero allocations.
   */
  static bool isAncestor(const SegmentedOpsSpool &spool,
                         std::uint32_t potentialAncestor,
                         std::uint32_t descendant);
};

} // namespace xanadu

#endif // XANADU_OSMIC_WALKER_HPP
