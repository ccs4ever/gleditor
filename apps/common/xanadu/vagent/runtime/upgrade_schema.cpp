#include "common/xanadu/vagent/runtime/upgrade_schema.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "common/xanadu/provenance.hpp"

namespace xanadu::vagent::runtime {
namespace {

class Canonical {
public:
  void tag(const std::string_view value) { string(value); }
  void string(const std::string_view value) {
    data_ += std::format("{}:", value.size());
    data_.append(value);
  }
  void integer(const std::int64_t value) {
    tag("i64");
    string(std::to_string(value));
  }
  void unsignedInteger(const std::uint64_t value) {
    tag("u64");
    string(std::to_string(value));
  }
  void boolean(const bool value) { tag(value ? "true" : "false"); }
  void count(const std::size_t value) {
    tag("count");
    string(std::to_string(value));
  }
  void optionalString(const std::optional<std::string> &value) {
    tag(value ? "some" : "none");
    if (value) string(*value);
  }
  void optionalExpr(const ExprPtr &expr) {
    tag(expr ? "some-expr" : "none-expr");
    if (expr) expression(*expr);
  }
  void typeExpr(const TypeExpr &type) {
    switch (type.kind) {
    case TypeExpr::Kind::Named:
      tag("type.named");
      string(type.name);
      count(type.args.size());
      for (const auto &arg : type.args) typeExpr(arg);
      return;
    case TypeExpr::Kind::List:
      tag("type.list");
      count(type.args.size());
      for (const auto &arg : type.args) typeExpr(arg);
      return;
    case TypeExpr::Kind::Unit:
      tag("type.unit");
      return;
    }
    unreachable();
  }
  void parameter(const Param &param) {
    tag("parameter");
    string(param.name);
    typeExpr(param.type);
  }
  void term(const Term &value) {
    tag("term");
    string(value.name);
    count(value.args.size());
    for (const auto &arg : value.args) {
      if (arg.logicVar) {
        tag("logic-var");
        string(*arg.logicVar);
      } else {
        tag("term-expr");
        optionalExpr(arg.expr);
      }
    }
  }
  void goalCall(const GoalCall &value) {
    tag("goal-call");
    string(value.name);
    count(value.args.size());
    for (const auto &arg : value.args) optionalExpr(arg);
  }
  void expression(const Expr &expr) {
    std::visit([this](const auto &node) { expressionNode(node); }, expr.node);
  }
  [[nodiscard]] const std::string &data() const { return data_; }

private:
  [[noreturn]] static void unreachable() {
    throw std::logic_error("invalid AST enum");
  }
  template <class> static constexpr bool kDependentFalse = false;
  void expressionNode(const ELiteral &n) {
    tag("ELiteral");
    switch (n.kind) {
    case ELiteral::Kind::Int:
      tag("int");
      integer(n.intValue);
      break;
    case ELiteral::Kind::Float:
      tag("float64");
      unsignedInteger(std::bit_cast<std::uint64_t>(n.floatValue));
      break;
    case ELiteral::Kind::Str:
      tag("string");
      string(n.text);
      break;
    case ELiteral::Kind::Bool:
      tag("bool");
      boolean(n.boolValue);
      break;
    case ELiteral::Kind::Duration:
      tag("duration-ms");
      integer(n.durationMs);
      break;
    case ELiteral::Kind::Unit:
      tag("unit");
      break;
    case ELiteral::Kind::None:
      tag("none");
      break;
    }
  }
  void expressionNode(const EName &n) {
    tag("EName");
    string(n.name);
  }
  void expressionNode(const ESpecial &n) {
    tag("ESpecial");
    switch (n.kind) {
    case ESpecial::Kind::Self:
      tag("self");
      break;
    case ESpecial::Kind::Parent:
      tag("parent");
      break;
    case ESpecial::Kind::Inner:
      tag("inner");
      break;
    case ESpecial::Kind::Outer:
      tag("outer");
      break;
    case ESpecial::Kind::Occurrence:
      tag("occurrence");
      break;
    }
  }
  void expressionNode(const EUnary &n) {
    tag("EUnary");
    switch (n.op) {
    case EUnary::Op::Neg:
      tag("neg");
      break;
    case EUnary::Op::Generate:
      tag("generate");
      break;
    case EUnary::Op::Size:
      tag("size");
      break;
    case EUnary::Op::Not:
      tag("not");
      break;
    }
    optionalExpr(n.operand);
  }
  void expressionNode(const EBinary &n) {
    tag("EBinary");
    switch (n.op) {
    case EBinary::Op::Mul:
      tag("mul");
      break;
    case EBinary::Op::Div:
      tag("div");
      break;
    case EBinary::Op::Mod:
      tag("mod");
      break;
    case EBinary::Op::Add:
      tag("add");
      break;
    case EBinary::Op::Sub:
      tag("sub");
      break;
    case EBinary::Op::Concat:
      tag("concat");
      break;
    case EBinary::Op::Lt:
      tag("lt");
      break;
    case EBinary::Op::Le:
      tag("le");
      break;
    case EBinary::Op::Gt:
      tag("gt");
      break;
    case EBinary::Op::Ge:
      tag("ge");
      break;
    case EBinary::Op::Eq:
      tag("eq");
      break;
    case EBinary::Op::Ne:
      tag("ne");
      break;
    case EBinary::Op::Alt:
      tag("alt");
      break;
    case EBinary::Op::Scan:
      tag("scan");
      break;
    case EBinary::Op::Assign:
      tag("assign");
      break;
    case EBinary::Op::RevAssign:
      tag("reverse-assign");
      break;
    case EBinary::Op::And:
      tag("and");
      break;
    }
    optionalExpr(n.left);
    optionalExpr(n.right);
  }
  void expressionNode(const ERange &n) {
    tag("ERange");
    optionalExpr(n.lo);
    optionalExpr(n.hi);
    optionalExpr(n.by);
  }
  void expressionNode(const ECall &n) {
    tag("ECall");
    optionalExpr(n.callee);
    count(n.args.size());
    for (const auto &arg : n.args) optionalExpr(arg);
  }
  void expressionNode(const EIndex &n) {
    tag("EIndex");
    optionalExpr(n.base);
    optionalExpr(n.index);
  }
  void expressionNode(const EField &n) {
    tag("EField");
    optionalExpr(n.base);
    string(n.name);
  }
  void expressionNode(const EList &n) {
    tag("EList");
    count(n.elems.size());
    for (const auto &item : n.elems) optionalExpr(item);
  }
  void expressionNode(const ERecord &n) {
    tag("ERecord");
    string(n.type);
    count(n.fields.size());
    for (const auto &field : n.fields) {
      string(field.name);
      optionalExpr(field.value);
    }
  }
  void expressionNode(const EBlock &n) {
    tag("EBlock");
    count(n.stmts.size());
    for (const auto &stmt : n.stmts) {
      tag(stmt.isLet ? "let" : "expr-stmt");
      string(stmt.name);
      tag(stmt.type ? "typed" : "untyped");
      if (stmt.type) typeExpr(*stmt.type);
      optionalExpr(stmt.value);
    }
  }
  void expressionNode(const EIf &n) {
    tag("EIf");
    optionalExpr(n.cond);
    optionalExpr(n.thenBranch);
    optionalExpr(n.elseBranch);
  }
  void expressionNode(const EWhile &n) {
    tag("EWhile");
    optionalExpr(n.cond);
    optionalExpr(n.body);
  }
  void expressionNode(const EFor &n) {
    tag("EFor");
    string(n.binder);
    optionalExpr(n.iterable);
    tag(n.believesTerm ? "belief-loop" : "ordinary-loop");
    if (n.believesTerm) term(*n.believesTerm);
    optionalExpr(n.body);
  }
  void expressionNode(const EBelieves &n) {
    tag("EBelieves");
    term(n.term);
    optionalExpr(n.from);
  }
  void expressionNode(const EFindall &n) {
    tag("EFindall");
    if (n.templ.logicVar) {
      tag("logic-var");
      string(*n.templ.logicVar);
    } else
      optionalExpr(n.templ.expr);
    term(n.goal);
  }
  void expressionNode(const EEmit &n) {
    tag("EEmit");
    optionalExpr(n.event);
    optionalExpr(n.space);
    optionalExpr(n.recipients);
  }
  void expressionNode(const ETell &n) {
    tag("ETell");
    boolean(n.untell);
    optionalExpr(n.to);
    term(n.fact);
  }
  void expressionNode(const EBeliefChange &n) {
    tag("EBeliefChange");
    switch (n.kind) {
    case EBeliefChange::Kind::Believe:
      tag("believe");
      break;
    case EBeliefChange::Kind::Forget:
      tag("forget");
      break;
    case EBeliefChange::Kind::Revise:
      tag("revise");
      break;
    }
    term(n.fact);
  }
  void expressionNode(const ESpawn &n) {
    tag("ESpawn");
    string(n.agent);
    count(n.args.size());
    for (const auto &a : n.args) optionalExpr(a);
    optionalExpr(n.space);
  }
  void expressionNode(const EAdopt &n) {
    tag("EAdopt");
    goalCall(n.goal);
  }
  void expressionNode(const EAchieve &n) {
    tag("EAchieve");
    goalCall(n.goal);
    optionalExpr(n.by);
    optionalExpr(n.within);
    optionalExpr(n.orElse);
  }
  void expressionNode(const EAsk &n) {
    tag("EAsk");
    optionalExpr(n.question);
    optionalExpr(n.within);
    optionalExpr(n.orElse);
  }
  void expressionNode(const EAwait &n) {
    tag("EAwait");
    tag(n.term ? "term" : "event");
    if (n.term) term(*n.term);
    string(n.eventType);
    string(n.binder);
    optionalExpr(n.guard);
    optionalExpr(n.within);
    optionalExpr(n.orElse);
  }
  void expressionNode(const EDrop &n) {
    tag("EDrop");
    optionalExpr(n.target);
  }
  void expressionNode(const EStop &n) {
    tag("EStop");
    optionalExpr(n.target);
    boolean(n.keepChildren);
  }
  void expressionNode(const ETerminate &n) {
    tag("ETerminate");
    boolean(n.keepChildren);
  }
  void expressionNode(const EInstall &n) {
    tag("EInstall");
    switch (n.kind) {
    case EInstall::Kind::Install:
      tag("install");
      break;
    case EInstall::Kind::Uninstall:
      tag("uninstall");
      break;
    case EInstall::Kind::Mount:
      tag("mount");
      break;
    case EInstall::Kind::Unmount:
      tag("unmount");
      break;
    }
    string(n.name);
    boolean(n.hasParens);
    count(n.args.size());
    for (const auto &a : n.args) optionalExpr(a);
  }
  void expressionNode(const EReturn &n) {
    tag("EReturn");
    optionalExpr(n.value);
  }
  void expressionNode(const EYield &) { tag("EYield"); }
  void expressionNode(const EConsume &) { tag("EConsume"); }
  void expressionNode(const EFail &) { tag("EFail"); }
  template <class T> void expressionNode(const T &) {
    static_assert(kDependentFalse<T>,
                  "add canonical encoding for every runtime Expr node");
  }
  std::string data_;
};

UpgradeField field(const FieldInfo &value) { return {value.name, value.type}; }
UpgradeParameter param(const ParamInfo &value, const bool key = false) {
  return {value.name, value.type, key};
}

UpgradePlan planSchema(const PlanInfo &info, const PlanDecl &decl,
                       const std::string_view owner,
                       const std::vector<GoalInfo> &goals,
                       const ContainerInfo *context) {
  Canonical canonical;
  canonical.tag("vagent-plan-ast-1");
  canonical.string(owner);
  canonical.string(decl.name);
  canonical.string(decl.goal);
  canonical.count(decl.params.size());
  for (const auto &binding : decl.params) canonical.string(binding);
  const auto goal =
      std::find_if(goals.begin(), goals.end(), [&](const GoalInfo &item) {
        return item.name == info.goal;
      });
  if (goal != goals.end()) {
    canonical.tag("resolved-goal");
    canonical.string(goal->name);
    canonical.count(goal->params.size());
    for (const auto &parameter : goal->params) {
      canonical.string(parameter.name);
      canonical.string(typeName(parameter.type));
    }
    for (const auto &binding : info.params) {
      const auto parameter = std::find_if(
          goal->params.begin(), goal->params.end(),
          [&](const ParamInfo &item) { return item.name == binding; });
      canonical.tag("captured-binding");
      canonical.string(binding);
      if (parameter != goal->params.end()) {
        canonical.string(typeName(parameter->type));
      } else
        canonical.tag("invalid-checked-binding");
    }
  } else
    canonical.tag("unresolved-goal-impossible-after-check");
  if (context) {
    canonical.tag("owner-context");
    canonical.string(context->name);
    canonical.count(context->params.size());
    for (const auto &parameter : context->params) {
      canonical.string(parameter.name);
      canonical.string(typeName(parameter.type));
    }
    canonical.count(context->fields.size());
    for (const auto &field : context->fields) {
      canonical.string(field.name);
      canonical.string(typeName(field.type));
    }
  }
  canonical.optionalExpr(decl.where);
  canonical.optionalExpr(decl.body);
  const auto bytes = std::string("vagent-plan-ast-1\0", 18) + canonical.data();
  return {owner.empty() ? decl.name : std::string(owner) + "." + decl.name,
          info.goal, info.params, "vagent-plan-ast-1",
          xanadu::sha256Hex(bytes)};
}

void findPlans(const CheckedUnit &checked, UpgradeSchema &schema) {
  if (!checked.unit) return;
  for (const auto &top : checked.unit->decls) {
    if (const auto *plan = std::get_if<PlanDecl>(&top)) {
      if (const auto it = checked.plans.find(plan->name);
          it != checked.plans.end()) {
        std::vector<GoalInfo> goals;
        for (const auto &[goalName, goal] : checked.goals) {
          (void)goalName;
          goals.push_back(goal);
        }
        auto value = planSchema(it->second, *plan, {}, goals, nullptr);
        schema.plans.insert_or_assign(value.qualifiedName, std::move(value));
      }
    }
    auto containerPlans = [&](const auto &decl, const auto &map, auto &dest) {
      const auto *container = std::get_if<std::decay_t<decltype(decl)>>(&top);
      if (!container) return;
      const auto checkedContainer = map.find(container->name);
      if (checkedContainer == map.end()) return;
      for (const auto &member : container->members) {
        const auto *plan = std::get_if<PlanDecl>(&member);
        if (!plan) continue;
        // The checker records only top-level plans; a member plan's header is
        // its declaration, so the schema must not depend on a checked entry.
        const auto info = std::find_if(
            checkedContainer->second.plans.begin(),
            checkedContainer->second.plans.end(),
            [&](const PlanInfo &p) { return p.name == plan->name; });
        const PlanInfo declared{plan->name, plan->goal, plan->params,
                                plan->loc};
        {
          std::vector<GoalInfo> goals = checkedContainer->second.goals;
          for (const auto &[goalName, goal] : checked.goals) {
            (void)goalName;
            if (std::none_of(goals.begin(), goals.end(),
                             [&](const GoalInfo &item) {
                               return item.name == goal.name;
                             }))
              goals.push_back(goal);
          }
          auto value = planSchema(
              info != checkedContainer->second.plans.end() ? *info : declared,
              *plan, container->name, goals, &checkedContainer->second);
          dest.at(container->name)
              .plans.insert_or_assign(plan->name, std::move(value));
        }
      }
    };
    std::visit(
        [&](const auto &decl) {
          using T = std::decay_t<decltype(decl)>;
          if constexpr (std::is_same_v<T, AgentDecl>)
            containerPlans(decl, checked.agents, schema.agents);
          else if constexpr (std::is_same_v<T, SkillDecl>)
            containerPlans(decl, checked.skills, schema.skills);
          else if constexpr (std::is_same_v<T, BehaviorDecl>)
            containerPlans(decl, checked.behaviors, schema.behaviors);
        },
        top);
  }
}

void compareFields(const std::string_view owner, const std::string_view symbol,
                   const std::vector<UpgradeField> &oldFields,
                   const std::vector<UpgradeField> &newFields,
                   std::vector<UpgradeChange> &out) {
  std::map<std::string_view, const UpgradeField *, std::less<>> oldMap, newMap;
  for (const auto &f : oldFields) oldMap.emplace(f.name, &f);
  for (const auto &f : newFields) newMap.emplace(f.name, &f);
  for (const auto &[name, oldField] : oldMap) {
    const auto next = newMap.find(name);
    if (next == newMap.end())
      out.push_back({UpgradeChangeKind::RemovedField, std::string(owner),
                     std::string(symbol), std::string(name), oldField->type});
    else if (oldField->type != next->second->type)
      out.push_back({UpgradeChangeKind::ChangedFieldType, std::string(owner),
                     std::string(symbol), std::string(name), oldField->type,
                     next->second->type});
  }
  for (const auto &[name, newField] : newMap)
    if (!oldMap.contains(name))
      out.push_back({UpgradeChangeKind::AddedField,
                     std::string(owner),
                     std::string(symbol),
                     std::string(name),
                     {},
                     newField->type});
}

void comparePlans(const auto &oldPlans, const auto &newPlans,
                  const std::string_view owner,
                  std::vector<UpgradeChange> &out) {
  for (const auto &[name, oldPlan] : oldPlans) {
    const auto next = newPlans.find(name);
    if (next == newPlans.end()) {
      out.push_back({UpgradeChangeKind::RemovedPlan, std::string(owner), name});
      continue;
    }
    if (oldPlan.astEncodingVersion != next->second.astEncodingVersion)
      out.push_back({UpgradeChangeKind::PlanHashVersion,
                     std::string(owner),
                     name,
                     {},
                     {},
                     {},
                     oldPlan.astEncodingVersion,
                     next->second.astEncodingVersion});
    else if (oldPlan.astHash != next->second.astHash)
      out.push_back({UpgradeChangeKind::ChangedPlanHash,
                     std::string(owner),
                     name,
                     {},
                     {},
                     {},
                     oldPlan.astHash,
                     next->second.astHash});
  }
}

} // namespace

std::expected<UpgradeSchema, UpgradeSchemaError>
makeUpgradeSchema(const CheckedUnit &checked) {
  UpgradeSchema result;
  auto containers = [](const auto &source, auto &destination) {
    for (const auto &[name, input] : source) {
      UpgradeContainer output;
      output.kind = input.kind;
      for (const auto &p : input.params) output.parameters.push_back(param(p));
      for (const auto &f : input.fields) output.fields.push_back(field(f));
      destination.emplace(name, std::move(output));
    }
  };
  containers(checked.agents, result.agents);
  containers(checked.skills, result.skills);
  containers(checked.behaviors, result.behaviors);
  for (const auto &[name, predicate] : checked.predicates) {
    UpgradePredicate output;
    for (const auto &p : predicate.params)
      output.parameters.push_back(param({p.name, p.type}, p.key));
    if (predicate.policy)
      output.epistemicPolicy = EpistemicPolicySchema{
          *predicate.policy, policyHash(*predicate.policy)};
    result.predicates.emplace(name, std::move(output));
  }
  for (const auto &[name, record] : checked.records)
    for (const auto &f : record.fields)
      result.records[name].push_back(field(f));
  for (const auto &[name, event] : checked.events)
    if (!event.builtin)
      for (const auto &f : event.fields)
        result.events[name].push_back(field(f));
  findPlans(checked, result);
  return result;
}

std::vector<UpgradeChange>
compareUpgradeSchemas(const UpgradeSchema &oldSchema,
                      const UpgradeSchema &newSchema) {
  std::vector<UpgradeChange> out;
  auto compareContainerSet = [&](const auto &oldSet, const auto &newSet,
                                 const std::string_view kind) {
    for (const auto &[name, oldValue] : oldSet) {
      const auto next = newSet.find(name);
      if (next == newSet.end()) continue;
      const auto &newValue = next->second;
      bool matching = oldValue.parameters.size() == newValue.parameters.size();
      if (matching)
        for (std::size_t i = 0; i < oldValue.parameters.size(); ++i)
          if (oldValue.parameters[i].name != newValue.parameters[i].name ||
              oldValue.parameters[i].type != newValue.parameters[i].type)
            matching = false;
      if (!matching)
        out.push_back(
            {UpgradeChangeKind::ContainerParameters, std::string(kind), name});
      compareFields(kind, name, oldValue.fields, next->second.fields, out);
      comparePlans(oldValue.plans, next->second.plans, name, out);
    }
  };
  compareContainerSet(oldSchema.agents, newSchema.agents, "agent");
  compareContainerSet(oldSchema.skills, newSchema.skills, "skill");
  compareContainerSet(oldSchema.behaviors, newSchema.behaviors, "behavior");
  for (const auto &[name, oldFields] : oldSchema.records) {
    const auto next = newSchema.records.find(name);
    if (next == newSchema.records.end())
      out.push_back({UpgradeChangeKind::RemovedField, "record", name});
    else
      compareFields("record", name, oldFields, next->second, out);
  }
  for (const auto &[name, oldFields] : oldSchema.events)
    if (const auto next = newSchema.events.find(name);
        next != newSchema.events.end())
      compareFields("event", name, oldFields, next->second, out);
  for (const auto &[name, oldPred] : oldSchema.predicates) {
    const auto next = newSchema.predicates.find(name);
    if (next == newSchema.predicates.end()) {
      out.push_back({UpgradeChangeKind::RemovedPredicate, "predicate", name});
      continue;
    }
    if (oldPred.parameters.size() != next->second.parameters.size()) {
      out.push_back({UpgradeChangeKind::PredicateArity, "predicate", name});
      continue;
    }
    std::vector<std::size_t> oldKeys, newKeys;
    for (std::size_t i = 0; i < oldPred.parameters.size(); ++i) {
      if (oldPred.parameters[i].key) oldKeys.push_back(i);
      if (next->second.parameters[i].key) newKeys.push_back(i);
      if (oldPred.parameters[i].type != next->second.parameters[i].type)
        out.push_back({UpgradeChangeKind::PredicateParameterType, "predicate",
                       name, oldPred.parameters[i].name,
                       oldPred.parameters[i].type,
                       next->second.parameters[i].type});
    }
    if (oldKeys != newKeys)
      out.push_back(
          {UpgradeChangeKind::PredicateKeyPositions, "predicate", name});
    const auto hashOf = [](const UpgradePredicate &p) {
      return p.epistemicPolicy ? p.epistemicPolicy->hash : std::string();
    };
    if (hashOf(oldPred) != hashOf(next->second)) {
      UpgradeChange change{UpgradeChangeKind::EpistemicPolicy, "predicate",
                           name};
      change.oldHash = hashOf(oldPred);
      change.newHash = hashOf(next->second);
      out.push_back(std::move(change));
    }
  }
  comparePlans(oldSchema.plans, newSchema.plans, {}, out);
  return out;
}

bool upgradeChangeRequiresRefusal(const UpgradeChange &change) noexcept {
  if (change.owner == "record") return true;
  switch (change.kind) {
  case UpgradeChangeKind::ContainerParameters:
  case UpgradeChangeKind::ChangedFieldType:
  case UpgradeChangeKind::PredicateArity:
  case UpgradeChangeKind::PredicateParameterType:
  case UpgradeChangeKind::PredicateKeyPositions:
  case UpgradeChangeKind::EpistemicPolicy:
    return true;
  default:
    return false;
  }
}

std::string_view upgradeChangeName(const UpgradeChangeKind kind) noexcept {
  switch (kind) {
  case UpgradeChangeKind::ContainerParameters:
    return "ContainerParameters";
  case UpgradeChangeKind::AddedField:
    return "AddedField";
  case UpgradeChangeKind::RemovedField:
    return "RemovedField";
  case UpgradeChangeKind::ChangedFieldType:
    return "ChangedFieldType";
  case UpgradeChangeKind::PredicateArity:
    return "PredicateArity";
  case UpgradeChangeKind::PredicateParameterType:
    return "PredicateParameterType";
  case UpgradeChangeKind::PredicateKeyPositions:
    return "PredicateKeyPositions";
  case UpgradeChangeKind::RemovedPredicate:
    return "RemovedPredicate";
  case UpgradeChangeKind::RemovedPlan:
    return "RemovedPlan";
  case UpgradeChangeKind::ChangedPlanHash:
    return "ChangedPlanHash";
  case UpgradeChangeKind::PlanHashVersion:
    return "PlanHashVersion";
  case UpgradeChangeKind::EpistemicPolicy:
    return "EpistemicPolicy";
  }
  return "Unknown";
}

} // namespace xanadu::vagent::runtime
