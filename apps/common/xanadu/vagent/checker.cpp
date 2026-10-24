/**
 * @file checker.cpp
 * @brief vagent checker (design §8.2, §8.3, §8.7, §8.8, §8.9 restrictions).
 *
 * One pass declares names, one resolves signatures into the symbol tables,
 * one walks the bodies. Errors accumulate instead of unwinding, so a program
 * with three mistakes reports three; an expression that fails to check has
 * type `Any`, which every rule accepts, so one mistake does not breed more.
 *
 * Positions are threaded down the walk as a `Pos`: `bounded` is §8.3's
 * bounded position, `susp` is §6.6's suspension position, `used` says whether
 * the value of the expression is read.
 */
#include "common/xanadu/vagent/checker.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <functional>
#include <ranges>
#include <unordered_set>
#include <utility>
#include <variant>

namespace xanadu::vagent {

std::string_view checkErrorName(CheckErrorCode code) noexcept {
  using C = CheckErrorCode;
  switch (code) {
  case C::DuplicateName:
    return "DuplicateName";
  case C::UnknownName:
    return "UnknownName";
  case C::UnknownType:
    return "UnknownType";
  case C::UnknownField:
    return "UnknownField";
  case C::UnknownPredicate:
    return "UnknownPredicate";
  case C::UnknownQuestion:
    return "UnknownQuestion";
  case C::UnknownGoal:
    return "UnknownGoal";
  case C::UnknownCapacity:
    return "UnknownCapacity";
  case C::UnknownSkill:
    return "UnknownSkill";
  case C::UnknownBehavior:
    return "UnknownBehavior";
  case C::UnknownAgent:
    return "UnknownAgent";
  case C::UnknownFunction:
    return "UnknownFunction";
  case C::AmbiguousCall:
    return "AmbiguousCall";
  case C::ReservedName:
    return "ReservedName";
  case C::ReservedField:
    return "ReservedField";
  case C::CaseConvention:
    return "CaseConvention";
  case C::OccurrenceOutsideHandler:
    return "OccurrenceOutsideHandler";
  case C::InheritanceCycle:
    return "InheritanceCycle";
  case C::InvalidExtends:
    return "InvalidExtends";
  case C::TypeMismatch:
    return "TypeMismatch";
  case C::ArityMismatch:
    return "ArityMismatch";
  case C::MissingField:
    return "MissingField";
  case C::DuplicateField:
    return "DuplicateField";
  case C::NotAnEvent:
    return "NotAnEvent";
  case C::NotCallable:
    return "NotCallable";
  case C::NotAPlace:
    return "NotAPlace";
  case C::IfWithoutElse:
    return "IfWithoutElse";
  case C::BoolCondition:
    return "BoolCondition";
  case C::ImpureCondition:
    return "ImpureCondition";
  case C::ImpureAnswer:
    return "ImpureAnswer";
  case C::BoundedOnly:
    return "BoundedOnly";
  case C::SuspensionOutsidePlan:
    return "SuspensionOutsidePlan";
  case C::SuspensionPosition:
    return "SuspensionPosition";
  case C::SuspensionInBelievesLoop:
    return "SuspensionInBelievesLoop";
  case C::EffectInMethod:
    return "EffectInMethod";
  case C::EffectPosition:
    return "EffectPosition";
  case C::ReturnOutsideMethod:
    return "ReturnOutsideMethod";
  case C::ConsumeOutsideHandler:
    return "ConsumeOutsideHandler";
  case C::EffectWithoutWithin:
    return "EffectWithoutWithin";
  case C::EffectInSkill:
    return "EffectInSkill";
  case C::NonNativeEffect:
    return "NonNativeEffect";
  case C::MissingImplementation:
    return "MissingImplementation";
  case C::SignatureMismatch:
    return "SignatureMismatch";
  case C::PlanParamNotInGoal:
    return "PlanParamNotInGoal";
  case C::AlternationOfLiterals:
    return "AlternationOfLiterals";
  }
  return "Unknown";
}

std::string formatCheckError(const CheckError &error) {
  return std::format("{}:{}: {}: {}", error.loc.line, error.loc.column,
                     checkErrorName(error.code), error.message);
}

std::string typeName(const Type &t) {
  using K = Type::Kind;
  switch (t.kind) {
  case K::Any:
    return "Any";
  case K::Never:
    return "Never";
  case K::Int:
    return "Int";
  case K::Float:
    return "Float";
  case K::Bool:
    return "Bool";
  case K::Str:
    return "Str";
  case K::Unit:
    return "()";
  case K::Duration:
    return "Duration";
  case K::Time:
    return "Time";
  case K::Agent:
    return "Agent";
  case K::Space:
    return "Space";
  case K::Intention:
    return "Intention";
  case K::Timer:
    return "Timer";
  case K::Event:
    return "Event";
  case K::Opt:
    return "Opt<" + (t.args.empty() ? "?" : typeName(t.args[0])) + ">";
  case K::List:
    return "[" + (t.args.empty() ? "?" : typeName(t.args[0])) + "]";
  case K::Map:
    return "Map<" + (t.args.size() < 2 ? "?" : typeName(t.args[0])) + ", " +
           (t.args.size() < 2 ? "?" : typeName(t.args[1])) + ">";
  case K::Named:
    return t.name;
  }
  return "?";
}

const std::vector<FieldInfo> &implicitEventFields() {
  // `source` and `space` are Opt: a host delivery has neither (§5.5).
  static const std::vector<FieldInfo> fields = {
      {"source",
       Type{Type::Kind::Opt, {}, {Type{Type::Kind::Agent, {}, {}}}},
       {}},
      {"space",
       Type{Type::Kind::Opt, {}, {Type{Type::Kind::Space, {}, {}}}},
       {}},
      {"time", Type{Type::Kind::Time, {}, {}}, {}},
      {"ordinal", Type{Type::Kind::Int, {}, {}}, {}},
  };
  return fields;
}

std::vector<const MethodInfo *> allMethods(const CheckedUnit &unit,
                                           std::string_view capacity) {
  std::vector<const MethodInfo *> out;
  std::unordered_set<std::string> seen;
  std::function<void(std::string_view)> walk = [&](std::string_view name) {
    if (!seen.insert(std::string(name)).second) {
      return;
    }
    const auto it = unit.capacities.find(std::string(name));
    if (it == unit.capacities.end()) {
      return;
    }
    for (const auto &m : it->second.methods) {
      out.push_back(&m);
    }
    for (const auto &base : it->second.extends) {
      walk(base);
    }
  };
  walk(capacity);
  return out;
}

namespace {

using K = Type::Kind;

template <typename... Fs> struct Overloaded : Fs... {
  using Fs::operator()...;
};
template <typename... Fs> Overloaded(Fs...) -> Overloaded<Fs...>;

Type basic(K k) { return Type{k, {}, {}}; }
Type listOf(Type t) { return Type{K::List, {}, {std::move(t)}}; }
Type optOf(Type t) { return Type{K::Opt, {}, {std::move(t)}}; }
Type namedType(std::string n) { return Type{K::Named, std::move(n), {}}; }

bool isPascal(std::string_view s) {
  return !s.empty() && std::isupper(static_cast<unsigned char>(s.front())) != 0;
}
bool isSnake(std::string_view s) {
  return !s.empty() &&
         (std::islower(static_cast<unsigned char>(s.front())) != 0 ||
          s.front() == '_');
}

bool containsAny(const Type &t) {
  return t.kind == K::Any || t.kind == K::Never ||
         std::ranges::any_of(t.args, containsAny);
}

/// What a name declares; builtin and declared names share one namespace.
enum class TypeKind : std::uint8_t {
  Event,
  Record,
  Capacity,
  Skill,
  Behavior,
  Agent,
  Goal
};

enum class SymKind : std::uint8_t { Local, Param, Field, Binder };
struct Symbol {
  Type type;
  SymKind kind{SymKind::Local};
};
using Scope = std::unordered_map<std::string, Symbol>;

enum class Purity : std::uint8_t { None, Strict, ReadOnly };
enum class Body : std::uint8_t {
  None,
  Handler,
  Plan,
  Method,
  Answer,
  Lifecycle
};

struct Pos {
  bool bounded{false};
  bool susp{false};
  bool used{true};
};
constexpr Pos operandPos{false, false, true};

/// What a def does, for §8.2's "the checker has found pure".
struct DefFacts {
  bool action{false};
  bool assign{false};
  bool fieldWrite{false};
  bool effect{false};
  bool unknownCall{false};
  std::vector<std::string> callees;
};

struct PurityRequirement {
  std::string key;
  Purity mode{Purity::Strict};
  SourceLocation loc{};
};

template <typename T> struct Restore {
  T &ref;
  T saved;
  explicit Restore(T &r) : ref(r), saved(r) {}
  Restore(T &r, T value) : ref(r), saved(r) { ref = std::move(value); }
  Restore(const Restore &)            = delete;
  Restore &operator=(const Restore &) = delete;
  ~Restore() { ref = std::move(saved); }
};

bool isComparison(const Expr &e) {
  const auto *b = std::get_if<EBinary>(&e.node);
  if (b == nullptr) {
    return false;
  }
  using Op = EBinary::Op;
  return b->op == Op::Lt || b->op == Op::Le || b->op == Op::Gt ||
         b->op == Op::Ge || b->op == Op::Eq || b->op == Op::Ne;
}

const std::vector<std::string> builtinCapacityNames = {"Schedules", "Clock",
                                                       "Accepts", "Logging"};

class Checker {
public:
  explicit Checker(const Unit &unit) : unit_(unit) { out_.unit = &unit; }

  std::expected<CheckedUnit, std::vector<CheckError>> run() {
    installBuiltins();
    collectImports();
    declareNames();
    resolveSignatures();
    checkBodies();
    verifyPurity();
    if (!errors_.empty()) {
      std::ranges::stable_sort(errors_, [](const auto &a, const auto &b) {
        return std::pair(a.loc.line, a.loc.column) <
               std::pair(b.loc.line, b.loc.column);
      });
      return std::unexpected(std::move(errors_));
    }
    return std::move(out_);
  }

private:
  const Unit &unit_;
  CheckedUnit out_;
  std::vector<CheckError> errors_;

  std::unordered_map<std::string, TypeKind> typeNames_;
  std::unordered_set<std::string> valueNames_; ///< predicates and questions
  std::unordered_set<std::string> opaqueTypes_;
  std::unordered_set<std::string> opaqueValues_;
  std::unordered_set<std::string>
      usersOfOpaque_; ///< containers that `uses` one
  std::unordered_map<std::string, DefFacts> facts_;
  std::vector<PurityRequirement> requirements_;

  std::vector<Scope> scopes_;
  Purity purity_{Purity::None};
  Body body_{Body::None};
  std::optional<Type> occurrence_;
  std::optional<Type> returnType_;
  DefFacts *curFacts_{nullptr};
  bool inBelievesLoop_{false};
  bool lenient_{false}; ///< top-level goals and plans: the agent is not known
  const ContainerInfo *cur_{nullptr};

  // -- errors ---------------------------------------------------------------

  void err(CheckErrorCode code, SourceLocation loc, std::string message) {
    errors_.push_back({code, std::move(message), loc});
  }
  void warn(CheckErrorCode code, SourceLocation loc, std::string message) {
    out_.warnings.push_back({code, std::move(message), loc});
  }

  // -- scopes ---------------------------------------------------------------

  struct ScopeGuard {
    Checker &c;
    explicit ScopeGuard(Checker &checker) : c(checker) {
      c.scopes_.emplace_back();
    }
    ScopeGuard(const ScopeGuard &)            = delete;
    ScopeGuard &operator=(const ScopeGuard &) = delete;
    ~ScopeGuard() { c.scopes_.pop_back(); }
  };

  const Symbol *lookup(const std::string &name) const {
    for (const auto &scope : std::views::reverse(scopes_)) {
      const auto it = scope.find(name);
      if (it != scope.end()) {
        return &it->second;
      }
    }
    return nullptr;
  }

  /// `$` names are reserved, a value name is snake_case, and nothing is
  /// declared twice in one scope. Returns false when `name` binds nothing.
  bool declare(const std::string &name, Type type, SymKind kind,
               SourceLocation loc) {
    if (name == "_") {
      return false;
    }
    if (!checkValueName(name, loc)) {
      return false;
    }
    auto &scope = scopes_.back();
    if (scope.contains(name)) {
      err(CheckErrorCode::DuplicateName, loc,
          std::format("`{}` is already declared in this scope", name));
      return false;
    }
    scope.emplace(name, Symbol{std::move(type), kind});
    return true;
  }

  /// Container and signature symbols were validated when the signature was
  /// resolved; repeating the checks would report each mistake twice.
  void put(const std::string &name, Type type, SymKind kind) {
    if (name != "_") {
      scopes_.back().insert_or_assign(name, Symbol{std::move(type), kind});
    }
  }

  bool checkValueName(const std::string &name, SourceLocation loc) {
    if (name.starts_with('$')) {
      err(CheckErrorCode::ReservedName, loc,
          std::format("`{}`: names beginning with `$` are reserved", name));
      return false;
    }
    if (!isSnake(name)) {
      err(CheckErrorCode::CaseConvention, loc,
          std::format("`{}` names a value, so it must be snake_case", name));
      return false;
    }
    return true;
  }
  bool checkTypeName(const std::string &name, SourceLocation loc) {
    if (name.starts_with('$')) {
      err(CheckErrorCode::ReservedName, loc,
          std::format("`{}`: names beginning with `$` are reserved", name));
      return false;
    }
    if (!isPascal(name)) {
      err(CheckErrorCode::CaseConvention, loc,
          std::format("`{}` names a type, so it must be PascalCase", name));
      return false;
    }
    return true;
  }

  // -- types ----------------------------------------------------------------

  [[nodiscard]] bool isEventName(const std::string &n) const {
    return out_.events.contains(n);
  }
  [[nodiscard]] bool isSubtype(const std::string &sub,
                               const std::string &super) const {
    if (sub == super) {
      return true;
    }
    const auto it = out_.events.find(sub);
    return it != out_.events.end() &&
           std::ranges::find(it->second.supertypes, super) !=
               it->second.supertypes.end();
  }

  /// May a value of type `actual` be stored where `expected` is wanted?
  [[nodiscard]] bool assignable(const Type &expected,
                                const Type &actual) const {
    if (expected.kind == K::Any || actual.kind == K::Any ||
        actual.kind == K::Never) {
      return true;
    }
    if (expected.kind == K::Opt) {
      if (actual.kind == K::Opt) {
        return assignable(expected.args[0], actual.args[0]);
      }
      return assignable(expected.args[0], actual);
    }
    if (actual.kind == K::Opt) {
      return false;
    }
    if (expected.kind == K::Event) {
      return actual.kind == K::Event ||
             (actual.kind == K::Named && isEventName(actual.name));
    }
    if (expected.kind != actual.kind) {
      return false;
    }
    switch (expected.kind) {
    case K::List:
      return assignable(expected.args[0], actual.args[0]);
    case K::Map:
      return assignable(expected.args[0], actual.args[0]) &&
             assignable(expected.args[1], actual.args[1]);
    case K::Named:
      return isSubtype(actual.name, expected.name);
    default:
      return true;
    }
  }

  /// The type both arms can be read as, preferring the more specific.
  [[nodiscard]] std::optional<Type> join(const Type &a, const Type &b) const {
    if (a.kind == K::Never) {
      return b;
    }
    if (b.kind == K::Never) {
      return a;
    }
    const bool ab = assignable(a, b);
    const bool ba = assignable(b, a);
    if (ab && ba) {
      return containsAny(a) ? b : a;
    }
    if (ab) {
      return a;
    }
    if (ba) {
      return b;
    }
    return std::nullopt;
  }

  /// Reading an `Opt<T>` succeeds with the value or fails (§8.2).
  static Type unwrap(const Type &t) { return t.kind == K::Opt ? t.args[0] : t; }

  void expectType(const Type &expected, const Type &actual, SourceLocation loc,
                  std::string_view what) {
    if (!assignable(expected, actual)) {
      err(CheckErrorCode::TypeMismatch, loc,
          std::format("{}: expected {}, found {}", what, typeName(expected),
                      typeName(actual)));
    }
  }

  Type resolveType(const TypeExpr &te) {
    switch (te.kind) {
    case TypeExpr::Kind::Unit:
      return basic(K::Unit);
    case TypeExpr::Kind::List:
      return listOf(te.args.empty() ? basic(K::Any) : resolveType(te.args[0]));
    case TypeExpr::Kind::Named:
      break;
    }
    static const std::unordered_map<std::string, K> scalars = {
        {"Int", K::Int},
        {"Float", K::Float},
        {"Bool", K::Bool},
        {"Str", K::Str},
        {"Duration", K::Duration},
        {"Time", K::Time},
        {"Agent", K::Agent},
        {"Space", K::Space},
        {"Intention", K::Intention},
        {"Timer", K::Timer},
        {"Event", K::Event}};
    if (const auto it = scalars.find(te.name); it != scalars.end()) {
      if (!te.args.empty()) {
        err(CheckErrorCode::TypeMismatch, te.loc,
            std::format("`{}` takes no type arguments", te.name));
      }
      return basic(it->second);
    }
    if (te.name == "Opt") {
      if (te.args.size() != 1) {
        err(CheckErrorCode::TypeMismatch, te.loc,
            "`Opt` takes exactly one type argument");
        return basic(K::Any);
      }
      return optOf(resolveType(te.args[0]));
    }
    if (te.name == "Map") {
      if (te.args.size() != 2) {
        err(CheckErrorCode::TypeMismatch, te.loc,
            "`Map` takes a key type and a value type");
        return basic(K::Any);
      }
      return Type{
          K::Map, {}, {resolveType(te.args[0]), resolveType(te.args[1])}};
    }
    if (const auto it = typeNames_.find(te.name); it != typeNames_.end()) {
      if (it->second == TypeKind::Event || it->second == TypeKind::Record) {
        return namedType(te.name);
      }
      err(CheckErrorCode::UnknownType, te.loc,
          std::format("`{}` is not a value type", te.name));
      return basic(K::Any);
    }
    if (opaqueTypes_.contains(te.name)) {
      return basic(K::Any);
    }
    err(CheckErrorCode::UnknownType, te.loc,
        std::format("unknown type `{}`", te.name));
    return basic(K::Any);
  }

  // -- pass 0: builtins and imports -----------------------------------------

  void installBuiltins() {
    auto ev = [&](const char *name,
                  std::vector<std::pair<const char *, K>> fields) {
      EventInfo info;
      info.name    = name;
      info.builtin = true;
      for (const auto &[n, k] : fields) {
        info.fields.push_back({n, basic(k), {}});
      }
      typeNames_.emplace(name, TypeKind::Event);
      out_.events.emplace(name, std::move(info));
    };
    ev("Initialize", {});
    ev("Destroy", {});
    ev("GoalAchieved", {{"intention", K::Intention}, {"goal", K::Str}});
    ev("GoalFailed",
       {{"intention", K::Intention}, {"goal", K::Str}, {"cause", K::Str}});
    ev("GoalDropped", {{"intention", K::Intention}, {"goal", K::Str}});
    ev("PlanFailed",
       {{"intention", K::Intention}, {"plan", K::Str}, {"cause", K::Str}});
    ev("EffectFailed",
       {{"capacity", K::Str}, {"method", K::Str}, {"cause", K::Str}});
    ev("ConditionFault", {{"intention", K::Intention}, {"cause", K::Str}});
    ev("ChildDown", {{"child", K::Agent}});
    ev("ChildStopping", {{"child", K::Agent}, {"cause", K::Str}});
    ev("SpawnFailed", {{"child", K::Agent}});
    ev("DeliveryFailed",
       {{"to", K::Agent}, {"kind", K::Str}, {"cause", K::Str}});
    ev("Orphaned", {{"former_parent", K::Agent}});
    ev("Moved", {{"name", K::Agent}});

    auto method = [](const char *owner, const char *name,
                     std::vector<ParamInfo> params, Type result,
                     bool hasResult) {
      MethodInfo m;
      m.owner     = owner;
      m.name      = name;
      m.params    = std::move(params);
      m.result    = std::move(result);
      m.hasResult = hasResult;
      return m;
    };
    auto cap = [&](const char *name, std::vector<MethodInfo> methods) {
      CapacityInfo c;
      c.name    = name;
      c.methods = std::move(methods);
      c.builtin = true;
      typeNames_.emplace(name, TypeKind::Capacity);
      out_.capacities.emplace(name, std::move(c));
    };
    const Type timer = basic(K::Timer);
    cap("Schedules",
        {method("Schedules", "after",
                {{"d", basic(K::Duration)}, {"e", basic(K::Event)}}, timer,
                true),
         method("Schedules", "periodic",
                {{"d", basic(K::Duration)}, {"e", basic(K::Event)}}, timer,
                true),
         method("Schedules", "cancel", {{"t", timer}}, basic(K::Unit), false)});
    cap("Clock", {method("Clock", "now", {}, basic(K::Time), true)});
    cap("Accepts",
        {method("Accepts", "accepts",
                {{"sender", basic(K::Agent)}, {"goal_name", basic(K::Str)}},
                basic(K::Unit), false)});
    cap("Logging", {method("Logging", "info", {{"m", basic(K::Str)}},
                           basic(K::Unit), false),
                    method("Logging", "debug", {{"m", basic(K::Str)}},
                           basic(K::Unit), false)});
  }

  void collectImports() {
    // Declarations are not known yet, so names are recorded and the ones the
    // unit declares itself are dropped after pass A.
    for (const auto &imp : unit_.imports) {
      for (const auto &n : imp.names) {
        pendingImports_.push_back(n);
      }
    }
  }
  std::vector<std::string> pendingImports_;

  // -- pass A: names --------------------------------------------------------

  void declareType(const std::string &name, TypeKind kind, SourceLocation loc) {
    checkTypeName(name, loc);
    if (!typeNames_.emplace(name, kind).second) {
      err(CheckErrorCode::DuplicateName, loc,
          std::format("`{}` is already declared", name));
    }
  }
  void declareValue(const std::string &name, SourceLocation loc) {
    checkValueName(name, loc);
    if (!valueNames_.insert(name).second) {
      err(CheckErrorCode::DuplicateName, loc,
          std::format("`{}` is already declared", name));
    }
  }

  void declareNames() {
    for (const auto &d : unit_.decls) {
      std::visit(
          Overloaded{
              [&](const EventDecl &x) {
                declareType(x.name, TypeKind::Event, x.loc);
              },
              [&](const RecordDecl &x) {
                declareType(x.name, TypeKind::Record, x.loc);
              },
              [&](const PredicateDecl &x) { declareValue(x.name, x.loc); },
              [&](const QuestionDecl &x) { declareValue(x.name, x.loc); },
              [&](const CapacityDecl &x) {
                declareType(x.name, TypeKind::Capacity, x.loc);
              },
              [&](const SkillDecl &x) {
                declareType(x.name, TypeKind::Skill, x.loc);
              },
              [&](const BehaviorDecl &x) {
                declareType(x.name, TypeKind::Behavior, x.loc);
              },
              [&](const AgentDecl &x) {
                declareType(x.name, TypeKind::Agent, x.loc);
              },
              [&](const GoalDecl &x) {
                declareType(x.name, TypeKind::Goal, x.loc);
              },
              [&](const PlanDecl &) {}},
          d);
    }
    for (const auto &n : pendingImports_) {
      if (typeNames_.contains(n) || valueNames_.contains(n)) {
        continue;
      }
      out_.opaqueImports.push_back(n);
      (isPascal(n) ? opaqueTypes_ : opaqueValues_).insert(n);
    }
  }

  // -- pass B: signatures ---------------------------------------------------

  std::vector<ParamInfo> resolveParams(const std::vector<Param> &params) {
    std::vector<ParamInfo> out;
    std::unordered_set<std::string> seen;
    for (const auto &p : params) {
      checkValueName(p.name, p.loc);
      if (!seen.insert(p.name).second) {
        err(CheckErrorCode::DuplicateName, p.loc,
            std::format("parameter `{}` is declared twice", p.name));
      }
      out.push_back({p.name, resolveType(p.type)});
    }
    return out;
  }

  std::vector<FieldInfo> resolveFields(const std::vector<Field> &fields,
                                       std::vector<FieldInfo> inherited,
                                       bool isEvent) {
    std::vector<FieldInfo> out = std::move(inherited);
    for (const auto &f : fields) {
      checkValueName(f.name, f.loc);
      const auto &implicit = implicitEventFields();
      if (isEvent && std::ranges::any_of(implicit, [&](const FieldInfo &i) {
            return i.name == f.name;
          })) {
        err(CheckErrorCode::ReservedField, f.loc,
            std::format("`{}` is an implicit event field and cannot be "
                        "declared",
                        f.name));
        continue;
      }
      if (std::ranges::any_of(
              out, [&](const FieldInfo &o) { return o.name == f.name; })) {
        err(CheckErrorCode::DuplicateName, f.loc,
            std::format("field `{}` is already declared", f.name));
        continue;
      }
      out.push_back({f.name, resolveType(f.type), f.loc});
    }
    return out;
  }

  void resolveEvents() {
    std::unordered_map<std::string, const EventDecl *> decls;
    for (const auto &d : unit_.decls) {
      if (const auto *e = std::get_if<EventDecl>(&d)) {
        decls.emplace(e->name, e);
      }
    }
    std::unordered_map<std::string, int> state; // 1 visiting, 2 done
    std::function<void(const EventDecl &)> resolve = [&](const EventDecl &d) {
      if (state[d.name] == 2) {
        return;
      }
      if (state[d.name] == 1) {
        err(CheckErrorCode::InheritanceCycle, d.loc,
            std::format("`{}` extends itself through its supertypes", d.name));
        return;
      }
      state[d.name] = 1;
      EventInfo info;
      info.name = d.name;
      info.loc  = d.loc;
      std::vector<FieldInfo> inherited;
      if (d.extends) {
        info.extends    = *d.extends;
        const auto base = decls.find(*d.extends);
        if (base != decls.end()) {
          if (base->second == &d) {
            err(CheckErrorCode::InheritanceCycle, d.loc,
                std::format("`{}` extends itself", d.name));
          } else {
            resolve(*base->second);
            if (const auto bi = out_.events.find(*d.extends);
                bi != out_.events.end()) {
              inherited       = bi->second.fields;
              info.supertypes = {*d.extends};
              info.supertypes.insert(info.supertypes.end(),
                                     bi->second.supertypes.begin(),
                                     bi->second.supertypes.end());
            }
          }
        } else if (const auto bi = out_.events.find(*d.extends);
                   bi != out_.events.end()) {
          inherited       = bi->second.fields;
          info.supertypes = {*d.extends};
        } else if (opaqueTypes_.contains(*d.extends)) {
          info.supertypes = {*d.extends};
        } else {
          err(typeNames_.contains(*d.extends) ? CheckErrorCode::InvalidExtends
                                              : CheckErrorCode::UnknownType,
              d.loc,
              std::format("`{}` extends `{}`, which is not an event", d.name,
                          *d.extends));
        }
      }
      info.fields   = resolveFields(d.fields, std::move(inherited), true);
      state[d.name] = 2;
      out_.events.emplace(d.name, std::move(info));
    };
    // Declaration order, so a cycle is reported at its first member. Only the
    // first declaration of a name is a real one; duplicates were reported in
    // pass A.
    for (const auto &d : unit_.decls) {
      if (const auto *e = std::get_if<EventDecl>(&d);
          e != nullptr && decls.at(e->name) == e) {
        resolve(*e);
      }
    }
  }

  MethodInfo resolveMethod(const MethodSig &s, const std::string &owner) {
    MethodInfo m;
    m.name      = s.name;
    m.owner     = owner;
    m.params    = resolveParams(s.params);
    m.hasResult = s.result.has_value();
    m.result    = s.result ? resolveType(*s.result) : basic(K::Unit);
    m.isEffect  = s.isEffect;
    m.preempts  = s.preempts;
    m.withinNs  = s.withinNs;
    m.loc       = s.loc;
    checkValueName(s.name, s.loc);
    return m;
  }

  void resolveCapacities() {
    for (const auto &d : unit_.decls) {
      const auto *c = std::get_if<CapacityDecl>(&d);
      if (c == nullptr || out_.capacities.contains(c->name)) {
        continue;
      }
      CapacityInfo info;
      info.name = c->name;
      info.loc  = c->loc;
      std::unordered_set<std::string> seen;
      for (const auto &sig : c->methods) {
        if (!seen.insert(sig.name).second) {
          err(CheckErrorCode::DuplicateName, sig.loc,
              std::format("method `{}` is declared twice in `{}`", sig.name,
                          c->name));
          continue;
        }
        if (sig.isEffect && !sig.withinNs) {
          err(CheckErrorCode::EffectWithoutWithin, sig.loc,
              std::format("effect `{}` must declare `within` (§6.6)",
                          sig.name));
        }
        info.methods.push_back(resolveMethod(sig, c->name));
      }
      for (const auto &b : c->extends) {
        const auto it = typeNames_.find(b);
        if (it == typeNames_.end() && !opaqueTypes_.contains(b)) {
          err(CheckErrorCode::UnknownCapacity, c->loc,
              std::format("`{}` extends unknown capacity `{}`", c->name, b));
        } else if (it != typeNames_.end() && it->second != TypeKind::Capacity) {
          err(CheckErrorCode::InvalidExtends, c->loc,
              std::format("`{}` extends `{}`, which is not a capacity", c->name,
                          b));
        }
        info.extends.push_back(b);
      }
      out_.capacities.emplace(c->name, std::move(info));
    }
    // Cycles among capacities.
    for (const auto &[name, cap] : out_.capacities) {
      if (cap.builtin) {
        continue;
      }
      std::unordered_set<std::string> seen;
      std::function<bool(const std::string &)> reaches =
          [&](const std::string &n) {
            const auto it = out_.capacities.find(n);
            if (it == out_.capacities.end()) {
              return false;
            }
            return std::ranges::any_of(it->second.extends, [&](auto &b) {
              if (b == name) {
                return true;
              }
              return seen.insert(b).second && reaches(b);
            });
          };
      if (reaches(name)) {
        err(CheckErrorCode::InheritanceCycle, cap.loc,
            std::format("capacity `{}` extends itself", name));
      }
    }
  }

  GoalInfo resolveGoal(const GoalDecl &g) {
    GoalInfo info;
    info.name   = g.name;
    info.kind   = g.kind;
    info.params = resolveParams(g.params);
    info.loc    = g.loc;
    return info;
  }

  bool capacityKnown(const std::string &name) const {
    return out_.capacities.contains(name) || opaqueTypes_.contains(name);
  }

  ContainerInfo buildContainer(ContainerInfo::Kind kind,
                               const std::string &name,
                               const std::vector<Param> &params,
                               const std::vector<Member> &members,
                               SourceLocation loc) {
    ContainerInfo info;
    info.kind   = kind;
    info.name   = name;
    info.params = resolveParams(params);
    info.loc    = loc;
    std::unordered_set<std::string> names;
    for (const auto &p : info.params) {
      names.insert(p.name);
    }
    std::unordered_set<std::string> methodNames;
    std::unordered_set<std::string> answerNames;
    std::unordered_set<std::string> planNames;
    for (const auto &m : members) {
      std::visit(
          Overloaded{
              [&](const UsesDecl &u) {
                for (const auto &c : u.capacities) {
                  if (!capacityKnown(c)) {
                    err(CheckErrorCode::UnknownCapacity, u.loc,
                        std::format("unknown capacity `{}`", c));
                    continue;
                  }
                  if (opaqueTypes_.contains(c)) {
                    usersOfOpaque_.insert(name);
                  }
                  info.uses.push_back(c);
                }
              },
              [&](const VarDecl &v) {
                checkValueName(v.name, v.loc);
                if (!names.insert(v.name).second) {
                  err(CheckErrorCode::DuplicateName, v.loc,
                      std::format("`{}` is already a parameter or field of "
                                  "`{}`",
                                  v.name, name));
                }
                info.fields.push_back({v.name, resolveType(v.type), v.loc});
              },
              [&](const MethodDef &d) {
                if (d.sig.isEffect) {
                  err(CheckErrorCode::EffectInSkill, d.sig.loc,
                      std::format("`{}` is an effect with a body; only native "
                                  "skills implement effects (§5.3)",
                                  d.sig.name));
                }
                if (!methodNames.insert(d.sig.name).second) {
                  err(CheckErrorCode::DuplicateName, d.sig.loc,
                      std::format("method `{}` is declared twice in `{}`",
                                  d.sig.name, name));
                  return;
                }
                info.methods.push_back(resolveMethod(d.sig, name));
              },
              [&](const AnswerDef &a) {
                if (!answerNames.insert(a.name).second) {
                  err(CheckErrorCode::DuplicateName, a.loc,
                      std::format("`answer {}` appears twice in `{}`", a.name,
                                  name));
                }
                info.answers.push_back(a.name);
                checkAnswerSignature(a);
              },
              [&](const GoalDecl &g) {
                if (std::ranges::any_of(info.goals, [&](const GoalInfo &x) {
                      return x.name == g.name;
                    })) {
                  err(CheckErrorCode::DuplicateName, g.loc,
                      std::format("goal `{}` is declared twice in `{}`", g.name,
                                  name));
                  return;
                }
                checkTypeName(g.name, g.loc);
                info.goals.push_back(resolveGoal(g));
              },
              [&](const PlanDecl &p) {
                checkValueName(p.name, p.loc);
                if (!planNames.insert(p.name).second) {
                  err(CheckErrorCode::DuplicateName, p.loc,
                      std::format("plan `{}` is declared twice in `{}`", p.name,
                                  name));
                }
              },
              [&](const auto &) {}},
          m);
    }
    return info;
  }

  void checkAnswerSignature(const AnswerDef &a) {
    const auto q = out_.questions.find(a.name);
    if (q == out_.questions.end()) {
      if (!opaqueValues_.contains(a.name)) {
        err(CheckErrorCode::UnknownQuestion, a.loc,
            std::format("`answer {}` implements no declared question", a.name));
      }
      return;
    }
    const auto params = resolveParamsQuiet(a.params);
    const Type result = resolveTypeQuiet(a.result);
    bool same         = params.size() == q->second.params.size();
    for (std::size_t i = 0; same && i < params.size(); ++i) {
      same = params[i].type == q->second.params[i].type;
    }
    if (!same || !(result == q->second.result)) {
      err(CheckErrorCode::SignatureMismatch, a.loc,
          std::format("`answer {}` differs from the declared question",
                      a.name));
    }
  }

  std::vector<ParamInfo> resolveParamsQuiet(const std::vector<Param> &ps) {
    const auto saved = errors_.size();
    auto out         = resolveParams(ps);
    errors_.resize(saved);
    return out;
  }
  Type resolveTypeQuiet(const TypeExpr &t) {
    const auto saved = errors_.size();
    auto out         = resolveType(t);
    errors_.resize(saved);
    return out;
  }

  bool implementsCapacity(const ContainerInfo &skill,
                          const std::string &capacity) const {
    return std::ranges::any_of(skill.implements, [&](const std::string &c) {
      if (c == capacity) {
        return true;
      }
      std::unordered_set<std::string> seen;
      std::function<bool(const std::string &)> walk =
          [&](const std::string &n) {
            if (n == capacity) {
              return true;
            }
            if (!seen.insert(n).second) {
              return false;
            }
            const auto it = out_.capacities.find(n);
            return it != out_.capacities.end() &&
                   std::ranges::any_of(it->second.extends, walk);
          };
      return walk(c);
    });
  }

  void checkSkillImplements(const SkillDecl &s, ContainerInfo &info) {
    info.native = s.native;
    for (const auto &c : s.implements) {
      if (!capacityKnown(c)) {
        err(CheckErrorCode::UnknownCapacity, s.loc,
            std::format("`{}` implements unknown capacity `{}`", s.name, c));
        continue;
      }
      info.implements.push_back(c);
    }
    if (s.native) {
      return;
    }
    for (const auto &c : info.implements) {
      for (const MethodInfo *m : allMethods(out_, c)) {
        if (m->isEffect) {
          err(CheckErrorCode::NonNativeEffect, s.loc,
              std::format("`{}` is not native but `{}` has effect `{}`; only "
                          "native skills implement effects (§5.3)",
                          s.name, m->owner, m->name));
          continue;
        }
        const auto mine =
            std::ranges::find(info.methods, m->name, &MethodInfo::name);
        if (mine == info.methods.end()) {
          err(CheckErrorCode::MissingImplementation, s.loc,
              std::format("`{}` does not implement `{}.{}`", s.name, m->owner,
                          m->name));
          continue;
        }
        bool same = mine->params.size() == m->params.size() &&
                    mine->hasResult == m->hasResult &&
                    (!m->hasResult || mine->result == m->result);
        for (std::size_t i = 0; same && i < m->params.size(); ++i) {
          same = mine->params[i].type == m->params[i].type;
        }
        if (!same) {
          err(CheckErrorCode::SignatureMismatch, mine->loc,
              std::format("`{}.{}` differs from `{}.{}`", s.name, m->name,
                          m->owner, m->name));
        }
      }
    }
  }

  void resolveSignatures() {
    resolveEvents();
    for (const auto &d : unit_.decls) {
      std::visit(Overloaded{[&](const RecordDecl &r) {
                              if (out_.records.contains(r.name)) {
                                return;
                              }
                              RecordInfo info;
                              info.name   = r.name;
                              info.loc    = r.loc;
                              info.fields = resolveFields(r.fields, {}, false);
                              out_.records.emplace(r.name, std::move(info));
                            },
                            [&](const auto &) {}},
                 d);
    }
    for (const auto &d : unit_.decls) {
      std::visit(
          Overloaded{[&](const PredicateDecl &p) {
                       PredicateInfo info;
                       info.name = p.name;
                       info.loc  = p.loc;
                       std::unordered_set<std::string> seen;
                       for (const auto &pp : p.params) {
                         checkValueName(pp.name, p.loc);
                         if (!seen.insert(pp.name).second) {
                           err(CheckErrorCode::DuplicateName, p.loc,
                               std::format("predicate argument `{}` repeats",
                                           pp.name));
                         }
                         info.params.push_back(
                             {pp.name, resolveType(pp.type), pp.key});
                       }
                       out_.predicates.emplace(p.name, std::move(info));
                     },
                     [&](const QuestionDecl &q) {
                       QuestionInfo info;
                       info.name   = q.name;
                       info.loc    = q.loc;
                       info.params = resolveParams(q.params);
                       info.result = resolveType(q.result);
                       out_.questions.emplace(q.name, std::move(info));
                     },
                     [&](const GoalDecl &g) {
                       if (!out_.goals.contains(g.name)) {
                         out_.goals.emplace(g.name, resolveGoal(g));
                       }
                     },
                     [&](const auto &) {}},
          d);
    }
    resolveCapacities();
    // Containers last: they name capacities, goals, questions and predicates.
    for (const auto &d : unit_.decls) {
      std::visit(
          Overloaded{
              [&](const SkillDecl &s) {
                if (out_.skills.contains(s.name)) {
                  return;
                }
                auto info = buildContainer(ContainerInfo::Kind::Skill, s.name,
                                           s.params, s.members, s.loc);
                checkSkillImplements(s, info);
                out_.skills.emplace(s.name, std::move(info));
              },
              [&](const BehaviorDecl &b) {
                if (!out_.behaviors.contains(b.name)) {
                  out_.behaviors.emplace(
                      b.name,
                      buildContainer(ContainerInfo::Kind::Behavior, b.name,
                                     b.params, b.members, b.loc));
                }
              },
              [&](const AgentDecl &a) {
                if (!out_.agents.contains(a.name)) {
                  out_.agents.emplace(
                      a.name, buildContainer(ContainerInfo::Kind::Agent, a.name,
                                             a.params, a.members, a.loc));
                }
              },
              [&](const PlanDecl &p) {
                checkValueName(p.name, p.loc);
                if (out_.plans.contains(p.name)) {
                  err(CheckErrorCode::DuplicateName, p.loc,
                      std::format("plan `{}` is declared twice", p.name));
                  return;
                }
                out_.plans.emplace(p.name,
                                   PlanInfo{p.name, p.goal, p.params, p.loc});
              },
              [&](const auto &) {}},
          d);
    }
  }

  // -- pass C: bodies -------------------------------------------------------

  void checkBodies() {
    for (const auto &d : unit_.decls) {
      std::visit(Overloaded{[&](const CapacityDecl &c) { checkCapacity(c); },
                            [&](const GoalDecl &g) {
                              const Restore lenient(lenient_, true);
                              checkGoalClauses(g, out_.goals.at(g.name).params);
                            },
                            [&](const PlanDecl &p) {
                              const Restore lenient(lenient_, true);
                              checkPlan(p, nullptr);
                            },
                            [&](const SkillDecl &s) {
                              checkContainer(out_.skills.at(s.name), s.members);
                            },
                            [&](const BehaviorDecl &b) {
                              checkContainer(out_.behaviors.at(b.name),
                                             b.members);
                            },
                            [&](const AgentDecl &a) {
                              checkContainer(out_.agents.at(a.name), a.members);
                            },
                            [&](const auto &) {}},
                 d);
    }
  }

  void checkCapacity(const CapacityDecl &c) {
    for (const auto &sig : c.methods) {
      ScopeGuard scope(*this);
      const auto params = resolveParamsQuiet(sig.params);
      for (const auto &p : params) {
        put(p.name, p.type, SymKind::Local);
      }
      for (const auto &contract : sig.contracts) {
        checkCondition(*contract.condition);
      }
    }
  }

  /// A pure condition: a guard, `where`, goal clause, contract, or the test of
  /// an `if` or `while` (§8.2).
  Type checkCondition(const Expr &e) {
    const Restore purity(purity_, Purity::Strict);
    const Type t = infer(e, operandPos);
    checkTruth(e, t);
    return t;
  }

  void checkTruth(const Expr &e, const Type &t) {
    // `&` and `|` already checked each operand; their own type is the last
    // operand's, which is not a new place a Bool can be mistaken for a test.
    if (const auto *b = std::get_if<EBinary>(&e.node);
        b != nullptr &&
        (b->op == EBinary::Op::And || b->op == EBinary::Op::Alt)) {
      return;
    }
    if (t.kind == K::Bool && !isComparison(e)) {
      err(CheckErrorCode::BoolCondition, e.loc,
          "a `Bool` value is a success even when false: compare with "
          "`== true`");
    }
  }

  void checkGoalClauses(const GoalDecl &g,
                        const std::vector<ParamInfo> &params) {
    ScopeGuard scope(*this);
    for (const auto &p : params) {
      put(p.name, p.type, SymKind::Local);
    }
    for (const auto &clause : g.clauses) {
      checkCondition(*clause.condition);
    }
  }

  void checkContainer(const ContainerInfo &info,
                      const std::vector<Member> &members) {
    const Restore cur(cur_, &info);
    {
      // Field initialisers see the parameters and the fields before them.
      ScopeGuard init(*this);
      for (const auto &p : info.params) {
        put(p.name, p.type, SymKind::Param);
      }
      for (const auto &m : members) {
        if (const auto *v = std::get_if<VarDecl>(&m)) {
          const Restore purity(purity_, Purity::Strict);
          const Type t = infer(*v->init, operandPos);
          expectType(resolveTypeQuiet(v->type), t, v->init->loc,
                     std::format("initialiser of `{}`", v->name));
          put(v->name, resolveTypeQuiet(v->type), SymKind::Field);
        }
      }
    }
    ScopeGuard scope(*this);
    for (const auto &p : info.params) {
      put(p.name, p.type, SymKind::Param);
    }
    for (const auto &f : info.fields) {
      put(f.name, f.type, SymKind::Field);
    }
    for (const auto &m : members) {
      std::visit(Overloaded{[&](const HandlerDef &h) { checkHandler(h); },
                            [&](const MethodDef &d) { checkMethod(d, info); },
                            [&](const AnswerDef &a) { checkAnswer(a, info); },
                            [&](const GoalDecl &g) {
                              const auto it = std::ranges::find(
                                  info.goals, g.name, &GoalInfo::name);
                              if (it != info.goals.end()) {
                                checkGoalClauses(g, it->params);
                              }
                            },
                            [&](const PlanDecl &p) { checkPlan(p, &info); },
                            [&](const InstallDecl &i) {
                              checkInstallLike(EInstall::Kind::Install, i.name,
                                               i.args, i.hasParens, i.loc);
                            },
                            [&](const BehaviorUse &b) {
                              checkInstallLike(EInstall::Kind::Mount, b.name,
                                               b.args, b.hasParens, b.loc);
                            },
                            [&](const BeliefDecl &b) {
                              const Restore purity(purity_, Purity::Strict);
                              checkTerm(b.fact);
                            },
                            [&](const LifecycleDef &l) {
                              const Restore body(body_, Body::Lifecycle);
                              DefFacts facts;
                              const Restore ff(curFacts_, &facts);
                              (void)infer(*l.body, Pos{true, false, false});
                            },
                            [&](const auto &) {}},
                 m);
    }
  }

  void checkHandler(const HandlerDef &h) {
    ScopeGuard scope(*this);
    const Restore body(body_, Body::Handler);
    const Restore occ(occurrence_, std::optional<Type>{});
    if (const auto *ev = std::get_if<EventTrigger>(&h.trigger)) {
      Type t = basic(K::Any);
      if (isEventName(ev->eventType)) {
        t = namedType(ev->eventType);
      } else if (opaqueTypes_.contains(ev->eventType)) {
        t = basic(K::Any);
      } else if (typeNames_.contains(ev->eventType)) {
        err(CheckErrorCode::NotAnEvent, h.loc,
            std::format("`{}` is not an event", ev->eventType));
      } else {
        err(CheckErrorCode::UnknownType, h.loc,
            std::format("unknown event `{}`", ev->eventType));
      }
      occurrence_ = t;
      if (ev->binder) {
        declare(*ev->binder, t, SymKind::Binder, h.loc);
      }
    } else {
      const Restore purity(purity_, Purity::Strict);
      checkTerm(std::get<BeliefTrigger>(h.trigger).fact);
    }
    if (h.guard) {
      checkCondition(*h.guard);
    }
    (void)infer(*h.body, Pos{true, false, false});
  }

  void checkBodyOfMethod(const MethodSig &sig, const Expr &body,
                         const ContainerInfo &owner, Body kind) {
    ScopeGuard scope(*this);
    const MethodInfo *info = nullptr;
    for (const auto &m : owner.methods) {
      if (m.name == sig.name) {
        info = &m;
        break;
      }
    }
    const Restore bodyKind(body_, kind);
    DefFacts &facts = facts_[owner.name + "::" + sig.name];
    const Restore ff(curFacts_, &facts);
    const auto params = resolveParamsQuiet(sig.params);
    for (std::size_t i = 0; i < params.size(); ++i) {
      put(params[i].name, params[i].type, SymKind::Local);
    }
    for (const auto &contract : sig.contracts) {
      checkCondition(*contract.condition);
    }
    const bool hasResult = sig.result.has_value();
    const Type result =
        hasResult ? resolveTypeQuiet(*sig.result) : basic(K::Unit);
    const Restore ret(returnType_,
                      std::optional<Type>{hasResult ? result : basic(K::Any)});
    const Type t = infer(body, Pos{true, false, hasResult});
    if (hasResult) {
      expectType(result, t, body.loc,
                 std::format("body of `{}`", info ? info->name : sig.name));
    }
  }

  void checkMethod(const MethodDef &d, const ContainerInfo &owner) {
    checkBodyOfMethod(d.sig, *d.body, owner, Body::Method);
  }

  void checkAnswer(const AnswerDef &a, const ContainerInfo &owner) {
    ScopeGuard scope(*this);
    const Restore bodyKind(body_, Body::Answer);
    const Restore purity(purity_, Purity::ReadOnly);
    DefFacts &facts = facts_[owner.name + "::answer " + a.name];
    const Restore ff(curFacts_, &facts);
    const auto params = resolveParamsQuiet(a.params);
    for (std::size_t i = 0; i < params.size(); ++i) {
      put(params[i].name, params[i].type, SymKind::Local);
    }
    const Type result = resolveTypeQuiet(a.result);
    const Restore ret(returnType_, std::optional<Type>{result});
    const Type t = infer(*a.body, Pos{true, false, true});
    expectType(result, t, a.body->loc, std::format("answer `{}`", a.name));
  }

  void checkPlan(const PlanDecl &p, const ContainerInfo *owner) {
    const GoalInfo *goal = nullptr;
    if (owner != nullptr) {
      const auto it = std::ranges::find(owner->goals, p.goal, &GoalInfo::name);
      if (it != owner->goals.end()) {
        goal = &*it;
      }
    }
    if (goal == nullptr) {
      if (const auto it = out_.goals.find(p.goal); it != out_.goals.end()) {
        goal = &it->second;
      }
    }
    if (goal == nullptr && !opaqueTypes_.contains(p.goal)) {
      err(CheckErrorCode::UnknownGoal, p.loc,
          std::format("plan `{}` is for unknown goal `{}`", p.name, p.goal));
    }
    ScopeGuard scope(*this);
    const Restore body(body_, Body::Plan);
    const Restore ret(returnType_, std::optional<Type>{});
    if (goal != nullptr) {
      if (p.hasParams) {
        for (const auto &n : p.params) {
          const auto gp = std::ranges::find(goal->params, n, &ParamInfo::name);
          if (gp == goal->params.end()) {
            err(CheckErrorCode::PlanParamNotInGoal, p.loc,
                std::format("`{}` is not a parameter of goal `{}`", n,
                            goal->name));
            continue;
          }
          put(n, gp->type, SymKind::Local);
        }
      } else {
        for (const auto &gp : goal->params) {
          put(gp.name, gp.type, SymKind::Local);
        }
      }
    } else {
      for (const auto &n : p.params) {
        put(n, basic(K::Any), SymKind::Local);
      }
    }
    if (p.where) {
      checkCondition(*p.where);
    }
    (void)infer(*p.body, Pos{true, true, false});
  }

  void checkInstallLike(EInstall::Kind kind, const std::string &name,
                        const std::vector<ExprPtr> &args, bool hasParens,
                        SourceLocation loc) {
    (void)hasParens;
    const ContainerInfo *target = nullptr;
    const bool wantSkill =
        kind == EInstall::Kind::Install || kind == EInstall::Kind::Uninstall;
    const auto &table = wantSkill ? out_.skills : out_.behaviors;
    if (const auto it = table.find(name); it != table.end()) {
      target = &it->second;
    } else if (!opaqueTypes_.contains(name)) {
      err(wantSkill ? CheckErrorCode::UnknownSkill
                    : CheckErrorCode::UnknownBehavior,
          loc,
          std::format("unknown {} `{}`", wantSkill ? "skill" : "behavior",
                      name));
    }
    if (target != nullptr &&
        (kind == EInstall::Kind::Install || kind == EInstall::Kind::Mount)) {
      checkArgs(target->params, args, loc, name);
    } else {
      for (const auto &a : args) {
        (void)infer(*a, operandPos);
      }
    }
  }

  // -- terms ----------------------------------------------------------------

  void bindLogicVar(const std::string &name, const Type &type,
                    SourceLocation loc) {
    if (const Symbol *s = lookup(name)) {
      expectType(s->type, type, loc, std::format("`?{}`", name));
      return;
    }
    declare(name, type, SymKind::Local, loc);
  }

  void checkTerm(const Term &t) {
    const auto it = out_.predicates.find(t.name);
    if (it == out_.predicates.end()) {
      if (!opaqueValues_.contains(t.name)) {
        err(CheckErrorCode::UnknownPredicate, t.loc,
            std::format("unknown predicate `{}`", t.name));
      }
      for (const auto &a : t.args) {
        if (a.logicVar) {
          bindLogicVar(*a.logicVar, basic(K::Any), t.loc);
        } else if (a.expr && !isWildcard(*a.expr)) {
          (void)infer(*a.expr, operandPos);
        }
      }
      return;
    }
    const auto &params = it->second.params;
    if (params.size() != t.args.size()) {
      err(CheckErrorCode::ArityMismatch, t.loc,
          std::format("predicate `{}` takes {} arguments, found {}", t.name,
                      params.size(), t.args.size()));
    }
    for (std::size_t i = 0; i < t.args.size(); ++i) {
      const Type want = i < params.size() ? params[i].type : basic(K::Any);
      const auto &a   = t.args[i];
      if (a.logicVar) {
        bindLogicVar(*a.logicVar, want, t.loc);
      } else if (a.expr && !isWildcard(*a.expr)) {
        const Type got = infer(*a.expr, operandPos);
        expectType(want, got, a.expr->loc,
                   std::format("argument {} of `{}`", i + 1, t.name));
      }
    }
  }

  static bool isWildcard(const Expr &e) {
    const auto *n = std::get_if<EName>(&e.node);
    return n != nullptr && n->name == "_";
  }

  // -- calls and arguments --------------------------------------------------

  void checkArgs(const std::vector<ParamInfo> &params,
                 const std::vector<ExprPtr> &args, SourceLocation loc,
                 std::string_view what) {
    if (params.size() != args.size()) {
      err(CheckErrorCode::ArityMismatch, loc,
          std::format("`{}` takes {} arguments, found {}", what, params.size(),
                      args.size()));
    }
    for (std::size_t i = 0; i < args.size(); ++i) {
      const Type got = infer(*args[i], operandPos);
      if (i < params.size()) {
        expectType(params[i].type, got, args[i]->loc,
                   std::format("argument `{}` of `{}`", params[i].name, what));
      }
    }
  }

  // -- the effect / action / suspension rules --------------------------------

  void noteAction(SourceLocation loc, std::string_view what) {
    if (purity_ == Purity::Strict) {
      err(CheckErrorCode::ImpureCondition, loc,
          std::format("`{}` is an action; conditions are pure (§8.2)", what));
    } else if (purity_ == Purity::ReadOnly) {
      err(CheckErrorCode::ImpureAnswer, loc,
          std::format("`{}` in an answer; answers are read-only (§7.1)", what));
    }
    if (curFacts_ != nullptr) {
      curFacts_->action = true;
    }
  }

  void noteAssign(SourceLocation loc, bool isField) {
    if (purity_ == Purity::Strict) {
      err(CheckErrorCode::ImpureCondition, loc,
          "an assignment in a condition; conditions are pure (§8.2)");
    } else if (purity_ == Purity::ReadOnly && isField) {
      err(CheckErrorCode::ImpureAnswer, loc,
          "a field write in an answer; answers are read-only (§7.1)");
    }
    if (curFacts_ != nullptr) {
      curFacts_->assign = true;
      if (isField) {
        curFacts_->fieldWrite = true;
      }
    }
  }

  void requireBounded(SourceLocation loc, Pos pos, std::string_view what) {
    if (!pos.bounded) {
      err(CheckErrorCode::BoundedOnly, loc,
          std::format("`{}` is allowed in bounded positions only; use `<-` or "
                      "move it to its own statement (§8.3)",
                      what));
    }
  }

  void requireSuspension(SourceLocation loc, Pos pos, std::string_view what) {
    if (purity_ != Purity::None) {
      noteAction(loc, what);
      return;
    }
    if (curFacts_ != nullptr) {
      curFacts_->action = true;
    }
    if (body_ != Body::Plan) {
      err(CheckErrorCode::SuspensionOutsidePlan, loc,
          std::format("`{}` suspends and is allowed in plans only (§8.3)",
                      what));
      return;
    }
    if (inBelievesLoop_) {
      err(CheckErrorCode::SuspensionInBelievesLoop, loc,
          std::format("`{}` inside a `for believes` loop (§6.6)", what));
      return;
    }
    if (!pos.susp) {
      err(CheckErrorCode::SuspensionPosition, loc,
          std::format("`{}` must be a statement of a plan, or the value of a "
                      "`let` or `:=` statement, not part of an expression "
                      "(§6.6)",
                      what));
    }
  }

  void requireEffectPosition(SourceLocation loc, Pos pos,
                             std::string_view name) {
    if (purity_ != Purity::None) {
      noteAction(loc, name);
      return;
    }
    if (curFacts_ != nullptr) {
      curFacts_->effect = true;
    }
    switch (body_) {
    case Body::Plan:
      if (inBelievesLoop_) {
        err(CheckErrorCode::SuspensionInBelievesLoop, loc,
            std::format("effect `{}` inside a `for believes` loop (§6.6)",
                        name));
      } else if (!pos.susp) {
        err(CheckErrorCode::EffectPosition, loc,
            std::format("effect `{}` suspends and must be a statement of a "
                        "plan (§8.3)",
                        name));
      }
      break;
    case Body::Handler:
      if (!pos.bounded) {
        err(CheckErrorCode::EffectPosition, loc,
            std::format("effect `{}` in a handler must be in a bounded "
                        "position (§8.3)",
                        name));
      }
      break;
    default:
      err(CheckErrorCode::EffectInMethod, loc,
          std::format("effect `{}` may not be called here: never in methods, "
                      "conditions or answers (§8.3)",
                      name));
    }
  }

  // -- expressions ----------------------------------------------------------

  Type infer(const Expr &e, Pos pos) {
    return std::visit(
        [&](const auto &n) -> Type { return this->node(n, e, pos); }, e.node);
  }

  Type node(const ELiteral &n, const Expr &, Pos) {
    switch (n.kind) {
    case ELiteral::Kind::Int:
      return basic(K::Int);
    case ELiteral::Kind::Float:
      return basic(K::Float);
    case ELiteral::Kind::Str:
      return basic(K::Str);
    case ELiteral::Kind::Bool:
      return basic(K::Bool);
    case ELiteral::Kind::Duration:
      return basic(K::Duration);
    case ELiteral::Kind::Unit:
      return basic(K::Unit);
    case ELiteral::Kind::None:
      return optOf(basic(K::Any));
    }
    return basic(K::Any);
  }

  Type node(const EName &n, const Expr &e, Pos) {
    if (n.name.starts_with('$')) {
      err(CheckErrorCode::ReservedName, e.loc,
          std::format("`{}`: names beginning with `$` are reserved", n.name));
      return basic(K::Any);
    }
    if (n.name == "_") {
      err(CheckErrorCode::UnknownName, e.loc,
          "`_` binds nothing and cannot be read");
      return basic(K::Any);
    }
    if (const Symbol *s = lookup(n.name)) {
      return unwrap(s->type);
    }
    if (lenient_ || opaqueValues_.contains(n.name) ||
        opaqueTypes_.contains(n.name)) {
      return basic(K::Any);
    }
    err(CheckErrorCode::UnknownName, e.loc,
        std::format("unknown name `{}`", n.name));
    return basic(K::Any);
  }

  Type node(const ESpecial &n, const Expr &e, Pos) {
    switch (n.kind) {
    case ESpecial::Kind::Self:
    case ESpecial::Kind::Parent:
      return basic(K::Agent);
    case ESpecial::Kind::Inner:
    case ESpecial::Kind::Outer:
      return basic(K::Space);
    case ESpecial::Kind::Occurrence:
      if (!occurrence_) {
        err(CheckErrorCode::OccurrenceOutsideHandler, e.loc,
            "`occurrence` is bound in an event handler only (§8.8)");
        return basic(K::Any);
      }
      return *occurrence_;
    }
    return basic(K::Any);
  }

  Type node(const EUnary &n, const Expr &e, Pos) {
    const Type t = infer(*n.operand, operandPos);
    switch (n.op) {
    case EUnary::Op::Neg:
      if (t.kind != K::Int && t.kind != K::Float && t.kind != K::Duration &&
          t.kind != K::Any) {
        err(CheckErrorCode::TypeMismatch, e.loc,
            std::format("cannot negate {}", typeName(t)));
        return basic(K::Any);
      }
      return t;
    case EUnary::Op::Generate:
      if (t.kind == K::List) {
        return t.args[0];
      }
      if (t.kind == K::Str || t.kind == K::Any) {
        return t.kind == K::Str ? basic(K::Str) : t;
      }
      err(CheckErrorCode::TypeMismatch, e.loc,
          std::format("`!` generates the elements of a list or the characters "
                      "of a string, not {}",
                      typeName(t)));
      return basic(K::Any);
    case EUnary::Op::Size:
      if (t.kind != K::List && t.kind != K::Str && t.kind != K::Map &&
          t.kind != K::Any) {
        err(CheckErrorCode::TypeMismatch, e.loc,
            std::format("`*` takes the size of a list, string or map, not {}",
                        typeName(t)));
      }
      return basic(K::Int);
    case EUnary::Op::Not:
      checkTruth(*n.operand, t);
      return basic(K::Unit);
    }
    return basic(K::Any);
  }

  Type arithmetic(EBinary::Op op, const Type &l, const Type &r,
                  SourceLocation loc) {
    using Op     = EBinary::Op;
    auto numeric = [](const Type &t) {
      return t.kind == K::Int || t.kind == K::Float;
    };
    auto bad = [&]() {
      err(CheckErrorCode::TypeMismatch, loc,
          std::format("operands of this operator must have one numeric type; "
                      "found {} and {}",
                      typeName(l), typeName(r)));
      return basic(K::Any);
    };
    if (l.kind == K::Any && r.kind == K::Any) {
      return l;
    }
    if (l.kind == K::Any || r.kind == K::Any) {
      const Type &other = l.kind == K::Any ? r : l;
      if (numeric(other) || other.kind == K::Duration ||
          other.kind == K::Time) {
        return other;
      }
      return bad();
    }
    if (numeric(l) && l.kind == r.kind) {
      if (op == Op::Mod && l.kind != K::Int) {
        return bad();
      }
      return l;
    }
    if (l.kind == K::Duration && r.kind == K::Duration &&
        (op == Op::Add || op == Op::Sub)) {
      return l;
    }
    if (l.kind == K::Duration && r.kind == K::Int && op == Op::Mul) {
      return l;
    }
    if (l.kind == K::Int && r.kind == K::Duration && op == Op::Mul) {
      return r;
    }
    if (l.kind == K::Time && r.kind == K::Duration &&
        (op == Op::Add || op == Op::Sub)) {
      return l;
    }
    if (l.kind == K::Time && r.kind == K::Time && op == Op::Sub) {
      return basic(K::Duration);
    }
    return bad();
  }

  Type node(const EBinary &n, const Expr &e, Pos pos) {
    using Op = EBinary::Op;
    switch (n.op) {
    case Op::Assign:
    case Op::RevAssign:
      return assignment(n, e, pos);
    case Op::And:
    case Op::Alt: {
      const Type l = infer(*n.left, operandPos);
      const Type r = infer(*n.right, operandPos);
      checkTruth(*n.left, l);
      checkTruth(*n.right, r);
      if (n.op == Op::And) {
        return r;
      }
      warnAlternation(n);
      const auto j = join(l, r);
      if (!j) {
        err(CheckErrorCode::TypeMismatch, e.loc,
            std::format("the arms of `|` have different types: {} and {}",
                        typeName(l), typeName(r)));
        return basic(K::Any);
      }
      return *j;
    }
    case Op::Scan: {
      const Type l = infer(*n.left, operandPos);
      expectType(basic(K::Str), l, n.left->loc, "the subject of `?`");
      return infer(*n.right, operandPos);
    }
    case Op::Concat: {
      const Type l = infer(*n.left, operandPos);
      const Type r = infer(*n.right, operandPos);
      const bool okKind =
          (l.kind == K::Str || l.kind == K::List || l.kind == K::Any);
      if (!okKind) {
        err(CheckErrorCode::TypeMismatch, e.loc,
            std::format("`++` joins strings or lists, not {}", typeName(l)));
        return basic(K::Any);
      }
      const auto j = join(l, r);
      if (!j ||
          (j->kind != K::Str && j->kind != K::List && j->kind != K::Any)) {
        err(CheckErrorCode::TypeMismatch, e.loc,
            std::format("`++` operands differ: {} and {}", typeName(l),
                        typeName(r)));
        return basic(K::Any);
      }
      return *j;
    }
    case Op::Lt:
    case Op::Le:
    case Op::Gt:
    case Op::Ge:
    case Op::Eq:
    case Op::Ne: {
      const Type l       = infer(*n.left, operandPos);
      const Type r       = infer(*n.right, operandPos);
      const bool ordered = n.op != Op::Eq && n.op != Op::Ne;
      if (!assignable(l, r) && !assignable(r, l)) {
        err(CheckErrorCode::TypeMismatch, e.loc,
            std::format("`==` compares values of one type; found {} and {} "
                        "(§8.2)",
                        typeName(l), typeName(r)));
      } else if (ordered) {
        const Type &t = l.kind == K::Any ? r : l;
        if (t.kind != K::Any && t.kind != K::Int && t.kind != K::Float &&
            t.kind != K::Str && t.kind != K::Duration && t.kind != K::Time) {
          err(CheckErrorCode::TypeMismatch, e.loc,
              std::format("{} has no order", typeName(t)));
        }
      }
      return r;
    }
    default: {
      const Type l = infer(*n.left, operandPos);
      const Type r = infer(*n.right, operandPos);
      return arithmetic(n.op, l, r, e.loc);
    }
    }
  }

  void warnAlternation(const EBinary &alt) {
    const auto *l = std::get_if<EBinary>(&alt.left->node);
    if (l != nullptr && isComparison(*alt.left) &&
        std::holds_alternative<ELiteral>(l->right->node) &&
        std::holds_alternative<ELiteral>(alt.right->node)) {
      warn(CheckErrorCode::AlternationOfLiterals, alt.left->loc,
           "`x == 1 | 2` parses as `(x == 1) | 2` and always succeeds; write "
           "`x == (1 | 2)`");
    }
  }

  struct Place {
    Type type; ///< as declared, `Opt` included
    bool isField{false};
  };

  std::optional<Place> place(const Expr &e) {
    if (const auto *n = std::get_if<EName>(&e.node)) {
      if (n->name.starts_with('$')) {
        err(CheckErrorCode::ReservedName, e.loc,
            std::format("`{}`: `$` fields cannot be assigned", n->name));
        return std::nullopt;
      }
      const Symbol *s = lookup(n->name);
      if (s == nullptr) {
        if (!lenient_ && !opaqueValues_.contains(n->name)) {
          err(CheckErrorCode::UnknownName, e.loc,
              std::format("unknown name `{}`", n->name));
        }
        return std::nullopt;
      }
      if (s->kind == SymKind::Binder) {
        err(CheckErrorCode::NotAPlace, e.loc,
            std::format("`{}` is an event and cannot be assigned", n->name));
        return std::nullopt;
      }
      return Place{s->type,
                   s->kind == SymKind::Field || s->kind == SymKind::Param};
    }
    if (const auto *f = std::get_if<EField>(&e.node)) {
      const auto root = place(*f->base);
      if (!root) {
        return std::nullopt;
      }
      const Type base = unwrap(root->type);
      if (const auto field = fieldOf(base, f->name, e.loc, true)) {
        return Place{*field, root->isField};
      }
      return std::nullopt;
    }
    if (const auto *i = std::get_if<EIndex>(&e.node)) {
      const auto root = place(*i->base);
      (void)infer(*i->index, operandPos);
      if (!root) {
        return std::nullopt;
      }
      const Type base = unwrap(root->type);
      if (base.kind == K::List) {
        return Place{base.args[0], root->isField};
      }
      if (base.kind == K::Map) {
        return Place{base.args[1], root->isField};
      }
      if (base.kind == K::Any) {
        return Place{base, root->isField};
      }
      err(CheckErrorCode::NotAPlace, e.loc,
          std::format("cannot assign into an element of {}", typeName(base)));
      return std::nullopt;
    }
    err(CheckErrorCode::NotAPlace, e.loc,
        "the target of an assignment is a local, a field, or a field or index "
        "of one (§8.3)");
    return std::nullopt;
  }

  /// A field of a record or event value. `forWrite` refuses the implicit
  /// event fields, which are readable and not assignable (§5.5).
  std::optional<Type> fieldOf(const Type &base, const std::string &name,
                              SourceLocation loc, bool forWrite) {
    if (base.kind == K::Any) {
      return basic(K::Any);
    }
    if (base.kind != K::Named) {
      err(CheckErrorCode::UnknownField, loc,
          std::format("{} has no field `{}`", typeName(base), name));
      return std::nullopt;
    }
    const std::vector<FieldInfo> *fields = nullptr;
    bool isEvent                         = false;
    if (const auto it = out_.events.find(base.name); it != out_.events.end()) {
      fields  = &it->second.fields;
      isEvent = true;
    } else if (const auto rt = out_.records.find(base.name);
               rt != out_.records.end()) {
      fields = &rt->second.fields;
    }
    if (fields == nullptr) {
      return basic(K::Any);
    }
    for (const auto &f : *fields) {
      if (f.name == name) {
        return f.type;
      }
    }
    if (isEvent) {
      for (const auto &f : implicitEventFields()) {
        if (f.name == name) {
          if (forWrite) {
            err(CheckErrorCode::ReservedField, loc,
                std::format("`{}` is an implicit event field and cannot be "
                            "assigned (§5.5)",
                            name));
            return std::nullopt;
          }
          return f.type;
        }
      }
    }
    err(CheckErrorCode::UnknownField, loc,
        std::format("`{}` has no field `{}`", base.name, name));
    return std::nullopt;
  }

  Type assignment(const EBinary &n, const Expr &e, Pos pos) {
    const bool irreversible = n.op == EBinary::Op::Assign;
    if (irreversible) {
      requireBounded(e.loc, pos, ":=");
    }
    const auto target = place(*n.left);
    const Type rhs    = infer(*n.right, Pos{pos.bounded, pos.susp, true});
    if (target) {
      expectType(target->type, rhs, n.right->loc, "assigned value");
      noteAssign(e.loc, target->isField);
    } else if (curFacts_ != nullptr) {
      curFacts_->assign = true;
    }
    return basic(K::Unit);
  }

  Type node(const ERange &n, const Expr &e, Pos) {
    for (const Expr *part : {n.lo.get(), n.hi.get(), n.by.get()}) {
      if (part == nullptr) {
        continue;
      }
      const Type t = infer(*part, operandPos);
      if (t.kind != K::Int && t.kind != K::Any) {
        err(CheckErrorCode::TypeMismatch, part->loc,
            std::format("`to` ranges over Int, found {}", typeName(t)));
      }
    }
    (void)e;
    return basic(K::Int);
  }

  const MethodInfo *findOwnDef(const std::string &name) const {
    if (cur_ == nullptr) {
      return nullptr;
    }
    for (const auto &m : cur_->methods) {
      if (m.name == name) {
        return &m;
      }
    }
    return nullptr;
  }

  static std::string capKey(const MethodInfo &m) {
    return "cap:" + m.owner + "." + m.name;
  }

  Type finishCall(const MethodInfo &m, const std::string &key,
                  const ECall &call, const Expr &e, Pos pos) {
    checkArgs(m.params, call.args, e.loc, m.name);
    const bool builtinImpure = m.owner == "Schedules" || m.owner == "Logging";
    if (m.isEffect) {
      requireEffectPosition(e.loc, pos, m.name);
    } else if (builtinImpure) {
      noteAction(e.loc, m.owner + "." + m.name);
    } else {
      if (curFacts_ != nullptr) {
        curFacts_->callees.push_back(key);
      }
      if (purity_ != Purity::None) {
        requirements_.push_back({key, purity_, e.loc});
      }
    }
    return m.result;
  }

  Type unknownCall(const ECall &call, const Expr &e) {
    for (const auto &a : call.args) {
      (void)infer(*a, operandPos);
    }
    if (curFacts_ != nullptr) {
      curFacts_->unknownCall = true;
    }
    if (purity_ != Purity::None) {
      requirements_.push_back({"?unknown", purity_, e.loc});
    }
    return basic(K::Any);
  }

  Type node(const ECall &n, const Expr &e, Pos pos) {
    if (const auto *name = std::get_if<EName>(&n.callee->node)) {
      return bareCall(*name, n, e, pos);
    }
    if (const auto *f = std::get_if<EField>(&n.callee->node)) {
      if (const auto *cap = std::get_if<EName>(&f->base->node)) {
        if (out_.capacities.contains(cap->name)) {
          for (const MethodInfo *m : allMethods(out_, cap->name)) {
            if (m->name == f->name) {
              return finishCall(*m, capKey(*m), n, e, pos);
            }
          }
          err(CheckErrorCode::UnknownFunction, e.loc,
              std::format("capacity `{}` has no method `{}`", cap->name,
                          f->name));
          return unknownCallQuiet(n);
        }
        if (opaqueTypes_.contains(cap->name)) {
          return unknownCall(n, e);
        }
      }
    }
    err(CheckErrorCode::NotCallable, e.loc,
        "only a method, a capacity method `Capacity.method(...)` or a "
        "function can be called; ask another agent with `ask a.q(...)`");
    return unknownCallQuiet(n);
  }

  Type unknownCallQuiet(const ECall &call) {
    for (const auto &a : call.args) {
      (void)infer(*a, operandPos);
    }
    return basic(K::Any);
  }

  Type builtinFunction(const std::string &name, const ECall &n, const Expr &e,
                       bool &handled) {
    handled   = true;
    auto args = [&](std::vector<ParamInfo> ps) {
      checkArgs(ps, n.args, e.loc, name);
    };
    if (name == "text") {
      if (n.args.size() != 1) {
        err(CheckErrorCode::ArityMismatch, e.loc,
            "`text` takes exactly one argument");
      }
      for (const auto &a : n.args) {
        (void)infer(*a, operandPos);
      }
      return basic(K::Str);
    }
    if (name == "intentions") {
      args({});
      return listOf(basic(K::Intention));
    }
    if (name == "is_maintain") {
      args({{"i", basic(K::Intention)}});
      return basic(K::Unit);
    }
    if (name == "restorers") {
      args({{"i", basic(K::Intention)}});
      return listOf(basic(K::Intention));
    }
    if (name == "members") {
      args({{"s", basic(K::Space)}});
      return listOf(basic(K::Agent));
    }
    if (name == "children") {
      args({});
      return listOf(basic(K::Agent));
    }
    handled = false;
    return basic(K::Any);
  }

  Type bareCall(const EName &name, const ECall &n, const Expr &e, Pos pos) {
    if (const MethodInfo *own = findOwnDef(name.name)) {
      return finishCall(*own, cur_->name + "::" + own->name, n, e, pos);
    }
    // Used capacities, then the capacities every agent uses (§8.5).
    std::vector<std::string> caps;
    if (cur_ != nullptr) {
      caps = cur_->uses;
    }
    caps.insert(caps.end(), builtinCapacityNames.begin(),
                builtinCapacityNames.end());
    const MethodInfo *found = nullptr;
    std::string foundCap;
    for (const auto &c : caps) {
      for (const MethodInfo *m : allMethods(out_, c)) {
        if (m->name != name.name) {
          continue;
        }
        if (found != nullptr && found != m) {
          err(CheckErrorCode::AmbiguousCall, e.loc,
              std::format("`{}` is defined by `{}` and `{}`; qualify it as "
                          "`{}.{}(...)`",
                          name.name, foundCap, c, c, name.name));
          return unknownCallQuiet(n);
        }
        found    = m;
        foundCap = c;
      }
    }
    if (found != nullptr) {
      return finishCall(*found, capKey(*found), n, e, pos);
    }
    bool handled    = false;
    const Type type = builtinFunction(name.name, n, e, handled);
    if (handled) {
      return type;
    }
    if (opaqueValues_.contains(name.name)) {
      for (const auto &a : n.args) {
        (void)infer(*a, operandPos);
      }
      return basic(K::Any);
    }
    if (cur_ != nullptr && usersOfOpaque_.contains(cur_->name)) {
      return unknownCall(n, e);
    }
    if (lenient_) {
      return unknownCall(n, e);
    }
    err(CheckErrorCode::UnknownFunction, e.loc,
        std::format("unknown function or method `{}`", name.name));
    return unknownCallQuiet(n);
  }

  Type node(const EIndex &n, const Expr &e, Pos) {
    const Type b = infer(*n.base, operandPos);
    const Type i = infer(*n.index, operandPos);
    switch (b.kind) {
    case K::List:
      expectType(basic(K::Int), i, n.index->loc, "list index");
      return b.args[0];
    case K::Str:
      expectType(basic(K::Int), i, n.index->loc, "string index");
      return basic(K::Str);
    case K::Map:
      expectType(b.args[0], i, n.index->loc, "map key");
      return b.args[1];
    case K::Any:
      return b;
    default:
      err(CheckErrorCode::TypeMismatch, e.loc,
          std::format("{} cannot be indexed", typeName(b)));
      return basic(K::Any);
    }
  }

  Type node(const EField &n, const Expr &e, Pos) {
    const Type base = infer(*n.base, operandPos);
    const auto t    = fieldOf(base, n.name, e.loc, false);
    return t ? unwrap(*t) : basic(K::Any);
  }

  Type node(const EList &n, const Expr &e, Pos) {
    Type elem  = basic(K::Any);
    bool first = true;
    for (const auto &x : n.elems) {
      const Type t = infer(*x, operandPos);
      if (first) {
        elem  = t;
        first = false;
        continue;
      }
      const auto j = join(elem, t);
      if (!j) {
        err(CheckErrorCode::TypeMismatch, x->loc,
            std::format("list elements differ: {} and {}", typeName(elem),
                        typeName(t)));
      } else {
        elem = *j;
      }
    }
    (void)e;
    return listOf(elem);
  }

  Type node(const ERecord &n, const Expr &e, Pos) {
    const std::vector<FieldInfo> *decl = nullptr;
    if (const auto it = out_.events.find(n.type); it != out_.events.end()) {
      decl = &it->second.fields;
    } else if (const auto rt = out_.records.find(n.type);
               rt != out_.records.end()) {
      decl = &rt->second.fields;
    } else if (opaqueTypes_.contains(n.type)) {
      for (const auto &f : n.fields) {
        (void)infer(*f.value, operandPos);
      }
      return basic(K::Any);
    } else {
      err(CheckErrorCode::UnknownType, e.loc,
          std::format("unknown event or record `{}`", n.type));
      for (const auto &f : n.fields) {
        (void)infer(*f.value, operandPos);
      }
      return basic(K::Any);
    }
    std::unordered_set<std::string> seen;
    for (const auto &f : n.fields) {
      const Type got = infer(*f.value, operandPos);
      if (std::ranges::any_of(
              implicitEventFields(),
              [&](const FieldInfo &i) { return i.name == f.name; }) &&
          isEventName(n.type)) {
        err(CheckErrorCode::ReservedField, f.value->loc,
            std::format("`{}` is an implicit event field; the runtime sets it "
                        "(§5.5)",
                        f.name));
        continue;
      }
      const auto field = std::ranges::find(*decl, f.name, &FieldInfo::name);
      if (field == decl->end()) {
        err(CheckErrorCode::UnknownField, f.value->loc,
            std::format("`{}` has no field `{}`", n.type, f.name));
        continue;
      }
      if (!seen.insert(f.name).second) {
        err(CheckErrorCode::DuplicateField, f.value->loc,
            std::format("field `{}` is given twice", f.name));
        continue;
      }
      expectType(field->type, got, f.value->loc,
                 std::format("field `{}` of `{}`", f.name, n.type));
    }
    for (const auto &f : *decl) {
      if (!seen.contains(f.name) && f.type.kind != K::Opt) {
        err(CheckErrorCode::MissingField, e.loc,
            std::format("`{}` needs field `{}`", n.type, f.name));
      }
    }
    return namedType(n.type);
  }

  Type node(const EBlock &n, const Expr &, Pos pos) {
    ScopeGuard scope(*this);
    Type last = basic(K::Unit);
    for (std::size_t i = 0; i < n.stmts.size(); ++i) {
      const auto &s     = n.stmts[i];
      const bool isLast = i + 1 == n.stmts.size();
      const Pos sp{true, pos.susp, s.isLet || (isLast && pos.used)};
      const Type t = infer(*s.value, sp);
      if (s.isLet) {
        Type declared = t;
        if (s.type) {
          declared = resolveType(*s.type);
          expectType(declared, t, s.value->loc,
                     std::format("`let {}`", s.name));
        }
        // `let x := e` where e is an Opt read already unwrapped.
        declare(s.name, declared, SymKind::Local, s.loc);
        last = basic(K::Unit);
      } else {
        last = t;
      }
    }
    return last;
  }

  Type node(const EIf &n, const Expr &e, Pos pos) {
    Type thenType = basic(K::Unit);
    {
      ScopeGuard scope(*this);
      (void)checkCondition(*n.cond);
      const Pos bp{pos.bounded, pos.susp, pos.used};
      thenType = infer(*n.thenBranch, bp);
    }
    if (!n.elseBranch) {
      if (pos.used && thenType.kind != K::Unit && thenType.kind != K::Never &&
          thenType.kind != K::Any) {
        err(CheckErrorCode::IfWithoutElse, e.loc,
            std::format("`if` without `else` succeeds with `()` when the test "
                        "fails, so the branch must be `()`, not {} (§8.2)",
                        typeName(thenType)));
      }
      return basic(K::Unit);
    }
    const Type elseType =
        infer(*n.elseBranch, Pos{pos.bounded, pos.susp, pos.used});
    if (const auto j = join(thenType, elseType)) {
      return *j;
    }
    if (pos.used) {
      err(CheckErrorCode::TypeMismatch, e.loc,
          std::format("the arms of `if` have different types: {} and {}",
                      typeName(thenType), typeName(elseType)));
      return basic(K::Any);
    }
    return basic(K::Unit);
  }

  Type node(const EWhile &n, const Expr &, Pos pos) {
    ScopeGuard scope(*this);
    (void)checkCondition(*n.cond);
    (void)infer(*n.body, Pos{pos.bounded, pos.susp, false});
    return basic(K::Unit);
  }

  Type node(const EFor &n, const Expr &e, Pos pos) {
    ScopeGuard scope(*this);
    if (!n.iterable) {
      // `for believes term do body`
      const Restore loop(inBelievesLoop_, true);
      {
        const Restore purity(purity_, Purity::Strict);
        if (n.believesTerm) {
          checkTerm(*n.believesTerm);
        }
      }
      (void)infer(*n.body, Pos{pos.bounded, false, false});
      return basic(K::Unit);
    }
    const Type it = infer(*n.iterable, operandPos);
    Type elem     = basic(K::Any);
    switch (it.kind) {
    case K::List:
      elem = it.args[0];
      break;
    case K::Int:
      elem = it; // a range yields Int
      break;
    case K::Str:
      elem = it;
      break;
    case K::Any:
      break;
    default:
      err(CheckErrorCode::TypeMismatch, n.iterable->loc,
          std::format("`for` iterates a list or a range, not {}",
                      typeName(it)));
    }
    declare(n.binder, elem, SymKind::Local, e.loc);
    (void)infer(*n.body, Pos{pos.bounded, pos.susp, false});
    return basic(K::Unit);
  }

  Type node(const EBelieves &n, const Expr &, Pos) {
    checkTerm(n.term);
    if (n.from) {
      const Type t = infer(*n.from, operandPos);
      expectType(basic(K::Agent), t, n.from->loc, "`from`");
    }
    return basic(K::Unit);
  }

  Type node(const EFindall &n, const Expr &, Pos) {
    ScopeGuard scope(*this);
    checkTerm(n.goal);
    if (n.templ.logicVar) {
      const Symbol *s = lookup(*n.templ.logicVar);
      if (s == nullptr) {
        err(CheckErrorCode::UnknownName, n.goal.loc,
            std::format("`?{}` does not occur in the goal", *n.templ.logicVar));
        return listOf(basic(K::Any));
      }
      return listOf(unwrap(s->type));
    }
    if (isWildcard(*n.templ.expr)) {
      return listOf(basic(K::Any));
    }
    return listOf(infer(*n.templ.expr, operandPos));
  }

  // -- actions (§8.6) -------------------------------------------------------

  Type node(const EEmit &n, const Expr &e, Pos) {
    noteAction(e.loc, "emit");
    const Type ev = infer(*n.event, operandPos);
    if (ev.kind == K::Named && !isEventName(ev.name)) {
      err(CheckErrorCode::NotAnEvent, n.event->loc,
          std::format("`{}` is a record, not an event", ev.name));
    } else if (ev.kind != K::Named && ev.kind != K::Event &&
               ev.kind != K::Any) {
      err(CheckErrorCode::NotAnEvent, n.event->loc,
          std::format("`emit` takes an event, not {}", typeName(ev)));
    }
    if (n.space) {
      expectType(basic(K::Space), infer(*n.space, operandPos), n.space->loc,
                 "`in`");
    }
    if (n.recipients) {
      const Type r = infer(*n.recipients, operandPos);
      if (!assignable(basic(K::Agent), r) &&
          !assignable(listOf(basic(K::Agent)), r)) {
        err(CheckErrorCode::TypeMismatch, n.recipients->loc,
            std::format("`for` takes an Agent or a list of agents, not {}",
                        typeName(r)));
      }
    }
    return basic(K::Unit);
  }

  Type node(const ETell &n, const Expr &e, Pos) {
    noteAction(e.loc, n.untell ? "untell" : "tell");
    const Type to = infer(*n.to, operandPos);
    if (!assignable(basic(K::Agent), to) &&
        !assignable(listOf(basic(K::Agent)), to)) {
      err(CheckErrorCode::TypeMismatch, n.to->loc,
          std::format("recipient must be an Agent or a list of agents, not {}",
                      typeName(to)));
    }
    checkTerm(n.fact);
    return basic(K::Unit);
  }

  Type node(const EBeliefChange &n, const Expr &e, Pos) {
    noteAction(e.loc, n.kind == EBeliefChange::Kind::Believe  ? "believe"
                      : n.kind == EBeliefChange::Kind::Forget ? "forget"
                                                              : "revise");
    checkTerm(n.fact);
    return basic(K::Unit);
  }

  Type node(const ESpawn &n, const Expr &e, Pos) {
    noteAction(e.loc, "spawn");
    const ContainerInfo *agent = nullptr;
    if (const auto it = out_.agents.find(n.agent); it != out_.agents.end()) {
      agent = &it->second;
    } else if (!opaqueTypes_.contains(n.agent)) {
      err(CheckErrorCode::UnknownAgent, e.loc,
          std::format("unknown agent `{}`", n.agent));
    }
    if (agent != nullptr) {
      checkArgs(agent->params, n.args, e.loc, n.agent);
    } else {
      for (const auto &a : n.args) {
        (void)infer(*a, operandPos);
      }
    }
    if (n.space) {
      expectType(basic(K::Space), infer(*n.space, operandPos), n.space->loc,
                 "`in`");
    }
    return basic(K::Agent);
  }

  const GoalInfo *findGoal(const std::string &name) const {
    if (cur_ != nullptr) {
      for (const auto &g : cur_->goals) {
        if (g.name == name) {
          return &g;
        }
      }
    }
    const auto it = out_.goals.find(name);
    return it == out_.goals.end() ? nullptr : &it->second;
  }

  void checkGoalCall(const GoalCall &g) {
    if (const GoalInfo *info = findGoal(g.name)) {
      checkArgs(info->params, g.args, g.loc, g.name);
      return;
    }
    if (!opaqueTypes_.contains(g.name)) {
      err(CheckErrorCode::UnknownGoal, g.loc,
          std::format("unknown goal `{}`", g.name));
    }
    for (const auto &a : g.args) {
      (void)infer(*a, operandPos);
    }
  }

  Type node(const EAdopt &n, const Expr &e, Pos) {
    noteAction(e.loc, "adopt");
    checkGoalCall(n.goal);
    return basic(K::Intention);
  }

  /// The `else` of `achieve`, `ask` and `await`: bounded when the form is,
  /// never a suspension position, with `cause` bound (§8.8).
  Type checkElse(const ExprPtr &orElse, Pos pos) {
    if (!orElse) {
      return basic(K::Never);
    }
    ScopeGuard scope(*this);
    declare("cause", basic(K::Str), SymKind::Local, orElse->loc);
    return infer(*orElse, Pos{pos.bounded, false, pos.used});
  }

  Type node(const EAchieve &n, const Expr &e, Pos pos) {
    requireSuspension(e.loc, pos, "achieve");
    checkGoalCall(n.goal);
    if (n.by) {
      expectType(basic(K::Agent), infer(*n.by, operandPos), n.by->loc, "`by`");
      expectType(basic(K::Duration), infer(*n.within, operandPos),
                 n.within->loc, "`within`");
    }
    (void)checkElse(n.orElse, Pos{pos.bounded, false, false});
    return basic(K::Unit);
  }

  Type node(const EAsk &n, const Expr &e, Pos pos) {
    requireSuspension(e.loc, pos, "ask");
    Type result      = basic(K::Any);
    const auto *call = std::get_if<ECall>(&n.question->node);
    const auto *f =
        call != nullptr ? std::get_if<EField>(&call->callee->node) : nullptr;
    if (f == nullptr) {
      err(CheckErrorCode::NotCallable, n.question->loc,
          "`ask` takes `agent.question(args)`");
    } else {
      expectType(basic(K::Agent), infer(*f->base, operandPos), f->base->loc,
                 "the asked agent");
      if (const auto q = out_.questions.find(f->name);
          q != out_.questions.end()) {
        checkArgs(q->second.params, call->args, n.question->loc, f->name);
        result = q->second.result;
      } else {
        if (!opaqueValues_.contains(f->name)) {
          err(CheckErrorCode::UnknownQuestion, n.question->loc,
              std::format("`{}` is not a declared question", f->name));
        }
        for (const auto &a : call->args) {
          (void)infer(*a, operandPos);
        }
      }
    }
    expectType(basic(K::Duration), infer(*n.within, operandPos), n.within->loc,
               "`within`");
    if (n.orElse) {
      const Type alt = checkElse(n.orElse, Pos{pos.bounded, false, true});
      expectType(result, alt, n.orElse->loc, "`else` of `ask`");
    }
    return result;
  }

  Type node(const EAwait &n, const Expr &e, Pos pos) {
    requireSuspension(e.loc, pos, "await");
    Type result = basic(K::Unit);
    if (n.term) {
      // Bound variables stay readable by the statements after the wait: the
      // resumed statement reads the copy captured at match time (§6.6).
      checkTerm(*n.term);
    }
    ScopeGuard scope(*this);
    if (!n.term) {
      Type t = basic(K::Any);
      if (isEventName(n.eventType)) {
        t = namedType(n.eventType);
      } else if (!opaqueTypes_.contains(n.eventType)) {
        err(CheckErrorCode::UnknownType, e.loc,
            std::format("unknown event `{}`", n.eventType));
      }
      declare(n.binder, t, SymKind::Binder, e.loc);
      result = t;
    }
    if (n.guard) {
      checkCondition(*n.guard);
    }
    expectType(basic(K::Duration), infer(*n.within, operandPos), n.within->loc,
               "`within`");
    (void)checkElse(n.orElse, Pos{pos.bounded, false, false});
    return result;
  }

  Type node(const EDrop &n, const Expr &e, Pos) {
    noteAction(e.loc, "drop");
    expectType(basic(K::Intention), infer(*n.target, operandPos), n.target->loc,
               "`drop`");
    return basic(K::Unit);
  }

  Type node(const EStop &n, const Expr &e, Pos) {
    noteAction(e.loc, "stop");
    expectType(basic(K::Agent), infer(*n.target, operandPos), n.target->loc,
               "`stop`");
    return basic(K::Unit);
  }

  Type node(const ETerminate &, const Expr &e, Pos pos) {
    noteAction(e.loc, "terminate");
    requireBounded(e.loc, pos, "terminate");
    return basic(K::Unit);
  }

  Type node(const EInstall &n, const Expr &e, Pos) {
    const char *what = n.kind == EInstall::Kind::Install     ? "install"
                       : n.kind == EInstall::Kind::Uninstall ? "uninstall"
                       : n.kind == EInstall::Kind::Mount     ? "mount"
                                                             : "unmount";
    noteAction(e.loc, what);
    checkInstallLike(n.kind, n.name, n.args, n.hasParens, e.loc);
    return basic(K::Unit);
  }

  Type node(const EReturn &n, const Expr &e, Pos pos) {
    requireBounded(e.loc, pos, "return");
    if (body_ != Body::Method && body_ != Body::Answer) {
      err(CheckErrorCode::ReturnOutsideMethod, e.loc,
          "`return` is accepted in methods, answers and `def`s; a handler or "
          "plan ends at the end of its block or by `fail` (§8.9)");
    }
    if (n.value) {
      const Type t = infer(*n.value, operandPos);
      if (returnType_) {
        expectType(*returnType_, t, n.value->loc, "returned value");
      }
    }
    return basic(K::Never);
  }

  Type node(const EYield &, const Expr &e, Pos pos) {
    requireSuspension(e.loc, pos, "yield");
    return basic(K::Unit);
  }

  Type node(const EConsume &, const Expr &e, Pos pos) {
    noteAction(e.loc, "consume");
    requireBounded(e.loc, pos, "consume");
    if (body_ != Body::Handler) {
      err(CheckErrorCode::ConsumeOutsideHandler, e.loc,
          "`consume` stops the event's remaining handlers and belongs in a "
          "handler");
    }
    return basic(K::Unit);
  }

  Type node(const EFail &, const Expr &, Pos) { return basic(K::Never); }

  // -- purity of defs -------------------------------------------------------

  bool pureKey(const std::string &key, Purity mode,
               std::unordered_set<std::string> &visiting) {
    if (key == "?unknown") {
      return false;
    }
    if (!visiting.insert(key).second) {
      return true; // a cycle adds no impurity of its own
    }
    bool ok = true;
    if (key.starts_with("cap:")) {
      // A capacity method is as pure as every skill that implements it.
      const auto dot  = key.rfind('.');
      const auto cap  = key.substr(4, dot - 4);
      const auto meth = key.substr(dot + 1);
      for (const auto &[sname, skill] : out_.skills) {
        if (!implementsCapacity(skill, cap)) {
          continue;
        }
        if (skill.native || !pureKey(sname + "::" + meth, mode, visiting)) {
          ok = false;
          break;
        }
      }
    } else {
      const auto it = facts_.find(key);
      if (it == facts_.end()) {
        ok = false;
      } else {
        const DefFacts &f = it->second;
        ok                = !f.action && !f.effect && !f.unknownCall &&
             (mode == Purity::Strict ? !f.assign : !f.fieldWrite);
        for (const auto &c : f.callees) {
          if (!ok) {
            break;
          }
          ok = pureKey(c, mode, visiting);
        }
      }
    }
    visiting.erase(key);
    return ok;
  }

  void verifyPurity() {
    for (const auto &r : requirements_) {
      std::unordered_set<std::string> visiting;
      if (pureKey(r.key, r.mode, visiting)) {
        continue;
      }
      if (r.mode == Purity::Strict) {
        err(CheckErrorCode::ImpureCondition, r.loc,
            "a condition may call only `def`s that contain no action, "
            "assignment or effect, directly or through what they call (§8.2)");
      } else {
        err(CheckErrorCode::ImpureAnswer, r.loc,
            "an answer may call only read-only `def`s, transitively (§7.1)");
      }
    }
  }
};

} // namespace

std::expected<CheckedUnit, std::vector<CheckError>> check(const Unit &unit) {
  return Checker(unit).run();
}

} // namespace xanadu::vagent
