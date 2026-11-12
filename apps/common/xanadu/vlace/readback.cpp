/**
 * @file readback.cpp
 * @brief Card L1 owns this skeleton: cells -> host Value for `show`, `render`
 *        and `forceValue`. Per-kind hooks live in the owning card's file:
 *        scalars here (L1), thunk and closure cells in lower_kernel.cpp (L2),
 *        constructed terms and deep rendering in lower_data.cpp (L3).
 */
#include <stdexcept>
#include <variant>

#include "common/xanadu/vlace/lower_cards.hpp"

namespace xanadu::vlace::lower {

namespace {

ValuePtr makeValue(Value::Kind kind, zigzag::CellRef cell) {
  auto v  = std::make_shared<Value>();
  v->kind = kind;
  v->cell = cell;
  return v;
}

/// The unwrapped result of a hook, or the hook's own failure.
std::expected<ValuePtr, EvalError> unwrap(Lowered<ValuePtr> hook) {
  if (!hook) return std::unexpected(std::move(hook.error()));
  return *hook;
}

} // namespace

// The classification order: the kernel hook first (a thunk cell may also hold a
// scalar-looking memo, and a closure root is a term), then the scalar hook,
// then the data hook. The first hook that returns a value decides, so a later
// card adds a kind by making its own hook claim the cells it owns; this file
// does not change. A cell no hook claims is refused ("values").
std::expected<ValuePtr, EvalError> readbackCell(Compiler &compiler,
                                                zigzag::CellRef cell) {
  if (auto kernel = readbackKernel(compiler, cell)) return unwrap(*kernel);
  if (auto scalar = readbackScalar(compiler, cell)) return unwrap(*scalar);
  if (auto data = readbackData(compiler, cell)) return unwrap(*data);
  return notLowered("values");
}

std::optional<Lowered<ValuePtr>> readbackScalar(Compiler &compiler,
                                                zigzag::CellRef cell) {
  auto &core = compiler.core();
  // A register that references another cell holds that cell's handle, not a
  // scalar of its own; reading its value here would return the handle. L3 owns
  // referenced cells.
  if (const auto ref = compiler.vm().cellReferenceOf(cell); ref && *ref != cell)
    return std::nullopt;
  // A term root keeps its fields on +d.grab; a scalar never has any, so a cell
  // with one belongs to the data hook.
  if (core.arena().linked(cell, core.dims().grab, zigzag::Posward))
    return std::nullopt;
  const auto content = core.render(cell);
  if (!content) return std::nullopt;
  return std::visit(
      [&](const auto &held) -> Lowered<ValuePtr> {
        using T = std::decay_t<decltype(held)>;
        if constexpr (std::is_same_v<T, std::int64_t>) {
          auto v = makeValue(Value::Kind::Int, cell);
          v->i   = held;
          return v;
        } else if constexpr (std::is_same_v<T, double>) {
          auto v = makeValue(Value::Kind::Float, cell);
          v->f   = held;
          return v;
        } else if constexpr (std::is_same_v<T, bool>) {
          auto v = makeValue(Value::Kind::Bool, cell);
          v->b   = held;
          return v;
        } else {
          auto v = makeValue(Value::Kind::Str, cell);
          v->s   = held;
          return v;
        }
      },
      *content);
}

std::string renderValue(Compiler &compiler, const ValuePtr &v,
                        int depthBudget) {
  if (!v) {
    throw std::logic_error("vlace rendering needs a value to render");
  }
  if (depthBudget <= 0) return "...";
  ValuePtr forced = v;
  if (v->kind == Value::Kind::Thunk) {
    auto result = compiler.forceValue(v); // L2: runs the force frame on the VM
    if (!result) return "<error>";
    forced = *result;
    if (!forced) return "<null>";
  }
  if (forced->kind == Value::Kind::Ctor)
    return renderData(compiler, *forced, depthBudget); // L3
  return Compiler::show(forced);
}

} // namespace xanadu::vlace::lower
