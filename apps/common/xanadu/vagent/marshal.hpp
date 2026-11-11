#pragma once

// Shared Expr→cell marshal for the term-cell vocabulary every surface
// reads: belief banks, emit payloads, host-step terms and [Cell]-valued
// record literals. `decls.cpp` (host steps, holds patterns) and
// `compiler.cpp` (expression emit) marshal through the same functions so
// the flat +d.step sibling-chain encoding stays one shape — a leaf's own
// +d.step edge is its next sibling, never a child, which is what makes
// positional unification over it well-defined.

#include <bit>
#include <cstdint>
#include <expected>
#include <format>
#include <string>

#include "common/xanadu/scalar.hpp"
#include "common/xanadu/vagent/ast.hpp"
#include "common/xanadu/vagent/token.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace xanadu::vagent {

// The unqualified spellings records.hpp and the sibling vagent headers
// share — declared here so this header does not rely on include order.
using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::Posward;
using zigzag::vortex::VortexCore;

/// A `+d.grab`-shaped term cell: functor name as content, args appended
/// on +d.step. V1 marshals ground terms only — LogicVariable patterns
/// (forget's `?x`) go through marshalPatternExpr instead. A Record
/// literal `S{f: v, …}` marshals functor `S` with each field value a
/// +d.step child carrying the field name on +d.name — the shape Find
/// (`term.field`) and event-payload fields already read. Literal
/// children carry dual channels: text for textOf equality (belief
/// term walks) plus the typed bits the bound-value readers take.
///
/// +d.step is one dimension, so a nested term cannot keep its own arg
/// rank — linking the head into the parent's rank splices its args in
/// as apparent siblings, and no edge afterwards can say which rank
/// member was an arg. A compound arg is therefore marked on
/// `+d.marshal` at marshal time: the only fact a flat-rank reader (the
/// wire codec) can trust about nesting is one the marshal writes.
[[nodiscard]] inline std::expected<CellRef, Diagnostic>
marshalTermExpr(zigzag::ArenaManifold &arena, VortexCore &core,
                const Expr &expr) {
  const auto &dims = core.dims();
  const auto namedLiteral = [&](const Expr &literal) -> CellRef {
    const auto cell = arena.makeCell(literal.text);
    switch (literal.kind) {
    case ExprKind::Integer:
      zigzag::expectWritten(arena.setValueBits(
          cell, xanadu::ValueKind::Int64,
          std::bit_cast<std::uint64_t>(
              static_cast<std::int64_t>(std::stoll(literal.text)))));
      break;
    case ExprKind::Float:
      zigzag::expectWritten(arena.setValueBits(
          cell, xanadu::ValueKind::Double,
          xanadu::canonicalDoubleBits(std::stod(literal.text))));
      break;
    case ExprKind::Boolean:
      zigzag::expectWritten(arena.setValueBits(
          cell, xanadu::ValueKind::Bool, literal.text == "true" ? 1 : 0));
      break;
    default:
      break;
    }
    return cell;
  };
  switch (expr.kind) {
  case ExprKind::Record:
  case ExprKind::Term: {
    const auto cell = arena.makeCell(expr.text);
    for (std::size_t i = 0; i < expr.children.size(); ++i) {
      auto arg = marshalTermExpr(arena, core, *expr.children[i]);
      if (!arg) return arg;
      if (expr.kind == ExprKind::Record && i < expr.names.size())
        zigzag::expectWritten(arena.link(*arg, dims.name, DimVector::POS,
                                         arena.makeCell(expr.names[i])));
      if (expr.children[i]->kind == ExprKind::Record ||
          expr.children[i]->kind == ExprKind::Term)
        zigzag::expectWritten(
            arena.link(*arg, core.findOrMintDimension("d.marshal"),
                       DimVector::POS,
                       arena.makeCell(expr.children[i]->kind ==
                                              ExprKind::Record
                                          ? "record"
                                          : "term")));
      zigzag::expectWritten(
          arena.link(zigzag::rankTail(arena, cell, dims.step), dims.step,
                     DimVector::POS, *arg));
    }
    return cell;
  }
  case ExprKind::String:
  case ExprKind::Integer:
  case ExprKind::Float:
  case ExprKind::Boolean:
    return namedLiteral(expr);
  default:
    return std::unexpected(Diagnostic{
        DiagnosticCode::InvalidPosition,
        std::format("host-step term argument does not lower: kind {}",
                    static_cast<int>(expr.kind)),
        expr.loc});
  }
}

/// marshalTermExpr's pattern form: LogicVariable arguments marshal as
/// '?'-prefixed text cells — MetaRuntime's unification reads them as
/// capture-or-constrain names, `?_`/`_` as pure wildcards. Records
/// carry field names the same way so a pattern spelling a record's
/// shape unifies positionally with a marshaled record fact.
[[nodiscard]] inline std::expected<CellRef, Diagnostic>
marshalPatternExpr(zigzag::ArenaManifold &arena, VortexCore &core,
                   const Expr &expr) {
  if (expr.kind == ExprKind::LogicVariable) {
    const auto text = expr.text.starts_with('?')
                          ? expr.text
                          : std::format("?{}", expr.text);
    return arena.makeCell(text);
  }
  // marshalTermExpr recurses on itself, so a Term or Record carrying a
  // variable anywhere below the root would still refuse — the pattern
  // walk re-does the compound cases with itself as the child step.
  if (expr.kind == ExprKind::Term || expr.kind == ExprKind::Record) {
    const auto &dims = core.dims();
    const auto  cell = arena.makeCell(expr.text);
    for (std::size_t i = 0; i < expr.children.size(); ++i) {
      auto arg = marshalPatternExpr(arena, core, *expr.children[i]);
      if (!arg) return arg;
      if (expr.kind == ExprKind::Record && i < expr.names.size())
        zigzag::expectWritten(arena.link(*arg, dims.name, DimVector::POS,
                                         arena.makeCell(expr.names[i])));
      zigzag::expectWritten(
          arena.link(zigzag::rankTail(arena, cell, dims.step), dims.step,
                     DimVector::POS, *arg));
    }
    return cell;
  }
  return marshalTermExpr(arena, core, expr);
}

} // namespace xanadu::vagent
