#include "common/xanadu/vql/compiler.hpp"

#include <array>
#include <map>
#include <stdexcept>

namespace xanadu::vql {
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;

void VQLCompiler::validateShapeProjection(const ShapeLiteral &shape) {
  if (shape.rules.size() > remainingShapeProjectionRules_)
    throw std::invalid_argument(
        "compiled query exceeds maximum shape projection size 4096");
  remainingShapeProjectionRules_ -= shape.rules.size();
  for (const auto &entry : shape.rules) {
    if (const auto *wildcard = std::get_if<ShapeWildcard>(&entry.rule)) {
      if (*wildcard != ShapeWildcard::Everything &&
          *wildcard != ShapeWildcard::Nothing)
        throw std::invalid_argument("invalid shape projection wildcard");
      continue;
    }
    const auto *rule = std::get_if<DimensionShapeRule>(&entry.rule);
    if (!rule || !rule->dimName.starts_with("d.") || rule->dimName.size() <= 2)
      throw std::invalid_argument(
          "shape projections require static canonical d.name dimensions");
    if (rule->direction != DimVector::POS && rule->direction != DimVector::NEG)
      throw std::invalid_argument("invalid shape projection direction");
    if (!rule->targetValue) continue;
    // Dynamic RHS values must observe the original owner, including links
    // removed by the mask. Their preparation needs a separate lowering stage.
    if (rule->negated || rule->isBidir ||
        !std::holds_alternative<ScalarLiteral>(rule->targetValue->kind))
      throw std::invalid_argument(
          "shape projection assignments require a scalar literal and one "
          "non-negated direction");
  }
}

void VQLCompiler::compileShapeProjection(const ShapeLiteral &shape,
                                         const CellRef owner) {
  bool defaultKeep = false;
  std::map<DimRef, std::array<std::optional<bool>, 2>> masks;
  for (const auto &entry : shape.rules) {
    if (const auto *wildcard = std::get_if<ShapeWildcard>(&entry.rule)) {
      defaultKeep = *wildcard == ShapeWildcard::Everything;
      masks.clear();
      continue;
    }
    const auto &rule = std::get<DimensionShapeRule>(entry.rule);
    auto &mask       = masks[resolveDimension(rule.dimName)];
    if (rule.negated)
      mask = {false, false};
    else if (rule.isBidir)
      mask = {true, true};
    else
      mask[rule.direction == DimVector::POS ? 0 : 1] = true;
  }

  const CellRef group = beginMutationGroup("#PROJECTION_GROUP");
  const CellRef mask  = emitOp(OpcodeKind::VqlMaskLinks, "#PROJECTION_MASK");
  emitInput(mask, owner);
  emitInputConstant(mask, defaultKeep);
  for (const auto &[dimension, directions] : masks) {
    emitInputConstant(mask, static_cast<std::int64_t>(dimension));
    const std::int64_t bits = (directions[0].value_or(defaultKeep) ? 1 : 0) |
                              (directions[1].value_or(defaultKeep) ? 2 : 0);
    emitInputConstant(mask, bits);
  }
  for (const auto &entry : shape.rules) {
    const auto *rule = std::get_if<DimensionShapeRule>(&entry.rule);
    if (!rule || !rule->targetValue) continue;
    const auto &literal = std::get<ScalarLiteral>(rule->targetValue->kind);
    const CellRef constant =
        std::visit([this](const auto &value) { return emitConstant(value); },
                   literal.value);
    // Constants belong to the graph; every executed assignment needs its own
    // scalar cell so reciprocal links cannot steal a prior owner's value.
    const CellRef box = emitOp(OpcodeKind::VqlBoxScalar, "#PROJECTION_LITERAL");
    emitInput(box, constant);
    const CellRef target = core_.arena().makeCell();
    emitOutput(box, target);
    const CellRef link = emitOp(OpcodeKind::Link, "#PROJECTION_ASSIGN");
    emitInput(link, owner);
    emitInputConstant(
        link, static_cast<std::int64_t>(resolveDimension(rule->dimName)));
    emitInputConstant(link, static_cast<std::int64_t>(toSign(rule->direction)));
    emitInput(link, target);
  }
  endMutationGroup(group, "#PROJECTION_GROUP_END");
}
} // namespace xanadu::vql
