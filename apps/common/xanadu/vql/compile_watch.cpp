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
#include "common/xanadu/zigzag/query_clone_master.hpp"

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
    auto name = block.perspectiveName;
    if (block.perspectiveIsVariable) {
      // perspective($p) names the perspective held in $p, not one called "p".
      // The qualifier is part of the emitted steps, so the value has to be
      // known while compiling.
      const auto bound = varBindings_.find(block.perspectiveName);
      const auto value = bound != varBindings_.end() && bound->second
                             ? core_.render(*bound->second)
                             : std::nullopt;
      if (!value || !std::holds_alternative<std::string>(*value))
        refuseUnsupported("perspective($" + block.perspectiveName +
                          ") needs a variable bound to a text constant while "
                          "the query is compiled; a run-time perspective name "
                          "is not lowered");
      name = std::get<std::string>(*value);
    }
    name = normalizePerspective(std::move(name));
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
  const auto literalText =
      [](const StoreAnchor &anchor) -> std::optional<std::string> {
    if (anchor.args.empty()) return std::nullopt;
    const auto *literal = std::get_if<ScalarLiteral>(&anchor.args.front().kind);
    if (literal == nullptr)
      refuseUnsupported(
          "a mount anchor argument must be a text literal: the mount roots are "
          "resolved while the query is compiled");
    const auto *text = std::get_if<std::string>(&literal->value);
    if (text == nullptr)
      refuseUnsupported("a mount anchor argument must be a text literal");
    return *text;
  };
  // The oracle's resolveStoreAnchor for the anchors that name this process's
  // own stores; the roots are cells that already exist, so a mount mints none.
  const auto resolveRoots =
      [&](const StoreAnchor &anchor) -> std::vector<CellRef> {
    std::vector<CellRef> roots;
    if (!coordinator_) return roots;
    const auto &method = anchor.method;
    const auto text    = literalText(anchor);
    if (method == "current") {
      if (const auto home = coordinator_->homeAnchor()) roots.push_back(*home);
    } else if (method == "local" && text) {
      for (const auto &info : coordinator_->stores()) {
        if (info.path != *text && info.label != *text) continue;
        if (info.homeCell) roots.push_back(*info.homeCell);
        break;
      }
    } else if (method == "system" && text) {
      if (const auto home = coordinator_->resolveNamedStore(*text))
        roots.push_back(*home);
    } else if (method.empty() && !anchor.sliceName.empty()) {
      if (const auto home = coordinator_->resolveNamedStore(anchor.sliceName))
        roots.push_back(*home);
    }
    if (method.empty() && roots.empty() && text) {
      if (const auto home = coordinator_->resolveNamedStore(*text))
        roots.push_back(*home);
    }
    if (method.empty() && roots.empty()) {
      if (const auto home = coordinator_->homeAnchor()) roots.push_back(*home);
    }
    if (anchor.derefMaster) {
      for (auto &root : roots)
        root = zigzag::queryCloneMaster(core_.arena(), root, core_.dims().clone)
                   .value_or(root);
    }
    return roots;
  };
  const auto qualified = [](const std::string &name) {
    return name.starts_with("d.") ? name : "d." + name;
  };
  const auto mount = [&](const MountDeclaration &declaration) {
    const auto &method = declaration.storeAnchor.method;
    if (!isLocalStoreName(method) && !method.empty())
      refuseUnsupported("mount over remote store anchor '" + method + "' as " +
                        declaration.dimName +
                        " requires a connected ZZ9P session");
    auto roots = resolveRoots(declaration.storeAnchor);
    if (roots.empty())
      throw QueryRefusal(QueryErrorCategory::Runtime,
                         "mount anchor for " + declaration.dimName +
                             " did not resolve in the local coordinator");
    CompilationOptions::Mount entry{.direction = declaration.direction,
                                    .roots     = roots};
    mounts_[declaration.dimName]            = entry;
    mounts_[qualified(declaration.dimName)] = std::move(entry);
    mountBindings_.push_back(CompilationResult::SessionBinding{
        .name      = declaration.dimName,
        .kind      = CompilationResult::SessionBinding::Kind::Mount,
        .slots     = std::move(roots),
        .direction = declaration.direction});
  };
  const auto unmount = [&](const UnmountDeclaration &declaration) {
    const auto canonical = qualified(declaration.dimName);
    const auto shortName = canonical.substr(2);
    const auto found     = mounts_.find(canonical);
    if (found == mounts_.end() ||
        found->second.direction != declaration.direction)
      throw QueryRefusal(QueryErrorCategory::Runtime,
                         "no mount is registered for " + canonical +
                             " in the requested direction");
    mounts_.erase(canonical);
    mounts_.erase(shortName);
    mountBindings_.push_back(CompilationResult::SessionBinding{
        .name      = declaration.dimName,
        .kind      = CompilationResult::SessionBinding::Kind::Unmount,
        .direction = declaration.direction});
  };
  // As the oracle ran them: the ordered declarations when there are any,
  // otherwise the legacy mount list.
  if (query.namespaceDeclarations.empty()) {
    for (const auto &declaration : query.mounts) mount(declaration);
    return;
  }
  for (const auto &declaration : query.namespaceDeclarations) {
    if (const auto *entry = std::get_if<MountDeclaration>(&declaration))
      mount(*entry);
    else
      unmount(std::get<UnmountDeclaration>(declaration));
  }
}

} // namespace xanadu::vql
