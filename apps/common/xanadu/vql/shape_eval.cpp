/**
 * @file shape_eval.cpp
 * @brief First-class VQL shape values, predicates, and local projections.
 */
#include "common/xanadu/vql/invocation.hpp"
#include "common/xanadu/vql/vql_engine.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <unordered_set>

namespace xanadu::vql {

zigzag::CellRef VQLEngine::makeShapeHandle(ShapeLiteral shape) {
  const auto handle = arena().makeCell();
  shapeHandles_.emplace(handle, std::move(shape));
  return handle;
}

std::optional<ShapeLiteral>
VQLEngine::shapeFromHandle(const zigzag::CellRef handle) const {
  const auto found = shapeHandles_.find(handle);
  return found == shapeHandles_.end() ? std::nullopt
                                      : std::optional{found->second};
}

ShapeLiteral
VQLEngine::expandShape(const ShapeLiteral &shape,
                       const std::optional<zigzag::CellRef> context) {
  ShapeLiteral result;
  std::unordered_set<zigzag::CellRef> visited;
  std::function<void(const ShapeLiteral &)> append;
  const auto appendHandle = [&](const zigzag::CellRef handle) {
    if (!visited.insert(handle).second) return;
    if (const auto nested = shapeFromHandle(handle)) append(*nested);
    visited.erase(handle);
  };
  append = [&](const ShapeLiteral &source) {
    for (const auto &rule : source.rules) {
      arena().consumeWork();
      if (const auto *name = std::get_if<std::string>(&rule.rule)) {
        const auto found = env_.find(*name);
        if (found == env_.end())
          throw TypeContractError(
              std::format("shape variable {} is not bound", *name));
        bool foundShape = false;
        for (const auto cell : found->second) {
          if (!shapeHandles_.contains(cell)) continue;
          foundShape = true;
          appendHandle(cell);
        }
        if (!foundShape)
          throw TypeContractError(
              std::format("variable {} does not contain a Shape value", *name));
        continue;
      }
      if (const auto *nested =
              std::get_if<std::shared_ptr<ShapeLiteral>>(&rule.rule);
          nested && *nested) {
        append(**nested);
        continue;
      }
      if (const auto *fn =
              std::get_if<std::shared_ptr<FunctionInvocation>>(&rule.rule);
          fn && *fn) {
        const auto found = userDefs_.find((*fn)->name);
        if (found == userDefs_.end())
          throw TypeContractError(
              std::format("shape function '{}' is not defined", (*fn)->name));
        requirePositionalArguments(**fn);
        bool foundShape = false;
        for (const auto cell :
             callUserDef(found->second, (*fn)->args, context)) {
          if (shapeHandles_.contains(cell)) {
            foundShape = true;
            appendHandle(cell);
          }
        }
        if (!foundShape)
          throw TypeContractError(std::format(
              "function '{}' did not return a Shape value", (*fn)->name));
        continue;
      }
      result.rules.push_back(rule);
    }
  };
  append(shape);
  return result;
}

std::optional<ShapeLiteral>
VQLEngine::shapeValue(const ValueExpr &expr,
                      const std::optional<zigzag::CellRef> context) {
  if (const auto *literal =
          std::get_if<std::shared_ptr<ShapeLiteral>>(&expr.kind)) {
    return *literal ? std::optional{expandShape(**literal, context)}
                    : std::nullopt;
  }
  std::vector<zigzag::CellRef> handles;
  if (const auto *name = std::get_if<std::string>(&expr.kind)) {
    if (const auto found = env_.find(*name); found != env_.end())
      for (const auto cell : found->second)
        if (shapeHandles_.contains(cell)) handles.push_back(cell);
  } else if (const auto *fn =
                 std::get_if<std::shared_ptr<FunctionInvocation>>(&expr.kind);
             fn && *fn) {
    if (const auto found = userDefs_.find((*fn)->name);
        found != userDefs_.end()) {
      const auto *body =
          std::get_if<std::shared_ptr<ValueExpr>>(&found->second.body);
      if (body && *body &&
          std::holds_alternative<std::shared_ptr<ShapeLiteral>>(
              (*body)->kind)) {
        requirePositionalArguments(**fn);
        for (const auto cell : callUserDef(found->second, (*fn)->args, context))
          if (shapeHandles_.contains(cell)) handles.push_back(cell);
      }
    }
  }
  if (handles.empty()) return std::nullopt;
  ShapeLiteral result;
  for (const auto handle : handles)
    if (const auto nested = shapeFromHandle(handle))
      result.rules.insert(result.rules.end(), nested->rules.begin(),
                          nested->rules.end());
  return expandShape(result, context);
}

std::pair<zigzag::DimRef, zigzag::DimRef>
VQLEngine::shapeAxis(const zigzag::CellRef cell, const std::string_view name) {
  const auto arenaDim = resolveDimension(name);
  const auto foreign  = arena().foreignOf(cell);
  if (!foreign) return {arenaDim, arenaDim};
  const auto space = arena().spaceAt(foreign->space);
  if (!space || !space->manifold || !space->reader) return {arenaDim, arenaDim};
  auto foreignDim = arena().dimIn(foreign->space, arenaDim);
  if (!foreignDim)
    foreignDim = space->manifold->dimensionNamed(name, *space->reader);
  if (!foreignDim) return {arenaDim, arenaDim};
  arena().bindDimension(arenaDim, foreign->space, *foreignDim);
  return {arenaDim, *foreignDim};
}

zigzag::DimRef VQLEngine::arenaDimensionFor(const zigzag::CellRef cell,
                                            const zigzag::DimRef foreignDim) {
  const auto foreign = arena().foreignOf(cell);
  if (!foreign) return foreignDim;
  if (const auto bound = arena().arenaDimFor(foreign->space, foreignDim))
    return *bound;
  const auto space = arena().spaceAt(foreign->space);
  if (!space || !space->manifold || !space->reader) return foreignDim;
  const auto name = space->manifold->textOf(foreignDim, *space->reader);
  if (name.empty()) return foreignDim;
  const auto arenaDim = resolveDimension(name);
  arena().bindDimension(arenaDim, foreign->space, foreignDim);
  return arenaDim;
}

namespace {
using DirectionMask = std::array<std::optional<bool>, 2>;
using ShapeMasks    = std::unordered_map<zigzag::DimRef, DirectionMask>;

std::size_t directionIndex(const zigzag::DimVector direction) {
  return direction == zigzag::DimVector::POS ? 0U : 1U;
}
} // namespace

bool VQLEngine::evaluateShapePredicate(
    const ShapeLiteral &shape, const std::optional<zigzag::CellRef> context) {
  if (!context || !arena().contains(*context)) return false;
  const auto expanded = expandShape(shape, context);
  ShapeMasks dimensions;
  bool includeEverything = false;
  bool exact             = expanded.rules.empty();
  bool sawWildcard       = false;
  for (const auto &rule : expanded.rules) {
    if (const auto *wildcard = std::get_if<ShapeWildcard>(&rule.rule)) {
      sawWildcard       = true;
      includeEverything = *wildcard == ShapeWildcard::Everything;
      exact             = *wildcard == ShapeWildcard::Nothing;
      dimensions.clear();
    } else if (const auto *dimension =
                   std::get_if<DimensionShapeRule>(&rule.rule)) {
      const auto axis = shapeAxis(*context, dimension->dimName);
      auto &mask      = dimensions[axis.second];
      if (dimension->negated)
        mask = {false, false};
      else if (dimension->isBidir)
        mask = {true, true};
      else
        mask[directionIndex(dimension->direction)] = true;
    }
  }
  const auto links   = arena().dimensionsOf(*context);
  bool hasConnection = false;
  for (const auto &link : links)
    hasConnection |= link.pos != zigzag::noCell || link.neg != zigzag::noCell;
  if (sawWildcard && includeEverything && !hasConnection) return false;

  for (const auto &rule : expanded.rules) {
    const auto *dimension = std::get_if<DimensionShapeRule>(&rule.rule);
    if (!dimension) continue;
    const auto axis = shapeAxis(*context, dimension->dimName);
    const auto dim  = axis.first;
    const auto pos =
        arena().linkedMinting(*context, dim, zigzag::DimVector::POS);
    const auto neg =
        arena().linkedMinting(*context, dim, zigzag::DimVector::NEG);
    if (dimension->negated) {
      if (pos || neg) return false;
      continue;
    }
    std::vector<zigzag::CellRef> candidates;
    if ((dimension->isBidir ||
         dimension->direction == zigzag::DimVector::POS) &&
        pos)
      candidates.push_back(*pos);
    if ((dimension->isBidir ||
         dimension->direction == zigzag::DimVector::NEG) &&
        neg)
      candidates.push_back(*neg);
    if (candidates.empty()) return false;
    if (dimension->targetValue) {
      const auto nested = shapeValue(*dimension->targetValue, context);
      if (nested) {
        if (!std::ranges::any_of(candidates, [&](const auto neighbor) {
              return evaluateShapePredicate(*nested, neighbor);
            }))
          return false;
      } else {
        const auto expected =
            evaluateValueExpr(*dimension->targetValue, context);
        if (!expected ||
            !std::ranges::any_of(candidates, [&](const auto neighbor) {
              const auto actual = core().render(neighbor);
              return actual && compareValues(*actual, CompOp::Equal, *expected);
            }))
          return false;
      }
    }
  }
  if (exact) {
    for (const auto &link : links) {
      const auto found = dimensions.find(link.dim);
      for (const auto direction :
           {zigzag::DimVector::POS, zigzag::DimVector::NEG}) {
        if (!link.neighbor(direction)) continue;
        const auto index = directionIndex(direction);
        const bool allowed =
            found != dimensions.end() && found->second[index].has_value()
                ? *found->second[index]
                : includeEverything;
        if (!allowed) return false;
      }
    }
  }
  return true;
}

void VQLEngine::applyShapeProjection(const ShapeLiteral &shape,
                                     const zigzag::CellRef cell) {
  ShapeMasks dimensions;
  bool includeEverything = false;
  for (const auto &rule : shape.rules) {
    if (const auto *wildcard = std::get_if<ShapeWildcard>(&rule.rule)) {
      includeEverything = *wildcard == ShapeWildcard::Everything;
      dimensions.clear();
    } else if (const auto *dimension =
                   std::get_if<DimensionShapeRule>(&rule.rule)) {
      const auto axis = shapeAxis(cell, dimension->dimName);
      auto &mask      = dimensions[axis.second];
      if (dimension->negated)
        mask = {false, false};
      else if (dimension->isBidir)
        mask = {true, true};
      else
        mask[directionIndex(dimension->direction)] = true;
    }
  }
  const auto original = arena().dimensionsOf(cell);
  const std::vector<zigzag::DimLink> links(original.begin(), original.end());
  for (const auto &link : links) {
    const auto found = dimensions.find(link.dim);
    for (const auto direction :
         {zigzag::DimVector::POS, zigzag::DimVector::NEG}) {
      if (!link.neighbor(direction)) continue;
      const auto index = directionIndex(direction);
      const bool allowed =
          found != dimensions.end() && found->second[index].has_value()
              ? *found->second[index]
              : includeEverything;
      if (!allowed)
        zigzag::expectWritten(
            arena().unlink(cell, arenaDimensionFor(cell, link.dim), direction));
    }
  }

  for (const auto &rule : shape.rules) {
    const auto *dimension = std::get_if<DimensionShapeRule>(&rule.rule);
    if (!dimension || dimension->negated || !dimension->targetValue) continue;
    const auto dim = shapeAxis(cell, dimension->dimName).first;
    auto neighbor  = arena().linkedMinting(cell, dim, dimension->direction);
    if (const auto nested = shapeValue(*dimension->targetValue, cell)) {
      if (!neighbor) neighbor = arena().makeCell();
      zigzag::expectWritten(
          arena().link(cell, dim, dimension->direction, *neighbor));
      applyShapeProjection(*nested, *neighbor);
      continue;
    }
    std::vector<zigzag::CellRef> targets;
    if (const auto *path = std::get_if<std::shared_ptr<PathExpression>>(
            &dimension->targetValue->kind);
        path && *path) {
      targets = evaluatePath(**path, {cell});
    } else if (const auto *name =
                   std::get_if<std::string>(&dimension->targetValue->kind)) {
      if (const auto found = env_.find(*name); found != env_.end())
        targets = found->second;
    } else if (const auto *fn =
                   std::get_if<std::shared_ptr<FunctionInvocation>>(
                       &dimension->targetValue->kind);
               fn && *fn) {
      if (const auto found = userDefs_.find((*fn)->name);
          found != userDefs_.end()) {
        requirePositionalArguments(**fn);
        targets = callUserDef(found->second, (*fn)->args, cell);
      }
    }
    if (!targets.empty()) neighbor = targets.front();
    if (!neighbor) {
      if (const auto value = evaluateValueExpr(*dimension->targetValue, cell))
        neighbor = cellFromValue(*value);
    }
    if (neighbor)
      zigzag::expectWritten(
          arena().link(cell, dim, dimension->direction, *neighbor));
  }
}

} // namespace xanadu::vql
