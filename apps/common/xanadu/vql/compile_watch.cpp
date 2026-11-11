// Q4: watch statements, scoped perspective blocks, mount declarations.
//
// A `watch` statement is a snapshot with no live observation unless a `since`
// baseline backed by store history says what changed (R-WATCH). Live
// observation is VQLEngine::subscribeWatch / pollWatch. A scoped perspective
// block is sugar for writing `@name:` on every step in it, so it lowers by
// qualifying the steps and compiling them as explicit perspective steps; the
// ambient selection therefore cannot outlive the block, on success or failure.
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vql/compiler.hpp"

namespace xanadu::vql {
using zigzag::CellRef;
using zigzag::vortex::OpcodeKind;

namespace {

constexpr std::string_view pollingApi =
    "use VQLEngine::subscribeWatch and pollWatch to observe changes";

[[noreturn]] void refuseUnsupported(const std::string &message) {
  throw QueryRefusal(QueryErrorCategory::Unsupported, message);
}

/// Store anchors and generators that name this process's own stores. Every
/// other name needs a ZZ9P session, which a compiled program cannot open.
bool isLocalStoreName(const std::string_view name) {
  return name == "current" || name == "local" || name == "system";
}

std::string normalizePerspective(std::string name) {
  if (name.starts_with('@')) name.erase(0, 1);
  if (name.ends_with(':')) name.pop_back();
  return name;
}

/// Rewrites the steps written directly in a perspective block into explicit
/// `@name:` qualifiers. Creation steps keep the canonical subject: the
/// direct evaluator never routed them through a perspective either.
void qualifyPath(PathExpression &path, const std::string &perspective) {
  if (!path.anchorPredicates.empty() || path.anchorProjection ||
      path.shapeProjection || path.cloneTail)
    refuseUnsupported(
        "a perspective block cannot qualify anchor predicates, projections or "
        "a clone tail; write the perspective qualifier on the steps that need "
        "it");
  for (auto &step : path.steps) {
    if (!step.predicates.empty() || !step.shapePredicates.empty() ||
        step.projectionAfter || step.capturedFunctor)
      refuseUnsupported(
          "a perspective block cannot qualify a step that has predicates, a "
          "projection or a captured functor; write the perspective qualifier "
          "on the steps that need it");
    if (auto *morphism = std::get_if<PathMorphism>(&step.selector)) {
      const bool literalTargets =
          std::ranges::all_of(morphism->targets, [](const ValueExpr &target) {
            return std::holds_alternative<ScalarLiteral>(target.kind);
          });
      if (!literalTargets)
        refuseUnsupported(
            "a perspective block cannot qualify a morphism whose targets are "
            "computed; write the perspective qualifier on the morphism");
      if (morphism->perspective.empty()) morphism->perspective = perspective;
      continue;
    }
    if (std::holds_alternative<PerspectiveStep>(step.selector) ||
        std::holds_alternative<GenesisSelector>(step.selector) ||
        std::holds_alternative<RootSelectorStep>(step.selector))
      continue;
    const auto *dimension = std::get_if<SignedDimensionStep>(&step.selector);
    if (dimension == nullptr || dimension->dimName.starts_with('$'))
      refuseUnsupported(
          "a perspective block cannot qualify this kind of step (function "
          "call, document or slice selector, lambda, functor alias, macro "
          "group, virtual dimension or pivot); write the perspective "
          "qualifier explicitly");
    if (!dimension->creates.empty()) {
      const bool computed = std::ranges::any_of(
          dimension->creates, [](const CreateValue &create) {
            return create.kind == CreateValue::Kind::Expression;
          });
      if (computed)
        refuseUnsupported("a perspective block cannot qualify a creation "
                          "with a computed value");
      continue;
    }
    auto inner = std::make_shared<PathStep>(std::move(step));
    step = PathStep{.selector = PerspectiveStep{.perspective = perspective,
                                                .targetStep  = inner}};
  }
}

Statement qualifyStatement(const Statement &statement,
                           const std::string &perspective) {
  if (const auto *path = std::get_if<PathExpression>(&statement)) {
    auto qualified = *path;
    qualifyPath(qualified, perspective);
    return qualified;
  }
  if (const auto *weave = std::get_if<WeaveStatement>(&statement)) {
    auto qualified = *weave;
    for (auto &item : qualified.effect.items) {
      auto *effect = std::get_if<PathExpression>(&item.item);
      if (effect == nullptr)
        refuseUnsupported(
            "a perspective block over a weave with let or for items is not "
            "compiled yet");
      qualifyPath(*effect, perspective);
    }
    return qualified;
  }
  if (std::holds_alternative<ExecutionBlock>(statement))
    refuseUnsupported("a perspective block over a for, let or return block is "
                      "not compiled yet; qualify its paths with @name:");
  if (std::holds_alternative<WatchStatement>(statement))
    refuseUnsupported("a watch statement inside a perspective block is not "
                      "compiled: the block cannot qualify the watched path");
  // A nested block selects its own perspective instead of composing.
  return statement;
}

} // namespace

std::optional<CellRef>
VQLCompiler::compileWatchOrPerspectiveStatement(const Statement &statement) {
  std::function<std::optional<CellRef>(const Statement &)> compileStatement;

  const auto lowerWatch =
      [&](const WatchStatement &watch) -> std::optional<CellRef> {
    if (watch.targetPath.anchor.kind == AnchorKind::StoreGenerator &&
        watch.targetPath.anchor.generator &&
        !isLocalStoreName(watch.targetPath.anchor.generator->opName))
      refuseUnsupported("watch over remote discovery '" +
                        watch.targetPath.anchor.generator->opName +
                        "' requires a connected ZZ9P session");
    if (!watch.sinceVersion)
      refuseUnsupported(
          "a watch statement needs a 'since' baseline backed by store history "
          "to say what changed; without one it would only be a snapshot with "
          "no baseline (" +
          std::string(pollingApi) + ")");
    if (!coordinator_ || coordinator_->stores().empty())
      refuseUnsupported(
          "watch since requires a target backed by a local Store; this arena "
          "has no store-backed history to take the baseline from (" +
          std::string(pollingApi) + ")");
    // Waiting on the history card: the baseline must pin the whole watched
    // path once and compare it with the current path (membership, rendered
    // value and topology). A per-cell existence test would miss a cell that
    // existed but was not yet on the path, so nothing is emitted.
    refuseUnsupported(
        "watch since requires a path-level version pin, which is not lowered "
        "yet (" +
        std::string(pollingApi) + ")");
  };

  const auto lowerPerspective = [&](const PerspectiveBlock &block) {
    const auto name = normalizePerspective(block.perspectiveName);
    std::optional<CellRef> result;
    lastResultCells_.clear();
    if (block.body) {
      for (const auto &inner : block.body->statements) {
        lastResultCells_.clear();
        result = compileStatement(qualifyStatement(inner, name));
      }
    }
    return result;
  };

  compileStatement = [&](const Statement &stmt) -> std::optional<CellRef> {
    if (const auto *path = std::get_if<PathExpression>(&stmt))
      return compilePathExpression(*path, core_.home());
    if (const auto *block = std::get_if<ExecutionBlock>(&stmt))
      return compileExecutionBlock(*block);
    if (const auto *weave = std::get_if<WeaveStatement>(&stmt))
      return compileExecutionBlock(
          ExecutionBlock{.action = ActionClause{.clause = weave->effect}});
    if (const auto *watch = std::get_if<WatchStatement>(&stmt))
      return lowerWatch(*watch);
    return lowerPerspective(std::get<PerspectiveBlock>(stmt));
  };

  return compileStatement(statement);
}

void VQLCompiler::compileMountDeclarations(const QueryExpression &query) {
  const auto refuse = [](const std::string &method, const std::string &dim) {
    if (!isLocalStoreName(method) && !method.empty())
      refuseUnsupported("mount over remote store anchor '" + method + "' as " +
                        dim + " requires a connected ZZ9P session");
    refuseUnsupported(
        "mount " + dim +
        " has no compiled dispatch: dimension steps cannot consult a "
        "session mount table yet, and a mount that resolves nothing would "
        "silently read as an empty dimension");
  };
  for (const auto &mount : query.mounts)
    refuse(mount.storeAnchor.method, mount.dimName);
  for (const auto &declaration : query.namespaceDeclarations) {
    if (const auto *mount = std::get_if<MountDeclaration>(&declaration))
      refuse(mount->storeAnchor.method, mount->dimName);
    else
      refuseUnsupported(
          "unmount " + std::get<UnmountDeclaration>(declaration).dimName +
          " has no compiled dispatch: no mount table exists to remove it "
          "from");
  }
}

} // namespace xanadu::vql
