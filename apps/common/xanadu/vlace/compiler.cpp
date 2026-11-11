/**
 * @file compiler.cpp
 * @brief The vlace Compiler front: public API, show, and the persistent
 *        lowering state. Lowering itself is in lower_*.cpp.
 */
#include "common/xanadu/vlace/compiler.hpp"

#include <format>
#include <istream>
#include <ostream>

#include "common/xanadu/vlace/lower_cards.hpp"
#include "common/xanadu/vlace/lower_context.hpp"
#include "common/xanadu/zigzag/projection_session.hpp"

namespace xanadu::vlace {

Compiler::Compiler(zigzag::vortex::VortexCore &core,
                   zigzag::vortex::VortexVM &vm)
    : core_(core), vm_(vm), state_(std::make_unique<lower::LowerState>()) {}

Compiler::~Compiler() = default;

std::string Compiler::show(const ValuePtr &v) {
  if (!v) return "<null>";
  switch (v->kind) {
  case Value::Kind::Int:
    return std::to_string(v->i);
  case Value::Kind::Float:
    return std::format("{}", v->f);
  case Value::Kind::Str:
    return v->s;
  case Value::Kind::Bool:
    return v->b ? "True" : "False";
  case Value::Kind::Closure:
    return "<function>";
  case Value::Kind::Ctor:
    return v->tag.empty() ? "<ctor>" : v->tag;
  case Value::Kind::Thunk:
    return "<thunk>";
  }
  return "<?>";
}

std::string Compiler::render(const ValuePtr &v, int depthBudget) {
  return lower::renderValue(*this, v, depthBudget);
}

lower::LowerState &Compiler::lowerState() {
  if (!state_->builtinsInstalled()) {
    lower::installBuiltinCtors(*state_);
    state_->markBuiltinsInstalled();
  }
  return *state_;
}

zigzag::ProjectionSession *
Compiler::sessionFor(const zigzag::CellRef cell) const noexcept {
  for (const auto &s : sessions_) {
    if (s->ownsCell(cell)) return s.get();
  }
  return nullptr;
}

std::expected<ValuePtr, EvalError> Compiler::evaluate(const Expr &expr) {
  return lower::evaluateExpressionOnVm(*this, expr);
}

std::expected<ValuePtr, EvalError>
Compiler::evaluateProgram(const Program &prog) {
  return lower::evaluateProgramOnVm(*this, prog);
}

std::expected<ValuePtr, EvalError> Compiler::forceValue(ValuePtr value) {
  return lower::forceValueOnVm(*this, std::move(value));
}

std::expected<ValuePtr, EvalError>
Compiler::runIO(const ValuePtr &action, std::istream &in, std::ostream &out) {
  return lower::runIoOnVm(*this, action, in, out);
}

} // namespace xanadu::vlace
