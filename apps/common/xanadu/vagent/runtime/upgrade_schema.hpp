#ifndef COMMON_XANADU_VAGENT_RUNTIME_UPGRADE_SCHEMA_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_UPGRADE_SCHEMA_HPP

#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vagent/runtime/checker.hpp"
#include "common/xanadu/vagent/runtime/epistemic_policy.hpp"

namespace xanadu::vagent::runtime {

struct UpgradeField {
  std::string name;
  Type type;
};

struct UpgradeParameter {
  std::string name;
  Type type;
  bool key{false};
};

struct UpgradePlan {
  std::string qualifiedName;
  std::string goal;
  std::vector<std::string> bindings;
  std::string astEncodingVersion{"vagent-plan-ast-1"};
  std::string astHash;
};

// Disengaged means the predicate has no `arbitrated by` policy; an invalid
// clause is a checker error and never collapses to absent.
struct EpistemicPolicySchema {
  ArbitrationPolicy policy;
  std::string hash; ///< `policyHash(policy)`, compared on upgrade and restore
};

struct UpgradePredicate {
  std::vector<UpgradeParameter> parameters;
  std::optional<EpistemicPolicySchema> epistemicPolicy;
};

struct UpgradeContainer {
  ContainerInfo::Kind kind{ContainerInfo::Kind::Agent};
  std::vector<UpgradeParameter> parameters;
  std::vector<UpgradeField> fields;
  std::map<std::string, UpgradePlan, std::less<>> plans;
};

struct UpgradeSchema {
  std::map<std::string, UpgradeContainer, std::less<>> agents;
  std::map<std::string, UpgradeContainer, std::less<>> skills;
  std::map<std::string, UpgradeContainer, std::less<>> behaviors;
  std::map<std::string, UpgradePredicate, std::less<>> predicates;
  std::map<std::string, std::vector<UpgradeField>, std::less<>> records;
  std::map<std::string, std::vector<UpgradeField>, std::less<>> events;
  std::map<std::string, UpgradePlan, std::less<>> plans;
};

enum class UpgradeChangeKind {
  ContainerParameters,
  AddedField,
  RemovedField,
  ChangedFieldType,
  PredicateArity,
  PredicateParameterType,
  PredicateKeyPositions,
  RemovedPredicate,
  RemovedPlan,
  ChangedPlanHash,
  PlanHashVersion,
  /// A predicate gained, lost or changed its arbitration policy. Records
  /// scored under one policy must not be reinterpreted under another, so
  /// this refuses until the typed epistemic translator exists.
  EpistemicPolicy,
};

struct UpgradeChange {
  UpgradeChangeKind kind{};
  std::string owner;
  std::string symbol;
  std::string member;
  std::optional<Type> oldType;
  std::optional<Type> newType;
  std::string oldHash;
  std::string newHash;
};

struct UpgradeSchemaError {
  std::string message;
};

/// Build a stable, typed schema snapshot. Plan hashes use a versioned
/// canonical encoding of checked source AST; source locations are excluded.
[[nodiscard]] std::expected<UpgradeSchema, UpgradeSchemaError>
makeUpgradeSchema(const CheckedUnit &checked);

/// Compare two manifests. Breaking changes are returned as typed records for
/// CLI presentation or executor policy; removed fields remain inspectable.
[[nodiscard]] std::vector<UpgradeChange>
compareUpgradeSchemas(const UpgradeSchema &oldSchema,
                      const UpgradeSchema &newSchema);

[[nodiscard]] bool
upgradeChangeRequiresRefusal(const UpgradeChange &change) noexcept;

[[nodiscard]] std::string_view
upgradeChangeName(UpgradeChangeKind kind) noexcept;

} // namespace xanadu::vagent::runtime

#endif
