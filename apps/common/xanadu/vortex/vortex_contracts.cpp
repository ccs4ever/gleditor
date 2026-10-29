#include "common/xanadu/vortex/vortex_vm.hpp"

#include <cstdint>
#include <limits>
#include <unordered_map>

#include "common/xanadu/vql/contract_codec.hpp"
#include "common/xanadu/vql/contracts.hpp"

namespace zigzag::vortex {

VortexVM *
VortexVM::setContractCoordinator(xanadu::MultiStoreCoordinator &coordinator) {
  contractCoordinator_ = &coordinator;
  return this;
}

VortexVM *VortexVM::setContractCoordinator(ContractCoordinator coordinator) {
  contractCoordinator_ = coordinator ? &coordinator->get() : nullptr;
  return this;
}

VortexVM::ContractCoordinator VortexVM::contractCoordinator() const noexcept {
  if (!contractCoordinator_) return std::nullopt;
  return std::ref(*contractCoordinator_);
}

ExecutionResult VortexVM::executeTypeContract(OpcodeKind kind, CellRef opcode,
                                              std::vector<CellValue> &outputs) {
  const auto inputs                   = core_.inputsOf(opcode);
  const auto slots                    = core_.outputsOf(opcode);
  const bool required                 = kind == OpcodeKind::RequireContract;
  const std::size_t subjectCountIndex = required ? 2 : 1;
  const std::size_t firstSubject      = subjectCountIndex + 1;
  if (inputs.size() <= subjectCountIndex || (!required && slots.empty()) ||
      (required && !slots.empty())) {
    return {.success = false, .errorMessage = "invalid contract operands"};
  }
  // A failed call must clear every capture, even when the same slots were
  // populated by an earlier invocation of this program.
  if (!required && slots.size() > 1) {
    writeCellReferences(
        std::span(slots).subspan(1),
        std::vector<CellValue>(slots.size() - 1, std::int64_t{noCell}));
  }
  try {
    const auto descriptor = inputValue(inputs[0]);
    if (!std::holds_alternative<std::string>(descriptor))
      throw xanadu::vql::TypeContractError("contract descriptor must be text");
    for (const auto byte : std::get<std::string>(descriptor)) {
      (void)byte;
      core_.arena().consumeWork();
    }
    auto type =
        xanadu::vql::decodeTypeDescriptor(std::get<std::string>(descriptor));
    const auto names = xanadu::vql::ContractMatcher::contractCaptureNames(type);
    if (!required && slots.size() != names.size() + 1) {
      throw xanadu::vql::TypeContractError("contract capture slots disagree");
    }
    const auto subjectCountValue = inputValue(inputs[subjectCountIndex]);
    const auto *subjectCount = std::get_if<std::int64_t>(&subjectCountValue);
    if (!subjectCount || *subjectCount < 0 ||
        static_cast<std::uint64_t>(*subjectCount) >
            std::numeric_limits<std::size_t>::max()) {
      throw xanadu::vql::TypeContractError(
          "contract subject count must be a nonnegative integer");
    }
    const auto subjectCountSize = static_cast<std::size_t>(*subjectCount);
    const auto defaultNames     = xanadu::vql::contractDefaultVariables(type);
    if (subjectCountSize > inputs.size() - firstSubject ||
        defaultNames.size() !=
            inputs.size() - firstSubject - subjectCountSize) {
      throw xanadu::vql::TypeContractError(
          "contract subject and default slot counts disagree");
    }
    std::unordered_map<std::string, CellRef> defaultSlots;
    for (std::size_t i = 0; i < defaultNames.size(); ++i) {
      const auto slot = inputs[firstSubject + subjectCountSize + i];
      if (!core_.arena().contains(slot) ||
          !defaultSlots.emplace(defaultNames[i], slot).second) {
        throw xanadu::vql::TypeContractError(
            "contract default alias slots are invalid");
      }
    }
    xanadu::MultiStoreCoordinator fallback(core_);
    auto &coordinator = contractCoordinator_ ? *contractCoordinator_ : fallback;
    xanadu::vql::ContractMatcher matcher(core_, coordinator);
    std::vector<CellRef> subjects;
    for (std::size_t i = firstSubject; i < firstSubject + subjectCountSize;
         ++i) {
      core_.arena().consumeWork();
      forEachResultCell(inputs[i], [&](const CellRef cell) {
        core_.arena().consumeWork();
        subjects.push_back(cell);
      });
    }
    xanadu::vql::ContractBindings captures;
    const auto defaultValue =
        [&](const xanadu::vql::ValueExpr &expression,
            std::optional<CellRef>) -> std::vector<CellRef> {
      const auto *literal =
          std::get_if<xanadu::vql::ScalarLiteral>(&expression.kind);
      if (literal) {
        const auto cell = core_.arena().makeCell();
        std::visit(
            [&](const auto &value) {
              core_.writeValue(cell, 0, std::nullopt, CellValue{value});
            },
            literal->value);
        return {cell};
      }
      const auto *name = std::get_if<std::string>(&expression.kind);
      if (!name)
        throw xanadu::vql::TypeContractError(
            "compiled default expression requires an unsupported VM thunk");
      const auto found = defaultSlots.find(*name);
      if (found == defaultSlots.end())
        throw xanadu::vql::TypeContractError(
            "compiled contract default has no captured alias slot");
      std::vector<CellRef> values;
      forEachResultCell(found->second, [&](const CellRef cell) {
        core_.arena().consumeWork();
        if (!values.empty())
          throw xanadu::vql::TypeContractError(
              "contract default alias resolves to more than one cell");
        values.push_back(cell);
      });
      return values;
    };
    const bool matched = matcher.match(type, subjects, &captures, defaultValue);
    if (required) {
      if (!matched) {
        return {.success           = false,
                .contractViolation = ContractViolationKind::Postcondition,
                .failingClause     = opcode,
                .errorMessage =
                    core_.arena().textOf(inputs[1]) +
                    "() returned a value outside its declared type"};
      }
      return {};
    }
    if (matched) {
      std::vector<CellValue> values;
      for (const auto &name : names) {
        const auto found = captures.find(name);
        const auto cell  = found != captures.end() && !found->second.empty()
                               ? found->second.front()
                               : noCell;
        values.emplace_back(static_cast<std::int64_t>(cell));
      }
      writeCellReferences(std::span(slots).subspan(1), values);
    }
    outputs.emplace_back(matched);
    return {};
  } catch (const std::exception &error) {
    return {.success       = false,
            .failingClause = opcode,
            .errorMessage  = error.what()};
  }
}

} // namespace zigzag::vortex
