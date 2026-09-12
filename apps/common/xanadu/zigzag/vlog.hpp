/**
 * @file vlog.hpp
 * @brief Unification over an ArenaManifold: Vlog §4, as a program.
 *
 * The claim design/vlog-logic-extension.md makes is that resolution needs no
 * primitive of its own -- that unification is a program over link, value and
 * cloneMaster, and that a binding is one link rather than an assignment to be
 * undone. This file is that claim, written out, and it is deliberately small
 * because the argument is that there is nothing here.
 *
 * A term is a cell. Its functor is the cell's content; its arguments hang off
 * the Inputs Wing -- first on +d.grab, the rest chained on +d.step -- which is
 * Vortex's ordinary calling convention and not a term representation invented
 * for this. A variable is a cell on the d.vars rank whose clone master carries
 * nothing. A binding is a splice of two d.clone ranks.
 *
 * Nothing here allocates a heap, a trail or a binding environment: the clone
 * ranks *are* the bindings, the arena's mark/release is the undo, and a
 * failed unification leaves its partial bindings for the caller's release() to
 * take away -- which is what Prolog does too, and for the same reason.
 */
#ifndef ZIGZAG_VLOG_HPP
#define ZIGZAG_VLOG_HPP

#include <span>
#include <string_view>

#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace zigzag {

/**
 * @brief The four dimensions unification walks, and the arena it walks in.
 *
 * Held rather than looked up per call: a dimension is an ordinary cell (R2),
 * so there is no compiled-in ordinal to reach for and finding one by name is a
 * rank walk. A resolution engine does this a million times.
 */
struct Vlog {
  ArenaManifold &m;
  DimRef clone{noCell}; ///< where a binding lives
  DimRef grab{noCell};  ///< a term's first argument
  DimRef step{noCell};  ///< its second and subsequent
  DimRef vars{noCell};  ///< the rank that says "these cells are variables"

  /// Mint the four dimensions in @p arena and answer a Vlog over it.
  [[nodiscard]] static Vlog over(ArenaManifold &arena);

  // -- terms ------------------------------------------------------------------

  /// A fresh unbound variable: a bare cell on the d.vars rank.
  CellRef makeVar();

  /// A term whose functor reads as @p functor and whose arguments are @p args,
  /// chained on the Inputs Wing.
  CellRef makeTerm(std::string_view functor,
                   std::initializer_list<CellRef> args);
  CellRef makeTerm(std::string_view functor, std::span<const CellRef> args);

  /// @p ref's arguments, in order. Empty for an atom or a number.
  [[nodiscard]] std::vector<CellRef> argumentsOf(CellRef ref) const;

  // -- the two questions §3 turns on -----------------------------------------

  /// Vlog's `deref`, which is cloneMaster and was already there.
  [[nodiscard]] CellRef deref(CellRef ref) const noexcept {
    return m.cloneMaster(ref, clone);
  }

  /// Whether @p ref is an unbound variable.
  ///
  /// §3.2: membership of the d.vars rank *and* an empty value, which is what
  /// separates an unbound variable from a bound one -- and the rank membership
  /// is what separates it from the atom `''`. Once bound, deref() answers the
  /// term, which was never minted onto d.vars, so no "bound" marker is ever
  /// written.
  [[nodiscard]] bool isUnbound(CellRef ref) const noexcept;

  // -- §4 ---------------------------------------------------------------------

  /**
   * @brief Unify @p a and @p b, binding variables along the way.
   *
   * Leaves partial bindings behind on failure. That is not a defect: a caller
   * takes a Mark before a head unification and releases it on failure, which
   * is cheaper than unwinding one binding at a time and is the entire reason
   * §5.2 exists.
   *
   * Rational trees by default (§4.4): X = f(X) builds a cycle rather than
   * being refused, because a zzstructure is happy with one and cloneMaster()
   * already guards the walk.
   */
  bool unify(CellRef a, CellRef b);

  /// Bind the unbound variable @p v to @p t: splice v's clone rank onto the
  /// posward tail of t's, so the combined rank's master is t's master.
  void bind(CellRef v, CellRef t);

  /// The far end of @p from's rank along @p dim, cycle-bounded.
  [[nodiscard]] CellRef endOfRank(CellRef from, DimRef dim,
                                  bool negward) const noexcept;
};

} // namespace zigzag

#endif // ZIGZAG_VLOG_HPP
