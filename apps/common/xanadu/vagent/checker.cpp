#include "common/xanadu/vagent/checker.hpp"

#include "common/xanadu/vagent/cell_path.hpp"
#include "common/xanadu/vagent/code_path.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace xanadu::vagent {
namespace {

Type named(std::string name) { return {std::move(name), {}}; }
Type list(Type element) { return {"List", {std::move(element)}}; }
Type optional(Type element) { return {"Opt", {std::move(element)}}; }
const Type unit    = named("()");
const Type bottom  = named("<bottom>");
const Type unknown = named("<unknown>");
const Type any     = named("<any>");
const Type absent  = named("<none>");

bool snake(std::string_view name) {
  return !name.empty() &&
         (name.front() == '_' ||
          (name.front() >= 'a' && name.front() <= 'z')) &&
         std::ranges::none_of(name,
                              [](char c) { return c >= 'A' && c <= 'Z'; });
}
bool pascal(std::string_view name) {
  return !name.empty() && name.front() >= 'A' && name.front() <= 'Z';
}
bool comparison(std::string_view op) {
  return op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" ||
         op == ">=";
}
bool numeric(const Type &type) {
  return type.name == "Int" || type.name == "Float";
}
// §5.5's literal-expressible scalars — the only types a `static` may fold
// to; a Cell or Code result would bake a compile-arena handle into the
// sealed artifact as artifact data.
bool literalExpressible(const Type &type) {
  return type.name == "Int" || type.name == "Float" || type.name == "Str" ||
         type.name == "Bool" || type.name == "()";
}
bool sameDeadline(const Expr *left, const Expr *right) {
  if (!left || !right) return left == right;
  if (left->kind != ExprKind::Duration || right->kind != ExprKind::Duration)
    return false;
  const auto seconds = [](std::string text) -> std::optional<long double> {
    std::erase(text, '_');
    long double number{};
    auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), number);
    if (parsed.ec != std::errc{}) return std::nullopt;
    const std::string_view suffix(parsed.ptr, text.data() + text.size());
    if (suffix == "ms") return number / 1000.0L;
    if (suffix == "s") return number;
    if (suffix == "min") return number * 60.0L;
    return std::nullopt;
  };
  const auto a = seconds(left->text);
  const auto b = seconds(right->text);
  if (!a || !b) return false;
  const auto scale = std::max(std::abs(*a), std::abs(*b));
  return std::abs(*a - *b) <=
         std::numeric_limits<long double>::epsilon() * scale * 4.0L;
}
bool signaturesEquivalent(const Decl &left, const Decl &right) {
  if (left.parameters.size() != right.parameters.size() ||
      left.result.value_or(unit) != right.result.value_or(unit) ||
      left.effect != right.effect || left.preempts != right.preempts ||
      !sameDeadline(left.deadline.get(), right.deadline.get()))
    return false;
  for (std::size_t i = 0; i < left.parameters.size(); ++i)
    if (left.parameters[i].type != right.parameters[i].type) return false;
  return true;
}
bool directWaitPosition(const Expr &expression) {
  return expression.kind == ExprKind::Call ||
         expression.kind == ExprKind::Ask ||
         expression.kind == ExprKind::AwaitTerm ||
         expression.kind == ExprKind::AwaitEvent;
}

Type read(Type type) {
  if (type.name == "Opt" && type.arguments.size() == 1)
    return type.arguments.front();
  return type;
}

struct Binding {
  Type type;
  bool writable{true};
  std::size_t depth{};
  // §5.5's static-operand closedness: true when the name was minted by a
  // `let` whose initializer is itself closed — a literal, a `static`
  // value, or a composite of closed parts — so its value is determinable
  // at compile time. Fields, parameters, loop/query/recovery bindings and
  // `var`-bound names stay false: they only have values at run time.
  bool closed{false};
  // A code"…" CodeParam or a cap$N capture alias (§5.3): name-resolvable
  // but unvalued inside `static` — a parameter has no value until
  // instantiation and a capture names a cell, not a literal. The
  // closedness walk cites it as the refusal reason rather than the
  // generic "not closed".
  bool unvalued{false};
};
using Environment = std::map<std::string, Binding, std::less<>>;
using Symbols     = std::map<std::string, const Decl *, std::less<>>;
enum class Body {
  Handler,
  Plan,
  Method,
  Answer,
  Initializer,
  Lifecycle,
  Condition,
  // A code"…" literal's body (§5.1): `return` and the plan-suspension
  // forms are refused because they name positions a fragment never has;
  // `:=` still gates on the incoming bounded flag.
  Fragment
};
struct Context {
  const Decl *owner{};
  Body body{Body::Initializer};
  bool bounded{true};
  bool suspend{false};
  bool stageEffect{false};
  bool pure{false};
  bool readonly{false};
  bool beliefLoop{false};
  bool discard{false};
  std::optional<Type> result;
  std::size_t depth{};
};
struct Function {
  std::vector<Type> parameters;
  Type result{unit};
  bool effect{false};
  bool pure{false};
  bool variadic{false};
  const Decl *declaration{};
};

class Checker {
public:
  Checker(const Program &program, CheckLimits limits,
          const FragmentTable &fragments)
      : program_(program), limits_(limits), fragments_(fragments),
        inheritanceWork_(limits.maxInheritanceWork),
        purityWork_(limits.maxPurityWork),
        resolutionWork_(limits.maxResolutionWork) {}
  std::expected<void, std::vector<Diagnostic>> run();
  // check()'s infoOut seam: the per-fragment tables phase 3's descriptor
  // emission consumes — valid after a successful run().
  const FragmentInfoMap &fragmentInfo() const { return fragmentInfo_; }

private:
  // Inside a code"…" body every node loc is literal-relative, so one
  // innerDiagnostic arm per enclosing literal lands the src:m:l:c+o:c
  // chain — outermost literal's own loc first (§5.1).
  Diagnostic mapInner(Diagnostic diagnostic) const {
    for (auto it = innerLiterals_.rbegin(); it != innerLiterals_.rend(); ++it)
      diagnostic = innerDiagnostic(std::move(diagnostic), *it);
    return diagnostic;
  }
  void report(DiagnosticCode code, std::string detail, SourceLocation loc) {
    diagnostics_.push_back(mapInner(Diagnostic{code, std::move(detail), loc}));
  }
  void require(bool condition, DiagnosticCode code, std::string detail,
               SourceLocation loc) {
    if (!condition) report(code, std::move(detail), loc);
  }
  // §5.3's readability rule: a cap$N alias renders in messages as the
  // captured name, never 'cap$3'. The env records of every enclosing
  // literal — innermost first — map each alias one hop toward its
  // origin, so nested captures chase back to the source name.
  std::string_view displayName(std::string_view name) const {
    for (std::size_t hops = 0; hops < 16; ++hops) {
      bool mapped = false;
      for (auto it = envRecords_.rbegin(); it != envRecords_.rend(); ++it) {
        if (const auto found = (*it)->find(name); found != (*it)->end()) {
          name   = found->second;
          mapped = true;
          break;
        }
      }
      if (!mapped) return name;
    }
    return name;
  }
  // §5.1's reservation rule: splice/static (and run/value_of once std:meta
  // is imported) are contextual keywords, so declarations and bindings
  // that would shadow them are refused; `$`-bearing names on user-supplied
  // decls are refused too — the expand stage owns the cap$N namespace and
  // snake() accepts '$' today.
  void reservedName(std::string_view name, SourceLocation loc);
  void preflight() const;
  void buildResolutionIndex();
  bool sameSignature(const Decl &left, const Decl &right) const;
  void signatureWork(const Decl &decl) const;
  void resolutionWork(SourceLocation loc, std::size_t amount = 1) const;
  void buildInheritance();
  void inheritanceWork(SourceLocation loc, std::size_t amount = 1) const;
  bool inherits(std::string_view actual, std::string_view expected,
                DeclKind kind) const;
  const Decl *global(std::string_view name) const;
  const Decl *member(const Decl *owner, std::string_view name,
                     DeclKind kind) const;
  const Decl *goal(const Decl *owner, std::string_view name) const;
  bool event(std::string_view name) const;
  bool subtype(std::string_view actual, std::string_view expected) const;
  bool compatible(const Type &actual, const Type &expected) const;
  void expectType(const Type &actual, const Type &expected, SourceLocation loc);
  void validateType(const Type &type, SourceLocation loc);
  Environment environment(const Decl *owner) const;
  Environment copyEnvironment(const Environment &env, SourceLocation loc) const;
  void parameters(const Decl &declaration);
  void declaration(const Decl &declaration, const Decl *owner);
  void goalConditions(const Decl &declaration, const Decl *owner);
  void skill(const Decl &declaration);
  void capacityMethods(const Decl &capacity,
                       std::map<std::string, const Decl *> &out,
                       std::set<std::string> &visited) const;
  std::optional<Type> fieldType(const Type &type, std::string_view name) const;
  std::optional<Function> function(const Expr &callee, const Decl *owner,
                                   bool diagnose = true);
  bool pureBody(const Expr &expression, const Decl *owner);
  void derivePurity();
  void purityWork(SourceLocation loc);
  // §5.5's form-level closedness judgment: true when the expression can
  // be evaluated at compile time — every free name resolves to a binding
  // that is itself closed (a `let` of a literal or `static` value) and
  // nothing reads the arena (cells"…" in any mode, believes, findall,
  // run or any call), names an implicit name or an unvalued binding
  // (a CodeParam or a cap$N capture), or embeds a code"…" record
  // classified not-closed. `diagnose` reports the refusal reason at the
  // offending node; the let-initializer walk passes false — a non-closed
  // value is legal to bind, it only leaves the name unclosed.
  bool staticClosed(const Expr &expression, Environment &env, bool diagnose);
  Type infer(const Expr &expression, Environment &env, Context context);
  Type condition(const Expr &expression, Environment &env, Context context);
  Type place(const Expr &expression, Environment &env, Context context);
  Type term(const Expr &expression, Environment &env, Context context,
            bool bind);
  Type call(const Expr &expression, Environment &env, Context context);
  Type construct(const Expr &expression, Environment &env, Context context,
                 DeclKind kind, Type result);
  void arguments(const Expr &call, const std::vector<Type> &types,
                 Environment &env, Context context);
  void deadline(const Expr *expression, Environment &env, Context context,
                bool required);
  Type recover(const Expr &expression, Type value, Environment &env,
               Context context);
  void actionAllowed(const Expr &expression, Context context);
  // std:meta's checker-level special forms (§6.1, phase 3): `run` and
  // `reify` are not signature rows — run's second operand is the
  // language's only heterogeneous argument list, and reify returns the
  // operand's own Code<T> identity rather than a fixed row.
  enum class MetaForm : std::uint8_t { None, Run, Reify };
  MetaForm metaForm(const Expr &callee) const;
  Type metaRun(const Expr &call, Environment &env, Context context);
  Type metaReify(const Expr &call, Environment &env, Context context);
  static Context operand(Context context) {
    context.bounded     = false;
    context.suspend     = false;
    context.stageEffect = false;
    context.discard     = false;
    return context;
  }

  const Program &program_;
  CheckLimits limits_;
  const FragmentTable &fragments_;
  std::map<FragmentKey, FragmentInfo> fragmentInfo_;
  // Enclosing code"…" literals' locs, outermost first — each a position in
  // its parent's text coordinates. Body diagnostics wrap through
  // innerDiagnostic per level when they surface.
  std::vector<SourceLocation> innerLiterals_;
  // The same enclosing literals' §5.3 env records — cap$N alias →
  // captured name — innermost last, parallel to innerLiterals_. The
  // checker resolves cap$N names through these maps: they materialize
  // the stamped binding identity expand assigned the alias (a
  // stamp→binding table needs resolve internals the merged tree does
  // not carry, so the name channel is the resolution surface here).
  std::vector<const std::map<std::string, std::string, std::less<>> *>
      envRecords_;
  bool metaImported_{false};
  Symbols globals_;
  std::vector<Diagnostic> diagnostics_;
  std::map<const Decl *, bool> pure_;
  std::map<const Decl *, const Decl *> methodOwners_;
  std::set<std::pair<const Decl *, const Decl *>> checkedGoals_;
  std::set<const Decl *> checkedGoalDeclarations_;
  std::map<const Decl *, std::map<std::string, const Decl *>> capacityCache_;
  std::map<const Decl *, std::map<std::string, Type>> eventFieldCache_;
  mutable std::map<std::pair<const Decl *, const Decl *>, bool> ancestryCache_;
  mutable std::size_t inheritanceWork_;
  std::size_t purityWork_;
  mutable std::size_t resolutionWork_;
  std::map<const Decl *, std::map<DeclKind, Symbols>> memberIndex_;
  std::map<const Decl *, std::vector<std::string>> ownerUses_;
  mutable std::map<const Decl *, Environment> ownerEnvironments_;
  std::vector<const Decl *> skills_;
  std::map<const Decl *, std::map<std::string, const Parameter *, std::less<>>>
      parameterIndex_;
  std::map<const Decl *, std::size_t> signatureCosts_;
  mutable std::map<const Type *, std::size_t> typeWeights_;
};

void Checker::resolutionWork(SourceLocation loc, std::size_t amount) const {
  if (amount > resolutionWork_)
    throw Diagnostic{DiagnosticCode::InvalidDeclaration,
                     "symbol resolution exceeds CheckLimits::maxResolutionWork",
                     loc};
  resolutionWork_ -= amount;
}

void Checker::signatureWork(const Decl &decl) const {
  resolutionWork(decl.loc, signatureCosts_.at(&decl));
}

bool Checker::sameSignature(const Decl &left, const Decl &right) const {
  signatureWork(left);
  signatureWork(right);
  return signaturesEquivalent(left, right);
}

void Checker::buildResolutionIndex() {
  std::vector<const Decl *> pending;
  for (const auto &decl : program_.declarations) {
    resolutionWork(decl.loc);
    pending.push_back(&decl);
    if (decl.kind == DeclKind::Skill) skills_.push_back(&decl);
  }
  while (!pending.empty()) {
    const auto *owner = pending.back();
    pending.pop_back();
    std::size_t cost = 1;
    parameterIndex_.try_emplace(owner);
    for (const auto &parameter : owner->parameters) {
      resolutionWork(parameter.loc);
      cost += 1 + typeWeights_.at(&parameter.type);
      parameterIndex_[owner].emplace(parameter.name, &parameter);
    }
    if (owner->result) cost += typeWeights_.at(&*owner->result);
    signatureCosts_.emplace(owner, cost);
    for (const auto &item : owner->members) {
      resolutionWork(item.loc);
      // Keep the first declaration, matching the former source-order scan.
      memberIndex_[owner][item.kind].emplace(item.name, &item);
      if (item.kind == DeclKind::Uses) {
        resolutionWork(item.loc, item.names.size());
        auto &uses = ownerUses_[owner];
        uses.insert(uses.end(), item.names.begin(), item.names.end());
      }
      pending.push_back(&item);
    }
  }
}

void Checker::preflight() const {
  struct Item {
    const Decl *declaration{};
    const Expr *expression{};
    const Type *type{};
    std::size_t depth{1};
    SourceLocation loc{};
    bool expanded{false};
  };
  std::vector<Item> pending;
  std::size_t astNodes        = 0;
  std::size_t expressionNodes = 0;
  const auto refuse           = [](SourceLocation loc, std::string_view limit) {
    throw Diagnostic{DiagnosticCode::InvalidDeclaration,
                     "AST exceeds CheckLimits::" + std::string(limit), loc};
  };
  const auto push = [&](Item item) {
    // Charge before queueing, so wide trees cannot allocate an unbounded queue.
    if (astNodes >= limits_.maxAstNodes) refuse(item.loc, "maxAstNodes");
    ++astNodes;
    if (item.expression) {
      if (expressionNodes >= limits_.maxExpressionNodes)
        refuse(item.loc, "maxExpressionNodes");
      ++expressionNodes;
      if (item.depth > limits_.maxExpressionDepth)
        refuse(item.loc, "maxExpressionDepth");
    } else if (item.declaration) {
      if (item.depth > limits_.maxDeclarationDepth)
        refuse(item.loc, "maxDeclarationDepth");
    } else if (item.depth > limits_.maxTypeDepth) {
      refuse(item.loc, "maxTypeDepth");
    }
    pending.push_back(item);
  };
  for (const auto &decl : program_.declarations)
    push({.declaration = &decl, .loc = decl.loc});
  while (!pending.empty()) {
    const auto item = pending.back();
    pending.pop_back();
    if (item.expanded) {
      std::size_t weight = 1;
      for (const auto &argument : item.type->arguments)
        weight += typeWeights_.at(&argument);
      typeWeights_.emplace(item.type, weight);
      continue;
    }
    const auto expression = [&](const ExprPtr &value, std::size_t depth = 1) {
      if (value)
        push({.expression = value.get(), .depth = depth, .loc = value->loc});
    };
    const auto type = [&](const Type &value, std::size_t depth = 1) {
      push({.type = &value, .depth = depth, .loc = item.loc});
    };
    if (item.declaration) {
      const auto &decl = *item.declaration;
      if (decl.kind == DeclKind::Event && decl.names.size() > 1)
        throw Diagnostic{DiagnosticCode::InvalidDeclaration,
                         "event declarations permit only one base event",
                         decl.loc};
      for (const auto &parameter : decl.parameters) type(parameter.type);
      if (decl.result) type(*decl.result);
      for (const auto &child : decl.members)
        push(
            {.declaration = &child, .depth = item.depth + 1, .loc = child.loc});
      for (const auto &contract : decl.contracts)
        expression(contract.condition);
      for (const auto &clause : decl.goalClauses) expression(clause.condition);
      expression(decl.body);
      expression(decl.guard);
      expression(decl.initializer);
      expression(decl.trigger);
      expression(decl.deadline);
    } else if (item.expression) {
      const auto &expr = *item.expression;
      if (expr.annotation) type(*expr.annotation);
      for (const auto &child : expr.children) expression(child, item.depth + 1);
      for (const auto *metadata : {&expr.scope, &expr.recipients,
                                   &expr.deadline, &expr.guard, &expr.recovery})
        expression(*metadata, item.depth + 1);
    } else {
      auto expanded     = item;
      expanded.expanded = true;
      pending.push_back(expanded);
      for (const auto &argument : item.type->arguments)
        type(argument, item.depth + 1);
    }
  }
}

void Checker::inheritanceWork(SourceLocation loc, std::size_t amount) const {
  if (amount > inheritanceWork_) {
    throw Diagnostic{
        DiagnosticCode::InvalidDeclaration,
        "inheritance graph exceeds CheckLimits::maxInheritanceWork", loc};
  }
  inheritanceWork_ -= amount;
}

void Checker::buildInheritance() {
  std::map<const Decl *, std::size_t> pending;
  std::map<const Decl *, std::vector<const Decl *>> children;
  for (const auto &decl : program_.declarations) {
    if (global(decl.name) != &decl ||
        (decl.kind != DeclKind::Event && decl.kind != DeclKind::Capacity))
      continue;
    inheritanceWork(decl.loc);
    pending[&decl] = 0;
  }
  for (auto &[decl, count] : pending) {
    for (const auto &name : decl->names) {
      inheritanceWork(decl->loc);
      const auto *base = global(name);
      if (!base || base->kind != decl->kind) continue;
      ++count;
      children[base].push_back(decl);
    }
  }
  std::deque<const Decl *> ready;
  for (const auto &decl : program_.declarations) {
    const auto found = pending.find(&decl);
    if (found != pending.end() && found->second == 0) ready.push_back(&decl);
  }
  std::size_t processed = 0;
  while (!ready.empty()) {
    const auto *decl = ready.front();
    ready.pop_front();
    ++processed;
    inheritanceWork(decl->loc);
    if (decl->kind == DeclKind::Capacity) {
      auto &methods = capacityCache_[decl];
      for (const auto &name : decl->names) {
        const auto inherited = capacityCache_.find(global(name));
        if (inherited == capacityCache_.end()) continue;
        for (const auto &[methodName, signature] : inherited->second) {
          inheritanceWork(decl->loc);
          methods[methodName] = signature;
        }
      }
      for (const auto &method : decl->members) {
        if (method.kind != DeclKind::Method) continue;
        inheritanceWork(method.loc);
        methods[method.name] = &method;
      }
    } else {
      auto &fields = eventFieldCache_[decl];
      for (const auto &name : decl->names) {
        const auto inherited = eventFieldCache_.find(global(name));
        if (inherited == eventFieldCache_.end()) continue;
        for (const auto &[fieldName, type] : inherited->second) {
          inheritanceWork(decl->loc);
          fields[fieldName] = type;
        }
      }
      for (const auto &field : decl->parameters) {
        inheritanceWork(field.loc);
        fields[field.name] = field.type;
      }
    }
    if (const auto found = children.find(decl); found != children.end()) {
      for (const auto *derived : found->second) {
        inheritanceWork(derived->loc);
        if (--pending.at(derived) == 0) ready.push_back(derived);
      }
    }
  }
  if (processed != pending.size()) {
    for (const auto &decl : program_.declarations) {
      const auto found = pending.find(&decl);
      if (found != pending.end() && found->second != 0) {
        report(DiagnosticCode::InvalidDeclaration,
               "inheritance cycle is reachable from '" + decl.name + "'",
               decl.loc);
        break;
      }
    }
  }
}

bool Checker::inherits(std::string_view actual, std::string_view expected,
                       DeclKind kind) const {
  if (actual == expected) return true;
  const auto *source = global(actual);
  const auto *target = global(expected);
  if (!source || !target || source->kind != kind || target->kind != kind)
    return false;
  const auto key = std::pair{source, target};
  if (const auto cached = ancestryCache_.find(key);
      cached != ancestryCache_.end())
    return cached->second;
  std::vector<const Decl *> work{source};
  std::set<const Decl *> visited;
  bool found = false;
  while (!work.empty() && !found) {
    const auto *current = work.back();
    work.pop_back();
    if (!visited.insert(current).second) continue;
    inheritanceWork(current->loc);
    for (const auto &name : current->names) {
      inheritanceWork(current->loc);
      const auto *base = global(name);
      if (base == target) {
        found = true;
        break;
      }
      if (base && base->kind == kind) work.push_back(base);
    }
  }
  ancestryCache_[key] = found;
  return found;
}

const Decl *Checker::global(std::string_view name) const {
  auto found = globals_.find(name);
  return found == globals_.end() ? nullptr : found->second;
}
const Decl *Checker::member(const Decl *owner, std::string_view name,
                            DeclKind kind) const {
  if (!owner) return nullptr;
  resolutionWork(owner->loc);
  const auto indexed = memberIndex_.find(owner);
  if (indexed == memberIndex_.end()) return nullptr;
  const auto category = indexed->second.find(kind);
  if (category == indexed->second.end()) return nullptr;
  const auto found = category->second.find(name);
  return found == category->second.end() ? nullptr : found->second;
}
const Decl *Checker::goal(const Decl *owner, std::string_view name) const {
  if (const auto *local = member(owner, name, DeclKind::Goal)) return local;
  const auto *decl = global(name);
  if (decl && decl->kind == DeclKind::Goal) return decl;
  // §5.7's plan merge is name-keyed, so a mounted behavior's goal
  // member is adoptable under its own name — the agent's `behavior B`
  // members name which decls to ask. Creation members are the static
  // view; `mount` at run time can't reach the checker anyway.
  if (owner && owner->kind == DeclKind::Agent)
    for (const auto &use : owner->members)
      if (use.kind == DeclKind::BehaviorUse)
        if (const auto *behavior = global(use.name);
            behavior && behavior->kind == DeclKind::Behavior)
          if (const auto *contributed =
                  member(behavior, name, DeclKind::Goal))
            return contributed;
  return nullptr;
}

bool Checker::event(std::string_view name) const {
  static const std::set<std::string_view> builtins{
      "Event",          "Initialize",  "Destroy",       "GoalAchieved",
      "GoalFailed",     "GoalDropped", "PlanFailed",    "EffectFailed",
      "ConditionFault", "ChildDown",   "ChildStopping", "SpawnFailed",
      "DeliveryFailed", "Moved"};
  if (builtins.contains(name)) return true;
  auto *decl = global(name);
  return decl && decl->kind == DeclKind::Event;
}

bool Checker::subtype(std::string_view actual,
                      std::string_view expected) const {
  if (actual == expected) return true;
  if (expected == "Event" && event(actual)) return true;
  return inherits(actual, expected, DeclKind::Event);
}

void Checker::reservedName(std::string_view name, SourceLocation loc) {
  if (name.find('$') != std::string_view::npos) {
    report(DiagnosticCode::InvalidIdentifier,
           "name '" + std::string(name) +
               "' may not contain '$'; the expand stage owns the cap$N "
               "namespace",
           loc);
  }
  if (name == "splice" || name == "static" ||
      (metaImported_ && (name == "run" || name == "value_of"))) {
    report(DiagnosticCode::InvalidIdentifier,
           "'" + std::string(name) +
               "' is a reserved contextual keyword; declarations and "
               "bindings may not shadow it",
           loc);
  }
}

bool Checker::compatible(const Type &actual, const Type &expected) const {
  if (actual == bottom || actual == unknown || expected == unknown ||
      actual == any || expected == any)
    return true;
  if (expected.name == "Opt" && expected.arguments.size() == 1) {
    if (actual == absent) return true;
    if (actual.name == "Opt" && actual.arguments.size() == 1)
      return compatible(actual.arguments.front(), expected.arguments.front());
    return compatible(actual, expected.arguments.front());
  }
  // Bare Code is the §6.1 parameterized wildcard: it accepts a fragment of
  // any result type, while Code<T> stays invariant in T (§5.2 — a
  // Code<Int> never fits a Str slot or a Code<Str>).
  if (expected.name == "Code" && expected.arguments.empty() &&
      actual.name == "Code")
    return true;
  if (actual.name != expected.name) return subtype(actual.name, expected.name);
  if (actual.arguments.size() != expected.arguments.size()) return false;
  for (std::size_t i = 0; i < actual.arguments.size(); ++i) {
    if (!compatible(actual.arguments[i], expected.arguments[i])) return false;
  }
  return true;
}
void Checker::expectType(const Type &actual, const Type &expected,
                         SourceLocation loc) {
  if (!compatible(actual, expected)) {
    report(DiagnosticCode::TypeMismatch,
           "expected " + typeName(expected) + ", got " + typeName(actual), loc);
  }
}
void Checker::validateType(const Type &type, SourceLocation loc) {
  static const std::set<std::string_view> scalars{
      "Int",   "Float", "Str",       "Bool",  "()",    "Duration", "Time",
      "Agent", "Space", "Intention", "Timer", "Event", "Cell",     "Code"};
  const auto *decl = global(type.name);
  const bool user =
      decl && (decl->kind == DeclKind::Record || decl->kind == DeclKind::Event);
  std::size_t arity = type.name == "List" || type.name == "Opt" ? 1U
                      : type.name == "Map"                      ? 2U
                                                                : 0U;
  // §5.2's arity chain: Code maps to 0-or-1 — Code<T> the typed fragment,
  // bare Code the §6.1 wildcard.
  const bool codeArity = type.name == "Code" && type.arguments.size() <= 1;
  require(scalars.contains(type.name) || user || arity != 0 || codeArity,
          DiagnosticCode::UnknownName, "unknown type '" + type.name + "'", loc);
  require(codeArity || type.arguments.size() == arity,
          DiagnosticCode::TypeMismatch,
          "wrong number of type arguments for '" + type.name + "'", loc);
  for (const auto &argument : type.arguments) validateType(argument, loc);
}

Environment Checker::copyEnvironment(const Environment &env,
                                     SourceLocation loc) const {
  resolutionWork(loc, env.size());
  return env;
}

Environment Checker::environment(const Decl *owner) const {
  auto cached = ownerEnvironments_.find(owner);
  if (cached == ownerEnvironments_.end()) {
    Environment env{{"self", {named("Agent"), false, 0}},
                    {"parent", {optional(named("Agent")), false, 0}},
                    {"inner", {named("Space"), false, 0}},
                    {"outer", {named("Space"), false, 0}}};
    if (owner) {
      for (const auto &parameter : owner->parameters) {
        resolutionWork(parameter.loc);
        env[parameter.name] = {parameter.type, false, 0};
      }
      for (const auto &item : owner->members) {
        resolutionWork(item.loc);
        if (item.kind == DeclKind::Field && item.result)
          env[item.name] = {*item.result, true, 0};
      }
    }
    cached = ownerEnvironments_.emplace(owner, std::move(env)).first;
  }
  resolutionWork(owner ? owner->loc : SourceLocation{}, cached->second.size());
  return cached->second;
}

void Checker::parameters(const Decl &decl) {
  std::set<std::string> names;
  bool nonkey = false;
  for (const auto &parameter : decl.parameters) {
    validateType(parameter.type, parameter.loc);
    require(snake(parameter.name), DiagnosticCode::InvalidIdentifier,
            "parameter/field names must be snake_case", parameter.loc);
    reservedName(parameter.name, parameter.loc);
    require(names.insert(parameter.name).second, DiagnosticCode::DuplicateName,
            "duplicate parameter/field '" + parameter.name + "'",
            parameter.loc);
    if (decl.kind == DeclKind::Predicate) {
      require(!parameter.key || !nonkey, DiagnosticCode::InvalidDeclaration,
              "predicate key arguments must precede non-key arguments",
              parameter.loc);
      if (!parameter.key) nonkey = true;
    }
    if (decl.kind == DeclKind::Event) {
      require(parameter.name != "source" && parameter.name != "space" &&
                  parameter.name != "time" && parameter.name != "ordinal",
              DiagnosticCode::InvalidDeclaration,
              "event field '" + parameter.name + "' is implicit",
              parameter.loc);
    }
  }
}

void Checker::capacityMethods(const Decl &capacity,
                              std::map<std::string, const Decl *> &out,
                              std::set<std::string> &visited) const {
  if (!visited.insert(capacity.name).second) return;
  inheritanceWork(capacity.loc);
  const auto cached = capacityCache_.find(&capacity);
  if (cached == capacityCache_.end()) return;
  for (const auto &[name, signature] : cached->second) {
    inheritanceWork(capacity.loc);
    out[name] = signature;
  }
}

std::optional<Type> Checker::fieldType(const Type &type,
                                       std::string_view name) const {
  if (event(type.name)) {
    if (name == "source") return optional(named("Agent"));
    if (name == "space") return optional(named("Space"));
    if (name == "time") return named("Time");
    if (name == "ordinal") return named("Int");
    if (type.name == "GoalAchieved" || type.name == "GoalFailed" ||
        type.name == "GoalDropped" || type.name == "PlanFailed" ||
        type.name == "ConditionFault") {
      if (name == "intention") return named("Intention");
      if (name == "goal" || name == "plan" || name == "cause")
        return named("Str");
    }
    if (type.name == "EffectFailed") {
      if (name == "capacity" || name == "method" || name == "cause")
        return named("Str");
    }
    if (type.name == "ChildDown" || type.name == "ChildStopping" ||
        type.name == "SpawnFailed") {
      if (name == "child") return named("Agent");
      if (name == "cause" && type.name == "ChildStopping") return named("Str");
    }
    if (type.name == "DeliveryFailed") {
      if (name == "to") return named("Agent");
      if (name == "kind" || name == "cause") return named("Str");
    }
    if (type.name == "Moved" && name == "name") return named("Agent");
  }
  const auto *decl = global(type.name);
  if (!decl) return std::nullopt;
  if (decl->kind == DeclKind::Event) {
    inheritanceWork(decl->loc);
    const auto cached = eventFieldCache_.find(decl);
    if (cached != eventFieldCache_.end()) {
      const auto field = cached->second.find(std::string(name));
      if (field != cached->second.end()) return field->second;
    }
  } else if (decl->kind == DeclKind::Record) {
    for (const auto &field : decl->parameters) {
      resolutionWork(field.loc);
      if (field.name == name) return field.type;
    }
  }
  return std::nullopt;
}

std::optional<Function> Checker::function(const Expr &callee, const Decl *owner,
                                          bool diagnose) {
  std::string name;
  std::optional<std::string> qualifier;
  if (callee.kind == ExprKind::Identifier)
    name = callee.text;
  else if (callee.kind == ExprKind::Field && !callee.children.empty() &&
           callee.children.front()->kind == ExprKind::Identifier) {
    name      = callee.text;
    qualifier = callee.children.front()->text;
  } else {
    if (diagnose)
      report(DiagnosticCode::UnknownName, "expected a named method or function",
             callee.loc);
    return std::nullopt;
  }
  auto make = [&](const Decl &method) {
    signatureWork(method);
    Function result;
    for (const auto &parameter : method.parameters) {
      resolutionWork(parameter.loc);
      result.parameters.push_back(parameter.type);
    }
    result.result      = method.result.value_or(unit);
    result.effect      = method.effect;
    result.declaration = &method;
    return result;
  };
  if (!qualifier) {
    if (const auto *local = member(owner, name, DeclKind::Method)) {
      auto result = make(*local);
      result.pure = pure_.contains(local) && pure_.at(local);
      return result;
    }
  }
  static const std::vector<std::string> emptyUses;
  const auto ownerUses = ownerUses_.find(owner);
  const auto &uses =
      ownerUses == ownerUses_.end() ? emptyUses : ownerUses->second;
  std::optional<Function> resolved;
  for (const auto &capacityName : uses) {
    resolutionWork(callee.loc);
    if (qualifier && *qualifier != capacityName) continue;
    const auto *capacity = global(capacityName);
    if (!capacity || capacity->kind != DeclKind::Capacity) continue;
    std::map<std::string, const Decl *> methods;
    std::set<std::string> visited;
    capacityMethods(*capacity, methods, visited);
    const auto found = methods.find(name);
    if (found == methods.end()) continue;
    if (resolved) {
      if (diagnose)
        report(DiagnosticCode::UnknownName,
               "ambiguous capacity method '" + name + "'; qualify its capacity",
               callee.loc);
      return std::nullopt;
    }
    resolved               = make(*found->second);
    bool hasImplementation = false;
    bool allPure           = !resolved->effect;
    for (const auto *skillPointer : skills_) {
      const auto &skill = *skillPointer;
      resolutionWork(skill.loc);
      if (!std::ranges::any_of(
              skill.names, [&](const std::string &implemented) {
                resolutionWork(skill.loc);
                return inherits(implemented, capacityName, DeclKind::Capacity);
              }))
        continue;
      if (const auto *method = member(&skill, name, DeclKind::Method)) {
        hasImplementation = true;
        allPure = allPure && pure_.contains(method) && pure_.at(method);
      } else
        allPure = false;
    }
    resolved->pure = hasImplementation && allPure;
  }
  if (resolved) return resolved;

  auto builtin = [&](std::vector<Type> args, Type result, bool pure = true,
                     bool variadic = false) -> std::optional<Function> {
    return Function{std::move(args), std::move(result), false, pure, variadic,
                    nullptr};
  };
  if (!qualifier || *qualifier == "std:agent") {
    if (name == "text") return builtin({any}, named("Str"));
    if (name == "intentions") return builtin({}, list(named("Intention")));
    if (name == "is_maintain") return builtin({named("Intention")}, unit);
    if (name == "restorers")
      return builtin({named("Intention")}, list(named("Intention")));
    if (name == "members")
      return builtin({named("Space")}, list(named("Agent")));
    if (name == "children") return builtin({}, list(named("Agent")));
  }
  if ((!qualifier || *qualifier == "Clock") && name == "now")
    return builtin({}, named("Time"));
  if (!qualifier || *qualifier == "Schedules") {
    if (name == "after" || name == "periodic")
      return builtin({named("Duration"), named("Event")}, named("Timer"),
                     false);
    if (name == "cancel") return builtin({named("Timer")}, unit, false);
  }
  if ((!qualifier || *qualifier == "Logging") &&
      (name == "info" || name == "debug"))
    return builtin({named("Str")}, unit, false);
  if ((!qualifier || *qualifier == "Accepts") && name == "accepts")
    return builtin({named("Agent"), named("Str")}, unit);
  bool goalImported = false;
  for (const auto &imported : program_.imports) {
    resolutionWork(imported.loc, 1 + imported.symbols.size());
    if (imported.path != "std:goal") continue;
    const bool selected =
        imported.symbols.empty() ||
        std::ranges::find(imported.symbols, name) != imported.symbols.end();
    const bool qualified =
        !qualifier || *qualifier == imported.alias.value_or("std:goal");
    goalImported = goalImported || (selected && qualified);
  }
  if (goalImported) {
    if (name == "tab" || name == "move")
      return builtin({named("Int")}, named("Str"));
    if (name == "find" || name == "match" || name == "upto" || name == "many" ||
        name == "any")
      return builtin({named("Str")}, named("Int"));
    if (name == "bal") return builtin({}, named("Int"));
    if (name == "subject") return builtin({}, named("Str"));
    if (name == "pos") return builtin({}, named("Int"));
  }
  // §6.1's read surface: the reflect family plus the value reads, keyed
  // by the std:meta import exactly like std:goal's member rows.
  // `run`/`reify` are special forms in call(), not rows — run's second
  // operand is the language's only heterogeneous argument list.
  bool metaCallable = false;
  for (const auto &imported : program_.imports) {
    resolutionWork(imported.loc, 1 + imported.symbols.size());
    if (imported.path != "std:meta") continue;
    const bool selected =
        imported.symbols.empty() ||
        std::ranges::find(imported.symbols, name) != imported.symbols.end();
    const bool qualified =
        !qualifier || *qualifier == imported.alias.value_or("std:meta");
    metaCallable = metaCallable || (selected && qualified);
  }
  if (metaCallable) {
    const auto code = [](Type element) {
      return Type{"Code", {std::move(element)}};
    };
    if (name == "program" || name == "agent_cell")
      return builtin({}, named("Cell"));
    if (name == "module_named")
      return builtin({named("Str")}, optional(named("Cell")));
    if (name == "symbol_named")
      return builtin({named("Str"), named("Str")}, optional(named("Cell")));
    if (name == "clauses_of")
      return builtin({named("Cell")}, list(named("Cell")));
    if (name == "test_of" || name == "body_of")
      return builtin({named("Cell")}, optional(code(named("Cell"))));
    if (name == "plan_named")
      return builtin({named("Str")}, optional(named("Cell")));
    if (name == "code_named")
      return builtin({named("Str")}, optional(code(named("Cell"))));
    if (name == "label_of")
      return builtin({named("Cell")}, optional(named("Str")));
    if (name == "validate") return builtin({named("Cell")}, named("Bool"));
    if (name == "weight_of")
      return builtin({named("Cell")}, optional(named("Float")));
    if (name == "value_of") return builtin({named("Cell")}, any);
    if (name == "next_rand") return builtin({named("Cell")}, named("Int"));
    // §6.1's mutation half (phase 4): the graft verbs, their durable
    // learn_* twins — the "conditional on P16" flag means transport of
    // the patch records is phase 6's seam, while the session machinery
    // is this surface — plus prune/unlearn, set_weight and
    // sprout_synapse. Every one is impure: each writes arena edges.
    // A bare `Code` parameter is the parameterized wildcard —
    // `Type{"Code", {}}` accepts `Code<T>` for any T (`compatible`'s
    // empty-argument arm); `body` pins `Code<()>` explicitly.
    const auto fragment = Type{"Code", {}};
    const auto bodyCode = code(unit);
    if (name == "graft_plan" || name == "learn_plan")
      return builtin({named("Str"), named("Str"), fragment, bodyCode}, unit,
                     false);
    if (name == "graft_handler" || name == "learn_handler")
      return builtin(
          {named("Str"), named("Str"), fragment, bodyCode, named("Int")}, unit,
          false);
    if (name == "graft_where" || name == "learn_where")
      return builtin({named("Cell"), fragment}, unit, false);
    if (name == "graft_body" || name == "learn_body")
      return builtin({named("Cell"), bodyCode}, unit, false);
    if (name == "graft_method" || name == "learn_method")
      return builtin({named("Str"), named("Str"), fragment}, unit, false);
    if (name == "prune" || name == "unlearn")
      return builtin({named("Cell")}, unit, false);
    if (name == "set_weight")
      return builtin({named("Cell"), named("Float")}, unit, false);
    if (name == "sprout_synapse")
      return builtin({named("Str"), named("Str"), named("Float")},
                     named("Cell"), false);
  }
  if (diagnose)
    report(DiagnosticCode::UnknownName,
           "unknown method/function '" + name + "'", callee.loc);
  return std::nullopt;
}

void Checker::purityWork(SourceLocation loc) {
  if (purityWork_ == 0)
    throw Diagnostic{DiagnosticCode::InvalidDeclaration,
                     "purity analysis exceeds CheckLimits::maxPurityWork", loc};
  --purityWork_;
}

bool Checker::pureBody(const Expr &expression, const Decl *owner) {
  purityWork(expression.loc);
  switch (expression.kind) {
  case ExprKind::Binary:
    if (expression.text == ":=" || expression.text == "<-") return false;
    break;
  case ExprKind::Call: {
    if (expression.children.empty()) return false;
    auto callee = function(*expression.children.front(), owner, false);
    if (!callee || !callee->pure || callee->effect) return false;
    break;
  }
  case ExprKind::Emit:
  case ExprKind::Tell:
  case ExprKind::Untell:
  case ExprKind::Believe:
  case ExprKind::Forget:
  case ExprKind::Revise:
  case ExprKind::Spawn:
  case ExprKind::Adopt:
  case ExprKind::Achieve:
  case ExprKind::Ask:
  case ExprKind::AwaitTerm:
  case ExprKind::AwaitEvent:
  case ExprKind::Drop:
  case ExprKind::Install:
  case ExprKind::Uninstall:
  case ExprKind::Mount:
  case ExprKind::Unmount:
  case ExprKind::Stop:
  case ExprKind::Terminate:
  case ExprKind::Yield:
  case ExprKind::Consume:
    return false;
  case ExprKind::Cells: {
    // A path that creates is an arena write; traversal alone is pure.
    const auto path = parseCellsPath(expression.text, expression.loc);
    return !(path && path->mutating);
  }
  case ExprKind::Code:
    // §5.2: the literal itself is a pure value — it produces a fragment and
    // has no run-time arena effect, whatever the body's own purity is.
    return true;
  case ExprKind::Splice:
    // An unexpanded splice never types as pure — expand substitutes or
    // refuses it before check in the real pipeline, and a direct-check
    // caller gets the same conservative answer.
    return false;
  case ExprKind::Static:
    // §5.5: the folded node is pure exactly when its operand is — the
    // operand's own purity is what the 'static' arm verifies, so the
    // same answer serves derivePurity, conditions and this check alike.
    return !expression.children.empty() && expression.children.front() &&
           pureBody(*expression.children.front(), owner);
  default:
    break;
  }
  for (const auto &child : expression.children)
    if (child && !pureBody(*child, owner)) return false;
  const Expr *extra[] = {expression.scope.get(), expression.recipients.get(),
                         expression.deadline.get(), expression.guard.get(),
                         expression.recovery.get()};
  for (const auto *child : extra)
    if (child && !pureBody(*child, owner)) return false;
  return true;
}
void Checker::derivePurity() {
  for (const auto &[method, owner] : methodOwners_) {
    (void)owner;
    purityWork(method->loc);
    pure_[method] = !method->effect;
  }
  bool changed;
  do {
    changed = false;
    for (const auto &[method, owner] : methodOwners_) {
      purityWork(method->loc);
      if (pure_[method] && (!method->body || !pureBody(*method->body, owner))) {
        pure_[method] = false;
        changed       = true;
      }
    }
  } while (changed);
}

void Checker::actionAllowed(const Expr &expression, Context context) {
  if (context.pure || context.readonly)
    report(DiagnosticCode::ImpureCondition,
           "action is not allowed in a pure condition or read-only answer",
           expression.loc);
}
void Checker::arguments(const Expr &expression, const std::vector<Type> &types,
                        Environment &env, Context context) {
  const auto count =
      expression.children.empty() ? 0U : expression.children.size() - 1;
  require(count == types.size(), DiagnosticCode::TypeMismatch,
          "wrong number of call arguments", expression.loc);
  for (std::size_t i = 0; i < count; ++i) {
    auto actual = infer(*expression.children[i + 1], env, operand(context));
    if (i < types.size())
      expectType(actual, types[i], expression.children[i + 1]->loc);
  }
}
Checker::MetaForm Checker::metaForm(const Expr &callee) const {
  std::string_view name;
  std::optional<std::string_view> qualifier;
  if (callee.kind == ExprKind::Identifier) {
    name = callee.text;
  } else if (callee.kind == ExprKind::Field && !callee.children.empty() &&
             callee.children.front()->kind == ExprKind::Identifier) {
    name      = callee.text;
    qualifier = callee.children.front()->text;
  } else {
    return MetaForm::None;
  }
  const bool run   = name == "run";
  const bool reify = name == "reify";
  if (!run && !reify) return MetaForm::None;
  for (const auto &imported : program_.imports) {
    if (imported.path != "std:meta") continue;
    const bool selected =
        imported.symbols.empty() ||
        std::ranges::find(imported.symbols, name) != imported.symbols.end();
    const bool qualified =
        !qualifier || *qualifier == imported.alias.value_or("std:meta");
    if (selected && qualified) return run ? MetaForm::Run : MetaForm::Reify;
  }
  return MetaForm::None;
}

Type Checker::metaRun(const Expr &expression, Environment &env,
                      Context context) {
  // §6.1's `run(c, [args])` — the only call whose argument list is
  // heterogeneous, which is why it is a special form: each element
  // types on its own, and a statically-known fragment checks arity
  // and element types now; anything else defers to the runtime
  // descriptor.
  const auto argc = expression.children.size() - 1;
  require(argc == 1 || argc == 2, DiagnosticCode::TypeMismatch,
          "run takes a fragment and an optional argument list", expression.loc);
  if (argc == 0) return unknown;
  const Expr &operandExpr = *expression.children[1];
  const auto operandType  = infer(operandExpr, env, operand(context));
  const bool codeShaped =
      operandType.name == "Code" && operandType.arguments.size() <= 1;
  require(codeShaped, DiagnosticCode::TypeMismatch,
          "run's operand must be a Code fragment", operandExpr.loc);

  const Expr *argsExpr = argc == 2 ? expression.children[2].get() : nullptr;
  std::vector<const Expr *> argExprs;
  std::vector<Type> argTypes;
  if (argsExpr) {
    require(argsExpr->kind == ExprKind::List, DiagnosticCode::TypeMismatch,
            "run's argument list must be a list literal", argsExpr->loc);
    if (argsExpr->kind == ExprKind::List) {
      for (const auto &item : argsExpr->children) {
        argExprs.push_back(item.get());
        argTypes.push_back(infer(*item, env, operand(context)));
      }
    }
  }

  if (operandExpr.kind == ExprKind::Code) {
    const FragmentKey key{operandExpr.loc, operandExpr.stamp
                                               ? operandExpr.stamp->ordinal
                                               : std::nullopt};
    if (const auto found = fragments_.find(key);
        found != fragments_.end() && found->second.body.body) {
      const auto &parameters = found->second.body.parameters;
      for (const auto &parameter : parameters)
        if (parameter.type.name == "Code")
          report(DiagnosticCode::InvalidDeclaration,
                 "run of a Code-parameterized fragment: '" + parameter.name +
                     "' is splice-only — it has no value-level "
                     "argument to receive",
                 parameter.loc);
      require(argTypes.size() == parameters.size(),
              DiagnosticCode::TypeMismatch,
              "run argument count differs from the fragment's "
              "declared arity",
              expression.loc);
      for (std::size_t i = 0; i < argExprs.size() && i < parameters.size(); ++i)
        expectType(argTypes[i], parameters[i].type, argExprs[i]->loc);
    }
  }
  // run executes; a pure or read-only position refuses it like every
  // other action.
  actionAllowed(expression, context);
  // The §6.1 row: `run` gives back T under Code<T>, unknown under the
  // bare-Code wildcard.
  if (!codeShaped || operandType.arguments.empty()) return unknown;
  return operandType.arguments.front();
}

Type Checker::metaReify(const Expr &expression, Environment &env,
                        Context context) {
  const auto argc = expression.children.size() - 1;
  require(argc == 1, DiagnosticCode::TypeMismatch,
          "reify takes exactly one fragment", expression.loc);
  if (argc == 0) return unknown;
  const auto operandType =
      infer(*expression.children[1], env, operand(context));
  const bool codeShaped =
      operandType.name == "Code" && operandType.arguments.size() <= 1;
  require(codeShaped, DiagnosticCode::TypeMismatch,
          "reify's operand must be a Code fragment",
          expression.children[1]->loc);
  actionAllowed(expression, context);
  return codeShaped ? operandType : unknown;
}

Type Checker::call(const Expr &expression, Environment &env, Context context) {
  if (expression.children.empty()) {
    report(DiagnosticCode::InvalidDeclaration, "call has no callee",
           expression.loc);
    return unknown;
  }
  // The std:meta special forms intercept before ordinary callee
  // resolution so an unimported `run`/`reify` still resolves as a
  // user decl.
  if (metaImported_) {
    switch (metaForm(*expression.children.front())) {
    case MetaForm::Run:
      return metaRun(expression, env, context);
    case MetaForm::Reify:
      return metaReify(expression, env, context);
    case MetaForm::None:
      break;
    }
  }
  auto signature = function(*expression.children.front(), context.owner);
  if (!signature) {
    for (std::size_t i = 1; i < expression.children.size(); ++i)
      infer(*expression.children[i], env, operand(context));
    return unknown;
  }
  arguments(expression, signature->parameters, env, context);
  if ((context.pure || context.readonly) && !signature->pure)
    report(DiagnosticCode::ImpureCondition,
           "called method is not transitively pure", expression.loc);
  if (signature->effect) {
    const bool plan = context.body == Body::Plan && context.suspend &&
                      context.bounded && !context.beliefLoop;
    const bool handler =
        context.body == Body::Handler && context.bounded && context.stageEffect;
    require(plan || handler, DiagnosticCode::InvalidPosition,
            "effect calls must be a plan suspension statement or a bare "
            "bounded handler action",
            expression.loc);
  }
  if (expression.recovery) {
    require(
        signature->effect && context.body == Body::Plan && context.suspend &&
            context.bounded && !context.beliefLoop,
        DiagnosticCode::InvalidPosition,
        "effect recovery belongs only to a plan effect suspension statement",
        expression.loc);
    return recover(expression, signature->result, env, context);
  }
  return signature->result;
}

Type Checker::place(const Expr &expression, Environment &env, Context context) {
  if (expression.kind == ExprKind::Identifier) {
    const auto found = env.find(expression.text);
    if (found == env.end()) {
      report(DiagnosticCode::UnknownName,
             "unknown assignment target '" +
                 std::string(displayName(expression.text)) + "'",
             expression.loc);
      return unknown;
    }
    require(found->second.writable, DiagnosticCode::InvalidPosition,
            "assignment target is read-only", expression.loc);
    return found->second.type;
  }
  if (expression.kind == ExprKind::Field && !expression.children.empty()) {
    (void)place(*expression.children.front(), env, context);
    auto base = infer(*expression.children.front(), env, operand(context));
    if (auto field = fieldType(base, expression.text)) return *field;
    report(DiagnosticCode::UnknownName,
           "unknown field '" + expression.text + "'", expression.loc);
    return unknown;
  }
  if (expression.kind == ExprKind::Index && expression.children.size() == 2) {
    (void)place(*expression.children.front(), env, context);
    auto base  = infer(*expression.children.front(), env, operand(context));
    auto index = infer(*expression.children[1], env, operand(context));
    if (base.name == "List" && base.arguments.size() == 1) {
      expectType(index, named("Int"), expression.children[1]->loc);
      return base.arguments.front();
    }
    if (base.name == "Map" && base.arguments.size() == 2) {
      expectType(index, base.arguments.front(), expression.children[1]->loc);
      return base.arguments[1];
    }
  }
  report(DiagnosticCode::InvalidPosition,
         "assignment requires a local, field or index place", expression.loc);
  return unknown;
}

Type Checker::term(const Expr &expression, Environment &env, Context context,
                   bool bind) {
  const auto *predicate = global(expression.text);
  if (!predicate || predicate->kind != DeclKind::Predicate) {
    report(DiagnosticCode::UnknownName,
           "unknown predicate '" + expression.text + "'", expression.loc);
    return unknown;
  }
  require(expression.children.size() == predicate->parameters.size(),
          DiagnosticCode::TypeMismatch, "wrong number of predicate arguments",
          expression.loc);
  // An argument binds a `?name` into env at its declared position
  // type; inside a record literal the same rule applies field by
  // field, the record's declared field list supplying each position's
  // type. A record in a term spells the whole shape — the marshal is
  // positional, so a field set short of the declaration could never
  // unify and is refused like infer's record arm.
  const std::function<void(const Expr &, const Type &)> bindArgument =
      [&](const Expr &argument, const Type &expected) {
    if (argument.kind == ExprKind::LogicVariable) {
      require(snake(argument.text), DiagnosticCode::InvalidIdentifier,
              "query bindings must be snake_case", argument.loc);
      if (argument.text == "_") {
        require(bind, DiagnosticCode::InvalidPosition,
                "asserted facts cannot contain a wildcard", argument.loc);
        return;
      }
      auto found = env.find(argument.text);
      if (found == env.end()) {
        if (bind) {
          reservedName(argument.text, argument.loc);
          env[argument.text] = {expected, true, context.depth};
        } else
          report(DiagnosticCode::UnknownName,
                 "asserted facts must be ground; unbound '" + argument.text +
                     "'",
                 argument.loc);
      } else
        expectType(read(found->second.type), expected, argument.loc);
      return;
    }
    if (argument.kind == ExprKind::Identifier && argument.text == "_") {
      require(bind, DiagnosticCode::InvalidPosition,
              "asserted facts cannot contain a wildcard", argument.loc);
      return;
    }
    if (argument.kind == ExprKind::Record)
      if (const auto *decl = global(argument.text);
          decl && decl->kind == DeclKind::Record) {
        std::set<std::string> supplied;
        for (std::size_t f = 0;
             f < argument.children.size() && f < argument.names.size(); ++f) {
          const Type *fieldType = nullptr;
          for (const auto &param : decl->parameters)
            if (param.name == argument.names[f]) {
              fieldType = &param.type;
              break;
            }
          if (!fieldType) {
            report(DiagnosticCode::UnknownName,
                   "unknown record field '" + argument.names[f] + "'",
                   argument.loc);
            continue;
          }
          supplied.insert(argument.names[f]);
          bindArgument(*argument.children[f], *fieldType);
        }
        for (const auto &param : decl->parameters)
          require(supplied.contains(param.name),
                  DiagnosticCode::TypeMismatch,
                  "missing record field '" + param.name + "'", argument.loc);
        return;
      }
    expectType(infer(argument, env, operand(context)), expected,
               argument.loc);
  };
  for (std::size_t i = 0; i < expression.children.size(); ++i) {
    const auto expected = i < predicate->parameters.size()
                              ? predicate->parameters[i].type
                              : unknown;
    bindArgument(*expression.children[i], expected);
  }
  return unit;
}

Type Checker::condition(const Expr &expression, Environment &env,
                        Context context) {
  context.pure        = true;
  context.stageEffect = false;
  context.suspend     = false;
  auto value          = infer(expression, env, context);
  if (value.name == "Bool" &&
      !(expression.kind == ExprKind::Binary && comparison(expression.text)))
    report(DiagnosticCode::TypeMismatch,
           "a Bool condition must compare with '== true' or '== false'",
           expression.loc);
  return value;
}

void Checker::deadline(const Expr *expression, Environment &env,
                       Context context, bool required) {
  if (!expression) {
    if (required)
      report(DiagnosticCode::MissingDeadline,
             "an external wait requires a deadline", {});
    return;
  }
  expectType(infer(*expression, env, operand(context)), named("Duration"),
             expression->loc);
  const Expr *literal = expression;
  bool negative       = false;
  if (expression->kind == ExprKind::Unary && expression->text == "-" &&
      expression->children.size() == 1) {
    literal  = expression->children.front().get();
    negative = true;
  }
  if (literal->kind == ExprKind::Duration) {
    auto text = literal->text;
    std::erase(text, '_');
    double value{};
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), value);
    require(parsed.ec == std::errc{} && std::isfinite(value) && value > 0.0 &&
                !negative,
            DiagnosticCode::MissingDeadline,
            "a literal deadline must be positive", expression->loc);
  }
}

Type Checker::construct(const Expr &expression, Environment &env,
                        Context context, DeclKind kind, Type result) {
  if (expression.children.empty() ||
      expression.children.front()->kind != ExprKind::Call ||
      expression.children.front()->children.empty()) {
    report(DiagnosticCode::InvalidDeclaration,
           "action requires a named constructor/goal call", expression.loc);
    return unknown;
  }
  const auto &invocation = *expression.children.front();
  const auto &callee     = *invocation.children.front();
  const auto *decl = kind == DeclKind::Goal ? goal(context.owner, callee.text)
                                            : global(callee.text);
  if (!decl || decl->kind != kind) {
    report(DiagnosticCode::UnknownName,
           "unknown action target '" + callee.text + "'", callee.loc);
    return unknown;
  }
  signatureWork(*decl);
  std::vector<Type> types;
  for (const auto &parameter : decl->parameters)
    types.push_back(parameter.type);
  arguments(invocation, types, env, context);
  if (kind == DeclKind::Goal) goalConditions(*decl, context.owner);
  return result;
}
Type Checker::recover(const Expr &expression, Type value, Environment &env,
                      Context context) {
  if (!expression.recovery) return value;
  auto recoveryEnv        = copyEnvironment(env, expression.loc);
  recoveryEnv["cause"]    = {named("Str"), false, context.depth};
  auto recoveryContext    = context;
  recoveryContext.bounded = context.bounded;
  auto alternative = infer(*expression.recovery, recoveryEnv, recoveryContext);
  if (!context.discard)
    expectType(alternative, value, expression.recovery->loc);
  return value;
}

bool Checker::staticClosed(const Expr &expression, Environment &env,
                           bool diagnose) {
  resolutionWork(expression.loc);
  const auto refuse = [&](std::string detail) {
    if (diagnose)
      report(DiagnosticCode::InvalidPosition, std::move(detail),
             expression.loc);
    return false;
  };
  bool closed = true;
  // Fixed-role metadata walks in the ambient env except the recovery
  // arm, which binds `cause` for its own expression.
  for (const ExprPtr *slot : {&expression.scope, &expression.recipients,
                              &expression.deadline, &expression.guard})
    if (*slot) closed &= staticClosed(**slot, env, diagnose);
  if (expression.recovery) {
    auto local     = copyEnvironment(env, expression.loc);
    local["cause"] = {named("Str"), false, 0};
    closed = closed && staticClosed(*expression.recovery, local, diagnose);
  }
  switch (expression.kind) {
  case ExprKind::Integer:
  case ExprKind::Float:
  case ExprKind::String:
  case ExprKind::Duration:
  case ExprKind::Boolean:
  case ExprKind::None:
  case ExprKind::Unit:
    return closed;
  case ExprKind::Identifier:
  case ExprKind::LogicVariable: {
    // The implicit names name arena positions — refused as a form even
    // where an env binding exists for them.
    if (expression.text == "self" || expression.text == "parent" ||
        expression.text == "inner" || expression.text == "outer" ||
        expression.text == "occurrence")
      return refuse("'static' operand may not name the implicit '" +
                    std::string(expression.text) + "'");
    const auto found = env.find(expression.text);
    if (found == env.end()) return false; // infer's unknown-name covers it
    if (found->second.unvalued)
      return refuse("'static' operand name '" +
                    std::string(displayName(expression.text)) +
                    "' is unvalued at expansion — code parameters and "
                    "cap$N captures have no value until instantiation");
    if (!found->second.closed)
      return refuse("'static' operand name '" +
                    std::string(displayName(expression.text)) +
                    "' is not closed — it is bound at run time, not to a "
                    "literal or a static value");
    return closed;
  }
  case ExprKind::Cells:
    // Any mode — a non-mutating cells"…" is pureBody-pure but would read
    // the compile arena at evaluation time.
    return refuse("'static' operand may not read the arena through cells\"…\"");
  case ExprKind::Term:
  case ExprKind::Believes:
  case ExprKind::FindAll:
  case ExprKind::ForBelieves:
    return refuse("'static' operand may not read beliefs");
  case ExprKind::Call:
    // `run`, host and effect calls, and every other call alike — a
    // stage-0 eval has no callables; program-side names are not closed.
    return refuse(
        "'static' operand may not call methods or host/effect functions "
        "('run' included)");
  case ExprKind::Code: {
    // §5.6's per-fragment classification is the literal's
    // outside-reference verdict — a not-closed record leaves the whole
    // operand not-closed; a missing record is infer's named refusal.
    const FragmentKey key{expression.loc, expression.stamp
                                              ? expression.stamp->ordinal
                                              : std::nullopt};
    const auto found = fragments_.find(key);
    if (found == fragments_.end()) return false;
    if (!found->second.closed)
      return refuse("'static' operand's code\"…\" literal is not "
                    "closed — it references program-side names or cells");
    return closed;
  }
  case ExprKind::Static:
    // The inner static's operand must be closed for its own
    // evaluation; its own arms are verified when infer reaches it.
    for (const auto &child : expression.children)
      if (child) closed &= staticClosed(*child, env, diagnose);
    return closed;
  case ExprKind::Splice:
    // A residual splice is the pipeline's named refusal elsewhere.
    return false;
  case ExprKind::Let: {
    const auto value =
        expression.children.empty() || !expression.children.front()
            ? false
            : staticClosed(*expression.children.front(), env, diagnose);
    env[expression.text] = {unit, true, 0, value};
    return closed && value;
  }
  case ExprKind::If: {
    // Each arm is judged in its own env copy, as infer's branches are.
    for (const auto &child : expression.children) {
      auto local = copyEnvironment(env, expression.loc);
      if (child) closed &= staticClosed(*child, local, diagnose);
    }
    return closed;
  }
  case ExprKind::While: {
    auto local = copyEnvironment(env, expression.loc);
    for (const auto &child : expression.children)
      if (child) closed &= staticClosed(*child, local, diagnose);
    return closed;
  }
  case ExprKind::For: {
    auto local = copyEnvironment(env, expression.loc);
    if (!expression.children.empty() && expression.children.front())
      closed &= staticClosed(*expression.children.front(), local, diagnose);
    if (expression.binding && *expression.binding != "_")
      local[*expression.binding] = {unknown, true, 0};
    for (std::size_t i = 1; i < expression.children.size(); ++i)
      if (expression.children[i])
        closed &= staticClosed(*expression.children[i], local, diagnose);
    return closed;
  }
  // Impure statement forms — the pureBody arm names them; they are not
  // arena-reading *forms*, so the walk only marks them not-closed.
  case ExprKind::Emit:
  case ExprKind::Tell:
  case ExprKind::Untell:
  case ExprKind::Believe:
  case ExprKind::Forget:
  case ExprKind::Revise:
  case ExprKind::Spawn:
  case ExprKind::Adopt:
  case ExprKind::Achieve:
  case ExprKind::Ask:
  case ExprKind::AwaitTerm:
  case ExprKind::AwaitEvent:
  case ExprKind::Drop:
  case ExprKind::Install:
  case ExprKind::Uninstall:
  case ExprKind::Mount:
  case ExprKind::Unmount:
  case ExprKind::Stop:
  case ExprKind::Terminate:
  case ExprKind::Yield:
  case ExprKind::Consume:
  case ExprKind::Return:
    return false;
  default:
    for (const auto &child : expression.children)
      if (child) closed &= staticClosed(*child, env, diagnose);
    return closed;
  }
}

Type Checker::infer(const Expr &expression, Environment &env, Context context) {
  const auto child = [&](std::size_t index, Context mode) {
    if (index >= expression.children.size()) {
      report(DiagnosticCode::InvalidDeclaration, "malformed expression tree",
             expression.loc);
      return unknown;
    }
    return infer(*expression.children[index], env, mode);
  };
  switch (expression.kind) {
  case ExprKind::Integer: {
    auto text = expression.text;
    std::erase(text, '_');
    const bool hex = text.starts_with("0x") || text.starts_with("0X");
    std::uint64_t value{};
    const auto parsed =
        std::from_chars(text.data() + (hex ? 2 : 0), text.data() + text.size(),
                        value, hex ? 16 : 10);
    require(parsed.ec == std::errc{} &&
                parsed.ptr == text.data() + text.size() &&
                value <= static_cast<std::uint64_t>(
                             std::numeric_limits<std::int64_t>::max()),
            DiagnosticCode::TypeMismatch,
            "integer literal is outside the Int range", expression.loc);
    return named("Int");
  }
  case ExprKind::Float:
    return named("Float");
  case ExprKind::String:
    return named("Str");
  case ExprKind::Duration:
    return named("Duration");
  case ExprKind::Boolean:
    return named("Bool");
  case ExprKind::None:
    return absent;
  case ExprKind::Unit:
    return unit;
  case ExprKind::Identifier:
  case ExprKind::LogicVariable: {
    auto found = env.find(expression.text);
    if (found != env.end()) return read(found->second.type);
    report(DiagnosticCode::UnknownName,
           "unknown name '" + std::string(displayName(expression.text)) + "'",
           expression.loc);
    return unknown;
  }
  case ExprKind::Unary: {
    if (expression.text == "-" && expression.children.size() == 1 &&
        expression.children.front()->kind == ExprKind::Integer) {
      auto text = expression.children.front()->text;
      std::erase(text, '_');
      const bool hexadecimal = text.starts_with("0x") || text.starts_with("0X");
      std::uint64_t magnitude{};
      const auto parsed = std::from_chars(text.data() + (hexadecimal ? 2 : 0),
                                          text.data() + text.size(), magnitude,
                                          hexadecimal ? 16 : 10);
      if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
          magnitude == static_cast<std::uint64_t>(
                           std::numeric_limits<std::int64_t>::max()) +
                           1U)
        return named("Int");
    }
    if (expression.text == "not") {
      auto local = copyEnvironment(env, expression.loc);
      auto value =
          expression.children.empty()
              ? unknown
              : infer(*expression.children.front(), local, operand(context));
      if (value.name == "Bool" &&
          !(expression.children[0]->kind == ExprKind::Binary &&
            comparison(expression.children[0]->text)))
        report(DiagnosticCode::TypeMismatch,
               "a Bool operand of 'not' must be compared explicitly",
               expression.loc);
      return unit;
    }
    auto value = child(0, operand(context));
    if (expression.text == "-") {
      require(numeric(value) || value.name == "Duration" || value == unknown,
              DiagnosticCode::TypeMismatch,
              "unary '-' requires a numeric value or Duration", expression.loc);
      return value;
    }
    if (expression.text == "*") {
      require(value.name == "List" || value.name == "Map" ||
                  value.name == "Str" || value == unknown,
              DiagnosticCode::TypeMismatch,
              "size requires a string, list or map", expression.loc);
      return named("Int");
    }
    if (expression.text == "!") {
      if (value.name == "Str") return value;
      if (value.name == "List" && value.arguments.size() == 1)
        return value.arguments.front();
      report(DiagnosticCode::TypeMismatch,
             "generation requires a string or list", expression.loc);
      return unknown;
    }
    report(DiagnosticCode::InvalidDeclaration, "unknown unary operator",
           expression.loc);
    return unknown;
  }
  case ExprKind::Binary: {
    if (expression.children.size() != 2) {
      report(DiagnosticCode::InvalidDeclaration,
             "binary expression needs two operands", expression.loc);
      return unknown;
    }
    const auto &leftExpr  = *expression.children[0];
    const auto &rightExpr = *expression.children[1];
    if (expression.text == ":=" || expression.text == "<-") {
      actionAllowed(expression, context);
      if (expression.text == ":=")
        require(context.bounded, DiagnosticCode::InvalidPosition,
                "':=' is allowed only in a bounded position; use '<-' inside "
                "search",
                expression.loc);
      auto target = place(leftExpr, env, context);
      auto rhs    = operand(context);
      rhs.bounded = context.bounded && directWaitPosition(rightExpr);
      rhs.suspend = context.suspend && rhs.bounded && expression.text == ":=";
      expectType(infer(rightExpr, env, rhs), target, rightExpr.loc);
      return unit;
    }
    if (expression.text == "|") {
      auto leftEnv  = copyEnvironment(env, expression.loc);
      auto rightEnv = copyEnvironment(env, expression.loc);
      auto left     = infer(leftExpr, leftEnv, operand(context));
      auto right    = infer(rightExpr, rightEnv, operand(context));
      if (left == bottom) {
        env = std::move(rightEnv);
        return right;
      }
      if (right == bottom) {
        env = std::move(leftEnv);
        return left;
      }
      for (const auto &[name, binding] : leftEnv) {
        resolutionWork(expression.loc);
        auto other = rightEnv.find(name);
        if (!env.contains(name) && other != rightEnv.end() &&
            binding.type == other->second.type)
          env[name] = binding;
      }
      expectType(right, left, rightExpr.loc);
      for (const auto *part : {&leftExpr, &rightExpr}) {
        const auto &value = part == &leftExpr ? left : right;
        if (value.name == "Bool" &&
            !(part->kind == ExprKind::Binary && comparison(part->text)))
          report(DiagnosticCode::TypeMismatch,
                 "Bool alternatives must compare explicitly", part->loc);
      }
      return left;
    }
    auto left  = infer(leftExpr, env, operand(context));
    auto right = infer(rightExpr, env, operand(context));
    if (expression.text == "&") {
      if (left.name == "Bool" &&
          !(leftExpr.kind == ExprKind::Binary && comparison(leftExpr.text)))
        report(DiagnosticCode::TypeMismatch,
               "Bool conjunction operands must compare explicitly",
               leftExpr.loc);
      if (right.name == "Bool" &&
          !(rightExpr.kind == ExprKind::Binary && comparison(rightExpr.text)))
        report(DiagnosticCode::TypeMismatch,
               "Bool conjunction operands must compare explicitly",
               rightExpr.loc);
      return right;
    }
    if (comparison(expression.text)) {
      expectType(right, left, rightExpr.loc);
      if (expression.text != "==" && expression.text != "!=") {
        require(numeric(left) || left.name == "Str" || left.name == "Time" ||
                    left.name == "Duration" || left == unknown,
                DiagnosticCode::TypeMismatch,
                "ordered comparisons require numeric, Str, Time or Duration "
                "operands",
                expression.loc);
      }
      return right;
    }
    if (expression.text == "?") {
      expectType(left, named("Str"), leftExpr.loc);
      return right;
    }
    if (expression.text == "++") {
      require(left.name == "Str" || left.name == "List" || left == unknown,
              DiagnosticCode::TypeMismatch, "'++' requires strings or lists",
              expression.loc);
      expectType(right, left, rightExpr.loc);
      return left;
    }
    if ((expression.text == "+" || expression.text == "-") &&
        left.name == "Time" && right.name == "Duration")
      return left;
    if (expression.text == "-" && left.name == "Time" && right.name == "Time")
      return named("Duration");
    require(numeric(left) || left.name == "Duration" || left == unknown,
            DiagnosticCode::TypeMismatch,
            "arithmetic requires numeric operands", expression.loc);
    expectType(right, left, rightExpr.loc);
    if (expression.text == "%") expectType(left, named("Int"), leftExpr.loc);
    return left;
  }
  case ExprKind::Range:
    for (const auto &item : expression.children)
      expectType(infer(*item, env, operand(context)), named("Int"), item->loc);
    return named("<range>");
  case ExprKind::Call:
    return call(expression, env, context);
  case ExprKind::Index: {
    auto value = child(0, operand(context));
    auto index = child(1, operand(context));
    if (value.name == "List" && value.arguments.size() == 1) {
      expectType(index, named("Int"), expression.loc);
      return read(value.arguments.front());
    }
    if (value.name == "Str") {
      expectType(index, named("Int"), expression.loc);
      return value;
    }
    if (value.name == "Map" && value.arguments.size() == 2) {
      expectType(index, value.arguments.front(), expression.loc);
      return read(value.arguments[1]);
    }
    report(DiagnosticCode::TypeMismatch,
           "indexing requires a list, string or map", expression.loc);
    return unknown;
  }
  case ExprKind::Field: {
    auto base = child(0, operand(context));
    if (auto type = fieldType(base, expression.text)) return read(*type);
    report(DiagnosticCode::UnknownName,
           "unknown field '" + expression.text + "' on " + typeName(base),
           expression.loc);
    return unknown;
  }
  case ExprKind::Block: {
    auto local = copyEnvironment(env, expression.loc);
    ++context.depth;
    Type result = unit;
    for (std::size_t i = 0; i < expression.children.size(); ++i) {
      auto statement = context;
      statement.discard =
          context.discard || i + 1 != expression.children.size();
      statement.stageEffect = context.bounded && context.body == Body::Handler;
      statement.suspend =
          context.bounded && context.body == Body::Plan && !context.beliefLoop;
      result = infer(*expression.children[i], local, statement);
    }
    return result;
  }
  case ExprKind::Let: {
    require(snake(expression.text) && expression.text != "_",
            DiagnosticCode::InvalidIdentifier,
            "local names must be snake_case and cannot be '_'", expression.loc);
    reservedName(expression.text, expression.loc);
    auto rhs    = operand(context);
    rhs.bounded = context.bounded && !expression.children.empty() &&
                  directWaitPosition(*expression.children.front());
    rhs.suspend = context.suspend && rhs.bounded;
    auto value  = child(0, rhs);
    if (expression.annotation) {
      validateType(*expression.annotation, expression.loc);
      expectType(value, *expression.annotation, expression.loc);
      value = *expression.annotation;
    } else if (value == absent || (value.name == "List" &&
                                   value.arguments == std::vector<Type>{any})) {
      report(DiagnosticCode::TypeMismatch,
             "none and an empty list require a local type annotation",
             expression.loc);
    }
    auto previous = env.find(expression.text);
    require(previous == env.end() || previous->second.depth != context.depth,
            DiagnosticCode::DuplicateName,
            "duplicate local '" + expression.text + "'", expression.loc);
    // §5.5's rewritten closedness: a `let` name is closed for `static`
    // operands when its initializer is itself closed — a literal, a
    // `static`-bound value, or a composite of closed names. The walk is
    // quiet here: a non-closed value is legal to bind, it only leaves
    // the name unavailable to staged evaluation.
    const auto closed =
        expression.children.empty()
            ? false
            : staticClosed(*expression.children.front(), env, false);
    env[expression.text] = {value, true, context.depth, closed};
    return unit;
  }
  case ExprKind::List: {
    Type element = any;
    for (const auto &item : expression.children) {
      auto value = infer(*item, env, operand(context));
      if (element == any)
        element = value;
      else
        expectType(value, element, item->loc);
    }
    return list(element);
  }
  case ExprKind::Cells: {
    const auto path = parseCellsPath(expression.text, expression.loc);
    if (!path) {
      report(path.error().code, path.error().detail, expression.loc);
      return unknown;
    }
    if (path->mutating) actionAllowed(expression, context);
    if (path->anchor.kind == CellsPath::Anchor::Kind::Variable) {
      const auto &name   = path->anchor.variable;
      const auto binding = env.find(name);
      if (binding == env.end()) {
        report(DiagnosticCode::UnknownName,
               "cells anchor '$" + name + "' is not bound", expression.loc);
      } else {
        expectType(read(binding->second.type), named("Cell"), expression.loc);
      }
    }
    return list(named("Cell"));
  }
  case ExprKind::Record: {
    auto *decl = global(expression.text);
    if (!decl ||
        (decl->kind != DeclKind::Record && decl->kind != DeclKind::Event)) {
      report(DiagnosticCode::UnknownName,
             "unknown record/event type '" + expression.text + "'",
             expression.loc);
      return unknown;
    }
    std::map<std::string, Type> fields;
    std::set<std::string> visited;
    for (const Decl *current = decl;
         current && visited.insert(current->name).second;) {
      resolutionWork(expression.loc);
      for (const auto &field : current->parameters) {
        resolutionWork(field.loc);
        fields[field.name] = field.type;
      }
      current = current->kind == DeclKind::Event && !current->names.empty()
                    ? global(current->names.front())
                    : nullptr;
    }
    std::set<std::string> supplied;
    require(expression.names.size() == expression.children.size(),
            DiagnosticCode::InvalidDeclaration,
            "record field names and values do not match", expression.loc);
    for (std::size_t i = 0;
         i < expression.names.size() && i < expression.children.size(); ++i) {
      auto actual      = infer(*expression.children[i], env, operand(context));
      const auto &name = expression.names[i];
      require(supplied.insert(name).second, DiagnosticCode::DuplicateName,
              "duplicate record field '" + name + "'", expression.loc);
      if (auto found = fields.find(name); found != fields.end())
        expectType(actual, found->second, expression.children[i]->loc);
      else
        report(DiagnosticCode::UnknownName,
               "unknown or implicit record field '" + name + "'",
               expression.loc);
    }
    for (const auto &[name, type] : fields) {
      (void)type;
      require(supplied.contains(name), DiagnosticCode::TypeMismatch,
              "missing record field '" + name + "'", expression.loc);
    }
    return named(expression.text);
  }
  case ExprKind::If: {
    auto testEnv = copyEnvironment(env, expression.loc);
    if (!expression.children.empty())
      condition(*expression.children[0], testEnv, operand(context));
    auto thenEnv  = testEnv;
    auto thenType = expression.children.size() > 1
                        ? infer(*expression.children[1], thenEnv, context)
                        : unknown;
    if (expression.children.size() < 3) {
      expectType(thenType, unit, expression.loc);
      return unit;
    }
    auto elseEnv  = copyEnvironment(env, expression.loc);
    auto elseType = infer(*expression.children[2], elseEnv, context);
    if (context.discard) return unit;
    if (thenType == bottom) return elseType;
    if (elseType == bottom) return thenType;
    expectType(elseType, thenType, expression.loc);
    return thenType;
  }
  case ExprKind::While: {
    auto local = copyEnvironment(env, expression.loc);
    if (!expression.children.empty())
      condition(*expression.children[0], local, operand(context));
    if (expression.children.size() > 1)
      infer(*expression.children[1], local, context);
    return unit;
  }
  case ExprKind::For: {
    auto iterable = child(0, operand(context));
    auto local    = copyEnvironment(env, expression.loc);
    Type element  = iterable.name == "<range>" ? named("Int")
                    : iterable.name == "List" && iterable.arguments.size() == 1
                        ? iterable.arguments.front()
                        : unknown;
    require(
        element != unknown || iterable == unknown, DiagnosticCode::TypeMismatch,
        "for-in requires a list or range, not an individual generated value",
        expression.loc);
    if (expression.binding && *expression.binding != "_") {
      require(snake(*expression.binding), DiagnosticCode::InvalidIdentifier,
              "loop binding must be snake_case", expression.loc);
      reservedName(*expression.binding, expression.loc);
      local[*expression.binding] = {element, true, context.depth + 1};
    }
    if (expression.children.size() > 1)
      infer(*expression.children[1], local, context);
    return unit;
  }
  case ExprKind::ForBelieves: {
    auto local = copyEnvironment(env, expression.loc);
    if (!expression.children.empty())
      term(*expression.children[0], local, context, true);
    context.beliefLoop = true;
    context.suspend    = false;
    if (expression.children.size() > 1)
      infer(*expression.children[1], local, context);
    return unit;
  }
  case ExprKind::Term:
    return term(expression, env, context, true);
  case ExprKind::Believes:
    if (!expression.children.empty())
      term(*expression.children[0], env, context, true);
    if (expression.scope)
      expectType(infer(*expression.scope, env, operand(context)),
                 named("Agent"), expression.scope->loc);
    return unit;
  case ExprKind::FindAll: {
    auto local = copyEnvironment(env, expression.loc);
    if (expression.children.size() > 1)
      term(*expression.children[1], local, context, true);
    auto value = expression.children.empty()
                     ? unknown
                     : infer(*expression.children[0], local, operand(context));
    return list(value);
  }
  case ExprKind::Emit: {
    actionAllowed(expression, context);
    auto value = child(0, operand(context));
    require(event(value.name) || value == unknown, DiagnosticCode::TypeMismatch,
            "emit requires an event value", expression.loc);
    if (expression.scope)
      expectType(infer(*expression.scope, env, operand(context)),
                 named("Space"), expression.scope->loc);
    if (expression.recipients) {
      auto target = infer(*expression.recipients, env, operand(context));
      require(compatible(target, named("Agent")) ||
                  compatible(target, list(named("Agent"))),
              DiagnosticCode::TypeMismatch,
              "recipients must be an Agent or list of Agents",
              expression.recipients->loc);
    }
    return unit;
  }
  case ExprKind::Tell:
  case ExprKind::Untell:
    actionAllowed(expression, context);
    expectType(child(0, operand(context)), named("Agent"), expression.loc);
    if (expression.children.size() > 1)
      term(*expression.children[1], env, context, false);
    return unit;
  case ExprKind::Believe:
  case ExprKind::Forget:
  case ExprKind::Revise:
    actionAllowed(expression, context);
    if (!expression.children.empty())
      term(*expression.children[0], env, context,
           expression.kind == ExprKind::Forget);
    return unit;
  case ExprKind::Spawn:
    actionAllowed(expression, context);
    if (expression.scope)
      expectType(infer(*expression.scope, env, operand(context)),
                 named("Space"), expression.scope->loc);
    return construct(expression, env, context, DeclKind::Agent, named("Agent"));
  case ExprKind::Adopt:
    actionAllowed(expression, context);
    return construct(expression, env, context, DeclKind::Goal,
                     named("Intention"));
  case ExprKind::Achieve:
    actionAllowed(expression, context);
    require(context.body == Body::Plan && context.suspend && context.bounded &&
                !context.beliefLoop,
            DiagnosticCode::InvalidPosition,
            "achieve is a plan suspension statement", expression.loc);
    if (expression.recipients)
      expectType(infer(*expression.recipients, env, operand(context)),
                 named("Agent"), expression.recipients->loc);
    deadline(expression.deadline.get(), env, context,
             expression.recipients != nullptr);
    return recover(expression,
                   construct(expression, env, context, DeclKind::Goal, unit),
                   env, context);
  case ExprKind::Ask: {
    actionAllowed(expression, context);
    require(context.body == Body::Plan && context.suspend && context.bounded &&
                !context.beliefLoop,
            DiagnosticCode::InvalidPosition,
            "ask is a plan suspension statement", expression.loc);
    deadline(expression.deadline.get(), env, context, true);
    if (expression.children.empty() ||
        expression.children[0]->kind != ExprKind::Call ||
        expression.children[0]->children.empty()) {
      report(DiagnosticCode::InvalidDeclaration,
             "ask requires a peer.question(args) call", expression.loc);
      return unknown;
    }
    const auto &invocation = *expression.children[0];
    const auto &callee     = *invocation.children[0];
    if (callee.kind != ExprKind::Field || callee.children.empty()) {
      report(DiagnosticCode::InvalidDeclaration,
             "ask requires a peer.question(args) call", expression.loc);
      return unknown;
    }
    expectType(infer(*callee.children[0], env, operand(context)),
               named("Agent"), callee.loc);
    const auto *question = global(callee.text);
    if (!question || question->kind != DeclKind::Question) {
      report(DiagnosticCode::UnknownName,
             "unknown question '" + callee.text + "'", callee.loc);
      return unknown;
    }
    signatureWork(*question);
    std::vector<Type> types;
    for (const auto &parameter : question->parameters)
      types.push_back(parameter.type);
    arguments(invocation, types, env, context);
    return recover(expression, question->result.value_or(unit), env, context);
  }
  case ExprKind::AwaitTerm:
  case ExprKind::AwaitEvent: {
    actionAllowed(expression, context);
    require(context.body == Body::Plan && context.suspend && context.bounded &&
                !context.beliefLoop,
            DiagnosticCode::InvalidPosition,
            "await is a plan suspension statement", expression.loc);
    deadline(expression.deadline.get(), env, context, true);
    Type result = unit;
    if (expression.kind == ExprKind::AwaitTerm) {
      if (!expression.children.empty())
        term(*expression.children[0], env, context, true);
    } else {
      require(event(expression.text), DiagnosticCode::UnknownName,
              "unknown awaited event '" + expression.text + "'",
              expression.loc);
      result = named(expression.text);
      if (expression.binding) {
        reservedName(*expression.binding, expression.loc);
        env[*expression.binding] = {result, true, context.depth};
      }
    }
    if (expression.guard) condition(*expression.guard, env, operand(context));
    return recover(expression, result, env, context);
  }
  case ExprKind::Drop:
  case ExprKind::Stop:
    actionAllowed(expression, context);
    expectType(child(0, operand(context)),
               named(expression.kind == ExprKind::Drop ? "Intention" : "Agent"),
               expression.loc);
    return unit;
  case ExprKind::Install:
  case ExprKind::Mount:
    actionAllowed(expression, context);
    return construct(expression, env, context,
                     expression.kind == ExprKind::Install ? DeclKind::Skill
                                                          : DeclKind::Behavior,
                     unit);
  case ExprKind::Uninstall:
  case ExprKind::Unmount: {
    actionAllowed(expression, context);
    const auto *decl = global(expression.text);
    require(decl && decl->kind == (expression.kind == ExprKind::Uninstall
                                       ? DeclKind::Skill
                                       : DeclKind::Behavior),
            DiagnosticCode::UnknownName,
            "unknown skill/behavior '" + expression.text + "'", expression.loc);
    return unit;
  }
  case ExprKind::Return:
    require(context.bounded &&
                (context.body == Body::Method || context.body == Body::Answer),
            DiagnosticCode::InvalidPosition,
            "return belongs to a bounded method/answer position",
            expression.loc);
    if (context.result)
      expectType(expression.children.empty() ? unit
                                             : child(0, operand(context)),
                 *context.result, expression.loc);
    return bottom;
  case ExprKind::Yield:
    actionAllowed(expression, context);
    require(context.body == Body::Plan && context.suspend && context.bounded &&
                !context.beliefLoop,
            DiagnosticCode::InvalidPosition,
            "yield is a plan suspension statement", expression.loc);
    return unit;
  case ExprKind::Consume:
  case ExprKind::Terminate:
    actionAllowed(expression, context);
    require(context.bounded, DiagnosticCode::InvalidPosition,
            "consume/terminate requires a bounded statement position",
            expression.loc);
    return unit;
  case ExprKind::Fail:
    return bottom;
  case ExprKind::Code: {
    // §5.2/§7.1: a code"…" literal is a pure Code<T> value; T is the body's
    // checked result type, computed at the literal's position under the
    // checker's own env walk (still the scope truth until phase 2's
    // stamp-honoring). The record lookup keys on the literal's
    // SourceLocation plus stamp ordinal, so cloneExpr'd copies share it.
    const FragmentKey key{expression.loc, expression.stamp
                                              ? expression.stamp->ordinal
                                              : std::nullopt};
    const auto found = fragments_.find(key);
    if (found == fragments_.end()) {
      report(DiagnosticCode::InvalidDeclaration,
             "code\"…\" literal has no fragment-table record; the expand "
             "stage must run before check",
             expression.loc);
      return unknown;
    }
    const auto &record = found->second;
    const auto &parsed = record.body;
    if (!parsed.body) return unknown; // expand already reported the body
    // Every loc in the record is literal-relative; pushing the literal's
    // own loc makes report() qualify body diagnostics in the
    // src:m:l:c+o:c convention (§5.1), nesting included.
    innerLiterals_.push_back(expression.loc);
    envRecords_.push_back(&record.env);
    // The body binds its declared CodeParams in a copy of the literal's
    // env — writable locals, same as let-bound names — and checks as
    // ordinary code in the literal's own position mask, with the fragment
    // body kind so `return` and the suspension forms stay refused (§5.1,
    // M6) no matter which position the literal sits in.
    auto local = copyEnvironment(env, expression.loc);
    // §5.3's env records: each cap$N alias resolves to the binding its
    // captured name resolves to here — the env record is the
    // materialized content of the stamped identity the renamed
    // Identifier carries. The alias is a read-only capture (a splice is
    // never a place) and unvalued for `static` operands; a captured name
    // that is itself an alias resolves transitively, so seeding repeats
    // until no record remains unresolved.
    for (bool seeded = true; seeded;) {
      seeded = false;
      for (const auto &[alias, captured] : record.env) {
        if (const auto capturedIt = local.find(captured);
            capturedIt != local.end()) {
          // The cells"…" $-anchor spelling names the same capture —
          // vql's $var charset is [alnum_], so expand rewrites anchors
          // to cap_N while identifiers read cap$N (§5.3).
          const std::string *names[] = {&alias, nullptr};
          std::string anchor         = alias;
          const auto dollar          = anchor.find('$');
          if (dollar != std::string::npos) {
            anchor[dollar] = '_';
            names[1]       = &anchor;
          }
          for (const auto *name : names) {
            if (!name || local.contains(*name)) continue;
            local[*name] = {capturedIt->second.type, false, context.depth + 1,
                            false, true};
            seeded       = true;
          }
        }
      }
    }
    auto &info  = fragmentInfo_[key];
    info.arity  = parsed.parameters.size();
    info.closed = record.closed;
    std::set<std::string> names;
    for (const auto &parameter : parsed.parameters) {
      validateType(parameter.type, parameter.loc);
      require(snake(parameter.name), DiagnosticCode::InvalidIdentifier,
              "parameter/field names must be snake_case", parameter.loc);
      reservedName(parameter.name, parameter.loc);
      require(
          names.insert(parameter.name).second, DiagnosticCode::DuplicateName,
          "duplicate code parameter '" + parameter.name + "'", parameter.loc);
      local[parameter.name] = {parameter.type, true, context.depth + 1, false,
                               true};
    }
    auto bodyContext        = context;
    bodyContext.body        = Body::Fragment;
    bodyContext.suspend     = false;
    bodyContext.stageEffect = false;
    bodyContext.result      = std::nullopt;
    bodyContext.discard     = false;
    auto result             = infer(*parsed.body, local, bodyContext);
    // A body that can never produce (always fails) types as Code<()>.
    info.result = result == bottom ? unit : result;
    info.pure   = pureBody(*parsed.body, context.owner);
    envRecords_.pop_back();
    innerLiterals_.pop_back();
    return {"Code", {info.result}};
  }
  case ExprKind::Splice:
    // The pipeline refuses every splice in expand, so check never meets
    // one; a direct check() on an unexpanded tree gets the same named
    // refusal — the env-walk fallback keeps old callers working, it does
    // not revive forms that must not reach lowering.
    report(DiagnosticCode::Unsupported,
           "'splice' is refused by the expand stage", expression.loc);
    return unknown;
  case ExprKind::Static: {
    // §5.5 staged evaluation — the only compile-time eval. Four
    // refusals: the operand must be pureBody-pure, carry no
    // arena-reading *form* or non-closed name, produce a
    // literal-expressible scalar, and embed no not-closed fragment.
    if (expression.children.size() != 1 || !expression.children.front()) {
      report(DiagnosticCode::InvalidDeclaration,
             "'static' requires exactly one operand", expression.loc);
      return unknown;
    }
    const auto &operandExpr = *expression.children.front();
    // The operand is re-verified in the site's own position mask —
    // `static` is legal anywhere a pure value is, and the operand
    // keeps the site's obligations.
    auto value = infer(operandExpr, env, operand(context));
    require(pureBody(operandExpr, context.owner),
            DiagnosticCode::ImpureCondition, "'static' operand must be pure",
            operandExpr.loc);
    auto closedEnv = copyEnvironment(env, expression.loc);
    (void)staticClosed(operandExpr, closedEnv, true);
    require(value == unknown || literalExpressible(value),
            DiagnosticCode::TypeMismatch,
            "'static' result must be a literal-expressible scalar (Int, "
            "Float, Str, Bool or ()), not " +
                typeName(value),
            expression.loc);
    return value;
  }
  }
  report(DiagnosticCode::InvalidDeclaration, "unknown expression kind",
         expression.loc);
  return unknown;
}

void Checker::goalConditions(const Decl &decl, const Decl *owner) {
  if (!checkedGoals_.insert({&decl, owner}).second) return;
  checkedGoalDeclarations_.insert(&decl);
  signatureWork(decl);
  auto env = environment(owner);
  for (const auto &parameter : decl.parameters)
    env[parameter.name] = {parameter.type, true, 1};
  Context context;
  context.owner   = owner;
  context.body    = Body::Condition;
  context.depth   = 1;
  context.bounded = false;
  for (const auto &clause : decl.goalClauses) {
    auto local = copyEnvironment(env, decl.loc);
    if (clause.condition) condition(*clause.condition, local, context);
  }
}

void Checker::skill(const Decl &decl) {
  std::map<std::string, const Decl *> expected;
  for (const auto &name : decl.names) {
    const auto *capacity = global(name);
    if (!capacity || capacity->kind != DeclKind::Capacity) {
      report(DiagnosticCode::UnknownName,
             "unknown implemented capacity '" + name + "'", decl.loc);
      continue;
    }
    std::map<std::string, const Decl *> inherited;
    std::set<std::string> inheritedVisited;
    capacityMethods(*capacity, inherited, inheritedVisited);
    for (const auto &[methodName, signature] : inherited) {
      if (const auto previous = expected.find(methodName);
          previous != expected.end())
        require(sameSignature(*previous->second, *signature),
                DiagnosticCode::TypeMismatch,
                "implemented capacities disagree on method '" + methodName +
                    "'",
                decl.loc);
      expected[methodName] = signature;
    }
  }
  for (const auto &[name, signature] : expected) {
    if (decl.native) {
      require(signature->effect, DiagnosticCode::InvalidDeclaration,
              "native skills implement effect methods only", decl.loc);
      continue;
    }
    const auto *implementation = member(&decl, name, DeclKind::Method);
    if (!implementation) {
      report(DiagnosticCode::InvalidDeclaration,
             "skill lacks capacity method '" + name + "'", decl.loc);
      continue;
    }
    require(!signature->effect, DiagnosticCode::InvalidDeclaration,
            "an effect method requires a native skill", implementation->loc);
    signatureWork(*implementation);
    signatureWork(*signature);
    require(implementation->parameters.size() == signature->parameters.size(),
            DiagnosticCode::TypeMismatch,
            "capacity implementation has wrong argument count",
            implementation->loc);
    for (std::size_t i = 0; i < implementation->parameters.size() &&
                            i < signature->parameters.size();
         ++i)
      require(implementation->parameters[i].type ==
                  signature->parameters[i].type,
              DiagnosticCode::TypeMismatch,
              "capacity implementation argument type differs",
              implementation->parameters[i].loc);
    require(implementation->result.value_or(unit) ==
                signature->result.value_or(unit),
            DiagnosticCode::TypeMismatch,
            "capacity implementation result type differs", implementation->loc);
  }
}

void Checker::declaration(const Decl &decl, const Decl *owner) {
  const bool typeNameDeclaration =
      decl.kind == DeclKind::Event || decl.kind == DeclKind::Record ||
      decl.kind == DeclKind::Capacity || decl.kind == DeclKind::Skill ||
      decl.kind == DeclKind::Behavior || decl.kind == DeclKind::Agent ||
      decl.kind == DeclKind::Goal;
  const bool valueNameDeclaration =
      decl.kind == DeclKind::Predicate || decl.kind == DeclKind::Question ||
      decl.kind == DeclKind::Plan || decl.kind == DeclKind::Method ||
      decl.kind == DeclKind::Answer || decl.kind == DeclKind::Field;
  if (typeNameDeclaration)
    require(pascal(decl.name), DiagnosticCode::InvalidIdentifier,
            "type/event/goal names must be PascalCase", decl.loc);
  if (valueNameDeclaration)
    require(snake(decl.name), DiagnosticCode::InvalidIdentifier,
            "value/method/plan names must be snake_case", decl.loc);
  reservedName(decl.name, decl.loc);
  parameters(decl);
  if (decl.result) validateType(*decl.result, decl.loc);
  if (decl.kind == DeclKind::Event || decl.kind == DeclKind::Capacity) {
    for (const auto &name : decl.names) {
      const auto *base = global(name);
      const auto expected =
          decl.kind == DeclKind::Event ? DeclKind::Event : DeclKind::Capacity;
      require((expected == DeclKind::Event && name == "Event") ||
                  (base && base->kind == expected),
              DiagnosticCode::UnknownName,
              "unknown or invalid base type '" + name + "'", decl.loc);
    }
    if (decl.kind == DeclKind::Event) {
      for (const auto &baseName : decl.names) {
        for (const auto &field : decl.parameters)
          require(!fieldType(named(baseName), field.name),
                  DiagnosticCode::DuplicateName,
                  "event field duplicates an inherited field '" + field.name +
                      "'",
                  field.loc);
      }
    }
  }
  if (decl.kind == DeclKind::Capacity) {
    std::map<std::string, const Decl *> signatures;
    const auto merge = [&](const std::string &name, const Decl *signature) {
      if (const auto previous = signatures.find(name);
          previous != signatures.end())
        require(sameSignature(*previous->second, *signature),
                DiagnosticCode::TypeMismatch,
                "inherited capacity signatures disagree on method '" + name +
                    "'",
                decl.loc);
      signatures[name] = signature;
    };
    for (const auto &baseName : decl.names) {
      const auto *base = global(baseName);
      if (!base || base->kind != DeclKind::Capacity) continue;
      std::map<std::string, const Decl *> inherited;
      std::set<std::string> visited;
      capacityMethods(*base, inherited, visited);
      for (const auto &[name, signature] : inherited) merge(name, signature);
    }
    for (const auto &method : decl.members)
      if (method.kind == DeclKind::Method) merge(method.name, &method);
  }
  if (decl.kind == DeclKind::Agent || decl.kind == DeclKind::Behavior ||
      decl.kind == DeclKind::Skill || decl.kind == DeclKind::Capacity) {
    std::set<std::string> names;
    for (const auto &parameter : decl.parameters) names.insert(parameter.name);
    for (const auto &item : decl.members) {
      if (item.kind == DeclKind::Field || item.kind == DeclKind::Method ||
          item.kind == DeclKind::Answer || item.kind == DeclKind::Goal ||
          item.kind == DeclKind::Plan)
        require(names.insert(item.name).second, DiagnosticCode::DuplicateName,
                "duplicate member '" + item.name + "'", item.loc);
    }
    if (decl.kind == DeclKind::Skill) skill(decl);
    for (const auto &item : decl.members) declaration(item, &decl);
    return;
  }
  auto env = environment(owner);
  Context context;
  context.owner = owner;
  context.depth = 1;
  signatureWork(decl);
  for (const auto &parameter : decl.parameters)
    env[parameter.name] = {parameter.type, true, 1};
  switch (decl.kind) {
  case DeclKind::Field:
    context.pure = true;
    if (decl.initializer)
      expectType(infer(*decl.initializer, env, context),
                 decl.result.value_or(unknown), decl.loc);
    break;
  case DeclKind::Uses:
    for (const auto &name : decl.names) {
      const auto *capacity = global(name);
      const bool builtin   = name == "Clock" || name == "Schedules" ||
                             name == "Logging" || name == "Accepts";
      require(builtin || (capacity && capacity->kind == DeclKind::Capacity),
              DiagnosticCode::UnknownName,
              "unknown used capacity '" + name + "'", decl.loc);
    }
    break;
  case DeclKind::Install:
  case DeclKind::BehaviorUse: {
    const auto *target = global(decl.name);
    const auto expected =
        decl.kind == DeclKind::Install ? DeclKind::Skill : DeclKind::Behavior;
    if (!target || target->kind != expected) {
      report(DiagnosticCode::UnknownName,
             "unknown initial skill/behavior '" + decl.name + "'", decl.loc);
    } else if (decl.initializer) {
      signatureWork(*target);
      std::vector<Type> types;
      for (const auto &parameter : target->parameters)
        types.push_back(parameter.type);
      arguments(*decl.initializer, types, env, context);
    }
    break;
  }
  case DeclKind::Belief:
    if (decl.initializer) term(*decl.initializer, env, context, false);
    break;
  case DeclKind::Goal: {
    std::set<GoalClause::Kind> clauses;
    for (const auto &clause : decl.goalClauses) {
      require(clauses.insert(clause.kind).second, DiagnosticCode::DuplicateName,
              "duplicate goal clause", clause.loc);
      if (clause.kind == GoalClause::Kind::Succeed)
        require(decl.goalKind == GoalKind::Achieve,
                DiagnosticCode::InvalidDeclaration,
                "succeed when belongs to an achieve goal", clause.loc);
      if (clause.kind == GoalClause::Kind::Holds)
        require(decl.goalKind == GoalKind::Maintain,
                DiagnosticCode::InvalidDeclaration,
                "holds belongs to a maintain goal", clause.loc);
    }
    require(decl.goalKind.has_value(), DiagnosticCode::InvalidDeclaration,
            "goal kind is missing", decl.loc);
    if (decl.goalKind == GoalKind::Achieve)
      require(clauses.contains(GoalClause::Kind::Succeed),
              DiagnosticCode::InvalidDeclaration,
              "an achieve goal requires succeed when", decl.loc);
    if (decl.goalKind == GoalKind::Maintain)
      require(clauses.contains(GoalClause::Kind::Holds),
              DiagnosticCode::InvalidDeclaration,
              "a maintain goal requires holds", decl.loc);
    if (owner) goalConditions(decl, owner);
    break;
  }
  case DeclKind::Plan: {
    context.body       = Body::Plan;
    context.suspend    = true;
    context.discard    = true;
    const auto *target = decl.target ? goal(owner, *decl.target) : nullptr;
    if (!target) {
      report(DiagnosticCode::UnknownName,
             "unknown goal for plan '" + decl.name + "'", decl.loc);
    } else {
      std::set<std::string> bindings;
      if (decl.planBindings) {
        for (const auto &name : *decl.planBindings) {
          require(bindings.insert(name).second, DiagnosticCode::DuplicateName,
                  "duplicate plan binding '" + name + "'", decl.loc);
          reservedName(name, decl.loc);
          resolutionWork(decl.loc);
          const auto &parameters = parameterIndex_.at(target);
          auto parameter         = parameters.find(name);
          if (parameter == parameters.end())
            report(DiagnosticCode::UnknownName,
                   "unknown goal parameter '" + name + "'", decl.loc);
          else {
            resolutionWork(decl.loc, typeWeights_.at(&parameter->second->type));
            env[name] = {parameter->second->type, true, 1};
          }
        }
      } else {
        signatureWork(*target);
        for (const auto &parameter : target->parameters)
          env[parameter.name] = {parameter.type, true, 1};
      }
      goalConditions(*target, owner);
    }
    if (decl.guard) condition(*decl.guard, env, operand(context));
    if (decl.body) infer(*decl.body, env, context);
    break;
  }
  case DeclKind::Handler:
    context.body        = Body::Handler;
    context.discard     = true;
    context.stageEffect = true;
    if (decl.trigger && decl.trigger->kind == ExprKind::Term)
      term(*decl.trigger, env, context, true);
    else {
      require(event(decl.name), DiagnosticCode::UnknownName,
              "unknown handled event '" + decl.name + "'", decl.loc);
      env["occurrence"] = {named(decl.name), true, 1};
      if (decl.binding) {
        require(snake(*decl.binding), DiagnosticCode::InvalidIdentifier,
                "handler binding must be snake_case", decl.loc);
        reservedName(*decl.binding, decl.loc);
        env[*decl.binding] = {named(decl.name), true, 1};
      }
    }
    if (decl.guard) condition(*decl.guard, env, operand(context));
    if (decl.body) infer(*decl.body, env, context);
    break;
  case DeclKind::Method:
  case DeclKind::Answer:
    context.body = decl.kind == DeclKind::Answer ? Body::Answer : Body::Method;
    context.readonly = decl.kind == DeclKind::Answer;
    context.result   = decl.result.value_or(unit);
    if (decl.kind == DeclKind::Answer) {
      const auto *question = global(decl.name);
      if (!question || question->kind != DeclKind::Question) {
        report(DiagnosticCode::UnknownName,
               "answer has no declared question '" + decl.name + "'", decl.loc);
      } else {
        require(decl.parameters.size() == question->parameters.size(),
                DiagnosticCode::TypeMismatch,
                "answer argument count differs from question", decl.loc);
        for (std::size_t i = 0;
             i < decl.parameters.size() && i < question->parameters.size(); ++i)
          require(decl.parameters[i].type == question->parameters[i].type,
                  DiagnosticCode::TypeMismatch,
                  "answer argument type differs from question",
                  decl.parameters[i].loc);
        require(decl.result.value_or(unit) == question->result.value_or(unit),
                DiagnosticCode::TypeMismatch,
                "answer result type differs from question", decl.loc);
      }
    }
    if (decl.effect) {
      require(owner && owner->kind == DeclKind::Capacity && !decl.body,
              DiagnosticCode::InvalidDeclaration,
              "only a capacity signature/native skill may declare an effect",
              decl.loc);
      deadline(decl.deadline.get(), env, context, true);
    } else
      require(!decl.deadline, DiagnosticCode::InvalidDeclaration,
              "within on a method requires effect", decl.loc);
    require(!decl.preempts || decl.effect, DiagnosticCode::InvalidDeclaration,
            "preempts requires effect", decl.loc);
    for (const auto &contract : decl.contracts) {
      auto local = copyEnvironment(env, decl.loc);
      if (contract.condition)
        condition(*contract.condition, local, operand(context));
    }
    if (decl.body)
      expectType(infer(*decl.body, env, context), *context.result, decl.loc);
    break;
  case DeclKind::Lifecycle:
    context.body    = Body::Lifecycle;
    context.discard = true;
    if (decl.body) infer(*decl.body, env, context);
    break;
  default:
    break;
  }
}

std::expected<void, std::vector<Diagnostic>> Checker::run() try {
  preflight();
  buildResolutionIndex();
  for (const auto &imported : program_.imports) {
    const bool supported = imported.path == "std:agent" ||
                           imported.path == "std:goal" ||
                           imported.path == "std:meta";
    // §5.1: run/value_of become reserved names under the meta import.
    metaImported_ = metaImported_ || imported.path == "std:meta";
    require(supported, DiagnosticCode::InvalidDeclaration,
            "module loading is not available for import '" + imported.path +
                "'",
            imported.loc);
  }
  // §6.7: `src:`, `reify:` and `patch:` are minted-label prefixes, so a
  // module path heading into any of them could reroute a first-`:`
  // qualified lookup onto the module arm — user labels stay opaque.
  if (program_.module) {
    const auto &path = *program_.module;
    const auto colon = path.find(':');
    const auto head  = std::string_view{path}.substr(
        0, colon == std::string::npos ? path.size() : colon);
    require(head != "src" && head != "reify" && head != "patch",
            DiagnosticCode::InvalidIdentifier,
            "module path begins with the reserved label prefix '" +
                std::string(head) + ":'",
            program_.imports.empty() ? (program_.declarations.empty()
                                            ? SourceLocation{}
                                            : program_.declarations.front().loc)
                                     : program_.imports.front().loc);
  }
  static const std::set<std::string_view> builtinTypes{
      "Int",   "Float", "Str",   "Bool",      "Duration",
      "Time",  "Agent", "Space", "Intention", "Timer",
      "Event", "List",  "Map",   "Opt",       "Code"};
  for (const auto &decl : program_.declarations) {
    require(!builtinTypes.contains(decl.name), DiagnosticCode::DuplicateName,
            "declaration shadows built-in type '" + decl.name + "'", decl.loc);
    require(globals_.emplace(decl.name, &decl).second,
            DiagnosticCode::DuplicateName,
            "duplicate declaration '" + decl.name + "'", decl.loc);
    if (decl.kind == DeclKind::Event)
      require(decl.name != "Event" && decl.name != "Initialize" &&
                  decl.name != "Destroy" && decl.name != "GoalAchieved" &&
                  decl.name != "GoalFailed" && decl.name != "GoalDropped" &&
                  decl.name != "PlanFailed" && decl.name != "EffectFailed" &&
                  decl.name != "ConditionFault" && decl.name != "ChildDown" &&
                  decl.name != "ChildStopping" && decl.name != "SpawnFailed" &&
                  decl.name != "DeliveryFailed" && decl.name != "Moved",
              DiagnosticCode::DuplicateName,
              "event shadows a built-in event '" + decl.name + "'", decl.loc);
    for (const auto &method : decl.members)
      if (method.kind == DeclKind::Method) methodOwners_[&method] = &decl;
  }
  buildInheritance();
  derivePurity();
  for (const auto &decl : program_.declarations) declaration(decl, nullptr);
  // Global goal clauses are templates in the adopting agent's scope. A
  // referenced template was checked in that concrete scope above; an unused
  // template is still checked in its parameter-only scope for misspelled
  // predicates and malformed conditions.
  for (const auto &decl : program_.declarations) {
    if (decl.kind != DeclKind::Goal) continue;
    const bool bound = checkedGoalDeclarations_.contains(&decl);
    if (!bound) goalConditions(decl, nullptr);
  }
  if (!diagnostics_.empty()) return std::unexpected(std::move(diagnostics_));
  return {};
} catch (const Diagnostic &diagnostic) {
  diagnostics_.push_back(mapInner(diagnostic));
  return std::unexpected(std::move(diagnostics_));
}

} // namespace

std::expected<void, std::vector<Diagnostic>>
check(const Program &program, const FragmentTable &fragments,
      CheckLimits limits, FragmentInfoMap *infoOut) {
  Checker checker(program, limits, fragments);
  auto result = checker.run();
  if (result && infoOut) *infoOut = checker.fragmentInfo();
  return result;
}

std::expected<void, std::vector<Diagnostic>> check(const Program &program,
                                                   CheckLimits limits) {
  // The env-walk fallback: callers that never ran resolve/expand check
  // against an empty table; stamps are not yet consumed either way, and a
  // Code node found with no record is a named refusal in both paths.
  return check(program, FragmentTable{}, limits);
}

} // namespace xanadu::vagent
