#include "common/xanadu/vql/compiler.hpp"

#include <stdexcept>
#include <vector>

namespace xanadu::vql {
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;

CellRef VQLCompiler::compileShapePredicate(const ShapeLiteral &shape,
                                           const CellRef candidate) {
  struct ResolvedRule {
    const DimensionShapeRule *rule;
    DimRef dimension;
  };
  std::vector<ResolvedRule> rules;
  rules.reserve(shape.rules.size());
  for (const auto &entry : shape.rules) {
    if (std::holds_alternative<ShapeWildcard>(entry.rule)) {
      throw std::invalid_argument(
          "compiled shape predicates do not support wildcards");
    }
    const auto *dimension = std::get_if<DimensionShapeRule>(&entry.rule);
    if (!dimension) {
      throw std::invalid_argument(
          "compiled shape predicates do not support variable, function, "
          "pivot, virtual, or nested rules");
    }
    if (dimension->targetValue) {
      throw std::invalid_argument(
          "compiled shape predicates do not support value constraints");
    }
    if (!dimension->dimName.starts_with("d.") ||
        dimension->dimName.size() <= 2) {
      throw std::invalid_argument(
          "compiled shape predicates require canonical d.name dimensions");
    }
    rules.push_back({dimension, resolveDimension(dimension->dimName)});
  }
  if (rules.empty()) {
    throw std::invalid_argument(
        "compiled shape predicates do not support empty shapes or wildcards");
  }

  const auto falseCell = emitConstant(false);
  const auto trueCell  = emitConstant(true);
  const auto result    = core_.arena().makeCell();
  const auto failed    = vm_.mintOpcode(OpcodeKind::Nop, "#SHAPE_FAILED");
  const auto done      = vm_.mintOpcode(OpcodeKind::Nop, "#SHAPE_DONE");
  const auto probe = [&](const DimRef dimension, const DimVector direction) {
    const auto op = emitOp(OpcodeKind::ProbeLink, "#SHAPE_PROBE");
    emitInput(op, candidate);
    emitInputConstant(op, static_cast<std::int64_t>(dimension));
    emitInputConstant(op, static_cast<std::int64_t>(toSign(direction)));
    const CellRef neighbor = core_.arena().makeCell();
    emitOutput(op, neighbor);
    return neighbor;
  };
  const auto combine = [&](const OpcodeKind kind, const CellRef left,
                           const CellRef right) {
    const auto op = emitOp(kind, "#SHAPE_COMBINE");
    emitInput(op, left);
    emitInput(op, right);
    const CellRef value = core_.arena().makeCell();
    emitOutput(op, value);
    return value;
  };

  for (const auto &[rule, dimension] : rules) {
    CellRef condition;
    if (rule->negated) {
      const auto pos    = emitCellPresence(probe(dimension, DimVector::POS));
      const auto neg    = emitCellPresence(probe(dimension, DimVector::NEG));
      const auto either = combine(OpcodeKind::Or, pos, neg);
      const auto negate = emitOp(OpcodeKind::Not, "#SHAPE_ABSENT");
      emitInput(negate, either);
      condition = core_.arena().makeCell();
      emitOutput(negate, condition);
    } else if (rule->isBidir) {
      const auto pos = emitCellPresence(probe(dimension, DimVector::POS));
      const auto neg = emitCellPresence(probe(dimension, DimVector::NEG));
      condition      = combine(OpcodeKind::Or, pos, neg);
    } else {
      condition = emitCellPresence(probe(dimension, rule->direction));
    }

    const auto branch = emitOp(OpcodeKind::BranchFalseCell, "#SHAPE_AND");
    emitInput(branch, condition);
    emitInput(branch, failed);
  }

  const auto copyTrue = emitOp(OpcodeKind::CopyCellRefs, "#SHAPE_TRUE");
  emitInput(copyTrue, trueCell);
  emitOutput(copyTrue, result);
  const auto skipFailure =
      emitOp(OpcodeKind::BranchFalseCell, "#SHAPE_SUCCESS");
  emitInput(skipFailure, falseCell);
  emitInput(skipFailure, done);
  appendOpcode(failed);
  const auto copyFalse = emitOp(OpcodeKind::CopyCellRefs, "#SHAPE_FALSE");
  emitInput(copyFalse, falseCell);
  emitOutput(copyFalse, result);
  appendOpcode(done);
  return result;
}
} // namespace xanadu::vql
