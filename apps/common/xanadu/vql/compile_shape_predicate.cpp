#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/contracts.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
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
  std::size_t expanded = 0;
  const auto add       = [&](std::size_t amount) {
    if (amount > maxCompiledShapePredicateRules - expanded) {
      throw std::invalid_argument(
          "compiled shape predicates exceed maximum expanded size 4096");
    }
    expanded += amount;
  };
  try {
    // An empty shape is the exact test "no link at all", one probe.
    if (shape.rules.empty()) add(1);
    for (const auto &entry : shape.rules) {
      if (remainingValidationNodes == 0) {
        throw std::invalid_argument(
            "compiled query exceeds maximum shape validation size 4096");
      }
      --remainingValidationNodes;
      if (std::holds_alternative<ShapeWildcard>(entry.rule)) {
        // A wildcard costs at most the link-presence probe or the exactness
        // probe, each emitted once per shape.
        add(1);
        continue;
      }
      const auto *dimension = std::get_if<DimensionShapeRule>(&entry.rule);
      if (!dimension) {
        throw std::invalid_argument(
            "compiled shape predicates do not support pivot or virtual "
            "rules");
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

bool VQLCompiler::variableHoldsHandle(const std::string &name) const {
  const auto holds = [&](const CellRef cell) {
    return handleKindOf(cell) != HandleKind::None;
  };
  if (const auto found = streamBindings_.find(name);
      found != streamBindings_.end())
    return std::ranges::any_of(found->second, holds);
  if (const auto found = varBindings_.find(name);
      found != varBindings_.end() && found->second)
    return holds(*found->second);
  return false;
}

bool VQLCompiler::variableHoldsShape(const std::string &spelled) const {
  const std::string name =
      spelled.starts_with('$') ? spelled.substr(1) : spelled;
  const auto isShape = [&](const CellRef cell) {
    return handleKindOf(cell) == HandleKind::Shape;
  };
  if (const auto found = streamBindings_.find(name);
      found != streamBindings_.end())
    return std::ranges::any_of(found->second, isShape);
  if (const auto found = varBindings_.find(name);
      found != varBindings_.end() && found->second)
    return isShape(*found->second);
  return false;
}

bool VQLCompiler::returnsShapeLiteral(const DefDeclaration &definition) {
  const auto *body = std::get_if<std::shared_ptr<ValueExpr>>(&definition.body);
  return body && *body &&
         std::holds_alternative<std::shared_ptr<ShapeLiteral>>((*body)->kind);
}

std::optional<CellRef>
VQLCompiler::compileShapePredicateValue(const ValueExpr &expression,
                                        const CellRef candidate) {
  if (const auto *spelled = std::get_if<std::string>(&expression.kind)) {
    const std::string name =
        spelled->starts_with('$') ? spelled->substr(1) : *spelled;
    // A variable bound to no handle is an ordinary value: its truthiness is
    // what the caller computes. A handle that is not a shape (a lens, a view)
    // is refused by shapeOfVariable rather than read as a truthy cell.
    if (!variableHoldsHandle(name)) return std::nullopt;
    expandedPredicateShapes_.push_back(
        expandPredicateShape(shapeOfVariable(name)));
    return compileShapePredicate(expandedPredicateShapes_.back(), candidate);
  }
  if (const auto *call =
          std::get_if<std::shared_ptr<FunctionInvocation>>(&expression.kind);
      call && *call) {
    const auto definition = definitions_.find((*call)->name);
    if (definition == definitions_.end()) return std::nullopt;
    auto shape = resolveShapeValue(expression);
    if (!shape) return std::nullopt;
    expandedPredicateShapes_.push_back(expandPredicateShape(*shape));
    return compileShapePredicate(expandedPredicateShapes_.back(), candidate);
  }
  return std::nullopt;
}

std::optional<ShapeLiteral>
VQLCompiler::shapeNamedByTarget(const ValueExpr &target) {
  if (const auto *spelled = std::get_if<std::string>(&target.kind)) {
    const std::string name =
        spelled->starts_with('$') ? spelled->substr(1) : *spelled;
    // A target variable is only supported as a shape. An unbound one would
    // have to be bound by the match (spec 4.8.2), which is not lowered.
    if (!variableHoldsHandle(name) && !streamBindings_.contains(name) &&
        !varBindings_.contains(name))
      throw TypeContractError(
          "shape predicate target $" + name +
          " is not bound (binding a variable from a shape predicate is not "
          "supported)");
    return shapeOfVariable(name);
  }
  if (const auto *call =
          std::get_if<std::shared_ptr<FunctionInvocation>>(&target.kind);
      call && *call && definitions_.contains((*call)->name))
    return resolveShapeValue(target);
  return std::nullopt;
}

ShapeLiteral VQLCompiler::expandPredicateShape(const ShapeLiteral &shape) {
  ShapeLiteral flat = expandShape(shape);
  for (auto &entry : flat.rules) {
    auto *dimension = std::get_if<DimensionShapeRule>(&entry.rule);
    if (!dimension || !dimension->targetValue) continue;
    const ValueExpr &target = *dimension->targetValue;
    std::optional<ShapeLiteral> inner;
    if (const auto *nested =
            std::get_if<std::shared_ptr<ShapeLiteral>>(&target.kind);
        nested && *nested)
      inner = expandPredicateShape(**nested);
    else
      inner = shapeNamedByTarget(target);
    if (!inner) continue;
    // The rule is a copy of the author's; only its target changes.
    dimension->targetValue = std::make_shared<ValueExpr>(
        ValueExpr{.kind = std::make_shared<ShapeLiteral>(std::move(*inner))});
  }
  return flat;
}

namespace {
bool namesAValue(const ShapeLiteral &shape) {
  return std::ranges::any_of(shape.rules, [](const ShapeRule &entry) {
    if (std::holds_alternative<std::string>(entry.rule) ||
        std::holds_alternative<std::shared_ptr<FunctionInvocation>>(
            entry.rule) ||
        std::holds_alternative<std::shared_ptr<ShapeLiteral>>(entry.rule))
      return true;
    const auto *dimension = std::get_if<DimensionShapeRule>(&entry.rule);
    if (!dimension || !dimension->targetValue) return false;
    const auto &target = dimension->targetValue->kind;
    if (std::holds_alternative<std::string>(target) ||
        std::holds_alternative<std::shared_ptr<FunctionInvocation>>(target))
      return true;
    const auto *nested = std::get_if<std::shared_ptr<ShapeLiteral>>(&target);
    return nested && *nested && namesAValue(**nested);
  });
}
} // namespace

const ShapeLiteral &
VQLCompiler::resolvedPredicateShape(const ShapeLiteral &shape) {
  if (!namesAValue(shape)) return shape;
  expandedPredicateShapes_.push_back(expandPredicateShape(shape));
  return expandedPredicateShapes_.back();
}

CellRef VQLCompiler::compileShapePredicate(const ShapeLiteral &written,
                                           const CellRef candidate) {
  // Resolve names first: validation and emission both read the resolved
  // shape, so a variable lowers as the literal it stands for.
  const ShapeLiteral &shape = resolvedPredicateShape(written);
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
    if (!dimension) continue; // a wildcard, handled by the link probes below
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

  // Mask state in source order, as the deleted host interpreter built it: a
  // wildcard resets every earlier dimension rule, `{}` and `{!*}` make the test
  // exact (every link of the cell must be allowed), `{*}` makes it a
  // has-any-link test. Both are answered by VqlMaskProbe, which enumerates
  // links that a fixed list of ProbeLink dimensions cannot.
  constexpr std::size_t positiveSide = 0;
  constexpr std::size_t negativeSide = 1;
  std::map<DimRef, std::array<std::optional<bool>, 2>> masks;
  bool includeEverything = false;
  bool exact             = shape.rules.empty();
  bool sawWildcard       = false;
  {
    std::size_t resolvedIndex = 0;
    for (const auto &entry : shape.rules) {
      if (const auto *wildcard = std::get_if<ShapeWildcard>(&entry.rule)) {
        sawWildcard       = true;
        includeEverything = *wildcard == ShapeWildcard::Everything;
        exact             = *wildcard == ShapeWildcard::Nothing;
        masks.clear();
        continue;
      }
      const auto &resolved = rules[resolvedIndex++];
      auto &mask           = masks[resolved.dimension];
      if (resolved.rule->negated)
        mask = {false, false};
      else if (resolved.rule->isBidir)
        mask = {true, true};
      else
        mask[resolved.rule->direction == DimVector::POS ? positiveSide
                                                        : negativeSide] = true;
    }
  }

  const auto falseCell = emitConstant(false);
  const auto trueCell  = emitConstant(true);
  const auto result    = core_.arena().makeCell();
  const auto failed    = mintOpcode(OpcodeKind::Nop, "#SHAPE_FAILED");
  const auto done      = mintOpcode(OpcodeKind::Nop, "#SHAPE_DONE");
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
    const auto done = mintOpcode(OpcodeKind::Nop, "#SHAPE_TARGET_DONE");
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

  const auto probeMask = [&](const bool defaultKeep, const bool withPairs) {
    const auto op = emitOp(OpcodeKind::VqlMaskProbe, "#SHAPE_LINK_PROBE");
    emitInput(op, candidate);
    emitInputConstant(op, defaultKeep);
    if (withPairs)
      for (const auto &[dimension, sides] : masks) {
        emitInputConstant(op, static_cast<std::int64_t>(dimension));
        emitInputConstant(
            op, static_cast<std::int64_t>(
                    (sides[positiveSide].value_or(defaultKeep) ? 1 : 0) |
                    (sides[negativeSide].value_or(defaultKeep) ? 2 : 0)));
      }
    const CellRef removed = core_.arena().makeCell();
    emitOutput(op, removed);
    return removed;
  };
  if (sawWildcard && includeEverything) {
    // `{*, ...}` needs at least one link: a mask keeping nothing would remove
    // something exactly when the cell has one.
    branchFalse(probeMask(false, false), failed);
  }

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
      const auto tryNegative = mintOpcode(OpcodeKind::Nop, "#SHAPE_TRY_NEG");
      const auto matched     = mintOpcode(OpcodeKind::Nop, "#SHAPE_MATCHED");
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

  if (exact) {
    // Polarity: VqlMaskProbe answers true when the mask is NOT satisfied (a
    // link would be removed), and an exact shape holds when none would be.
    // The probe is emitted first: emission order is execution order.
    const auto removed = probeMask(false, true);
    const auto negate  = emitOp(OpcodeKind::Not, "#SHAPE_EXACT");
    emitInput(negate, removed);
    const CellRef satisfied = core_.arena().makeCell();
    emitOutput(negate, satisfied);
    branchFalse(satisfied, failed);
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
