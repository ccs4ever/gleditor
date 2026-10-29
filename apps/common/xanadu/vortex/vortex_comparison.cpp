#include "common/xanadu/vortex/vortex_vm.hpp"

#include <bit>
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

namespace zigzag::vortex {
namespace {

using ComparisonValue = std::variant<std::int64_t, double, bool, std::string>;

std::optional<CellValue>
comparisonOperand(const VortexCore &core, const CellReferenceMap &references,
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
  if (!core.arena().contains(cell)) return std::nullopt;

  const CellRef master =
      core.arena().cloneMaster(cell, core.dims().clone).value_or(cell);
  if (!core.arena().contains(master)) return std::nullopt;
  const auto kind = core.arena().valueKindOf(master);
  if (kind == xanadu::ValueKind::OpHandle ||
      kind == xanadu::ValueKind::ExternRef ||
      kind == xanadu::ValueKind::ForeignOpHandle) {
    throw std::invalid_argument(
        "VQL comparison does not support structural handles");
  }
  if (kind == xanadu::ValueKind::None ||
      (kind != xanadu::ValueKind::Double && kind != xanadu::ValueKind::Int64 &&
       kind != xanadu::ValueKind::Bool)) {
    const auto text = core.arena().textOf(master);
    for (const auto byte : text) {
      (void)byte;
      core.arena().consumeWork();
    }
    return CellValue{text};
  }
  return core.render(master);
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

bool applyComparison(const std::strong_ordering order,
                     const VqlComparisonKind kind) {
  switch (kind) {
  case VqlComparisonKind::Equal:
    return order == std::strong_ordering::equal;
  case VqlComparisonKind::NotEqual:
    return order != std::strong_ordering::equal;
  case VqlComparisonKind::LessThan:
    return order == std::strong_ordering::less;
  case VqlComparisonKind::LessEqual:
    return order != std::strong_ordering::greater;
  case VqlComparisonKind::GreaterThan:
    return order == std::strong_ordering::greater;
  case VqlComparisonKind::GreaterEqual:
    return order != std::strong_ordering::less;
  }
  return false;
}

std::optional<bool> compareSameOrTextFallback(const ComparisonValue &left,
                                              const ComparisonValue &right,
                                              const VqlComparisonKind kind) {
  if (left.index() != right.index()) {
    const bool mixedIntDouble = (std::holds_alternative<std::int64_t>(left) &&
                                 std::holds_alternative<double>(right)) ||
                                (std::holds_alternative<double>(left) &&
                                 std::holds_alternative<std::int64_t>(right));
    if (mixedIntDouble) return std::nullopt;

    const auto lText = canonicalText(left);
    const auto rText = canonicalText(right);
    return applyComparison(lText <=> rText, kind);
  }

  if (const auto *l = std::get_if<std::int64_t>(&left)) {
    return applyComparison(*l <=> std::get<std::int64_t>(right), kind);
  }
  if (const auto *l = std::get_if<double>(&left)) {
    const double r = std::get<double>(right);
    switch (kind) {
    case VqlComparisonKind::Equal:
      return *l == r;
    case VqlComparisonKind::NotEqual:
      return *l != r;
    case VqlComparisonKind::LessThan:
      return *l < r;
    case VqlComparisonKind::LessEqual:
      return *l <= r;
    case VqlComparisonKind::GreaterThan:
      return *l > r;
    case VqlComparisonKind::GreaterEqual:
      return *l >= r;
    }
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
    const auto inputs = core_.inputsOf(opcode);
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

    const auto result =
        compareSameOrTextFallback(classify(*left), classify(*right), kind);
    if (!result) {
      return fail("mixed Int and Float comparisons are not yet defined");
    }
    outputs.emplace_back(*result);
    return {};
  } catch (const std::exception &error) {
    return fail(error.what());
  }
}

} // namespace zigzag::vortex
