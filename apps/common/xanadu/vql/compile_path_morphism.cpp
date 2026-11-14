// Q10: PathMorphism steps (`$p/+d.comment => [ ... ]`), with or without a
// perspective qualifier.
#include "common/xanadu/vql/compiler.hpp"

namespace xanadu::vql {
using zigzag::CellRef;
using zigzag::vortex::OpcodeKind;

std::vector<CellRef> VQLCompiler::compilePathMorphismStep(
    const PathStep &step, const CellRef current, const bool hasContext) {
  const auto *morphism = std::get_if<PathMorphism>(&step.selector);
  if (!morphism) {
    throw QueryRefusal(QueryErrorCategory::Unsupported,
                       "expected path morphism selector");
  }
  const bool hasSubject =
      !std::holds_alternative<std::shared_ptr<PathExpression>>(
          morphism->subject.kind) ||
      std::get<std::shared_ptr<PathExpression>>(morphism->subject.kind) !=
          nullptr;
  if (hasSubject && morphism->perspective.empty()) {
    throw QueryRefusal(
        QueryErrorCategory::Unsupported,
        "this VQL path selector does not have Vortex graph lowering yet");
  }
  if (morphism->op != PathMorphism::Op::Arrow) {
    throw QueryRefusal(
        QueryErrorCategory::Unsupported,
        "this VQL path selector does not have Vortex graph lowering yet");
  }

  const auto subjectCell = hasContext ? current : core_.home();
  std::string dimName    = "d.content";
  auto direction         = zigzag::DimVector::POS;
  if (hasSubject) {
    if (const auto *lit = std::get_if<ScalarLiteral>(&morphism->subject.kind)) {
      if (const auto *str = std::get_if<std::string>(&lit->value)) {
        if (str->starts_with('+')) {
          dimName   = str->substr(1);
          direction = zigzag::DimVector::POS;
        } else if (str->starts_with('-')) {
          dimName   = str->substr(1);
          direction = zigzag::DimVector::NEG;
        } else {
          dimName = *str;
        }
      }
    } else if (const auto *str =
                   std::get_if<std::string>(&morphism->subject.kind)) {
      if (str->starts_with('+')) {
        dimName   = str->substr(1);
        direction = zigzag::DimVector::POS;
      } else if (str->starts_with('-')) {
        dimName   = str->substr(1);
        direction = zigzag::DimVector::NEG;
      } else {
        dimName = *str;
      }
    }
  }

  if (!morphism->perspective.empty()) {
    std::string normPersp = morphism->perspective;
    if (normPersp.starts_with('@')) normPersp = normPersp.substr(1);
    if (normPersp.ends_with(':'))
      normPersp = normPersp.substr(0, normPersp.size() - 1);
    dimName += "@" + normPersp;
  }
  const auto linkDim = resolveDimension(dimName);

  std::vector<CellRef> spawned;
  CellRef prevCell = subjectCell;
  for (const auto &targetExpr : morphism->targets) {
    CellRef newCell = zigzag::noCell;
    if (const auto *lit = std::get_if<ScalarLiteral>(&targetExpr.kind)) {
      if (const auto *str = std::get_if<std::string>(&lit->value)) {
        newCell = core_.arena().makeCell(*str);
      } else if (const auto *i = std::get_if<std::int64_t>(&lit->value)) {
        newCell = core_.arena().makeScalarCell(*i);
      } else if (const auto *d = std::get_if<double>(&lit->value)) {
        newCell = core_.arena().makeScalarCell(*d);
      } else if (const auto *b = std::get_if<bool>(&lit->value)) {
        newCell = core_.arena().makeScalarCell(*b);
      }
    } else if (const auto *str = std::get_if<std::string>(&targetExpr.kind)) {
      newCell = core_.arena().makeCell(*str);
    }
    if (newCell != zigzag::noCell) {
      CellRef linkOp = emitOp(OpcodeKind::Link, "#MORPHISM_LINK");
      emitInput(linkOp, prevCell);
      emitInputConstant(linkOp, static_cast<std::int64_t>(linkDim));
      emitInputConstant(linkOp,
                        static_cast<std::int64_t>(
                            direction == zigzag::DimVector::POS ? 1 : -1));
      emitInput(linkOp, newCell);

      if (core_.arena().contains(prevCell)) {
        static_cast<void>(
            core_.arena().link(prevCell, linkDim, direction, newCell));
      }
      spawned.push_back(newCell);
      prevCell = newCell;
    }
  }
  return spawned;
}

} // namespace xanadu::vql
