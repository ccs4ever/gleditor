// Q10: PathMorphism steps (`$p/+d.comment => [ ... ]`), with or without a
// perspective qualifier.
#include "common/xanadu/vql/compiler.hpp"

namespace xanadu::vql {
using zigzag::CellRef;

std::vector<CellRef> VQLCompiler::compilePathMorphismStep(
    const PathStep &step, const CellRef current, const bool hasContext) {
  const auto *morphism = std::get_if<PathMorphism>(&step.selector);
  if (!morphism) {
    throw QueryRefusal(QueryErrorCategory::Unsupported,
                       "expected path morphism selector");
  }
  const auto subjectCell = hasContext ? current : core_.home();
  std::string dimName    = "d.content";
  if (std::holds_alternative<std::string>(morphism->subject.kind)) {
    dimName = std::get<std::string>(morphism->subject.kind);
  }
  const auto linkDim = resolveDimension(dimName);

  std::vector<CellRef> spawned;
  for (const auto &targetExpr : morphism->targets) {
    if (std::holds_alternative<std::string>(targetExpr.kind)) {
      const auto &text      = std::get<std::string>(targetExpr.kind);
      const CellRef newCell = core_.arena().makeCell(text);
      if (core_.arena().link(subjectCell, linkDim, zigzag::DimVector::POS,
                             newCell)) {
        spawned.push_back(newCell);
      }
    } else if (std::holds_alternative<ScalarLiteral>(targetExpr.kind)) {
      const auto &lit = std::get<ScalarLiteral>(targetExpr.kind);
      if (std::holds_alternative<std::int64_t>(lit.value)) {
        const auto val        = std::get<std::int64_t>(lit.value);
        const CellRef newCell = core_.arena().makeScalarCell(val);
        if (core_.arena().link(subjectCell, linkDim, zigzag::DimVector::POS,
                               newCell)) {
          spawned.push_back(newCell);
        }
      } else if (std::holds_alternative<double>(lit.value)) {
        const auto val        = std::get<double>(lit.value);
        const CellRef newCell = core_.arena().makeScalarCell(val);
        if (core_.arena().link(subjectCell, linkDim, zigzag::DimVector::POS,
                               newCell)) {
          spawned.push_back(newCell);
        }
      }
    }
  }
  return spawned;
}

} // namespace xanadu::vql
