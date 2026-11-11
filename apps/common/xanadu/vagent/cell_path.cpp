#include "common/xanadu/vagent/cell_path.hpp"

#include <utility>
#include <variant>

#include "common/xanadu/vql/ast.hpp"
#include "common/xanadu/vql/parser.hpp"

namespace xanadu::vagent {
namespace {

[[noreturn]] void refuse(DiagnosticCode code, std::string detail,
                         SourceLocation loc) {
  throw Diagnostic{code, std::move(detail), loc};
}

void validateAnchor(const vql::AnchorNode &anchor, CellsPath &path,
                    SourceLocation loc) {
  if (anchor.derefMaster)
    refuse(DiagnosticCode::InvalidDeclaration,
           "clone-master dereferencing is not supported in cells literals",
           loc);
  if (anchor.createValue)
    refuse(DiagnosticCode::InvalidDeclaration,
           "creation anchors are not supported in cells literals", loc);
  if (anchor.generator || anchor.storeAnchor || anchor.versionPin ||
      anchor.kind == vql::AnchorKind::Microversion)
    refuse(DiagnosticCode::InvalidDeclaration,
           "store, generator and version anchors are not supported in cells "
           "literals",
           loc);
  switch (anchor.kind) {
  case vql::AnchorKind::Home:
    path.anchor.kind = CellsPath::Anchor::Kind::Home;
    return;
  case vql::AnchorKind::LiteralCellId:
    path.anchor.kind   = CellsPath::Anchor::Kind::LiteralCell;
    path.anchor.cellId = anchor.cellId;
    return;
  case vql::AnchorKind::Variable:
    path.anchor.kind     = CellsPath::Anchor::Kind::Variable;
    path.anchor.variable = anchor.name;
    return;
  default:
    refuse(DiagnosticCode::InvalidDeclaration,
           "this anchor is not supported in cells literals", loc);
  }
}

void validateFilter(const vql::ShapeLiteral &shape, CellsPath::Step &step,
                    SourceLocation loc) {
  if (step.hasFilter)
    refuse(DiagnosticCode::InvalidDeclaration,
           "a traversal step accepts at most one shape filter", loc);
  if (shape.rules.size() != 1)
    refuse(DiagnosticCode::InvalidDeclaration,
           "a shape filter in a cells literal is one dimension rule", loc);
  const auto *rule =
      std::get_if<vql::DimensionShapeRule>(&shape.rules.front().rule);
  if (!rule)
    refuse(DiagnosticCode::InvalidDeclaration,
           "a shape filter in a cells literal is a dimension presence rule",
           loc);
  if (rule->negated || rule->isBidir || rule->targetValue)
    refuse(
        DiagnosticCode::InvalidDeclaration,
        "negated, bidirectional and assigned shape filters are not supported "
        "in cells literals",
        loc);
  step.hasFilter = true;
  step.filterDim = rule->dimName;
  step.filterNeg = rule->direction == zigzag::DimVector::NEG;
}

void validateStep(const vql::PathStep &step, CellsPath &path,
                  SourceLocation loc) {
  // A step after a multi-creation step would have several subjects at once;
  // the subset cannot fan out, so it is refused rather than narrowed.
  if (!path.steps.empty() && path.steps.back().literals.size() > 1)
    refuse(DiagnosticCode::InvalidDeclaration,
           "a step cannot follow a multi-creation step in cells literals", loc);
  if (step.derefMaster || step.rangeClamp || step.versionPin)
    refuse(DiagnosticCode::InvalidDeclaration,
           "dereferences, range clamps and version pins are not supported in "
           "cells literals",
           loc);
  if (!step.predicates.empty())
    refuse(DiagnosticCode::InvalidDeclaration,
           "boolean step predicates are not supported in cells literals", loc);
  if (step.yieldMode != vql::YieldMode::Default)
    refuse(DiagnosticCode::InvalidDeclaration,
           "step yield modes are not supported in cells literals", loc);

  const auto *dim = std::get_if<vql::SignedDimensionStep>(&step.selector);
  if (!dim)
    refuse(DiagnosticCode::InvalidDeclaration,
           "cells literals support signed dimension steps only", loc);
  if (dim->dimName.empty() || dim->dimName.starts_with('$') || dim->isBidir ||
      dim->placement != vql::Placement::Default)
    refuse(DiagnosticCode::InvalidDeclaration,
           "variable, bidirectional and placed dimensions are not supported "
           "in cells literals",
           loc);

  CellsPath::Step out;
  out.dim = dim->dimName;
  out.neg = dim->direction == zigzag::DimVector::NEG;

  if (!step.shapePredicates.empty()) {
    if (step.shapePredicates.size() > 1)
      refuse(DiagnosticCode::InvalidDeclaration,
             "a traversal step accepts at most one shape filter", loc);
    validateFilter(step.shapePredicates.front(), out, loc);
  }

  if (dim->creates.empty()) {
    if (path.traversing)
      refuse(DiagnosticCode::InvalidDeclaration,
             "cells literals support at most one traversal step", loc);
    path.traversing = true;
  } else {
    if (out.neg)
      refuse(DiagnosticCode::InvalidDeclaration,
             "creations in cells literals are posward only", loc);
    if (path.traversing)
      refuse(DiagnosticCode::InvalidDeclaration,
             "a traversal step must be the last step of a cells path", loc);
    if (out.hasFilter)
      refuse(DiagnosticCode::InvalidDeclaration,
             "shape filters apply to traversal steps, not creations", loc);
    out.create    = true;
    path.mutating = true;
    for (const auto &create : dim->creates) {
      if (create.kind == vql::CreateValue::Kind::Expression)
        refuse(DiagnosticCode::InvalidDeclaration,
               "computed creation values are not supported in cells "
               "literals",
               loc);
      out.literals.push_back(create.literal);
    }
  }
  path.steps.push_back(std::move(out));
}

} // namespace

std::expected<CellsPath, Diagnostic> parseCellsPath(std::string_view source,
                                                    SourceLocation loc) {
  vql::QueryExpression query;
  try {
    vql::Parser parser(source);
    query = parser.parseQuery();
  } catch (const vql::ParseError &error) {
    return std::unexpected(
        Diagnostic{DiagnosticCode::Syntax,
                   std::string("invalid cells path: ") + error.what(), loc});
  }
  if (!query.defs.empty() || !query.mounts.empty() ||
      query.body.statements.size() != 1)
    return std::unexpected(Diagnostic{DiagnosticCode::InvalidDeclaration,
                                      "a cells literal holds exactly one path",
                                      loc});
  const auto *path = std::get_if<vql::PathExpression>(&query.expr);
  if (!path)
    return std::unexpected(
        Diagnostic{DiagnosticCode::InvalidDeclaration,
                   "a cells literal holds a path, not a block or weave", loc});
  try {
    if (path->isFunctor || !path->anchorPredicates.empty() ||
        path->shapeProjection || path->cloneTail)
      refuse(DiagnosticCode::InvalidDeclaration,
             "functors, anchor predicates, projections and clone tails are "
             "not supported in cells literals",
             loc);
    CellsPath result;
    validateAnchor(path->anchor, result, loc);
    for (const auto &step : path->steps) validateStep(step, result, loc);
    return result;
  } catch (const Diagnostic &diagnostic) {
    return std::unexpected(diagnostic);
  }
}

} // namespace xanadu::vagent
