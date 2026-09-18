/**
 * @file crum_node.hpp
 * @brief Generic B-enfilade primitives: the displacement/width monoid
 * concepts and the cache-conscious routing node built from them.
 *
 * An enfilade is a B-tree whose interior nodes carry two things per child
 * instead of one: a relative displacement (Dsp) into that child's own
 * coordinate frame, and a summary (Wid) of what the child's subtree holds.
 * Composing displacements and combining summaries are both associative, which
 * is what lets a query descend the tree touching O(log N) nodes rather than
 * scanning every leaf. Nothing here is specific to any one kind of content --
 * a Dsp/Wid pair for byte spans looks nothing like one for 2D layout
 * coordinates, and both fit the same tree.
 */
#ifndef GLEDITOR_ENFILADE_CRUM_NODE_HPP
#define GLEDITOR_ENFILADE_CRUM_NODE_HPP

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>

namespace gleditor::enfilade {

/// Bytes in one CPU cache line on every architecture this project targets.
/// Enfilade routing nodes are `alignas`ed to it so a descent touches exactly
/// one cache line per node rather than splitting across two.
inline constexpr std::size_t kCacheLineBytes = 64;

/**
 * @brief Concept defining an Enfilade Displacement Monoid (Dsp).
 *
 * Requirements:
 * - Default constructible (representing identity displacement e_D).
 * - compose(d1, d2) -> associative composition d1 ∘ d2.
 * - isIdentity() -> bool check for identity displacement.
 */
template <typename T>
concept DisplacementMonoid = requires(T a, T b) {
  { T{} };
  { a.compose(b) } -> std::same_as<T>;
  { a.isIdentity() } -> std::same_as<bool>;
};

/**
 * @brief Concept defining an Enfilade Width Monoid (Wid).
 *
 * Requirements:
 * - Default constructible (representing empty summary 0_W).
 * - combine(w1, w2) -> associative summary combination w1 ⊕ w2.
 * - isEmpty() -> bool check for empty summary.
 */
template <typename T>
concept WidthMonoid = requires(T a, T b) {
  { T{} };
  { a.combine(b) } -> std::same_as<T>;
  { a.isEmpty() } -> std::same_as<bool>;
};

/**
 * @brief Concept defining the distributive action of Dsp on Wid.
 *
 * d.act(w) -> distributes displacement d across width summary w.
 */
template <typename Dsp, typename Wid>
concept EnfiladeAction =
    DisplacementMonoid<Dsp> && WidthMonoid<Wid> && requires(Dsp d, Wid w) {
      { d.act(w) } -> std::same_as<Wid>;
    };

/**
 * @struct CrumNode
 * @brief A cache-conscious interior or leaf crum in a B-enfilade tree.
 *
 * Stores up to B child references alongside relative displacements (Dsp)
 * and branch summaries (Wid).
 *
 * @tparam Dsp The relative displacement monoid.
 * @tparam Wid The subtree summary width monoid.
 * @tparam B The maximum branching factor (default 8).
 */
template <typename Dsp, typename Wid, std::size_t B = 8>
struct alignas(kCacheLineBytes) CrumNode {
  static constexpr std::size_t BranchingFactor = B;

  std::array<Dsp, B> dsps{}; ///< Relative displacements into child frames.
  std::array<Wid, B> wids{}; ///< Subtree summaries of children.
  std::array<std::uint32_t, B> children{}; ///< Indices of child nodes.
  std::uint32_t parentIndex{0};            ///< Parent node index (0 if root).
  std::uint8_t childCount{0}; ///< Number of active children (0 <= count <= B).
  std::uint8_t isLeaf{0};     ///< 1 if leaf crum, 0 if interior.
  std::uint16_t reservedZero{0};

  [[nodiscard]] bool full() const noexcept { return childCount >= B; }
  [[nodiscard]] bool empty() const noexcept { return 0 == childCount; }

  /**
   * @brief Compute the total width summary across all active children.
   *
   * Evaluates ⨁ (d_i · w_i) in order.
   */
  [[nodiscard]] Wid totalWid() const noexcept {
    Wid acc{};
    for (std::size_t i = 0; i < childCount; ++i) {
      acc = acc.combine(dsps[i].act(wids[i]));
    }
    return acc;
  }
};

} // namespace gleditor::enfilade

#endif // GLEDITOR_ENFILADE_CRUM_NODE_HPP
