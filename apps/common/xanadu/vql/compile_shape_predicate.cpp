#include "common/xanadu/vql/compiler.hpp"

#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace xanadu::vql {
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::CellValue;
using zigzag::vortex::OpcodeKind;

namespace {
constexpr std::size_t maxShapeDepth = 128;

std::size_t validateShape(const ShapeLiteral &shape, std::size_t depth,
                          std::unordered_set<const ShapeLiteral *> &active,
                          std::size_t &remainingValidationNodes) {
  if (depth > maxShapeDepth) {
    throw std::invalid_argument(
        "compiled shape predicates exceed maximum nesting depth 128");
  }
  if (!active.insert(&shape).second) {
    throw std::invalid_argument("compiled shape predicates contain a cycle");
  }
  if (shape.rules.empty()) {
    active.erase(&shape);
    throw std::invalid_argument(
        "compiled shape predicates do not support empty shapes or wildcards");
  }

  std::size_t expanded = 0;
  const auto add       = [&](std::size_t amount) {
    if (amount > maxCompiledShapePredicateRules - expanded) {
      throw std::invalid_argument(
          "compiled shape predicates exceed maximum expanded size 4096");
    }
    expanded += amount;
  };
  try {
    for (const auto &entry : shape.rules) {
      if (remainingValidationNodes == 0) {
        throw std::invalid_argument(
            "compiled query exceeds maximum shape validation size 4096");
      }
      --remainingValidationNodes;
      if (std::holds_alternative<ShapeWildcard>(entry.rule)) {
        throw std::invalid_argument(
            "compiled shape predicates do not support wildcards");
      }
      const auto *dimension = std::get_if<DimensionShapeRule>(&entry.rule);
      if (!dimension) {
        throw std::invalid_argument(
            "compiled shape predicates do not support variable, function, "
            "pivot, or virtual rules");
      }
      if (!dimension->dimName.starts_with("d.") ||
          dimension->dimName.size() <= 2) {
        throw std::invalid_argument(
            "compiled shape predicates require canonical d.name dimensions");
      }

      // A bidirectional target emits a complete candidate predicate for each
      // direction, so account for that duplication before generating code.
      const std::size_t copies =
          dimension->isBidir && !dimension->negated ? 2 : 1;
      add(copies);
      if (!dimension->targetValue) continue;

      const auto &target = dimension->targetValue->kind;
      if (std::holds_alternative<ScalarLiteral>(target)) {
        if (!dimension->negated) add(copies);
        continue;
      }
      const auto *nested = std::get_if<std::shared_ptr<ShapeLiteral>>(&target);
      if (!nested || !*nested) {
        throw std::invalid_argument(
            "compiled shape predicates support only scalar literal or nested "
            "literal targets");
      }
      const auto nestedSize =
          validateShape(**nested, depth + 1, active, remainingValidationNodes);
      // Legacy !dimension rules ignore their target after checking absence.
      // Validate that target, but do not charge code that will not be emitted.
      if (!dimension->negated) {
        // The nested bytecode is emitted once for each candidate direction.
        for (std::size_t i = 0; i < copies; ++i) add(nestedSize);
      }
    }
  } catch (...) {
    active.erase(&shape);
    throw;
  }
  active.erase(&shape);
  return expanded;
}
} // namespace

CellRef VQLCompiler::compileShapePredicate(const ShapeLiteral &shape,
                                           const CellRef candidate) {
  std::unordered_set<const ShapeLiteral *> active;
  const auto expanded =
      validateShape(shape, 1, active, remainingShapePredicateValidationNodes_);
  if (expanded > remainingShapePredicateRules_) {
    throw std::invalid_argument(
        "compiled query exceeds maximum expanded shape predicate size 4096");
  }
  remainingShapePredicateRules_ -= expanded;
  return compileShapePredicateImpl(shape, candidate);
}

CellRef VQLCompiler::compileShapePredicateImpl(const ShapeLiteral &shape,
                                               const CellRef candidate) {

  struct ResolvedRule {
    const DimensionShapeRule *rule;
    DimRef dimension;
    std::optional<CellRef> scalarTarget;
  };
  std::vector<ResolvedRule> rules;
  rules.reserve(shape.rules.size());
  for (const auto &entry : shape.rules) {
    const auto *dimension = std::get_if<DimensionShapeRule>(&entry.rule);
    std::optional<CellRef> scalarTarget;
    if (dimension->targetValue && !dimension->negated) {
      if (const auto *literal =
              std::get_if<ScalarLiteral>(&dimension->targetValue->kind)) {
        const auto found =
            shapeTargetConstants_.find(dimension->targetValue.get());
        if (found != shapeTargetConstants_.end()) {
          scalarTarget = found->second;
        } else {
          scalarTarget = std::visit(
              [this](const auto &value) {
                return emitConstant(CellValue(value));
              },
              literal->value);
          shapeTargetConstants_.emplace(dimension->targetValue.get(),
                                        *scalarTarget);
        }
      }
    }
    rules.push_back(
        {dimension, resolveDimension(dimension->dimName), scalarTarget});
  }

  const auto falseCell = emitConstant(false);
  const auto trueCell  = emitConstant(true);
  const auto result    = core_.arena().makeCell();
  const auto failed    = vm_.mintOpcode(OpcodeKind::Nop, "#SHAPE_FAILED");
  const auto done      = vm_.mintOpcode(OpcodeKind::Nop, "#SHAPE_DONE");
  const auto probe     = [&](const DimRef dimension, const DimVector direction,
                         const CellRef anchor) {
    const auto op = emitOp(OpcodeKind::ProbeLink, "#SHAPE_PROBE");
    emitInput(op, anchor);
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
  const auto branchFalse = [&](const CellRef condition, const CellRef target) {
    const auto branch = emitOp(OpcodeKind::BranchFalseCell, "#SHAPE_BRANCH");
    emitInput(branch, condition);
    emitInput(branch, target);
  };
  const auto branchTrue = [&](const CellRef target) {
    const auto branch = emitOp(OpcodeKind::BranchFalseCell, "#SHAPE_JUMP");
    emitInput(branch, falseCell);
    emitInput(branch, target);
  };
  const auto compareTarget = [&](const CellRef neighbor,
                                 const CellRef expected) {
    const auto op = emitOp(OpcodeKind::VqlCompare, "#SHAPE_TARGET_EQ");
    emitInput(op, emitConstant(CellValue(static_cast<std::int64_t>(
                      zigzag::vortex::VqlComparisonKind::Equal))));
    emitInput(op, neighbor);
    emitInput(op, expected);
    const CellRef equal = core_.arena().makeCell();
    emitOutput(op, equal);
    return equal;
  };
  const auto candidateMatches = [&](const ResolvedRule &resolved,
                                    const CellRef neighbor) {
    const auto &rule    = *resolved.rule;
    const auto presence = emitCellPresence(neighbor);
    if (!rule.targetValue) return presence;

    const auto answer = core_.arena().makeCell();
    const auto copyNo = emitOp(OpcodeKind::CopyCellRefs, "#SHAPE_TARGET_NO");
    emitInput(copyNo, falseCell);
    emitOutput(copyNo, answer);
    const auto done = vm_.mintOpcode(OpcodeKind::Nop, "#SHAPE_TARGET_DONE");
    branchFalse(presence, done);

    CellRef matches;
    if (resolved.scalarTarget) {
      matches = compareTarget(neighbor, *resolved.scalarTarget);
    } else {
      const auto *nested =
          std::get_if<std::shared_ptr<ShapeLiteral>>(&rule.targetValue->kind);
      matches = compileShapePredicateImpl(**nested, neighbor);
    }
    const auto copyMatches =
        emitOp(OpcodeKind::CopyCellRefs, "#SHAPE_TARGET_RESULT");
    emitInput(copyMatches, matches);
    emitOutput(copyMatches, answer);
    appendOpcode(done);
    return answer;
  };

  for (const auto &resolved : rules) {
    const auto *rule     = resolved.rule;
    const auto dimension = resolved.dimension;
    if (rule->negated) {
      // Legacy shape matching defines !d as absence on both signs; it ignores
      // target values. Validation above still rejects dynamic target forms.
      const auto pos =
          emitCellPresence(probe(dimension, DimVector::POS, candidate));
      const auto neg =
          emitCellPresence(probe(dimension, DimVector::NEG, candidate));
      const auto either = combine(OpcodeKind::Or, pos, neg);
      const auto negate = emitOp(OpcodeKind::Not, "#SHAPE_ABSENT");
      emitInput(negate, either);
      const CellRef condition = core_.arena().makeCell();
      emitOutput(negate, condition);
      branchFalse(condition, failed);
      continue;
    }

    if (!rule->targetValue) {
      CellRef condition;
      if (rule->isBidir) {
        const auto pos =
            emitCellPresence(probe(dimension, DimVector::POS, candidate));
        const auto neg =
            emitCellPresence(probe(dimension, DimVector::NEG, candidate));
        condition = combine(OpcodeKind::Or, pos, neg);
      } else {
        condition =
            emitCellPresence(probe(dimension, rule->direction, candidate));
      }
      branchFalse(condition, failed);
      continue;
    }

    const auto tryDirection = [&](const DimVector direction,
                                  const CellRef nextDirection) {
      const auto neighbor = probe(dimension, direction, candidate);
      const auto matched  = candidateMatches(resolved, neighbor);
      branchFalse(matched, nextDirection);
    };
    if (rule->isBidir) {
      const auto tryNegative =
          vm_.mintOpcode(OpcodeKind::Nop, "#SHAPE_TRY_NEG");
      const auto matched = vm_.mintOpcode(OpcodeKind::Nop, "#SHAPE_MATCHED");
      tryDirection(DimVector::POS, tryNegative);
      branchTrue(matched);
      appendOpcode(tryNegative);
      tryDirection(DimVector::NEG, failed);
      appendOpcode(matched);
    } else {
      const auto neighbor = probe(dimension, rule->direction, candidate);
      branchFalse(candidateMatches(resolved, neighbor), failed);
    }
  }

  const auto copyTrue = emitOp(OpcodeKind::CopyCellRefs, "#SHAPE_TRUE");
  emitInput(copyTrue, trueCell);
  emitOutput(copyTrue, result);
  branchTrue(done);
  appendOpcode(failed);
  const auto copyFalse = emitOp(OpcodeKind::CopyCellRefs, "#SHAPE_FALSE");
  emitInput(copyFalse, falseCell);
  emitOutput(copyFalse, result);
  appendOpcode(done);
  return result;
}
} // namespace xanadu::vql
