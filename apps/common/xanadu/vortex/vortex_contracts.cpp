#include "common/xanadu/vortex/vortex_vm.hpp"

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
  const auto inputs              = core_.inputsOf(opcode);
  const auto slots               = core_.outputsOf(opcode);
  const bool required            = kind == OpcodeKind::RequireContract;
  const std::size_t firstSubject = required ? 2 : 1;
  if (inputs.size() < firstSubject || (!required && slots.empty())) {
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
    xanadu::MultiStoreCoordinator fallback(core_);
    auto &coordinator = contractCoordinator_ ? *contractCoordinator_ : fallback;
    xanadu::vql::ContractMatcher matcher(core_, coordinator);
    std::vector<CellRef> subjects;
    for (std::size_t i = firstSubject; i < inputs.size(); ++i) {
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
      if (!literal)
        throw xanadu::vql::TypeContractError(
            "compiled nonliteral default requires a lazy VM thunk");
      const auto cell = core_.arena().makeCell();
      std::visit(
          [&](const auto &value) {
            core_.writeValue(cell, 0, std::nullopt, CellValue{value});
          },
          literal->value);
      return {cell};
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
    const auto names = xanadu::vql::ContractMatcher::contractCaptureNames(type);
    if (slots.size() != names.size() + 1) {
      return {.success      = false,
              .errorMessage = "contract capture slots disagree"};
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
