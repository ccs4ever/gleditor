#include "common/xanadu/vortex/vortex_vm.hpp"

#include <bit>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "common/xanadu/scalar.hpp"
#include "common/xanadu/zigzag/arena_savepoint.hpp"
#include "common/xanadu/zigzag/query_scalar.hpp"

namespace zigzag::vortex {
namespace {

std::expected<zigzag::QueryScalar, std::string>
readSingleScalar(VortexVM &vm, const CellRef slot,
                 const std::string_view operation) {
  auto &arena = vm.core().arena();
  if (!arena.contains(slot))
    return std::unexpected(std::string(operation) + " input slot is missing");

  std::optional<CellRef> source;
  bool multiple = false;
  vm.forEachResultCell(slot, [&](const CellRef cell) {
    arena.consumeWork();
    if (!arena.contains(cell))
      throw std::invalid_argument(std::string(operation) +
                                  " source cell is missing");
    if (source)
      multiple = true;
    else
      source = cell;
  });
  if (!source)
    return std::unexpected(std::string(operation) +
                           " expects exactly one input cell");
  if (multiple)
    return std::unexpected(std::string(operation) +
                           " does not accept multiple input cells");

  const auto scalar =
      zigzag::readQueryScalar(arena, *source, vm.core().dims().clone,
                              vm.core().dims().runtimeObjectClaim.value());
  if (!scalar) {
    switch (scalar.error()) {
    case zigzag::QueryScalarReadError::Missing:
      return std::unexpected(std::string(operation) + " input cell is missing");
    case zigzag::QueryScalarReadError::InvalidForeign:
      return std::unexpected(std::string(operation) +
                             " foreign input has no valid source cell");
    case zigzag::QueryScalarReadError::UnsupportedHandle:
      return std::unexpected(std::string(operation) +
                             " does not accept structural handles");
    case zigzag::QueryScalarReadError::Unreadable:
      return std::unexpected(std::string(operation) +
                             " input scalar is unreadable");
    }
    return std::unexpected(std::string(operation) +
                           " input scalar is unreadable");
  }
  return *scalar;
}

bool exactlyRepresentableAsDouble(const std::int64_t value) noexcept {
  const auto magnitude =
      value < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(value)
                : static_cast<std::uint64_t>(value);
  if (magnitude == 0) return true;
  const int significantBits = std::bit_width(magnitude);
  constexpr int exactBits   = std::numeric_limits<double>::digits;
  return significantBits <= exactBits ||
         std::countr_zero(magnitude) >= significantBits - exactBits;
}

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

    const auto scalar = readSingleScalar(*this, inputSlot, "VqlString");
    if (!scalar) return fail(scalar.error());

    outputs.emplace_back(canonicalScalarText(*scalar));
    return {};
  } catch (const std::exception &error) {
    return fail(error.what());
  }
}

ExecutionResult VortexVM::executeVqlAdd(const CellRef opcode,
                                        std::vector<CellValue> &outputs) {
  const auto fail = [opcode](std::string message) {
    return ExecutionResult{.success       = false,
                           .failingClause = opcode,
                           .errorMessage  = std::move(message)};
  };

  try {
    const auto inputs      = inputsOf(opcode);
    const auto outputSlots = outputsOf(opcode);
    if (inputs.size() != 2 || outputSlots.size() != 1)
      return fail("VqlAdd expects two inputs and one output");

    const CellRef outputSlot = outputSlots.front();
    if (!core_.arena().contains(outputSlot) ||
        streamSlots_.contains(outputSlot) ||
        iteratorSlots_.contains(outputSlot))
      return fail("VqlAdd output must be one scalar arena slot");

    const auto left = readSingleScalar(*this, inputs[0], "VqlAdd");
    if (!left) return fail(left.error());
    const auto right = readSingleScalar(*this, inputs[1], "VqlAdd");
    if (!right) return fail(right.error());

    if (const auto *leftText = std::get_if<std::string>(&*left)) {
      const auto *rightText = std::get_if<std::string>(&*right);
      if (!rightText)
        return fail("VqlAdd requires two strings or numeric scalar values");
      outputs.emplace_back(*leftText + *rightText);
      return {};
    }

    if (const auto *leftInt = std::get_if<std::int64_t>(&*left)) {
      if (const auto *rightInt = std::get_if<std::int64_t>(&*right)) {
        std::int64_t sum{};
        if (__builtin_add_overflow(*leftInt, *rightInt, &sum))
          return fail("VqlAdd signed integer overflow");
        outputs.emplace_back(sum);
        return {};
      }
      if (const auto *rightDouble = std::get_if<double>(&*right)) {
        if (!exactlyRepresentableAsDouble(*leftInt))
          return fail("VqlAdd cannot exactly promote Int64 to Double");
        outputs.emplace_back(static_cast<double>(*leftInt) + *rightDouble);
        return {};
      }
    }

    if (const auto *leftDouble = std::get_if<double>(&*left)) {
      if (const auto *rightDouble = std::get_if<double>(&*right)) {
        outputs.emplace_back(*leftDouble + *rightDouble);
        return {};
      }
      if (const auto *rightInt = std::get_if<std::int64_t>(&*right)) {
        if (!exactlyRepresentableAsDouble(*rightInt))
          return fail("VqlAdd cannot exactly promote Int64 to Double");
        outputs.emplace_back(*leftDouble + static_cast<double>(*rightInt));
        return {};
      }
    }

    return fail("VqlAdd requires strings or numeric scalar values");
  } catch (const std::exception &error) {
    return fail(error.what());
  }
}

ExecutionResult VortexVM::executeVqlBoxScalar(const CellRef opcode,
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
      return fail("VqlBoxScalar expects one input and one output");

    const CellRef inputSlot  = inputs.front();
    const CellRef outputSlot = outputSlots.front();
    if (inputSlot == outputSlot || !core_.arena().contains(inputSlot) ||
        streamSlots_.contains(inputSlot) ||
        iteratorSlots_.contains(inputSlot) ||
        cellReferences_.contains(inputSlot))
      return fail("VqlBoxScalar input must be a distinct raw scalar slot");
    if (!core_.arena().contains(outputSlot) ||
        streamSlots_.contains(outputSlot) ||
        iteratorSlots_.contains(outputSlot))
      return fail("VqlBoxScalar output must be one scalar arena slot");

    const auto scalar = readSingleScalar(*this, inputSlot, "VqlBoxScalar");
    if (!scalar) return fail(scalar.error());

    core_.arena().consumeWork();
    const CellRef boxed = std::visit(
        [this](const auto &value) -> CellRef {
          using Value = std::remove_cvref_t<decltype(value)>;
          if constexpr (std::same_as<Value, std::string>) {
            return core_.arena().makeCell(value);
          } else {
            return core_.arena().makeScalarCell(value);
          }
        },
        *scalar);
    outputs.emplace_back(static_cast<std::int64_t>(boxed));
    return {};
  } catch (const std::exception &error) {
    return fail(error.what());
  }
}

ExecutionResult
VortexVM::executeCurrentTimestamp(const CellRef opcode,
                                  std::vector<CellValue> &outputs) {
  const auto fail = [opcode](std::string message) {
    return ExecutionResult{.success       = false,
                           .failingClause = opcode,
                           .errorMessage  = std::move(message)};
  };

  try {
    const auto inputs      = inputsOf(opcode);
    const auto outputSlots = outputsOf(opcode);
    if (!inputs.empty() || outputSlots.size() != 1)
      return fail("CurrentTimestamp expects no inputs and one output");
    const CellRef outputSlot = outputSlots.front();
    auto &arena              = core_.arena();
    if (!arena.contains(outputSlot) || streamSlots_.contains(outputSlot) ||
        iteratorSlots_.contains(outputSlot))
      return fail("CurrentTimestamp output must be one scalar arena slot");

    arena.consumeWork();
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto nanos =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now);
    const auto value = xanadu::scalarTimestamp(nanos.count());
    zigzag::ArenaSavepoint savepoint(arena);
    const CellRef timestamp = arena.makeCell(value.text);
    zigzag::expectWritten(
        arena.setValueBits(timestamp, value.kind, value.bits));
    outputs.emplace_back(static_cast<std::int64_t>(timestamp));
    savepoint.commit();
    return {};
  } catch (const std::exception &error) {
    return fail(error.what());
  }
}

} // namespace zigzag::vortex
