#include "common/xanadu/zigzag/vlog.hpp"

#include <algorithm>
#include <vector>

#include "common/xanadu/zigzag/cell_views.hpp"

namespace zigzag {

Vlog Vlog::over(ArenaManifold &arena) {
  return Vlog{.m     = arena,
              .clone = arena.makeCell("d.clone"),
              .grab  = arena.makeCell("d.grab"),
              .step  = arena.makeCell("d.step"),
              .vars  = arena.makeCell("d.vars")};
}

CellRef Vlog::endOfRank(const CellRef from, const DimRef dim,
                        const DimVector dir) const noexcept {
  return rankTail(m, from, dim, dir);
}

CellRef Vlog::makeVar() {
  const auto var = m.makeCell();
  // Onto the tail of the d.vars rank: variablehood is membership of a rank,
  // not a flag bit, following R12's precedent for dimensions. A rank is
  // already something the manifold can answer questions about; a bit is not.
  zigzag::expectWritten(
      m.link(endOfRank(vars, vars, DimVector::POS), vars, DimVector::POS, var));
  return var;
}

CellRef Vlog::makeTerm(const std::string_view functor,
                       const std::span<const CellRef> args) {
  const auto term  = m.makeCell(functor);
  CellRef previous = noCell;
  std::vector<CellRef> seen;
  for (const CellRef arg : args) {
    CellRef actualArg = arg;
    if (noCell != arg && (std::ranges::contains(seen, arg) ||
                          noCell != m.linked(arg, grab, DimVector::NEG) ||
                          noCell != m.linked(arg, step, DimVector::NEG) ||
                          noCell != m.linked(arg, step, DimVector::POS))) {
      actualArg = m.makeCell();
      zigzag::expectWritten(m.link(endOfRank(arg, clone, DimVector::POS), clone,
                                   DimVector::POS, actualArg));
    }
    seen.push_back(actualArg);
    if (noCell == previous) {
      zigzag::expectWritten(m.link(term, grab, DimVector::POS, actualArg));
    } else {
      zigzag::expectWritten(m.link(previous, step, DimVector::POS, actualArg));
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
  // A term's arguments are the d.step rank off its d.grab head. The rank
  // stops on a ring by itself; the contains() check stops a lasso at its
  // first repeat rather than at the traversal bound.
  for (const auto arg : rank(m, m.linked(deref(ref), grab), step)) {
    if (std::ranges::contains(args, arg)) {
      break;
    }
    args.push_back(arg);
  }
  return args;
}

bool Vlog::isUnbound(const CellRef ref) const noexcept {
  const auto cell       = deref(ref);
  const bool onVarsRank = noCell != m.linked(cell, vars, DimVector::NEG) ||
                          noCell != m.linked(cell, vars, DimVector::POS);
  return onVarsRank && m.contentOf(cell).empty() &&
         xanadu::ValueKind::None == m.valueKindOf(cell);
}

void Vlog::bind(const CellRef v, const CellRef t) {
  // One link. applyStructure()'s rule -- a link is one edge with two ends, and
  // setting it maintains both -- is what makes this the whole edit rather than
  // three edits and a repair, and a rank tail has no posward neighbour to
  // evict, so there is nothing else to fix up.
  zigzag::expectWritten(m.link(endOfRank(t, clone, DimVector::POS), clone,
                               DimVector::POS,
                               endOfRank(v, clone, DimVector::NEG)));
}

UnifyResult Vlog::unify(const CellRef a, const CellRef b) {
  const auto x = deref(a);
  const auto y = deref(b);
  if (x == y) {
    return {}; // already the same rank
  }
  if (isUnbound(x)) {
    bind(x, y);
    return {};
  }
  if (isUnbound(y)) {
    bind(y, x);
    return {};
  }

  // Both bound. Numbers compare as bits and never as text: R6 put the
  // canonical bits in the cell beside the rendering precisely so that a query
  // never has to parse, and every NaN having collapsed to one pattern is what
  // makes the comparison total.
  const auto kind = m.valueKindOf(x);
  if (xanadu::ValueKind::None != kind ||
      xanadu::ValueKind::None != m.valueKindOf(y)) {
    if (kind == m.valueKindOf(y) &&
        m.slot(x)->valueBits == m.slot(y)->valueBits) {
      return {};
    }
    return std::unexpected{UnifyFailure::ValueMismatch};
  }

  if (m.textOf(x) != m.textOf(y)) {
    return std::unexpected{UnifyFailure::FunctorMismatch};
  }

  const auto left  = argumentsOf(x);
  const auto right = argumentsOf(y);
  if (left.size() != right.size()) {
    return std::unexpected{UnifyFailure::ArityMismatch};
  }
  for (const auto [l, r] : std::views::zip(left, right)) {
    if (auto unified = unify(l, r); !unified) {
      return unified;
    }
  }
  return {};
}

} // namespace zigzag
