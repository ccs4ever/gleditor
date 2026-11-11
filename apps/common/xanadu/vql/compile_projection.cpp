#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/projection_syntax.hpp"

#include <algorithm>
#include <array>
#include <functional>
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

namespace {

bool hasStaticProjectionRuleShape(const ShapeLiteral &shape) {
  for (const ShapeRule &entry : shape.rules) {
    if (const auto *wildcard = std::get_if<ShapeWildcard>(&entry.rule)) {
      if (*wildcard != ShapeWildcard::Everything &&
          *wildcard != ShapeWildcard::Nothing)
        return false;
      continue;
    }
    const auto *rule = std::get_if<DimensionShapeRule>(&entry.rule);
    if (!rule || !rule->dimName.starts_with("d.") ||
        rule->dimName.size() <= 2 ||
        (rule->direction != DimVector::POS &&
         rule->direction != DimVector::NEG) ||
        (rule->targetValue && (rule->negated || rule->isBidir)))
      return false;
  }
  return true;
}

} // namespace

void VQLCompiler::validateShapeProjection(const ShapeLiteral &shape) {
  if (shape.rules.size() > remainingShapeProjectionRules_)
    throw std::invalid_argument(
        "compiled query exceeds maximum shape projection size 4096");
  remainingShapeProjectionRules_ -= shape.rules.size();
  if (!isStaticShapeProjectionSyntax(shape) &&
      !hasStaticProjectionRuleShape(shape))
    compileDynamicShapeProjection(shape);
  for (const auto &entry : shape.rules) {
    if (const auto *wildcard = std::get_if<ShapeWildcard>(&entry.rule)) {
      (void)wildcard;
      continue;
    }
    const auto *rule = std::get_if<DimensionShapeRule>(&entry.rule);
    if (!rule->targetValue) continue;

    const ValueExpr &target = *rule->targetValue;
    const auto rejectTarget = [&] {
      throw std::invalid_argument("shape projection target is not read-only "
                                  "or graph-supported");
    };
    const auto rejectPipelines = [this](const std::span<const CellRef> cells) {
      for (const CellRef cell : cells)
        if (core_.hasPipeline(cell))
          throw std::invalid_argument(
              "shape projection target cannot use attached pipelines");
    };

    std::function<void(const ValueExpr &)> validateComputedTarget;
    validateComputedTarget = [&](const ValueExpr &expression) {
      if (std::holds_alternative<ScalarLiteral>(expression.kind)) return;
      if (const auto *binary =
              std::get_if<std::shared_ptr<BinaryValueExpr>>(&expression.kind)) {
        if (!*binary || !(*binary)->left || !(*binary)->right) rejectTarget();
        validateComputedTarget(*(*binary)->left);
        validateComputedTarget(*(*binary)->right);
        return;
      }
      if (const auto *function =
              std::get_if<std::shared_ptr<FunctionInvocation>>(
                  &expression.kind)) {
        if (!*function || (*function)->isSliceRead ||
            std::ranges::any_of(
                (*function)->argNames,
                [](const std::string &name) { return !name.empty(); }))
          rejectTarget();
        const auto &call = **function;
        if (call.name == "#vql-current-timestamp") {
          if (!call.args.empty()) rejectTarget();
        } else if (call.name == "now" && !definitions_.contains(call.name)) {
          if (!call.args.empty()) rejectTarget();
        } else if (call.name == "string" && !definitions_.contains(call.name)) {
          if (call.args.size() != 1) rejectTarget();
          validateComputedTarget(call.args.front());
        } else if (readOnlyDefinitions_.contains(call.name)) {
          if (const auto captures = frameFreeCaptureNames_.find(call.name);
              captures != frameFreeCaptureNames_.end()) {
            for (const auto &name : captures->second) {
              if (const auto found = streamBindings_.find(name);
                  found != streamBindings_.end()) {
                rejectPipelines(found->second);
              } else if (const auto found = varBindings_.find(name);
                         found != varBindings_.end() && found->second) {
                const std::array<CellRef, 1> cells{*found->second};
                rejectPipelines(cells);
              } else {
                rejectTarget();
              }
            }
          }
          for (const auto &argument : call.args)
            validateComputedTarget(argument);
        } else {
          rejectTarget();
        }
        return;
      }
      if (const auto *path =
              std::get_if<std::shared_ptr<PathExpression>>(&expression.kind)) {
        if (!*path || !isSimpleProjectionPath(**path)) rejectTarget();
        const PathExpression &pathExpression = **path;
        switch (pathExpression.anchor.kind) {
        case AnchorKind::Context:
          break;
        case AnchorKind::Home:
        case AnchorKind::Root:
          if (core_.hasPipeline(core_.home()))
            throw std::invalid_argument(
                "shape projection target cannot use attached pipelines");
          break;
        case AnchorKind::LiteralCellId: {
          if (pathExpression.anchor.cellId >
              std::numeric_limits<CellRef>::max())
            rejectTarget();
          const CellRef cell =
              static_cast<CellRef>(pathExpression.anchor.cellId);
          if (!core_.arena().contains(cell)) rejectTarget();
          const std::array<CellRef, 1> cells{cell};
          rejectPipelines(cells);
          break;
        }
        case AnchorKind::Variable:
          if (functorBindings_.contains(pathExpression.anchor.name))
            rejectTarget();
          if (const auto found =
                  streamBindings_.find(pathExpression.anchor.name);
              found != streamBindings_.end()) {
            rejectPipelines(found->second);
          } else if (const auto found =
                         varBindings_.find(pathExpression.anchor.name);
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
        return;
      }
      if (const auto *name = std::get_if<std::string>(&expression.kind)) {
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
        return;
      }
      rejectTarget();
    };

    validateComputedTarget(target);

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
      if (!isSimpleProjectionPath(expression)) rejectTarget();

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
        break;
      }

    } else if (!std::holds_alternative<ScalarLiteral>(target.kind) &&
               !std::holds_alternative<std::shared_ptr<FunctionInvocation>>(
                   target.kind) &&
               !std::holds_alternative<std::shared_ptr<BinaryValueExpr>>(
                   target.kind)) {
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
