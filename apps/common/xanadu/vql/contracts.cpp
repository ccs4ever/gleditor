/**
 * @file contracts.cpp
 * @brief Shared VQL structural contract matching implementation.
 */
#include "common/xanadu/vql/contracts.hpp"

#include <algorithm>
#include <charconv>
#include <concepts>
#include <cstdlib>
#include <ranges>
#include <set>
#include <string_view>

#include "common/xanadu/microversion.hpp"

namespace xanadu::vql {
namespace {

bool stringToDouble(const std::string_view value, double &out) {
  if (value.empty()) return false;
  const std::string owned{value};
  char *end = nullptr;
  out       = std::strtod(owned.c_str(), &end);
  return end == owned.c_str() + owned.size();
}

bool stringToInt64(const std::string_view value, std::int64_t &out) {
  if (value.empty()) return false;
  const char *begin       = value.data();
  const char *end         = begin + value.size();
  const auto [ptr, error] = std::from_chars(begin, end, out);
  return error == std::errc{} && ptr == end;
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
      if (!std::holds_alternative<std::string>(parameter.target))
        throw TypeContractError(
            definition.name + "() variadic parameters require a variable name");
      if (parameter.defaultValue)
        throw TypeContractError(
            definition.name +
            "() variadic parameter defaults are not supported");
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
      } else if (const auto *generic = std::get_if<GenericType>(&member.kind)) {
        for (const auto &arg : generic->args) {
          if (arg) visitType(*arg);
        }
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
  if (!context || !core_.arena().contains(*context)) return false;
  for (const auto &field : pattern.fields) {
    core_.arena().consumeWork();
    if (field.isContext) {
      const std::vector<zigzag::CellRef> bound{*context};
      if (field.type &&
          !satisfiesType(*field.type, bound, &bindings, evaluateDefault)) {
        return false;
      }
      bindings["."] = {*context};
      if (!field.varName.empty()) bindings[field.varName] = {*context};
      continue;
    }
    const zigzag::DimRef dim = coordinator_.resolveDimension(field.dimName);
    auto linked = core_.arena().linkedMinting(*context, dim, field.direction);
    if (field.isBidir && !linked) {
      linked = core_.arena().linkedMinting(
          *context, dim,
          field.direction == zigzag::DimVector::POS ? zigzag::DimVector::NEG
                                                    : zigzag::DimVector::POS);
    }
    std::vector<zigzag::CellRef> bound;
    if (linked) {
      bound.push_back(*linked);
    } else if (field.defaultValue) {
      bound = evaluateDefault(*field.defaultValue, context);
    }
    if (field.type) {
      if (!satisfiesType(*field.type, bound, &bindings, evaluateDefault)) {
        return false;
      }
    } else if (bound.empty() && missing == MissingField::Refuse) {
      return false;
    }
    if (!field.varName.empty()) bindings[field.varName] = std::move(bound);
  }
  return true;
}

bool ContractMatcher::satisfiesType(
    const TypeExpr &type, const std::span<const zigzag::CellRef> cells,
    ContractBindings *const bindings, const DefaultEvaluator evaluateDefault) {
  core_.arena().consumeWork();
  validateType(type);
  for (const auto &member : type.unionType.members) {
    ContractBindings trial;
    if (satisfiesPrimary(member, cells, bindings != nullptr ? &trial : nullptr,
                         evaluateDefault)) {
      if (bindings != nullptr) {
        for (auto &[name, bound] : trial) {
          (*bindings)[name] = std::move(bound);
        }
      }
      return true;
    }
  }
  return false;
}

bool ContractMatcher::match(const TypeExpr &type,
                            const std::span<const zigzag::CellRef> cells,
                            ContractBindings *const bindings,
                            const DefaultEvaluator evaluateDefault) {
  return satisfiesType(type, cells, bindings, evaluateDefault);
}

bool ContractMatcher::satisfiesPrimary(
    const PrimaryType &type, const std::span<const zigzag::CellRef> cells,
    ContractBindings *const bindings, const DefaultEvaluator evaluateDefault) {
  if (const auto *generic = std::get_if<GenericType>(&type.kind)) {
    return std::ranges::all_of(cells, [&](const zigzag::CellRef cell) {
      return satisfiesType(*generic->args.front(), std::span{&cell, 1}, nullptr,
                           evaluateDefault) &&
             (!type.refinement || [&] {
               ContractBindings ignored;
               return matchDestructure(type.refinement->pattern, cell, ignored,
                                       MissingField::Refuse, evaluateDefault);
             }());
    });
  }
  if (cells.empty()) return type.isOptional;
  return cells.size() == 1 &&
         satisfiesAtom(type, cells.front(), bindings, evaluateDefault);
}

bool ContractMatcher::satisfiesAtom(const PrimaryType &type,
                                    const zigzag::CellRef cell,
                                    ContractBindings *const bindings,
                                    const DefaultEvaluator evaluateDefault) {
  ContractBindings ignored;
  ContractBindings &captures = bindings != nullptr ? *bindings : ignored;
  const bool inhabited       = std::visit(
      [&](const auto &held) -> bool {
        using T = std::remove_cvref_t<decltype(held)>;
        if constexpr (std::same_as<T, BasicType>) {
          return hasBasicType(held, cell);
        } else if constexpr (std::same_as<T, ShapeType>) {
          return core_.arena().contains(cell) &&
                 matchDestructure(held.pattern, cell, captures,
                                        MissingField::Refuse, evaluateDefault);
        } else if constexpr (std::same_as<T, std::shared_ptr<TypeExpr>>) {
          return satisfiesType(*held, std::span{&cell, 1}, bindings,
                                     evaluateDefault);
        } else {
          return false;
        }
      },
      type.kind);
  return inhabited &&
         (!type.refinement ||
          matchDestructure(type.refinement->pattern, cell, captures,
                           MissingField::Refuse, evaluateDefault));
}

bool ContractMatcher::hasBasicType(const BasicType &type,
                                   const zigzag::CellRef cell) const {
  using K           = BasicType::Kind;
  const auto &arena = core_.arena();
  if (type.kind == K::Contextual || !arena.contains(cell)) return false;
  const auto value = core_.render(cell);
  if (!value) return false;
  const auto &rendered = *value;
  const auto *text     = std::get_if<std::string>(&rendered);
  switch (type.kind) {
  case K::Any:
  case K::Cell:
    return true;
  case K::Dim:
    return std::ranges::contains(arena.dimensions(), cell) ||
           (text != nullptr && core_.findDimension(*text) == cell);
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
  case K::Version:
    if (text == nullptr || text->empty()) return false;
    try {
      (void)xanadu::MicroversionId::parse(*text);
      return true;
    } catch (const std::invalid_argument &) {
      return false;
    }
  case K::Int: {
    std::int64_t parsed = 0;
    return std::holds_alternative<std::int64_t>(rendered) ||
           (text != nullptr && stringToInt64(*text, parsed));
  }
  case K::Float: {
    double parsed = 0.0;
    return std::holds_alternative<double>(rendered) ||
           std::holds_alternative<std::int64_t>(rendered) ||
           (text != nullptr && stringToDouble(*text, parsed));
  }
  case K::String:
    return text != nullptr;
  case K::Bool:
    return std::holds_alternative<bool>(rendered) ||
           (text != nullptr && (*text == "true" || *text == "false"));
  case K::Contextual:
    return false;
  }
  return false;
}

} // namespace xanadu::vql
