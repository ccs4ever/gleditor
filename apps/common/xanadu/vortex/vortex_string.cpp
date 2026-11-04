#include "common/xanadu/vortex/vortex_vm.hpp"

#include <concepts>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "common/xanadu/scalar.hpp"
#include "common/xanadu/zigzag/query_scalar.hpp"

namespace zigzag::vortex {
namespace {

// Keep string conversion on the shared scalar reader and scalar formatter so
// query code sees the same clone, foreign-cell and canonical-text semantics.
std::string canonicalScalarText(const zigzag::QueryScalar &scalar) {
  return std::visit(
      [](const auto &value) -> std::string {
        using Value = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::same_as<Value, std::string>) {
          return value;
        } else {
          return xanadu::scalarValue(value).text;
        }
      },
      scalar);
}

} // namespace

ExecutionResult VortexVM::executeVqlString(const CellRef opcode,
                                           std::vector<CellValue> &outputs) {
  const auto fail = [opcode](std::string message) {
    return ExecutionResult{.success       = false,
                           .failingClause = opcode,
                           .errorMessage  = std::move(message)};
  };

  try {
    const auto inputs      = inputsOf(opcode);
    const auto outputSlots = outputsOf(opcode);
    if (inputs.size() != 1 || outputSlots.size() != 1)
      return fail("VqlString expects one input and one output");

    const CellRef inputSlot  = inputs.front();
    const CellRef outputSlot = outputSlots.front();
    if (!core_.arena().contains(inputSlot))
      return fail("VqlString input slot is missing");
    if (!core_.arena().contains(outputSlot) ||
        streamSlots_.contains(outputSlot) ||
        iteratorSlots_.contains(outputSlot))
      return fail("VqlString output must be one scalar arena slot");

    std::optional<CellRef> source;
    bool multiple = false;
    forEachResultCell(inputSlot, [&](const CellRef cell) {
      core_.arena().consumeWork();
      if (!core_.arena().contains(cell))
        throw std::invalid_argument("VqlString source cell is missing");
      if (source) {
        multiple = true;
      } else {
        source = cell;
      }
    });
    if (!source) return fail("VqlString expects exactly one input cell");
    if (multiple) return fail("VqlString does not accept multiple input cells");

    const auto scalar =
        zigzag::readQueryScalar(core_.arena(), *source, core_.dims().clone);
    if (!scalar) {
      switch (scalar.error()) {
      case zigzag::QueryScalarReadError::Missing:
        return fail("VqlString input cell is missing");
      case zigzag::QueryScalarReadError::InvalidForeign:
        return fail("VqlString foreign input has no valid source cell");
      case zigzag::QueryScalarReadError::UnsupportedHandle:
        return fail("VqlString does not accept structural handles");
      case zigzag::QueryScalarReadError::Unreadable:
        return fail("VqlString input scalar is unreadable");
      }
      return fail("VqlString input scalar is unreadable");
    }

    outputs.emplace_back(canonicalScalarText(*scalar));
    return {};
  } catch (const std::exception &error) {
    return fail(error.what());
  }
}

} // namespace zigzag::vortex
