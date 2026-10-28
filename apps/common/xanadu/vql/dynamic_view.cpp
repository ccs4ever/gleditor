#include "common/xanadu/vql/vql_engine.hpp"

#include <algorithm>

namespace xanadu::vql {

bool VQLEngine::bindLens(const LetClause &let) {
  const auto *value = std::get_if<ValueExpr>(&let.target);
  if (!value) return false;
  const auto *call =
      std::get_if<std::shared_ptr<FunctionInvocation>>(&value->kind);
  if (!call || (*call)->name != "lens") return false;
  const auto &fn = **call;
  if (fn.args.size() != 2 || fn.argNames.size() != 2) {
    throw std::runtime_error("lens requires get: and put: callbacks");
  }
  const PathLambda *get = nullptr;
  const PathLambda *put = nullptr;
  for (std::size_t index = 0; index < fn.args.size(); ++index) {
    const auto *lambda =
        std::get_if<std::shared_ptr<PathLambda>>(&fn.args[index].kind);
    if (!lambda || !*lambda) {
      throw std::runtime_error("lens callbacks must be path lambdas");
    }
    if (fn.argNames[index] == "get" && !get)
      get = lambda->get();
    else if (fn.argNames[index] == "put" && !put)
      put = lambda->get();
    else
      throw std::runtime_error(
          "lens requires distinct get: and put: callbacks");
  }
  if (!get || !put || get->params.size() != 1 || put->params.size() != 2) {
    throw std::runtime_error("lens get takes one parameter; put takes two");
  }
  env_.erase(let.varName);
  functors_.erase(let.varName);
  putGetLenses_.insert_or_assign(let.varName, PutGetLens{*get, *put});
  return true;
}

std::vector<zigzag::CellRef>
VQLEngine::applyLens(const PutGetLens &lens, const SignedDimensionStep &step,
                     const std::vector<zigzag::CellRef> &cells) {
  std::vector<zigzag::CellRef> result;
  const auto asArgument = [](zigzag::CellRef cell) {
    return ValueExpr{.kind = std::make_shared<PathExpression>(PathExpression{
                         .anchor = AnchorNode{.kind = AnchorKind::LiteralCellId,
                                              .cellId = cell}})};
  };
  for (auto source : cells) {
    arena().consumeWork();
    if (step.creates.empty()) {
      auto reached = executeLambda(lens.get, source);
      result.insert(result.end(), reached.begin(), reached.end());
      continue;
    }
    for (const auto &create : step.creates) {
      std::optional<zigzag::vortex::CellValue> value;
      if (create.kind == CreateValue::Kind::Literal)
        value = create.literal;
      else if (create.kind == CreateValue::Kind::Expression && create.expr)
        value = evaluateValueExpr(*create.expr, source);
      else
        value = std::string{};
      if (!value) continue;
      const auto replacement = cellFromValue(*value);
      DefDeclaration callback{
          .name = "lens.put", .params = lens.put.params, .body = lens.put.body};
      callUserDef(callback, {asArgument(source), asArgument(replacement)},
                  source);
      result.push_back(source);
    }
  }
  return result;
}

zigzag::CellRef VQLEngine::viewCell(zigzag::CellRef source,
                                    const std::shared_ptr<DynamicView> &view) {
  auto found       = view->handles.find(source);
  const auto value = core_->render(source);
  if (found != view->handles.end() && arena().contains(found->second)) {
    if (value) core_->writeValue(found->second, 0, std::nullopt, *value);
    return found->second;
  }
  const auto handle     = value ? cellFromValue(*value) : arena().makeCell();
  view->handles[source] = handle;
  viewCells_.insert_or_assign(handle, ViewCell{source, view});
  return handle;
}

std::vector<zigzag::CellRef>
VQLEngine::projectDynamic(const ShapeLiteral &shape,
                          const std::vector<zigzag::CellRef> &cells) {
  for (const auto &rule : shape.rules) {
    if (const auto *dimension = std::get_if<DimensionShapeRule>(&rule.rule);
        dimension && dimension->targetValue) {
      throw std::runtime_error(
          "dynamic projections cannot mix eager dimension assignments");
    }
  }
  auto view   = std::make_shared<DynamicView>();
  view->shape = shape;
  std::vector<zigzag::CellRef> result;
  for (auto cell : cells) {
    arena().consumeWork();
    result.push_back(viewCell(cell, view));
  }
  return result;
}

std::optional<std::vector<zigzag::CellRef>>
VQLEngine::traverseView(const SignedDimensionStep &step, zigzag::CellRef cell) {
  const auto found = viewCells_.find(cell);
  if (found == viewCells_.end()) return std::nullopt;
  // Recursive evaluation can grow the handle map, invalidating iterators.
  const auto source = found->second.source;
  const auto view   = found->second.view;
  auto physical     = step;
  bool allowed =
      std::ranges::none_of(view->shape.rules, [](const ShapeRule &rule) {
        return std::holds_alternative<ShapeWildcard>(rule.rule) ||
               std::holds_alternative<DimensionShapeRule>(rule.rule);
      });
  const VirtualDimensionDefRule *computed = nullptr;
  for (const auto &rule : view->shape.rules) {
    if (const auto *wildcard = std::get_if<ShapeWildcard>(&rule.rule)) {
      allowed = *wildcard == ShapeWildcard::Everything;
    } else if (const auto *mask = std::get_if<DimensionShapeRule>(&rule.rule)) {
      if (mask->dimName == step.dimName &&
          (mask->isBidir || mask->direction == step.direction)) {
        allowed = !mask->negated;
      }
    } else if (const auto *pivot =
                   std::get_if<DimensionPivotRule>(&rule.rule)) {
      if (pivot->fromDim == step.dimName) {
        physical.dimName = pivot->toDim;
        allowed          = true;
      }
    } else if (const auto *virt =
                   std::get_if<VirtualDimensionDefRule>(&rule.rule)) {
      if (virt->dimName == step.dimName && virt->direction == step.direction) {
        computed = virt;
        allowed  = true;
      }
    }
  }
  if (!allowed) return std::vector<zigzag::CellRef>{};
  if (computed) {
    if (!step.creates.empty()) {
      throw std::runtime_error("computed dimension has no put transform: " +
                               step.dimName);
    }
    if (!computed->expr) return std::vector<zigzag::CellRef>{};
    // Evaluate against the source, so a computed dimension cannot recursively
    // resolve itself through its own projection.
    auto value = evaluateValueExpr(*computed->expr, source);
    if (!value) return std::vector<zigzag::CellRef>{};
    return std::vector<zigzag::CellRef>{cellFromValue(*value)};
  }
  auto cells = physical.creates.empty() ? traverseDimension(physical, {source})
                                        : performCreates(physical, {source});
  for (auto &target : cells) target = viewCell(target, view);
  return cells;
}

} // namespace xanadu::vql
