/**
 * @file checker.hpp
 * @brief vagent checker: names, types, truth and failure, positions, purity
 * and the static restrictions of design §8.9 that the parser leaves alone.
 *
 * Refuses by returning every error found, never by throwing. A checked unit
 * carries resolved symbol tables as plain structs so the compiler can use
 * them without walking the AST for declarations again; it points at the
 * `Unit` it was made from, which must outlive it.
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_CHECKER_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_CHECKER_HPP

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/xanadu/vagent/runtime/ast.hpp"
#include "common/xanadu/vagent/runtime/epistemic_policy.hpp"
#include "common/xanadu/vagent/runtime/token.hpp"

namespace xanadu::vagent::runtime {

enum class CheckErrorCode : std::uint8_t {
  // Names (§8.8)
  DuplicateName,
  UnknownName,
  UnknownType,
  UnknownField,
  UnknownPredicate,
  UnknownQuestion,
  UnknownGoal,
  UnknownCapacity,
  UnknownSkill,
  UnknownBehavior,
  UnknownAgent,
  UnknownFunction,
  AmbiguousCall,
  ReservedName,   ///< a `$` name in user code
  ReservedField,  ///< declaring, setting or assigning an implicit event field
  CaseConvention, ///< PascalCase vs snake_case (§8.1)
  OccurrenceOutsideHandler,
  InheritanceCycle,
  InvalidExtends,
  // Types (§8.7)
  TypeMismatch,
  ArityMismatch,
  MissingField,
  DuplicateField,
  NotAnEvent,
  NotCallable,
  NotAPlace,
  IfWithoutElse, ///< `if c then e` in a value position with a non-`()` `e`
  // Truth and purity (§8.2)
  BoolCondition, ///< "compare with `== true`"
  ImpureCondition,
  ImpureAnswer,
  // Positions (§8.3, §6.6)
  BoundedOnly, ///< `:=`, `consume`, `terminate`, `return` in a speculative
               ///< position
  SuspensionOutsidePlan,
  SuspensionPosition,
  SuspensionInBelievesLoop,
  EffectInMethod,
  EffectPosition,
  ReturnOutsideMethod,
  ConsumeOutsideHandler,
  // Declarations (§8.9 notes)
  EffectWithoutWithin,
  EffectInSkill,   ///< an `effect` method with a body
  NonNativeEffect, ///< a vagent skill implements a capacity with effects
  MissingImplementation,
  SignatureMismatch,
  PlanParamNotInGoal,
  // Epistemic arbitration (P19.4)
  InvalidEpistemicPolicy,
  MissingClusterTolerance,
  EpistemicIngressOnly, ///< a program-level write or alias filter on an
                        ///< arbitrated predicate, which only ingestion writes
  // Warnings (CheckedUnit::warnings only)
  AlternationOfLiterals,
};

[[nodiscard]] std::string_view checkErrorName(CheckErrorCode code) noexcept;

struct CheckError {
  CheckErrorCode code{CheckErrorCode::TypeMismatch};
  std::string message;
  SourceLocation loc{};
};

/// "line:col: Code: message"
[[nodiscard]] std::string formatCheckError(const CheckError &error);

// -- Types ------------------------------------------------------------------

struct Type {
  enum class Kind : std::uint8_t {
    Any,   ///< unchecked: an imported name, or the result of an error
    Never, ///< `fail` and `return` match any type
    Int,
    Float,
    Bool,
    Str,
    Unit,
    Duration,
    Time,
    Agent,
    Space,
    Intention,
    Timer,
    Event, ///< the supertype of all events
    Opt,
    List,
    Map,
    Named ///< a declared event or record
  } kind{Kind::Any};
  std::string name;       ///< Named only
  std::vector<Type> args; ///< Opt, List: the element. Map: key, value.

  [[nodiscard]] bool operator==(const Type &) const = default;
};

[[nodiscard]] std::string typeName(const Type &type);

// -- Symbol tables ----------------------------------------------------------

struct FieldInfo {
  std::string name;
  Type type;
  SourceLocation loc{};
};

struct ParamInfo {
  std::string name;
  Type type;
};

struct EventInfo {
  std::string name;
  std::optional<std::string> extends;
  /// Every field a literal must supply: supertypes' first, then the event's
  /// own. The implicit fields of §5.5 are not here; see `implicitFields`.
  std::vector<FieldInfo> fields;
  /// The flattened supertype names, nearest first, as `d.extends` holds them.
  std::vector<std::string> supertypes;
  bool builtin{false};
  SourceLocation loc{};
};

struct RecordInfo {
  std::string name;
  std::vector<FieldInfo> fields;
  SourceLocation loc{};
};

struct PredicateParamInfo {
  std::string name;
  Type type;
  bool key{false};
};
struct PredicateInfo {
  std::string name;
  std::vector<PredicateParamInfo> params;
  /// Present for an `arbitrated by` predicate, whose clauses only checked
  /// ingestion writes.
  std::optional<ArbitrationPolicy> policy;
  SourceLocation loc{};
};

struct QuestionInfo {
  std::string name;
  std::vector<ParamInfo> params;
  Type result;
  SourceLocation loc{};
};

struct MethodInfo {
  std::string name;
  std::string owner; ///< capacity, skill, behavior or agent name
  std::vector<ParamInfo> params;
  Type result{Type::Kind::Unit, {}, {}};
  bool hasResult{false};
  bool isEffect{false};
  bool preempts{false};
  std::optional<std::int64_t> withinMs;
  SourceLocation loc{};
};

struct CapacityInfo {
  std::string name;
  std::vector<std::string> extends;
  std::vector<MethodInfo> methods; ///< declared on this capacity only
  bool builtin{false};
  SourceLocation loc{};
};

struct GoalInfo {
  std::string name;
  GoalDecl::Kind kind{GoalDecl::Kind::Perform};
  std::vector<ParamInfo> params;
  SourceLocation loc{};
};

struct PlanInfo {
  std::string name;
  std::string goal;
  /// The goal parameters the body sees: all of them when the header has no
  /// list, else the listed ones.
  std::vector<std::string> params;
  SourceLocation loc{};
};

/// A skill, behavior or agent.
struct ContainerInfo {
  enum class Kind : std::uint8_t { Skill, Behavior, Agent } kind{Kind::Agent};
  std::string name;
  bool native{false}; ///< skills only
  std::vector<ParamInfo> params;
  std::vector<FieldInfo> fields;    ///< `var` members
  std::vector<MethodInfo> methods;  ///< `def` members
  std::vector<std::string> answers; ///< questions this container answers
  std::vector<GoalInfo> goals;      ///< goals declared inside it
  std::vector<PlanInfo> plans;
  std::vector<std::string> uses;       ///< capacities from `uses`
  std::vector<std::string> implements; ///< skills only, as written
  SourceLocation loc{};
};

struct CheckedUnit {
  const Unit *unit{nullptr};
  std::unordered_map<std::string, EventInfo> events; ///< user and built-in
  std::unordered_map<std::string, RecordInfo> records;
  std::unordered_map<std::string, PredicateInfo> predicates;
  std::unordered_map<std::string, QuestionInfo> questions;
  std::unordered_map<std::string, CapacityInfo> capacities; ///< incl. built-in
  std::unordered_map<std::string, GoalInfo> goals;          ///< top-level
  std::unordered_map<std::string, PlanInfo> plans;          ///< top-level
  std::unordered_map<std::string, ContainerInfo> skills;
  std::unordered_map<std::string, ContainerInfo> behaviors;
  std::unordered_map<std::string, ContainerInfo> agents;
  /// Names brought in by `import` that this unit does not declare. Their
  /// declarations live in another module; uses of them are not type-checked.
  std::vector<std::string> opaqueImports;
  std::vector<CheckError> warnings;
};

/// The fields every event has and no declaration may give (§5.5).
[[nodiscard]] const std::vector<FieldInfo> &implicitEventFields();

/// All of `capacity`'s methods, its extended capacities' included.
[[nodiscard]] std::vector<const MethodInfo *>
allMethods(const CheckedUnit &unit, std::string_view capacity);

/// Check `unit`. On success the symbol tables; on failure every error found,
/// ordered by source position.
[[nodiscard]] std::expected<CheckedUnit, std::vector<CheckError>>
check(const Unit &unit);

} // namespace xanadu::vagent::runtime

#endif // COMMON_XANADU_VAGENT_RUNTIME_CHECKER_HPP
