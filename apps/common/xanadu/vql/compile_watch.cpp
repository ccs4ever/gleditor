// Q4: watch statements, scoped perspective blocks, mount declarations.
//
// A `watch` statement is a snapshot with no live observation unless a `since`
// baseline backed by store history says what changed (R-WATCH). Live
// observation is VQLEngine::subscribeWatch / pollWatch. A scoped perspective
// block is sugar for writing `@name:` on every step in it, so it lowers by
// qualifying the steps and compiling them as explicit perspective steps; the
// ambient selection therefore cannot outlive the block, on success or failure.
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

template <typename Action> struct RestoreScope {
  Action action;
  ~RestoreScope() { action(); }
};

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
  if (!path.anchorPredicates.empty())
    refuseUnsupported(
        "a perspective block cannot qualify anchor predicates; write the "
        "perspective qualifier on the steps that need it");
  for (auto &step : path.steps) {
    if (auto *morphism = std::get_if<PathMorphism>(&step.selector)) {
      if (morphism->perspective.empty()) morphism->perspective = perspective;
      continue;
    }
    const auto *dimension = std::get_if<SignedDimensionStep>(&step.selector);
    if (dimension == nullptr || dimension->dimName.starts_with('$') ||
        !dimension->creates.empty())
      continue;
    if (!step.predicates.empty() || !step.shapePredicates.empty())
      refuseUnsupported(
          "a perspective block cannot qualify a step that has predicates; "
          "write the perspective qualifier on the steps that need it");
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
  // A nested block selects its own perspective, and a watch statement refuses
  // on its own terms; neither composes with the enclosing name.
  return statement;
}

} // namespace

std::optional<CellRef>
VQLCompiler::compileWatchOrPerspectiveStatement(const Statement &statement) {
  std::function<std::optional<CellRef>(const Statement &)> compileStatement;

  const auto lowerWatch = [&](const WatchStatement &watch) {
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
    if (watch.targetPath.anchor.versionPin)
      refuseUnsupported(
          "watch since cannot be combined with a pinned path anchor");
    if (watch.asPattern)
      refuseUnsupported(
          "watch with a destructuring pattern is not compiled yet; bind a "
          "variable with 'as $name'");

    compilePathExpression(watch.targetPath, core_.home());
    const auto current = lastResultCells_;

    // A cell is changed when it has no existence at the baseline version. The
    // pin on a context anchor is lowered by the history card (Q3): until it
    // lands, the pinned lookup refuses by name and so does this statement.
    const CellRef changed = emitStreamClear();
    const CellRef begin   = emitOp(OpcodeKind::StreamBegin, "#WATCH_BEGIN");
    for (const auto cell : current) emitInput(begin, cell);
    const CellRef scan = core_.arena().makeCell();
    emitOutput(begin, scan);
    emitIteratorLoop(scan, [&](const CellRef item) {
      const auto savedContext = ambientContext_;
      const RestoreScope restoreContext{
          [&] { ambientContext_ = savedContext; }};
      ambientContext_ = item;
      PathExpression origin;
      origin.anchor.kind       = AnchorKind::Context;
      origin.anchor.versionPin = VersionPin{.versions = {*watch.sinceVersion}};
      compilePathExpression(origin, item);
      const CellRef existed = emitStreamPresence(lastResultCells_);
      const auto add        = mintOpcode(OpcodeKind::Nop, "#WATCH_ADD");
      const auto done       = mintOpcode(OpcodeKind::Nop, "#WATCH_SCAN_END");
      const auto choose = emitOp(OpcodeKind::BranchFalseCell, "#WATCH_EXISTED");
      emitInput(choose, existed);
      emitInput(choose, add);
      const auto skip = emitOp(OpcodeKind::BranchFalseCell, "#WATCH_SKIP");
      emitInputConstant(skip, false);
      emitInput(skip, done);
      appendOpcode(add);
      emitAppendStream(changed, std::span{&item, std::size_t{1}});
      appendOpcode(done);
    });

    const CellRef results = emitStreamClear();
    const CellRef stopped = emitStreamClear();
    const CellRef visit = emitOp(OpcodeKind::StreamBegin, "#WATCH_VISIT_BEGIN");
    emitInput(visit, changed);
    const CellRef iterator = core_.arena().makeCell();
    emitOutput(visit, iterator);
    emitIteratorLoop(iterator, [&](const CellRef item) {
      auto variables     = varBindings_;
      auto streams       = streamBindings_;
      auto functors      = functorBindings_;
      const auto context = ambientContext_;
      const RestoreScope restore{[&] {
        varBindings_     = std::move(variables);
        streamBindings_  = std::move(streams);
        functorBindings_ = std::move(functors);
        ambientContext_  = context;
      }};
      ambientContext_ = item;
      if (watch.asVar) {
        functorBindings_.erase(*watch.asVar);
        streamBindings_[*watch.asVar] = {item};
        varBindings_[*watch.asVar]    = item;
      }

      // `until` ends the walk after the cell that satisfied it: later cells
      // are skipped by a flag the loop cannot clear.
      const CellRef halted = emitStreamPresence(std::span{&stopped, 1});
      const auto run       = mintOpcode(OpcodeKind::Nop, "#WATCH_RUN");
      const auto end       = mintOpcode(OpcodeKind::Nop, "#WATCH_VISIT_END");
      const auto proceed = emitOp(OpcodeKind::BranchFalseCell, "#WATCH_HALTED");
      emitInput(proceed, halted);
      emitInput(proceed, run);
      const auto leave = emitOp(OpcodeKind::BranchFalseCell, "#WATCH_LEAVE");
      emitInputConstant(leave, false);
      emitInput(leave, end);
      appendOpcode(run);

      if (watch.body) {
        for (const auto &bodyStatement : watch.body->statements) {
          lastResultCells_.clear();
          compileStatement(bodyStatement);
          emitAppendStream(results, lastResultCells_);
        }
      }
      if (watch.untilCondition) {
        if (const auto satisfied =
                compileBooleanExpr(*watch.untilCondition, item)) {
          const auto keepGoing = mintOpcode(OpcodeKind::Nop, "#WATCH_UNTIL_NO");
          const auto test = emitOp(OpcodeKind::BranchFalseCell, "#WATCH_UNTIL");
          emitInput(test, *satisfied);
          emitInput(test, keepGoing);
          const CellRef stop = emitConstant(true);
          emitAppendStream(stopped, std::span{&stop, std::size_t{1}});
          appendOpcode(keepGoing);
        }
      }
      appendOpcode(end);
    });

    // A body that yields nothing reports the changed cells themselves.
    const CellRef produced = emitStreamPresence(std::span{&results, 1});
    const auto fill        = mintOpcode(OpcodeKind::Nop, "#WATCH_FILL");
    const auto finish      = mintOpcode(OpcodeKind::Nop, "#WATCH_END");
    const auto fillTest = emitOp(OpcodeKind::BranchFalseCell, "#WATCH_EMPTY");
    emitInput(fillTest, produced);
    emitInput(fillTest, fill);
    const auto jump = emitOp(OpcodeKind::BranchFalseCell, "#WATCH_FILLED");
    emitInputConstant(jump, false);
    emitInput(jump, finish);
    appendOpcode(fill);
    emitAppendStream(results, std::span{&changed, std::size_t{1}});
    appendOpcode(finish);

    lastResultCells_ = {results};
    return std::optional<CellRef>{results};
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
