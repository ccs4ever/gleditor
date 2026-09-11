#ifndef XANADU_ENFILADE_CRUM_NODE_HPP
#define XANADU_ENFILADE_CRUM_NODE_HPP

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>

namespace xanadu::enfilade {

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
struct alignas(64) CrumNode {
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

} // namespace xanadu::enfilade

#endif // XANADU_ENFILADE_CRUM_NODE_HPP
