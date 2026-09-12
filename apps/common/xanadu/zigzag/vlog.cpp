#include "common/xanadu/zigzag/vlog.hpp"

#include <algorithm>
#include <vector>

namespace zigzag {

Vlog Vlog::over(ArenaManifold &arena) {
  return Vlog{.m     = arena,
              .clone = arena.makeCell("d.clone"),
              .grab  = arena.makeCell("d.grab"),
              .step  = arena.makeCell("d.step"),
              .vars  = arena.makeCell("d.vars")};
}

CellRef Vlog::endOfRank(const CellRef from, const DimRef dim,
                        const bool negward) const noexcept {
  CellRef walk = from;
  for (std::size_t steps = 0; steps <= m.cellCount(); steps++) {
    const CellRef next = m.linked(walk, dim, negward);
    if (noCell == next || next == from) {
      return walk;
    }
    walk = next;
  }
  return walk;
}

CellRef Vlog::makeVar() {
  const auto var = m.makeCell();
  // Onto the tail of the d.vars rank: variablehood is membership of a rank,
  // not a flag bit, following R12's precedent for dimensions. A rank is
  // already something the manifold can answer questions about; a bit is not.
  m.link(endOfRank(vars, vars, false), vars, false, var);
  return var;
}

CellRef Vlog::makeTerm(const std::string_view functor,
                       const std::span<const CellRef> args) {
  const auto term  = m.makeCell(functor);
  CellRef previous = noCell;
  std::vector<CellRef> seen;
  for (const CellRef arg : args) {
    CellRef actualArg = arg;
    if (noCell != arg &&
        (std::find(seen.begin(), seen.end(), arg) != seen.end() ||
         noCell != m.linked(arg, grab, true) ||
         noCell != m.linked(arg, step, true) ||
         noCell != m.linked(arg, step, false))) {
      actualArg = m.makeCell();
      m.link(endOfRank(arg, clone, false), clone, false, actualArg);
    }
    seen.push_back(actualArg);
    if (noCell == previous) {
      m.link(term, grab, false, actualArg);
    } else {
      m.link(previous, step, false, actualArg);
    }
    previous = actualArg;
  }
  return term;
}

CellRef Vlog::makeTerm(const std::string_view functor,
                       const std::initializer_list<CellRef> args) {
  return makeTerm(functor, std::span<const CellRef>{args.begin(), args.end()});
}

std::vector<CellRef> Vlog::argumentsOf(const CellRef ref) const {
  std::vector<CellRef> args;
  const CellRef actual = deref(ref);
  CellRef arg          = m.linked(actual, grab, false);
  for (std::size_t steps = 0; noCell != arg && steps <= m.cellCount();
       steps++) {
    if (std::find(args.begin(), args.end(), arg) != args.end()) {
      break;
    }
    args.push_back(arg);
    arg = m.linked(arg, step, false);
  }
  return args;
}

bool Vlog::isUnbound(const CellRef ref) const noexcept {
  const auto cell       = deref(ref);
  const bool onVarsRank = noCell != m.linked(cell, vars, true) ||
                          noCell != m.linked(cell, vars, false);
  return onVarsRank && m.contentOf(cell).empty() &&
         xanadu::ValueKind::None == m.valueKindOf(cell);
}

void Vlog::bind(const CellRef v, const CellRef t) {
  // One link. applyStructure()'s rule -- a link is one edge with two ends, and
  // setting it maintains both -- is what makes this the whole edit rather than
  // three edits and a repair, and a rank tail has no posward neighbour to
  // evict, so there is nothing else to fix up.
  m.link(endOfRank(t, clone, false), clone, false, endOfRank(v, clone, true));
}

bool Vlog::unify(const CellRef a, const CellRef b) {
  const auto x = deref(a);
  const auto y = deref(b);
  if (x == y) {
    return true; // already the same rank
  }
  if (isUnbound(x)) {
    bind(x, y);
    return true;
  }
  if (isUnbound(y)) {
    bind(y, x);
    return true;
  }

  // Both bound. Numbers compare as bits and never as text: R6 put the
  // canonical bits in the cell beside the rendering precisely so that a query
  // never has to parse, and every NaN having collapsed to one pattern is what
  // makes the comparison total.
  const auto kind = m.valueKindOf(x);
  if (xanadu::ValueKind::None != kind ||
      xanadu::ValueKind::None != m.valueKindOf(y)) {
    return kind == m.valueKindOf(y) &&
           m.slot(x)->valueBits == m.slot(y)->valueBits;
  }

  if (m.textOf(x) != m.textOf(y)) {
    return false; // functor or atom mismatch
  }

  const auto left  = argumentsOf(x);
  const auto right = argumentsOf(y);
  if (left.size() != right.size()) {
    return false; // arity mismatch
  }
  for (std::size_t i = 0; i < left.size(); i++) {
    if (!unify(left[i], right[i])) {
      return false;
    }
  }
  return true;
}

} // namespace zigzag
