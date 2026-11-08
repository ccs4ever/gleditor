#include "common/xanadu/vortex/vortex_vm.hpp"

#include <bit>
#include <cmath>
#include <compare>
#include <concepts>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>

#include "common/xanadu/scalar.hpp"
#include "common/xanadu/zigzag/query_scalar.hpp"

namespace zigzag::vortex {
namespace {

using ComparisonValue = std::variant<std::int64_t, double, bool, std::string>;

std::optional<CellValue>
comparisonOperand(VortexCore &core, const CellReferenceMap &references,
                  const std::unordered_set<CellRef> &streamSlots,
                  const std::unordered_set<CellRef> &iteratorSlots,
                  const CellRef slot) {
  if (streamSlots.contains(slot) || iteratorSlots.contains(slot)) {
    throw std::invalid_argument("VQL comparison operands must be scalar");
  }

  CellRef cell = slot;
  if (const auto reference = references.find(slot);
      reference != references.end()) {
    if (!reference->second) return std::nullopt;
    cell = *reference->second;
  }
  auto scalar = zigzag::readQueryScalar(core.arena(), cell, core.dims().clone,
                                        core.dims().runtimeObjectClaim.value());
  if (scalar) return std::move(*scalar);
  switch (scalar.error()) {
  case zigzag::QueryScalarReadError::Missing:
    return std::nullopt;
  case zigzag::QueryScalarReadError::InvalidForeign:
    throw std::invalid_argument(
        "VQL comparison foreign operand has no valid source cell");
  case zigzag::QueryScalarReadError::UnsupportedHandle:
    throw std::invalid_argument(
        "VQL comparison does not support structural handles");
  case zigzag::QueryScalarReadError::Unreadable:
    throw std::invalid_argument("VQL comparison scalar value is unreadable");
  }
  throw std::invalid_argument("VQL comparison scalar value is unreadable");
}

ComparisonValue classifyText(std::string text) {
  const auto inferred = xanadu::inferTextValue(text);
  switch (inferred.kind) {
  case xanadu::ValueKind::Int64:
    return std::bit_cast<std::int64_t>(inferred.bits);
  case xanadu::ValueKind::Double:
    return std::bit_cast<double>(inferred.bits);
  case xanadu::ValueKind::Bool:
    return inferred.bits != 0;
  default:
    return text;
  }
}

ComparisonValue classify(CellValue value) {
  return std::visit(
      [](auto &&held) -> ComparisonValue {
        using T = std::remove_cvref_t<decltype(held)>;
        if constexpr (std::same_as<T, std::string>) {
          return classifyText(std::move(held));
        } else {
          return std::forward<decltype(held)>(held);
        }
      },
      std::move(value));
}

std::string canonicalText(const ComparisonValue &value) {
  return std::visit(
      [](const auto &held) -> std::string {
        using T = std::remove_cvref_t<decltype(held)>;
        if constexpr (std::same_as<T, std::string>) {
          return held;
        } else {
          return xanadu::scalarValue(held).text;
        }
      },
      value);
}

bool applyComparison(const std::partial_ordering order,
                     const VqlComparisonKind kind) {
  switch (kind) {
  case VqlComparisonKind::Equal:
    return order == std::partial_ordering::equivalent;
  case VqlComparisonKind::NotEqual:
    return order != std::partial_ordering::equivalent;
  case VqlComparisonKind::LessThan:
    return order == std::partial_ordering::less;
  case VqlComparisonKind::LessEqual:
    return order == std::partial_ordering::less ||
           order == std::partial_ordering::equivalent;
  case VqlComparisonKind::GreaterThan:
    return order == std::partial_ordering::greater;
  case VqlComparisonKind::GreaterEqual:
    return order == std::partial_ordering::greater ||
           order == std::partial_ordering::equivalent;
  }
  return false;
}

std::partial_ordering compareIntegerDouble(const std::int64_t integer,
                                           const double floating) noexcept {
  if (std::isnan(floating)) return std::partial_ordering::unordered;

  // INT64_MAX rounds up to 2^63 as a double, so guard the exclusive upper
  // bound before converting the truncated value to an integer.
  constexpr double signedLimit = 0x1p63;
  if (floating >= signedLimit) return std::partial_ordering::less;
  if (floating < -signedLimit) return std::partial_ordering::greater;

  const double integral = std::trunc(floating);
  const auto whole      = static_cast<std::int64_t>(integral);
  if (integer < whole) return std::partial_ordering::less;
  if (integer > whole) return std::partial_ordering::greater;
  if (floating > integral) return std::partial_ordering::less;
  if (floating < integral) return std::partial_ordering::greater;
  return std::partial_ordering::equivalent;
}

std::partial_ordering reverse(const std::partial_ordering order) noexcept {
  if (order == std::partial_ordering::less)
    return std::partial_ordering::greater;
  if (order == std::partial_ordering::greater)
    return std::partial_ordering::less;
  return order;
}

bool compareSameOrTextFallback(const ComparisonValue &left,
                               const ComparisonValue &right,
                               const VqlComparisonKind kind) {
  if (left.index() != right.index()) {
    if (const auto *l = std::get_if<std::int64_t>(&left)) {
      if (const auto *r = std::get_if<double>(&right)) {
        return applyComparison(compareIntegerDouble(*l, *r), kind);
      }
    } else if (const auto *l = std::get_if<double>(&left)) {
      if (const auto *r = std::get_if<std::int64_t>(&right)) {
        return applyComparison(reverse(compareIntegerDouble(*r, *l)), kind);
      }
    }

    const auto lText = canonicalText(left);
    const auto rText = canonicalText(right);
    return applyComparison(lText <=> rText, kind);
  }

  if (const auto *l = std::get_if<std::int64_t>(&left)) {
    return applyComparison(*l <=> std::get<std::int64_t>(right), kind);
  }
  if (const auto *l = std::get_if<double>(&left)) {
    const double r = std::get<double>(right);
    return applyComparison(*l <=> r, kind);
  }
  if (const auto *l = std::get_if<bool>(&left)) {
    return applyComparison(*l <=> std::get<bool>(right), kind);
  }
  const auto &l = std::get<std::string>(left);
  return applyComparison(l <=> std::get<std::string>(right), kind);
}

} // namespace

ExecutionResult VortexVM::executeVqlCompare(const CellRef opcode,
                                            std::vector<CellValue> &outputs) {
  const auto fail = [opcode](std::string message) {
    return ExecutionResult{.success       = false,
                           .failingClause = opcode,
                           .errorMessage  = std::move(message)};
  };
  try {
    const auto inputs = inputsOf(opcode);
    if (inputs.size() != 3) return fail("invalid VQL comparison operands");
    const auto selector = inputValue(inputs[0]);
    const auto *code    = std::get_if<std::int64_t>(&selector);
    if (!code || *code < static_cast<std::int64_t>(VqlComparisonKind::Equal) ||
        *code > static_cast<std::int64_t>(VqlComparisonKind::GreaterEqual)) {
      return fail("invalid VQL comparison selector");
    }
    const auto kind  = static_cast<VqlComparisonKind>(*code);
    const auto left  = comparisonOperand(core_, cellReferences_, streamSlots_,
                                         iteratorSlots_, inputs[1]);
    const auto right = comparisonOperand(core_, cellReferences_, streamSlots_,
                                         iteratorSlots_, inputs[2]);
    if (!left || !right) {
      outputs.emplace_back(false);
      return {};
    }

    outputs.emplace_back(
        compareSameOrTextFallback(classify(*left), classify(*right), kind));
    return {};
  } catch (const std::exception &error) {
    return fail(error.what());
  }
}

} // namespace zigzag::vortex
