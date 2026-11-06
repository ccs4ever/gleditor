#include "common/xanadu/vql/compiler.hpp"

#include <array>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

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
    if (rule->negated || rule->isBidir)
      throw std::invalid_argument(
          "shape projection assignments require one non-negated direction");

    const ValueExpr &target = *rule->targetValue;
    if (std::holds_alternative<ScalarLiteral>(target.kind)) continue;

    const auto rejectTarget = [&] {
      throw std::invalid_argument(
          "shape projection target must be an alias or read-only simple path");
    };
    const auto rejectPipelines = [this](const std::span<const CellRef> cells) {
      for (const CellRef cell : cells)
        if (core_.hasPipeline(cell))
          throw std::invalid_argument(
              "shape projection target cannot use attached pipelines");
    };

    if (const auto *name = std::get_if<std::string>(&target.kind)) {
      if (functorBindings_.contains(*name)) rejectTarget();
      if (const auto found = streamBindings_.find(*name);
          found != streamBindings_.end()) {
        rejectPipelines(found->second);
      } else if (const auto found = varBindings_.find(*name);
                 found != varBindings_.end() && found->second) {
        const std::array<CellRef, 1> cells{*found->second};
        rejectPipelines(cells);
      } else {
        rejectTarget();
      }
    } else if (const auto *path =
                   std::get_if<std::shared_ptr<PathExpression>>(&target.kind)) {
      if (!*path) rejectTarget();
      const PathExpression &expression = **path;
      if (expression.isFunctor || !expression.anchorPredicates.empty() ||
          expression.shapeProjection || expression.cloneTail ||
          expression.anchor.versionPin || expression.anchor.createValue ||
          expression.anchor.generator || expression.anchor.storeAnchor ||
          !expression.anchor.microversionId.empty())
        rejectTarget();

      switch (expression.anchor.kind) {
      case AnchorKind::Context:
        break;
      case AnchorKind::Home:
      case AnchorKind::Root:
        if (core_.hasPipeline(core_.home()))
          throw std::invalid_argument(
              "shape projection target cannot use attached pipelines");
        break;
      case AnchorKind::LiteralCellId:
        if (expression.anchor.cellId > std::numeric_limits<CellRef>::max())
          rejectTarget();
        {
          const CellRef cell = static_cast<CellRef>(expression.anchor.cellId);
          if (!core_.arena().contains(cell)) rejectTarget();
          const std::array<CellRef, 1> cells{cell};
          rejectPipelines(cells);
        }
        break;
      case AnchorKind::Variable:
        if (functorBindings_.contains(expression.anchor.name)) rejectTarget();
        if (const auto found = streamBindings_.find(expression.anchor.name);
            found != streamBindings_.end()) {
          rejectPipelines(found->second);
        } else if (const auto found = varBindings_.find(expression.anchor.name);
                   found != varBindings_.end() && found->second) {
          const std::array<CellRef, 1> cells{*found->second};
          rejectPipelines(cells);
        } else {
          rejectTarget();
        }
        break;
      default:
        rejectTarget();
      }

      for (const PathStep &step : expression.steps) {
        const auto *dimension =
            std::get_if<SignedDimensionStep>(&step.selector);
        if (!dimension || !dimension->dimName.starts_with("d.") ||
            dimension->dimName.size() <= 2 ||
            (dimension->direction != DimVector::POS &&
             dimension->direction != DimVector::NEG) ||
            !dimension->creates.empty() || !step.predicates.empty() ||
            !step.shapePredicates.empty() || step.versionPin ||
            step.yieldMode != YieldMode::Default || step.capturedFunctor)
          rejectTarget();
      }
    } else {
      rejectTarget();
    }

    if (resolveDimension(rule->dimName) == core_.dims().clone)
      throw std::invalid_argument(
          "shape projection target cannot assign the clone dimension");
  }
}

void VQLCompiler::compileShapeProjection(const ShapeLiteral &shape,
                                         const CellRef owner) {
  if (core_.hasPipeline(owner))
    throw std::invalid_argument(
        "shape projection target cannot use attached pipelines");

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
  std::vector<std::optional<CellRef>> preparedTargets(shape.rules.size());
  TypeExpr cellType;
  cellType.unionType.members.push_back(
      PrimaryType{.kind = BasicType{.kind = BasicType::Kind::Cell}});
  for (std::size_t index = 0; index < shape.rules.size(); ++index) {
    const auto *rule =
        std::get_if<DimensionShapeRule>(&shape.rules[index].rule);
    if (!rule || !rule->targetValue ||
        std::holds_alternative<ScalarLiteral>(rule->targetValue->kind))
      continue;

    const auto arguments       = compileArgument(*rule->targetValue, owner);
    const CellRef targetStream = normalizeFrameStream(arguments);
    const std::array<CellRef, 1> subjects{targetStream};
    const std::string diagnostic = "shape projection target " + rule->dimName;
    emitRequiredContractMatch(cellType, subjects, diagnostic);
    preparedTargets[index] = emitFirstCell(targetStream);
  }

  const CellRef mask = emitOp(OpcodeKind::VqlMaskLinks, "#PROJECTION_MASK");
  emitInput(mask, owner);
  emitInputConstant(mask, defaultKeep);
  for (const auto &[dimension, directions] : masks) {
    emitInputConstant(mask, static_cast<std::int64_t>(dimension));
    const std::int64_t bits = (directions[0].value_or(defaultKeep) ? 1 : 0) |
                              (directions[1].value_or(defaultKeep) ? 2 : 0);
    emitInputConstant(mask, bits);
  }
  for (std::size_t index = 0; index < shape.rules.size(); ++index) {
    const auto *rule =
        std::get_if<DimensionShapeRule>(&shape.rules[index].rule);
    if (!rule || !rule->targetValue) continue;
    CellRef target{};
    if (const auto *literal =
            std::get_if<ScalarLiteral>(&rule->targetValue->kind)) {
      const CellRef constant =
          std::visit([this](const auto &value) { return emitConstant(value); },
                     literal->value);
      // Constants belong to the graph; every executed assignment needs its
      // own scalar cell so reciprocal links cannot steal another owner's.
      const CellRef box =
          emitOp(OpcodeKind::VqlBoxScalar, "#PROJECTION_LITERAL");
      emitInput(box, constant);
      target = core_.arena().makeCell();
      emitOutput(box, target);
    } else {
      if (!preparedTargets[index])
        throw std::logic_error("shape projection target was not prepared");
      target = emitExistingTargetClone(*preparedTargets[index]);
    }
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
