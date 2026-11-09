/**
 * @file contracts.cpp
 * @brief Shared VQL structural contract matching implementation.
 */
#include "common/xanadu/vql/contracts.hpp"
#include "common/xanadu/vql/contract_driver.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <ranges>
#include <set>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/scalar.hpp"
#include "common/xanadu/zigzag/arena_savepoint.hpp"
#include "common/xanadu/zigzag/query_scalar.hpp"

namespace xanadu::vql {

namespace {

class SyncContractBackend {
public:
  using Bindings     = ContractBindings;
  using Continuation = gleditor::cpp26::function_ref<void()>;
  using Context      = std::optional<zigzag::CellRef>;
  using Dimension    = zigzag::DimRef;
  using Subject      = zigzag::CellRef;
  using Subjects     = std::span<const Subject>;
  using MissingField = ContractMatcher::MissingField;

  SyncContractBackend(zigzag::vortex::VortexCore &core,
                      MultiStoreCoordinator &coordinator,
                      ContractMatcher::DefaultEvaluator evaluateDefault)
      : core_(core), coordinator_(coordinator),
        evaluateDefault_(evaluateDefault) {}

  template <class Body>
  void scope(Bindings &bindings, const detail::ScopePolicy policy, Body &&body,
             Continuation success, Continuation failure) {
    Bindings trial = bindings;
    zigzag::ArenaSavepoint savepoint(core_.arena());
    bool settled = false;
    body(
        trial,
        [&] {
          if (settled) return;
          settled = true;
          if (policy == detail::ScopePolicy::CommitOnSuccess) {
            bindings.swap(trial);
            savepoint.commit();
          } else {
            savepoint.rollback();
          }
          resume(success);
        },
        [&] {
          if (settled) return;
          settled = true;
          savepoint.rollback();
          resume(failure);
        });
  }

  template <class Body>
  void anyArm(const std::size_t count, Bindings &bindings,
              const detail::ScopePolicy policy, Body &&body,
              Continuation success, Continuation failure) {
    for (std::size_t index = 0; index < count; ++index) {
      bool matched = false;
      scope(
          bindings, policy,
          [&](Bindings &trial, Continuation armSuccess,
              Continuation armFailure) {
            body(index, trial, armSuccess, armFailure);
          },
          [&] { matched = true; }, [] {});
      if (matched) {
        resume(success);
        return;
      }
    }
    resume(failure);
  }

  template <class Range, class Body>
  void allOf(const Range &range, Body &&body, Continuation success,
             Continuation failure) {
    for (const auto &item : range) {
      bool matched = false;
      body(item, [&] { matched = true; }, [] {});
      if (!matched) {
        resume(failure);
        return;
      }
    }
    resume(success);
  }

  template <class Success>
  void validContext(const Context context, Success &&success,
                    Continuation failure) const {
    if (context && core_.arena().contains(*context))
      success();
    else
      resume(failure);
  }

  template <class Success, class Failure>
  void empty(const Subjects &subjects, Success &&onEmpty,
             Failure &&onNonempty) const {
    if (subjects.empty())
      onEmpty();
    else
      onNonempty();
  }

  template <class Success>
  void single(const Subjects &subjects, Success &&onSingle,
              Continuation failure) const {
    if (subjects.size() == 1)
      onSingle(subjects.front());
    else
      resume(failure);
  }

  template <class Success>
  void probeLink(Context context, Dimension dimension,
                 zigzag::DimVector direction, Success &&onFound,
                 Continuation onMissing) const {
    const auto linked =
        core_.arena().linkedMinting(*context, dimension, direction);
    if (linked)
      onFound(*linked);
    else
      resume(onMissing);
  }

  template <class Values, class Failure>
  void evaluateDefault(const ValueExpr &expression, Context context,
                       const Bindings &defaultScope, Values &&onValues,
                       Failure &&onFailure) const {
    (void)defaultScope;
    (void)onFailure;
    const auto values = evaluateDefault_(expression, context);
    const Subjects subjects(values);
    onValues(subjects);
  }

  template <class Success>
  void probeBasic(const BasicType &type, Subject cell, Success &&onMatch,
                  Continuation failure) const {
    if (hasBasicType(type, cell))
      onMatch();
    else
      resume(failure);
  }

  template <class Success>
  void validSubject(Subject cell, Success &&onValid,
                    Continuation failure) const {
    if (core_.arena().contains(cell))
      onValid();
    else
      resume(failure);
  }

  template <class Action>
  void withSingleton(Subject cell, Action &&action) const {
    const std::array one{cell};
    const Subjects subjects(one);
    action(subjects);
  }
  [[nodiscard]] Context contextOf(Subject cell) const { return cell; }
  [[nodiscard]] Subject subjectOf(const Context context) const {
    return *context;
  }
  [[nodiscard]] Subjects emptySubjects() const { return {}; }
  [[nodiscard]] Dimension resolveDimension(std::string_view name) const {
    return coordinator_.resolveDimension(name);
  }
  void capture(Bindings &bindings, std::string_view name,
               const Subjects &values) const {
    bindings[std::string(name)].assign(values.begin(), values.end());
  }
  void consumeWork() const { core_.arena().consumeWork(); }
  void validateType(const TypeExpr &type) const {
    ContractMatcher::validateType(type);
  }
  void resume(const Continuation &continuation) const { continuation(); }

private:
  [[nodiscard]] bool hasBasicType(const BasicType &type,
                                  const Subject cell) const {
    using K     = BasicType::Kind;
    auto &arena = core_.arena();
    if (type.kind == K::Contextual || !arena.contains(cell)) return false;
    const auto scalarValue = [&] {
      auto scalar =
          zigzag::readQueryScalar(arena, cell, core_.dims().clone,
                                  core_.dims().runtimeObjectClaim.value());
      if (!scalar) {
        if (scalar.error() == zigzag::QueryScalarReadError::InvalidForeign)
          throw TypeContractError("contract scalar has no valid source cell");
        if (scalar.error() == zigzag::QueryScalarReadError::Unreadable)
          throw TypeContractError("contract scalar payload is unreadable");
      }
      return scalar;
    };
    switch (type.kind) {
    case K::Any:
    case K::Cell:
      return true;
    case K::Dim:
      if (const auto value = core_.render(cell)) {
        const auto *text = std::get_if<std::string>(&*value);
        return std::ranges::contains(arena.dimensions(), cell) ||
               (text != nullptr && core_.findDimension(*text) == cell);
      }
      return false;
    case K::Store:
      return coordinator_.coordinatorHome() == cell ||
             std::ranges::any_of(coordinator_.stores(), [&](const auto &info) {
               return info.storeCell == cell;
             });
    case K::Slice:
      return arena.home() == cell ||
             std::ranges::any_of(coordinator_.stores(), [&](const auto &info) {
               return info.homeCell == cell;
             });
    case K::Version: {
      auto scalar      = scalarValue();
      const auto *text = scalar ? std::get_if<std::string>(&*scalar) : nullptr;
      if (text == nullptr || text->empty()) return false;
      try {
        (void)xanadu::MicroversionId::parse(*text);
        return true;
      } catch (const std::invalid_argument &) {
        return false;
      }
    }
    case K::Int:
    case K::Float:
    case K::String:
    case K::Bool:
      break;
    case K::Contextual:
      return false;
    }

    const auto scalar = scalarValue();
    if (!scalar) return false;
    if (type.kind == K::String)
      return std::holds_alternative<std::string>(*scalar);

    const auto *text = std::get_if<std::string>(&*scalar);
    if (text == nullptr) {
      if (type.kind == K::Int)
        return std::holds_alternative<std::int64_t>(*scalar);
      if (type.kind == K::Float)
        return std::holds_alternative<double>(*scalar) ||
               std::holds_alternative<std::int64_t>(*scalar);
      return std::holds_alternative<bool>(*scalar);
    }

    const auto inferred = xanadu::inferTextValue(*text);
    switch (type.kind) {
    case K::Int:
      return inferred.kind == xanadu::ValueKind::Int64;
    case K::Float:
      return inferred.kind == xanadu::ValueKind::Double ||
             inferred.kind == xanadu::ValueKind::Int64;
    case K::Bool:
      return inferred.kind == xanadu::ValueKind::Bool;
    case K::Any:
    case K::Cell:
    case K::Dim:
    case K::Store:
    case K::Slice:
    case K::Version:
    case K::String:
    case K::Contextual:
      return false;
    }
    return false;
  }

  zigzag::vortex::VortexCore &core_;
  MultiStoreCoordinator &coordinator_;
  ContractMatcher::DefaultEvaluator evaluateDefault_;
};

template <class Action> bool runContract(Action &&action) {
  bool matched = false;
  std::forward<Action>(action)([&] { matched = true; },
                               [&] { matched = false; });
  return matched;
}

} // namespace

ContractMatcher::ContractMatcher(zigzag::vortex::VortexCore &core,
                                 MultiStoreCoordinator &coordinator)
    : core_(core), coordinator_(coordinator) {}

void ContractMatcher::validateType(const TypeExpr &type) {
  for (const auto &member : type.unionType.members) {
    if (const auto *basic = std::get_if<BasicType>(&member.kind)) {
      if (basic->kind == BasicType::Kind::Contextual) {
        throw TypeContractError("unknown type " + basic->name);
      }
    } else if (const auto *generic = std::get_if<GenericType>(&member.kind)) {
      if (generic->name != "Stream" || generic->args.size() != 1) {
        throw TypeContractError("unknown type " + generic->name + " with " +
                                std::to_string(generic->args.size()) +
                                " argument(s)");
      }
      if (!generic->args.front()) {
        throw TypeContractError("invalid null type argument for Stream");
      }
      validateType(*generic->args.front());
    } else if (const auto *nested =
                   std::get_if<std::shared_ptr<TypeExpr>>(&member.kind)) {
      if (!*nested) throw TypeContractError("invalid null nested type");
      validateType(**nested);
    }
    if (member.refinement) {
      for (const auto &field : member.refinement->pattern.fields) {
        if (field.type) validateType(*field.type);
      }
    }
    if (const auto *shape = std::get_if<ShapeType>(&member.kind)) {
      for (const auto &field : shape->pattern.fields) {
        if (field.type) validateType(*field.type);
      }
    }
  }
}

void validateFunctionSignature(const DefDeclaration &definition) {
  for (std::size_t index = 0; index < definition.params.size(); ++index) {
    const auto &parameter = definition.params[index];
    if (parameter.isVariadic) {
      if (index + 1 != definition.params.size())
        throw TypeContractError(definition.name +
                                "() variadic parameter must be last");
    }
    if (parameter.type) ContractMatcher::validateType(*parameter.type);
    if (const auto *pattern =
            std::get_if<DestructurePattern>(&parameter.target)) {
      for (const auto &field : pattern->fields)
        if (field.type) ContractMatcher::validateType(*field.type);
    }
  }
  if (definition.returnType)
    ContractMatcher::validateType(*definition.returnType);
}

std::vector<std::string>
ContractMatcher::contractCaptureNames(const TypeExpr &type) {
  std::set<std::string> names;
  std::function<void(const TypeExpr &)> visitType;
  std::function<void(const DestructurePattern &)> visitPattern;
  visitPattern = [&](const DestructurePattern &pattern) {
    for (const auto &field : pattern.fields) {
      if (field.isContext) names.insert(".");
      if (!field.varName.empty()) names.insert(field.varName);
      if (field.type) visitType(*field.type);
    }
  };
  visitType = [&](const TypeExpr &current) {
    for (const auto &member : current.unionType.members) {
      if (const auto *shape = std::get_if<ShapeType>(&member.kind)) {
        visitPattern(shape->pattern);
      } else if (const auto *nested =
                     std::get_if<std::shared_ptr<TypeExpr>>(&member.kind)) {
        if (*nested) visitType(**nested);
      } else if (std::holds_alternative<GenericType>(member.kind)) {
        // Generic arguments and refinements validate without exporting
        // captures.
        continue;
      }
      if (member.refinement) visitPattern(member.refinement->pattern);
    }
  };
  visitType(type);
  return {names.begin(), names.end()};
}

bool ContractMatcher::matchDestructure(
    const DestructurePattern &pattern,
    const std::optional<zigzag::CellRef> context, ContractBindings &bindings,
    const MissingField missing, const DefaultEvaluator evaluateDefault) {
  SyncContractBackend backend(core_, coordinator_, evaluateDefault);
  detail::ContractDriver driver(backend);
  const ContractBindings &defaultScope = bindings;
  return runContract([&](auto success, auto failure) {
    driver.matchPattern(pattern, context, bindings, defaultScope, missing,
                        success, failure);
  });
}

bool ContractMatcher::satisfiesType(
    const TypeExpr &type, const std::span<const zigzag::CellRef> cells,
    ContractBindings *const bindings, const DefaultEvaluator evaluateDefault) {
  SyncContractBackend backend(core_, coordinator_, evaluateDefault);
  detail::ContractDriver driver(backend);
  const ContractBindings emptyBindings;
  const ContractBindings &defaultScope = bindings ? *bindings : emptyBindings;
  return runContract([&](auto success, auto failure) {
    driver.matchType(type, cells, bindings, defaultScope, success, failure);
  });
}

bool ContractMatcher::match(const TypeExpr &type,
                            const std::span<const zigzag::CellRef> cells,
                            ContractBindings *const bindings,
                            const DefaultEvaluator evaluateDefault) {
  return satisfiesType(type, cells, bindings, evaluateDefault);
}

} // namespace xanadu::vql
