// Q10: PathMorphism steps (`$p/+d.comment => [ ... ]`), with or without a
// perspective qualifier.
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu::vql {
using zigzag::CellRef;
using zigzag::toSign;
using zigzag::vortex::CellValue;
using zigzag::vortex::OpcodeKind;

std::vector<CellRef> VQLCompiler::compilePathMorphismStep(
    const PathStep &step, const CellRef current, const bool hasContext) {
  const auto *morphism = std::get_if<PathMorphism>(&step.selector);
  if (!morphism) {
    throw QueryRefusal(QueryErrorCategory::Unsupported,
                       "expected path morphism selector");
  }
  const auto subjectCell = hasContext ? current : core_.home();

  std::string dimName         = "d.content";
  zigzag::DimVector direction = zigzag::DimVector::POS;

  const auto parseDimText = [&](std::string_view raw) {
    if (raw.starts_with('+')) {
      direction = zigzag::DimVector::POS;
      dimName   = std::string(raw.substr(1));
    } else if (raw.starts_with('-')) {
      direction = zigzag::DimVector::NEG;
      dimName   = std::string(raw.substr(1));
    } else {
      direction = zigzag::DimVector::POS;
      dimName   = std::string(raw);
    }
  };

  if (std::holds_alternative<std::string>(morphism->subject.kind)) {
    parseDimText(std::get<std::string>(morphism->subject.kind));
  } else if (std::holds_alternative<ScalarLiteral>(morphism->subject.kind)) {
    const auto &lit = std::get<ScalarLiteral>(morphism->subject.kind);
    if (std::holds_alternative<std::string>(lit.value)) {
      parseDimText(std::get<std::string>(lit.value));
    }
  } else if (std::holds_alternative<std::shared_ptr<PathExpression>>(
                 morphism->subject.kind)) {
    const auto &p =
        std::get<std::shared_ptr<PathExpression>>(morphism->subject.kind);
    if (p && !p->steps.empty() &&
        std::holds_alternative<SignedDimensionStep>(p->steps[0].selector)) {
      const auto &sd = std::get<SignedDimensionStep>(p->steps[0].selector);
      dimName        = sd.dimName;
      direction      = sd.direction;
    }
  }

  if (!morphism->perspective.empty()) {
    std::string normPersp = morphism->perspective;
    if (normPersp.starts_with('@')) normPersp.erase(0, 1);
    if (normPersp.ends_with(':')) normPersp.pop_back();
    dimName = "@" + normPersp + ":" + dimName;
  }

  const auto linkDim = resolveDimension(dimName);

  const CellRef group = beginMutationGroup("#PATH_MORPHISM_GROUP");
  std::vector<CellRef> spawned;
  CellRef attachPoint = emitExtreme(subjectCell, linkDim, direction);

  for (const auto &targetExpr : morphism->targets) {
    CellRef newOp = emitOp(OpcodeKind::New, "#MINT_CELL");
    emitInput(newOp, attachPoint);
    emitInputConstant(newOp, static_cast<std::int64_t>(linkDim));
    emitInputConstant(newOp, static_cast<std::int64_t>(toSign(direction)));
    emitInputConstant(newOp, true);

    const auto read = emitOp(OpcodeKind::Link, "#MINT_RESULT");
    emitInput(read, attachPoint);
    emitInputConstant(read, static_cast<std::int64_t>(linkDim));
    emitInputConstant(read, static_cast<std::int64_t>(toSign(direction)));
    CellRef freshCell = core_.arena().makeCell();
    emitOutput(read, freshCell);

    if (const auto *lit = std::get_if<ScalarLiteral>(&targetExpr.kind)) {
      std::visit(
          [&](const auto &v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (!std::is_same_v<T, std::monostate>) {
              CellRef valOp = emitOp(OpcodeKind::Value, "#INIT_VALUE");
              emitInput(valOp, freshCell);
              emitInputConstant(valOp, static_cast<std::int64_t>(0));
              emitInputConstant(valOp, static_cast<std::int64_t>(-1));
              emitInputConstant(valOp, CellValue(v));
            }
          },
          lit->value);
    } else {
      const auto valData = compileValueExpr(targetExpr, current);
      if (valData) {
        CellRef valOp = emitOp(OpcodeKind::Value, "#INIT_VALUE");
        emitInput(valOp, freshCell);
        emitInputConstant(valOp, static_cast<std::int64_t>(0));
        emitInputConstant(valOp, static_cast<std::int64_t>(-1));
        emitInput(valOp, *valData);
      }
    }

    spawned.push_back(freshCell);
    attachPoint = freshCell;
  }

  endMutationGroup(group, "#PATH_MORPHISM_GROUP_END");
  return spawned;
}

} // namespace xanadu::vql
