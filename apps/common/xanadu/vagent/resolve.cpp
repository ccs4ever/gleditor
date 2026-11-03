#include "common/xanadu/vagent/resolve.hpp"

#include "common/xanadu/vagent/cell_path.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xanadu::vagent {
namespace {

bool comparison(std::string_view op) {
  return op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" ||
         op == ">=";
}

// One environment instance, mirroring the checker's `Environment` map: every
// copyEnvironment in its infer() — block and branch bodies, `not`, recovery,
// contract and goal-clause locals — is a new scope id over a copied binding
// table, so a binding minted in a child can never leak to its parent. A
// table entry is the binding's identity — the scope that minted it plus its
// ordinal there — and an Identifier use receives it as its stamp verbatim.
struct Scope {
  std::uint64_t id{};
  std::map<std::string, ScopeStamp, std::less<>> bindings;
  std::uint64_t nextOrdinal{0};
};

// The pieces of the checker's Context the environment walk can observe:
// `owner` is the declaration whose members a nested name resolves against
// (plan goal lookup, adopt/achieve goal instantiation); `discard` is the
// value-position flag the if-type rule reads — it decides whether an
// alternation arm can be bottom, which decides whether a `|` arm's bindings
// survive.
struct WalkContext {
  const Decl *owner{};
  bool discard{false};
};

class Resolver {
public:
  Resolver(Program &program, ResolveLimits limits)
      : program_(program), limits_(limits) {}
  std::expected<void, std::vector<Diagnostic>> run();

private:
  void work(SourceLocation loc, std::size_t amount = 1) const;
  std::uint64_t freshId() { return ++lastScopeId_; }
  Scope copy(const Scope &scope, SourceLocation loc);
  ScopeStamp &mint(Scope &scope, const std::string &name, SourceLocation loc);
  void buildIndexes();
  const Scope &ownerScope(const Decl *owner);
  Decl *global(std::string_view name) const;
  Decl *member(const Decl *owner, std::string_view name, DeclKind kind) const;
  Decl *goal(const Decl *owner, std::string_view name) const;
  bool definitelyBottom(const Expr &expression, bool discard) const;
  void stampScopeOnly(Expr &expression, std::uint64_t scope, std::size_t depth);
  void declaration(Decl &declaration, const Decl *owner, std::size_t depth);
  void goalConditions(Decl &declaration, const Decl *owner);
  void term(Expr &expression, Scope &env, WalkContext context, bool bind,
            std::size_t depth);
  void place(Expr &expression, Scope &env, WalkContext context,
             std::size_t depth);
  void call(Expr &expression, Scope &env, WalkContext context,
            std::size_t depth);
  void arguments(Expr &call, Scope &env, WalkContext operand,
                 std::size_t depth);
  void construct(Expr &expression, Scope &env, WalkContext context,
                 DeclKind kind, std::size_t depth);
  void recover(Expr &expression, Scope &env, WalkContext context,
               std::size_t depth);
  void walk(Expr &expression, Scope &env, WalkContext context,
            std::size_t depth);

  Program &program_;
  ResolveLimits limits_;
  std::map<std::string, Decl *, std::less<>> globals_;
  std::map<const Decl *, Scope> ownerScopes_;
  std::map<const Decl *,
           std::map<DeclKind, std::map<std::string, Decl *, std::less<>>>>
      memberIndex_;
  std::map<const Decl *, std::map<std::string, const Parameter *, std::less<>>>
      parameterIndex_;
  std::set<std::pair<const Decl *, const Decl *>> checkedGoals_;
  std::set<const Decl *> checkedGoalDeclarations_;
  std::uint64_t lastScopeId_{0};
  mutable std::size_t resolutionWork_{limits_.maxResolutionWork};
};

void Resolver::work(SourceLocation loc, std::size_t amount) const {
  if (amount > resolutionWork_)
    throw Diagnostic{DiagnosticCode::InvalidDeclaration,
                     "symbol resolution exceeds "
                     "ResolveLimits::maxResolutionWork",
                     loc};
  resolutionWork_ -= amount;
}

Scope Resolver::copy(const Scope &scope, SourceLocation loc) {
  // The checker's copyEnvironment charges the map's size; keep the same
  // accounting for the same reason — deep scope copies are where flat
  // environments get expensive.
  work(loc, scope.bindings.size());
  return Scope{freshId(), scope.bindings, 0};
}

ScopeStamp &Resolver::mint(Scope &scope, const std::string &name,
                           SourceLocation loc) {
  work(loc);
  return scope.bindings
      .insert_or_assign(name, ScopeStamp{scope.id, scope.nextOrdinal++})
      .first->second;
}

void Resolver::buildIndexes() {
  // Same coverage as the checker's buildResolutionIndex: every declaration,
  // nested members included, is a potential name or parameter owner.
  std::vector<Decl *> pending;
  for (auto &decl : program_.declarations) {
    work(decl.loc);
    globals_.emplace(decl.name, &decl);
    pending.push_back(&decl);
  }
  while (!pending.empty()) {
    auto *owner = pending.back();
    pending.pop_back();
    parameterIndex_.try_emplace(owner);
    for (const auto &parameter : owner->parameters) {
      work(parameter.loc);
      parameterIndex_[owner].emplace(parameter.name, &parameter);
    }
    for (auto &item : owner->members) {
      work(item.loc);
      // Keep the first declaration, matching the checker's scan order.
      memberIndex_[owner][item.kind].emplace(item.name, &item);
      pending.push_back(&item);
    }
  }
}

const Scope &Resolver::ownerScope(const Decl *owner) {
  // The checker's environment(owner): implicit agent names plus the owner's
  // parameters and writable fields, cached so every member body resolves the
  // same logical bindings to the same identities.
  auto cached = ownerScopes_.find(owner);
  if (cached == ownerScopes_.end()) {
    Scope scope{freshId(), {}, 0};
    mint(scope, "self", {});
    mint(scope, "parent", {});
    mint(scope, "inner", {});
    mint(scope, "outer", {});
    if (owner) {
      for (const auto &parameter : owner->parameters)
        mint(scope, parameter.name, parameter.loc);
      for (const auto &item : owner->members)
        if (item.kind == DeclKind::Field && item.result)
          mint(scope, item.name, item.loc);
    }
    cached = ownerScopes_.emplace(owner, std::move(scope)).first;
  }
  return cached->second;
}

Decl *Resolver::global(std::string_view name) const {
  const auto found = globals_.find(name);
  return found == globals_.end() ? nullptr : found->second;
}

Decl *Resolver::member(const Decl *owner, std::string_view name,
                       DeclKind kind) const {
  if (!owner) return nullptr;
  work(owner->loc);
  const auto indexed = memberIndex_.find(owner);
  if (indexed == memberIndex_.end()) return nullptr;
  const auto category = indexed->second.find(kind);
  if (category == indexed->second.end()) return nullptr;
  const auto found = category->second.find(name);
  return found == category->second.end() ? nullptr : found->second;
}

Decl *Resolver::goal(const Decl *owner, std::string_view name) const {
  if (auto *local = member(owner, name, DeclKind::Goal)) return local;
  auto *decl = global(name);
  return decl && decl->kind == DeclKind::Goal ? decl : nullptr;
}

bool Resolver::definitelyBottom(const Expr &expression, bool discard) const {
  // A bottom arm abandons the search, so the checker's `|` adopts the
  // surviving alternative's environment wholesale — `believes p(?x) | fail`
  // is what lets x stay bound after the alternation. Bottom is a type
  // judgment; this is the conservative syntactic shadow resolve can make.
  // It must err toward "not bottom": a missed bottom falls back to the
  // definite-binding merge and leaves fewer stamps, never a phantom
  // binding.
  switch (expression.kind) {
  case ExprKind::Fail:
  case ExprKind::Return:
    return true;
  case ExprKind::Block:
    return !expression.children.empty() &&
           definitelyBottom(*expression.children.back(), discard);
  case ExprKind::If:
    // A discarded or else-less if yields unit, never bottom.
    return !discard && expression.children.size() >= 3 &&
           definitelyBottom(*expression.children[1], discard) &&
           definitelyBottom(*expression.children[2], discard);
  case ExprKind::Binary:
    if (expression.children.size() != 2) return false;
    if (expression.text == "|")
      return definitelyBottom(*expression.children[0], false) &&
             definitelyBottom(*expression.children[1], false);
    if (expression.text == "&" || expression.text == "?" ||
        comparison(expression.text))
      return definitelyBottom(*expression.children[1], false);
    if (expression.text == ":=" || expression.text == "<-") return false;
    // `++` and the arithmetic family return their left operand's type.
    return definitelyBottom(*expression.children[0], false);
  case ExprKind::Unary:
    // Only unary '-' propagates its operand's type.
    return expression.text == "-" && !expression.children.empty() &&
           definitelyBottom(*expression.children.front(), false);
  default:
    return false;
  }
}

void Resolver::stampScopeOnly(Expr &expression, std::uint64_t scope,
                              std::size_t depth) {
  // Subtrees the checker never env-resolves — call callees and the uninstall
  // target node resolve through declaration tables instead — still get the
  // scope stamp so "visited here" and "unresolved" stay distinct.
  if (depth > limits_.maxExpressionDepth)
    throw Diagnostic{DiagnosticCode::InvalidDeclaration,
                     "AST exceeds ResolveLimits::maxExpressionDepth",
                     expression.loc};
  work(expression.loc);
  expression.stamp = ScopeStamp{scope, std::nullopt};
  for (auto &child : expression.children)
    if (child) stampScopeOnly(*child, scope, depth + 1);
  for (const auto *metadata :
       {&expression.scope, &expression.recipients, &expression.deadline,
        &expression.guard, &expression.recovery})
    if (*metadata) stampScopeOnly(**metadata, scope, depth + 1);
}

void Resolver::term(Expr &expression, Scope &env, WalkContext context,
                    bool bind, std::size_t depth) {
  if (depth > limits_.maxExpressionDepth)
    throw Diagnostic{DiagnosticCode::InvalidDeclaration,
                     "AST exceeds ResolveLimits::maxExpressionDepth",
                     expression.loc};
  work(expression.loc);
  // The predicate name resolves through declaration tables, not env.
  expression.stamp = ScopeStamp{env.id, std::nullopt};
  const WalkContext operand{context.owner, false};
  for (auto &argument : expression.children) {
    if (argument->kind == ExprKind::LogicVariable) {
      work(argument->loc);
      argument->stamp = ScopeStamp{env.id, std::nullopt};
      if (argument->text == "_") continue;
      if (const auto found = env.bindings.find(argument->text);
          found != env.bindings.end()) {
        argument->stamp = found->second;
        continue;
      }
      // A query variable in a binding term is the binding it introduces; in
      // a ground-only term it stays unresolved for the checker to name.
      if (bind) argument->stamp = mint(env, argument->text, argument->loc);
      continue;
    }
    if (argument->kind == ExprKind::Identifier && argument->text == "_") {
      work(argument->loc);
      argument->stamp = ScopeStamp{env.id, std::nullopt};
      continue;
    }
    walk(*argument, env, operand, depth + 1);
  }
}

void Resolver::place(Expr &expression, Scope &env, WalkContext context,
                     std::size_t depth) {
  if (depth > limits_.maxExpressionDepth)
    throw Diagnostic{DiagnosticCode::InvalidDeclaration,
                     "AST exceeds ResolveLimits::maxExpressionDepth",
                     expression.loc};
  work(expression.loc);
  expression.stamp = ScopeStamp{env.id, std::nullopt};
  const WalkContext operand{context.owner, false};
  if (expression.kind == ExprKind::Identifier) {
    if (const auto found = env.bindings.find(expression.text);
        found != env.bindings.end())
      expression.stamp = found->second;
    return;
  }
  // The checker places the base and then infers it; the double walk stamps
  // identically, so mirroring the two calls is cheap.
  if (expression.kind == ExprKind::Field && !expression.children.empty()) {
    place(*expression.children.front(), env, context, depth + 1);
    walk(*expression.children.front(), env, operand, depth + 1);
    return;
  }
  if (expression.kind == ExprKind::Index && expression.children.size() == 2) {
    place(*expression.children.front(), env, context, depth + 1);
    walk(*expression.children.front(), env, operand, depth + 1);
    walk(*expression.children[1], env, operand, depth + 1);
    return;
  }
}

void Resolver::call(Expr &expression, Scope &env, WalkContext context,
                    std::size_t depth) {
  if (expression.children.empty()) return; // the checker names the shape
  // A callee is a method/function name plus an optional namespace qualifier;
  // it never resolves through the environment.
  stampScopeOnly(*expression.children.front(), env.id, depth + 1);
  const WalkContext operand{context.owner, false};
  for (std::size_t i = 1; i < expression.children.size(); ++i)
    walk(*expression.children[i], env, operand, depth + 1);
  if (expression.recovery) recover(expression, env, context, depth);
}

void Resolver::arguments(Expr &callExpr, Scope &env, WalkContext operand,
                         std::size_t depth) {
  for (std::size_t i = 1; i < callExpr.children.size(); ++i)
    walk(*callExpr.children[i], env, operand, depth + 1);
}

void Resolver::construct(Expr &expression, Scope &env, WalkContext context,
                         DeclKind kind, std::size_t depth) {
  if (expression.children.empty() ||
      expression.children.front()->kind != ExprKind::Call ||
      expression.children.front()->children.empty()) {
    // The checker reports the shape; stamp it so the dead call still reads
    // as visited rather than unresolved.
    for (auto &child : expression.children)
      stampScopeOnly(*child, env.id, depth + 1);
    return;
  }
  auto &invocation = *expression.children.front();
  auto &callee     = *invocation.children.front();
  stampScopeOnly(callee, env.id, depth + 1);
  // The Call itself wraps a declaration-name reference, never an env
  // resolution; stamping it here keeps the known-target path — which walks
  // the argument slots directly — as stamped as the unknown-target one.
  invocation.stamp = ScopeStamp{env.id, std::nullopt};
  auto *decl       = kind == DeclKind::Goal ? goal(context.owner, callee.text)
                                            : global(callee.text);
  if (!decl || decl->kind != kind) {
    // An unknown target ends the checker's visit before the arguments —
    // keep the same coverage so nothing stamped here claims a scope the
    // checker never gave it.
    stampScopeOnly(invocation, env.id, depth + 1);
    return;
  }
  const WalkContext operand{context.owner, false};
  arguments(invocation, env, operand, depth + 1);
  if (kind == DeclKind::Goal) goalConditions(*decl, context.owner);
}

void Resolver::recover(Expr &expression, Scope &env, WalkContext context,
                       std::size_t depth) {
  if (!expression.recovery) return;
  auto local = copy(env, expression.loc);
  mint(local, "cause", expression.recovery->loc);
  walk(*expression.recovery, local, context, depth + 1);
}

void Resolver::walk(Expr &expression, Scope &env, WalkContext context,
                    std::size_t depth) {
  if (depth > limits_.maxExpressionDepth)
    throw Diagnostic{DiagnosticCode::InvalidDeclaration,
                     "AST exceeds ResolveLimits::maxExpressionDepth",
                     expression.loc};
  work(expression.loc);
  expression.stamp = ScopeStamp{env.id, std::nullopt};
  const WalkContext operand{context.owner, false};
  switch (expression.kind) {
  case ExprKind::Integer:
  case ExprKind::Float:
  case ExprKind::String:
  case ExprKind::Duration:
  case ExprKind::Boolean:
  case ExprKind::None:
  case ExprKind::Unit:
  case ExprKind::Yield:
  case ExprKind::Consume:
  case ExprKind::Terminate:
  case ExprKind::Fail:
    return;
  case ExprKind::Identifier:
  case ExprKind::LogicVariable:
    // An env hit stamps the binding's identity — owning scope plus ordinal;
    // a miss keeps the scope-only stamp the entry assignment left, and the
    // checker reports the name itself.
    work(expression.loc);
    if (const auto found = env.bindings.find(expression.text);
        found != env.bindings.end())
      expression.stamp = found->second;
    return;
  case ExprKind::Unary:
    if (expression.text == "not") {
      auto local = copy(env, expression.loc);
      if (!expression.children.empty())
        walk(*expression.children.front(), local, operand, depth + 1);
      return;
    }
    if (!expression.children.empty())
      walk(*expression.children.front(), env, operand, depth + 1);
    return;
  case ExprKind::Binary: {
    if (expression.children.size() != 2) return; // malformed; checker names
    auto &left  = *expression.children[0];
    auto &right = *expression.children[1];
    if (expression.text == ":=" || expression.text == "<-") {
      place(left, env, context, depth + 1);
      walk(right, env, operand, depth + 1);
      return;
    }
    if (expression.text == "|") {
      auto leftEnv  = copy(env, expression.loc);
      auto rightEnv = copy(env, expression.loc);
      walk(left, leftEnv, operand, depth + 1);
      walk(right, rightEnv, operand, depth + 1);
      // A bottom arm abandons the search: the surviving alternative's
      // bindings become this scope's contents wholesale, adopted under
      // their original identities.
      if (definitelyBottom(left, false)) {
        env.bindings = rightEnv.bindings;
        return;
      }
      if (definitelyBottom(right, false)) {
        env.bindings = leftEnv.bindings;
        return;
      }
      // Otherwise only a name bound by both alternatives is definite after
      // the join — the checker's version also requires equal types; type
      // equality is the checker's to verify. The merged binding is a fresh
      // identity in this scope: either arm could have produced it.
      for (const auto &entry : leftEnv.bindings) {
        work(expression.loc);
        if (!env.bindings.contains(entry.first) &&
            rightEnv.bindings.contains(entry.first))
          mint(env, entry.first, expression.loc);
      }
      return;
    }
    walk(left, env, operand, depth + 1);
    walk(right, env, operand, depth + 1);
    return;
  }
  case ExprKind::Range:
  case ExprKind::List:
  case ExprKind::Record:
    for (auto &child : expression.children)
      if (child) walk(*child, env, operand, depth + 1);
    return;
  case ExprKind::Call:
    call(expression, env, context, depth);
    return;
  case ExprKind::Index:
    for (auto &child : expression.children)
      if (child) walk(*child, env, operand, depth + 1);
    return;
  case ExprKind::Field:
    if (!expression.children.empty())
      walk(*expression.children.front(), env, operand, depth + 1);
    return;
  case ExprKind::Block: {
    auto local = copy(env, expression.loc);
    for (std::size_t i = 0; i < expression.children.size(); ++i) {
      const WalkContext statement{context.owner,
                                  context.discard ||
                                      i + 1 != expression.children.size()};
      walk(*expression.children[i], local, statement, depth + 1);
    }
    return;
  }
  case ExprKind::Let:
    // The initializer resolves before the name binds — `let x := x` reads
    // the outer x, matching the checker's order.
    if (!expression.children.empty())
      walk(*expression.children.front(), env, operand, depth + 1);
    mint(env, expression.text, expression.loc);
    return;
  case ExprKind::Cells: {
    // A `$name` path anchor is an environment binding use; the checker
    // requires it Cell-typed. This stamps the Cells node itself — the
    // anchor is path text, not an Identifier child.
    const auto path = parseCellsPath(expression.text, expression.loc);
    if (path && path->anchor.kind == CellsPath::Anchor::Kind::Variable) {
      work(expression.loc);
      if (const auto found = env.bindings.find(path->anchor.variable);
          found != env.bindings.end())
        expression.stamp = found->second;
    }
    return;
  }
  case ExprKind::If: {
    // The test's bindings (believes ?x) reach the then branch only.
    auto testEnv = copy(env, expression.loc);
    if (!expression.children.empty())
      walk(*expression.children[0], testEnv, operand, depth + 1);
    if (expression.children.size() > 1) {
      auto thenEnv = copy(testEnv, expression.loc);
      walk(*expression.children[1], thenEnv, context, depth + 1);
    }
    if (expression.children.size() > 2) {
      auto elseEnv = copy(env, expression.loc);
      walk(*expression.children[2], elseEnv, context, depth + 1);
    }
    return;
  }
  case ExprKind::While: {
    // Test and body share one child scope: test bindings reach the body
    // and neither escapes.
    auto local = copy(env, expression.loc);
    if (!expression.children.empty())
      walk(*expression.children[0], local, operand, depth + 1);
    if (expression.children.size() > 1)
      walk(*expression.children[1], local, context, depth + 1);
    return;
  }
  case ExprKind::For: {
    if (!expression.children.empty())
      walk(*expression.children[0], env, operand, depth + 1);
    auto local = copy(env, expression.loc);
    if (expression.binding && *expression.binding != "_")
      mint(local, *expression.binding, expression.loc);
    if (expression.children.size() > 1)
      walk(*expression.children[1], local, context, depth + 1);
    return;
  }
  case ExprKind::ForBelieves: {
    auto local = copy(env, expression.loc);
    if (!expression.children.empty())
      term(*expression.children[0], local, context, true, depth + 1);
    if (expression.children.size() > 1)
      walk(*expression.children[1], local, context, depth + 1);
    return;
  }
  case ExprKind::Term:
    term(expression, env, context, true, depth + 1);
    return;
  case ExprKind::Believes:
    if (!expression.children.empty())
      term(*expression.children[0], env, context, true, depth + 1);
    if (expression.scope) walk(*expression.scope, env, operand, depth + 1);
    return;
  case ExprKind::FindAll: {
    // The collection term binds first; the template reads those bindings.
    auto local = copy(env, expression.loc);
    if (expression.children.size() > 1)
      term(*expression.children[1], local, context, true, depth + 1);
    if (!expression.children.empty())
      walk(*expression.children[0], local, operand, depth + 1);
    return;
  }
  case ExprKind::Emit:
    if (!expression.children.empty())
      walk(*expression.children[0], env, operand, depth + 1);
    if (expression.scope) walk(*expression.scope, env, operand, depth + 1);
    if (expression.recipients)
      walk(*expression.recipients, env, operand, depth + 1);
    return;
  case ExprKind::Tell:
  case ExprKind::Untell:
    if (!expression.children.empty())
      walk(*expression.children[0], env, operand, depth + 1);
    if (expression.children.size() > 1)
      term(*expression.children[1], env, context, false, depth + 1);
    return;
  case ExprKind::Believe:
  case ExprKind::Forget:
  case ExprKind::Revise:
    if (!expression.children.empty())
      term(*expression.children[0], env, context,
           expression.kind == ExprKind::Forget, depth + 1);
    return;
  case ExprKind::Spawn:
    if (expression.scope) walk(*expression.scope, env, operand, depth + 1);
    construct(expression, env, context, DeclKind::Agent, depth);
    return;
  case ExprKind::Adopt:
    construct(expression, env, context, DeclKind::Goal, depth);
    return;
  case ExprKind::Achieve:
    if (expression.recipients)
      walk(*expression.recipients, env, operand, depth + 1);
    if (expression.deadline)
      walk(*expression.deadline, env, operand, depth + 1);
    construct(expression, env, context, DeclKind::Goal, depth);
    recover(expression, env, context, depth);
    return;
  case ExprKind::Ask:
    if (expression.deadline)
      walk(*expression.deadline, env, operand, depth + 1);
    if (!expression.children.empty() &&
        expression.children[0]->kind == ExprKind::Call &&
        !expression.children[0]->children.empty() &&
        expression.children[0]->children[0]->kind == ExprKind::Field &&
        !expression.children[0]->children[0]->children.empty()) {
      // ask peer.question(args): the peer operand is env-resolved, the
      // question name is a global declaration name, and the Call itself
      // wraps that declaration-name reference.
      auto &invocation = *expression.children[0];
      auto &callee     = *invocation.children[0];
      invocation.stamp = ScopeStamp{env.id, std::nullopt};
      callee.stamp     = ScopeStamp{env.id, std::nullopt};
      walk(*callee.children[0], env, operand, depth + 1);
      arguments(invocation, env, operand, depth + 1);
    } else {
      // Not a peer.question(args) call — the checker names the shape.
      for (auto &child : expression.children)
        stampScopeOnly(*child, env.id, depth + 1);
    }
    recover(expression, env, context, depth);
    return;
  case ExprKind::AwaitTerm:
  case ExprKind::AwaitEvent:
    if (expression.deadline)
      walk(*expression.deadline, env, operand, depth + 1);
    if (expression.kind == ExprKind::AwaitTerm) {
      if (!expression.children.empty())
        term(*expression.children[0], env, context, true, depth + 1);
    } else if (expression.binding) {
      // An await binding stays bound in the same scope after resumption.
      mint(env, *expression.binding, expression.loc);
    }
    if (expression.guard) walk(*expression.guard, env, operand, depth + 1);
    recover(expression, env, context, depth);
    return;
  case ExprKind::Drop:
  case ExprKind::Stop:
  case ExprKind::Return:
    if (!expression.children.empty())
      walk(*expression.children[0], env, operand, depth + 1);
    return;
  case ExprKind::Install:
  case ExprKind::Mount:
    construct(expression, env, context,
              expression.kind == ExprKind::Install ? DeclKind::Skill
                                                   : DeclKind::Behavior,
              depth);
    return;
  case ExprKind::Uninstall:
  case ExprKind::Unmount:
    // The parser carries the target name as an Identifier child, but the
    // checker reads only expression.text — a declaration name, not an env
    // binding.
    for (auto &child : expression.children)
      stampScopeOnly(*child, env.id, depth + 1);
    return;
  }
  // Unhandled kinds are the checker's "unknown expression kind" refusal;
  // the scope stamp above is all resolve owes them.
}

void Resolver::goalConditions(Decl &decl, const Decl *owner) {
  // Goal clauses are templates checked per adopting scope; each site mints
  // its own instantiation scope exactly like the checker's copy.
  if (!checkedGoals_.insert({&decl, owner}).second) return;
  checkedGoalDeclarations_.insert(&decl);
  auto env = copy(ownerScope(owner), decl.loc);
  for (const auto &parameter : decl.parameters)
    mint(env, parameter.name, parameter.loc);
  for (auto &clause : decl.goalClauses) {
    auto local = copy(env, clause.loc);
    if (clause.condition) walk(*clause.condition, local, {owner, false}, 1);
  }
}

void Resolver::declaration(Decl &decl, const Decl *owner, std::size_t depth) {
  if (depth > limits_.maxDeclarationDepth)
    throw Diagnostic{DiagnosticCode::InvalidDeclaration,
                     "AST exceeds ResolveLimits::maxDeclarationDepth",
                     decl.loc};
  work(decl.loc);
  // The four container kinds expose members under a shared owner scope and
  // have no body of their own — the checker's declaration() returns after
  // recursing members for exactly these.
  if (decl.kind == DeclKind::Agent || decl.kind == DeclKind::Behavior ||
      decl.kind == DeclKind::Skill || decl.kind == DeclKind::Capacity) {
    for (auto &item : decl.members) declaration(item, &decl, depth + 1);
    return;
  }
  auto env = copy(ownerScope(owner), decl.loc);
  const WalkContext context{owner, false};
  for (const auto &parameter : decl.parameters)
    mint(env, parameter.name, parameter.loc);
  const WalkContext operand{owner, false};
  switch (decl.kind) {
  case DeclKind::Field:
    if (decl.initializer) walk(*decl.initializer, env, context, 1);
    return;
  case DeclKind::Install:
  case DeclKind::BehaviorUse: {
    const auto *target = global(decl.name);
    const auto expected =
        decl.kind == DeclKind::Install ? DeclKind::Skill : DeclKind::Behavior;
    if (!decl.initializer) return; // checker names a missing target
    if (!target || target->kind != expected) {
      // The checker stops at the name error without walking the call; stamp
      // the subtree scope-only so it still reads as visited.
      stampScopeOnly(*decl.initializer, env.id, 1);
      return;
    }
    // The Call wraps a declaration-name reference, never an env resolution;
    // the checker's arguments() walks only the operand slots, so the Call
    // and its callee take scope-only stamps like construct()'s do.
    decl.initializer->stamp = ScopeStamp{env.id, std::nullopt};
    if (!decl.initializer->children.empty())
      stampScopeOnly(*decl.initializer->children.front(), env.id, 1);
    arguments(*decl.initializer, env, operand, 1);
    return;
  }
  case DeclKind::Belief:
    if (decl.initializer) term(*decl.initializer, env, context, false, 1);
    return;
  case DeclKind::Goal:
    // A member goal's clauses instantiate in this owner's scope; a global
    // goal is picked up by run()'s parameter-only sweep.
    if (owner) goalConditions(decl, owner);
    return;
  case DeclKind::Plan: {
    auto *target = decl.target ? goal(owner, *decl.target) : nullptr;
    if (target) {
      if (decl.planBindings) {
        const auto &parameters = parameterIndex_.at(target);
        for (const auto &name : *decl.planBindings) {
          work(decl.loc);
          // Unknown plan bindings stay unbound for the checker to name.
          if (parameters.contains(name)) mint(env, name, decl.loc);
        }
      } else {
        for (const auto &parameter : target->parameters)
          mint(env, parameter.name, parameter.loc);
      }
      goalConditions(*target, owner);
    }
    if (decl.guard) walk(*decl.guard, env, operand, 1);
    if (decl.body) walk(*decl.body, env, {owner, true}, 1);
    return;
  }
  case DeclKind::Handler:
    if (decl.trigger && decl.trigger->kind == ExprKind::Term) {
      term(*decl.trigger, env, context, true, 1);
    } else {
      // A non-Term trigger names the handled event declaration — the
      // checker resolves it through decl.name, not the environment, so it
      // earns a scope-only stamp like an invocation callee does.
      if (decl.trigger) stampScopeOnly(*decl.trigger, env.id, 1);
      mint(env, "occurrence", decl.loc);
      if (decl.binding) mint(env, *decl.binding, decl.loc);
    }
    if (decl.guard) walk(*decl.guard, env, operand, 1);
    if (decl.body) walk(*decl.body, env, {owner, true}, 1);
    return;
  case DeclKind::Method:
  case DeclKind::Answer:
    if (decl.effect && decl.deadline) walk(*decl.deadline, env, operand, 1);
    for (auto &contract : decl.contracts) {
      auto local = copy(env, contract.loc);
      if (contract.condition) walk(*contract.condition, local, operand, 1);
    }
    if (decl.body) walk(*decl.body, env, context, 1);
    return;
  case DeclKind::Lifecycle:
    if (decl.body) walk(*decl.body, env, {owner, true}, 1);
    return;
  default:
    // Uses/event/record/predicate/question declarations bind no
    // environment-visible expressions.
    return;
  }
}

std::expected<void, std::vector<Diagnostic>> Resolver::run() try {
  buildIndexes();
  for (auto &decl : program_.declarations) declaration(decl, nullptr, 1);
  // A global goal's clauses are templates in each adopting agent's scope;
  // like the checker, an unreferenced template is still walked in its
  // parameter-only scope. A clause shared by several instantiation sites
  // records the last visit's stamps — per-site identities arrive with
  // expand's cloned bodies.
  for (auto &decl : program_.declarations)
    if (decl.kind == DeclKind::Goal &&
        !checkedGoalDeclarations_.contains(&decl))
      goalConditions(decl, nullptr);
  return {};
} catch (const Diagnostic &diagnostic) {
  return std::unexpected(std::vector{diagnostic});
}

} // namespace

std::expected<void, std::vector<Diagnostic>> resolve(Program &program,
                                                     ResolveLimits limits) {
  return Resolver(program, limits).run();
}

} // namespace xanadu::vagent
