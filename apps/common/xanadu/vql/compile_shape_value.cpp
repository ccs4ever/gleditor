// Q-SHAPEVAR: shape values, shape variables and shape-valued functions.
//
// A shape is syntax, so it is resolved while the query is compiled and lands
// on the static projection paths (`##{ ... }`, project, apply_shape). The
// handle cell a shape value is given exists only so a variable, a result or a
// host binding has something to hold; the VM never reads it. The table that
// maps a handle to its rules is the compiler's and the session's, not the
// graph's (decision D4).
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/contracts.hpp"
#include "common/xanadu/vql/invocation.hpp"

#include <algorithm>
#include <format>
#include <utility>

#include <gleditor/logging.hpp>

namespace xanadu::vql {
using zigzag::CellRef;

namespace {

template <class Action> struct RestoreScope {
  Action action;
  ~RestoreScope() { action(); }
};

[[noreturn]] void refuseShape(const std::string &message) {
  throw QueryRefusal(QueryErrorCategory::Unsupported, message);
}

/// A definition whose whole body is a shape literal: the only definitions the
/// compiler evaluates as shape values. Anything computed is refused by name
/// instead of being guessed at.
const ShapeLiteral *shapeBody(const DefDeclaration &definition) {
  const auto *body = std::get_if<std::shared_ptr<ValueExpr>>(&definition.body);
  if (!body || !*body) return nullptr;
  const auto *literal =
      std::get_if<std::shared_ptr<ShapeLiteral>>(&(*body)->kind);
  return literal && *literal ? literal->get() : nullptr;
}

} // namespace

CellRef VQLCompiler::registerShape(ShapeLiteral expanded) {
  const CellRef handle = core_.arena().makeCell();
  shapeHandles_.emplace(handle, std::move(expanded));
  return handle;
}

void VQLCompiler::appendShapeRules(ShapeLiteral &result,
                                   const ShapeLiteral &from) {
  // Expansion concatenates, so a shape that names itself twice per level
  // doubles each time; the projection ceiling is the one size the compiler
  // already refuses beyond.
  if (result.rules.size() + from.rules.size() > maxCompiledShapeProjectionRules)
    refuseShape(std::format("shape value exceeds maximum size {} rules",
                            maxCompiledShapeProjectionRules));
  result.rules.insert(result.rules.end(), from.rules.begin(), from.rules.end());
}

ShapeLiteral VQLCompiler::shapeOfVariable(const std::string &name) {
  if (functorBindings_.contains(name))
    throw TypeContractError("variable $" + name +
                            " does not contain a Shape value (it is a path "
                            "functor)");
  std::vector<CellRef> cells;
  if (const auto found = streamBindings_.find(name);
      found != streamBindings_.end()) {
    cells = found->second;
  } else if (const auto found = varBindings_.find(name);
             found != varBindings_.end() && found->second) {
    cells.push_back(*found->second);
  } else {
    throw TypeContractError("shape variable $" + name + " is not bound");
  }
  ShapeLiteral result;
  bool known = false;
  for (const CellRef cell : cells) {
    const auto kind = handleKindOf(cell);
    if (kind == HandleKind::None) continue;
    if (kind != HandleKind::Shape)
      throw TypeContractError(
          "variable $" + name + " does not contain a Shape value (it is a " +
          (kind == HandleKind::ViewHandle ? "dynamic view" : "lens") + ")");
    const auto *shape =
        std::get_if<ShapeLiteral>(&shapeHandles_.find(cell)->second);
    known = true;
    appendShapeRules(result, *shape);
  }
  if (!known)
    refuseShape("shape value '$" + name +
                "' is not statically known: it is bound to cells the compiler "
                "did not record as shapes (a value computed at run time, or a "
                "handle from another session)");
  return result;
}

ShapeLiteral VQLCompiler::expandShape(const ShapeLiteral &shape) {
  if (!shapesExpanding_.insert(&shape).second)
    refuseShape("shape cycle: a shape literal reaches itself through a "
                "variable or function it names");
  const RestoreScope leave{[&] { shapesExpanding_.erase(&shape); }};
  ShapeLiteral result;
  for (const ShapeRule &entry : shape.rules) {
    if (const auto *name = std::get_if<std::string>(&entry.rule)) {
      appendShapeRules(result, shapeOfVariable(*name));
    } else if (const auto *nested =
                   std::get_if<std::shared_ptr<ShapeLiteral>>(&entry.rule);
               nested && *nested) {
      appendShapeRules(result, expandShape(**nested));
    } else if (const auto *call =
                   std::get_if<std::shared_ptr<FunctionInvocation>>(
                       &entry.rule);
               call && *call) {
      const auto definition = definitions_.find((*call)->name);
      if (definition == definitions_.end())
        throw TypeContractError("shape function '" + (*call)->name +
                                "' is not defined");
      if (!shapeBody(definition->second))
        throw TypeContractError("function '" + (*call)->name +
                                "' did not return a Shape value");
      appendShapeRules(result, staticShapeOfCall(**call));
    } else {
      if (result.rules.size() >= maxCompiledShapeProjectionRules)
        refuseShape(std::format("shape value exceeds maximum size {} rules",
                                maxCompiledShapeProjectionRules));
      result.rules.push_back(entry);
    }
  }
  return result;
}

ShapeLiteral VQLCompiler::staticShapeOfCall(const FunctionInvocation &call) {
  const auto found = definitions_.find(call.name);
  const ShapeLiteral *body =
      found == definitions_.end() ? nullptr : shapeBody(found->second);
  if (!body)
    throw TypeContractError("function '" + call.name +
                            "' did not return a Shape value");
  const DefDeclaration &definition = found->second;
  requirePositionalArguments(call);
  if (std::ranges::contains(shapeCallsActive_, call.name))
    refuseShape("shape cycle: function '" + call.name +
                "' returns a shape defined in terms of itself");
  // Parameter contracts and defaults are run-time guards on a call; a shape
  // is resolved without running one, so a definition that relies on them is
  // refused rather than silently evaluated without its checks.
  if (definition.returnType)
    refuseShape("shape function '" + call.name +
                "' has a declared return type, which is not checked while "
                "the shape is resolved at compile time");
  if (call.args.size() != definition.params.size())
    refuseShape(std::format("shape function '{}' takes {} argument(s) and "
                            "was called with {}",
                            call.name, definition.params.size(),
                            call.args.size()));
  std::vector<std::pair<std::string, ShapeLiteral>> arguments;
  for (std::size_t index = 0; index < definition.params.size(); ++index) {
    const LambdaParam &parameter = definition.params[index];
    const auto *name             = std::get_if<std::string>(&parameter.target);
    if (!name || parameter.type || parameter.defaultValue ||
        parameter.isVariadic)
      refuseShape("shape function '" + call.name +
                  "' has a typed, defaulted, variadic or destructured "
                  "parameter, which is not evaluated while the shape is "
                  "resolved at compile time");
    auto value = resolveShapeValue(call.args[index]);
    if (!value)
      refuseShape("shape function '" + call.name + "' argument " +
                  std::to_string(index + 1) +
                  " is not a shape known while the query is compiled");
    arguments.emplace_back(*name, std::move(*value));
  }

  // The parameters are bound to handles for the body only, then the caller's
  // scope comes back exactly as it was.
  auto savedStreams   = streamBindings_;
  auto savedVariables = varBindings_;
  auto savedFunctors  = functorBindings_;
  shapeCallsActive_.push_back(call.name);
  const RestoreScope restore{[&] {
    streamBindings_  = std::move(savedStreams);
    varBindings_     = std::move(savedVariables);
    functorBindings_ = std::move(savedFunctors);
    shapeCallsActive_.pop_back();
  }};
  for (auto &[name, value] : arguments) {
    const CellRef handle = registerShape(std::move(value));
    functorBindings_.erase(name);
    streamBindings_[name] = {handle};
    varBindings_[name]    = handle;
  }
  return expandShape(*body);
}

std::optional<ShapeLiteral>
VQLCompiler::resolveShapeValue(const ValueExpr &expression) {
  if (const auto *literal =
          std::get_if<std::shared_ptr<ShapeLiteral>>(&expression.kind))
    return *literal ? std::optional{expandShape(**literal)} : std::nullopt;
  if (const auto *name = std::get_if<std::string>(&expression.kind))
    return shapeOfVariable(*name);
  if (const auto *call =
          std::get_if<std::shared_ptr<FunctionInvocation>>(&expression.kind);
      call && *call) {
    const auto definition = definitions_.find((*call)->name);
    if (definition != definitions_.end() && shapeBody(definition->second))
      return staticShapeOfCall(**call);
  }
  return std::nullopt;
}

std::optional<CellRef>
VQLCompiler::compileShapeCall(const FunctionInvocation &call) {
  const auto definition = definitions_.find(call.name);
  if (definition == definitions_.end() || !shapeBody(definition->second))
    return std::nullopt;
  const CellRef handle = registerShape(staticShapeOfCall(call));
  GLEDITOR_LOG_DEBUG("vql.compile", "shape function '{}' resolved to handle {}",
                     call.name, static_cast<std::uint64_t>(handle));
  return handle;
}

std::optional<CellRef>
VQLCompiler::compileShapeValue(const ShapeLiteral &shape,
                               const CellRef /*ctxCell*/) {
  return registerShape(expandShape(shape));
}

} // namespace xanadu::vql
