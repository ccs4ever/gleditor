// Q-SHAPEVAR: shape values, shape variables and shape-valued functions.
//
// A shape is syntax, so it is resolved while the query is compiled and lands
// on the static projection paths (`##{ ... }`, project, apply_shape). The
// handle cell a shape value is given holds the rules as cells too
// (compile_shape_cells.cpp), but the VM does not read them yet: the static
// paths read the table that maps a handle to its expanded rules, which is the
// compiler's and the session's, not the graph's (decision D4).
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/contracts.hpp"
#include "common/xanadu/vql/invocation.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>
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

bool VQLCompiler::returnsShapeGiven(const DefDeclaration &definition,
                                    const std::vector<bool> &argumentIsShape,
                                    const std::size_t depth) const {
  constexpr std::size_t maxFollowDepth = 64;
  if (depth > maxFollowDepth) return false;
  const auto *body = std::get_if<std::shared_ptr<ValueExpr>>(&definition.body);
  if (!body || !*body) return false;
  const ValueExpr &expression = **body;
  if (std::holds_alternative<std::shared_ptr<ShapeLiteral>>(expression.kind))
    return true;
  // What an expression in this definition's body is, given which parameters
  // were handed a shape: a literal, a parameter that was, or a call of a
  // definition that returns one in turn.
  const auto isShape = [&](const ValueExpr &value) {
    if (std::holds_alternative<std::shared_ptr<ShapeLiteral>>(value.kind))
      return true;
    const auto *name = std::get_if<std::string>(&value.kind);
    if (!name) return false;
    const std::string bare = name->starts_with('$') ? name->substr(1) : *name;
    for (std::size_t index = 0; index < definition.params.size(); ++index) {
      const auto *parameter =
          std::get_if<std::string>(&definition.params[index].target);
      if (parameter && *parameter == bare)
        return index < argumentIsShape.size() && argumentIsShape[index];
    }
    return false;
  };
  if (std::holds_alternative<std::string>(expression.kind))
    return isShape(expression);
  const auto *call =
      std::get_if<std::shared_ptr<FunctionInvocation>>(&expression.kind);
  if (!call || !*call) return false;
  const auto inner = definitions_.find((*call)->name);
  if (inner == definitions_.end()) return false;
  std::vector<bool> flags;
  for (const ValueExpr &argument : (*call)->args)
    flags.push_back(isShape(argument));
  return returnsShapeGiven(inner->second, flags, depth + 1);
}

bool VQLCompiler::callReturnsShape(const FunctionInvocation &call) const {
  const auto found = definitions_.find(call.name);
  if (found == definitions_.end()) return false;
  std::vector<bool> flags;
  for (const ValueExpr &argument : call.args)
    flags.push_back(argumentNamesShape(argument));
  return returnsShapeGiven(found->second, flags, 0);
}

void VQLCompiler::appendShapeRules(ShapeLiteral &result,
                                   const ShapeLiteral &from) {
  // Expansion concatenates, so a shape that names itself twice per level
  // doubles each time; the projection ceiling is the one size the compiler
  // already refuses beyond.
  if (result.rules.size() + from.rules.size() > shapeLimits_.maxRules)
    refuseShape(std::format("shape value exceeds maximum size {} rules",
                            shapeLimits_.maxRules));
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
  bool other = false;
  for (const CellRef cell : cells) {
    const auto kind = handleKindOf(cell);
    if (kind == HandleKind::None) {
      other = true;
      continue;
    }
    appendShapeRules(result, shapeOfHandle(cell, name));
    known = true;
  }
  if (!known)
    refuseShape("shape value '$" + name +
                "' is not statically known: it is bound to cells the compiler "
                "did not record as shapes (a value computed at run time, or a "
                "handle from another session)");
  if (other)
    refuseShape("shape value '$" + name +
                "' is bound to a mix of shape handles and other values; a "
                "shape cannot be taken from part of a stream");
  return result;
}

const ShapeLiteral &VQLCompiler::shapeOfHandle(const CellRef cell,
                                               const std::string &name) {
  switch (handleKindOf(cell)) {
  case HandleKind::Shape:
    // The table's key is a number, and a number can name another cell after a
    // release: the handle's own witness says whether this is the cell the
    // shape was made as.
    if (const auto dims = findShapeDimensions(core_))
      if (const auto stale = staleShapeReason(core_.arena(), *dims, cell))
        refuseShape(*stale + " (shape value '$" + name + "')");
    // A shape made in a loop or a function body keeps its computed targets as
    // expressions, read where this query consumes it. A later query has no
    // such scope: its free names would be read in whatever scope it has.
    if (const auto dims = findShapeDimensions(core_);
        dims && !lateHandles_.contains(cell) &&
        hasLateRule(core_.arena(), *dims, cell))
      refuseShape("shape value '$" + name +
                  "' was made in a loop or a function body of another query, "
                  "where its computed target is read at each use; make the "
                  "shape in the query that uses it");
    return std::get<ShapeLiteral>(shapeHandles_.find(cell)->second);
  case HandleKind::UnrecordedShape:
    refuseShape(
        "shape value '$" + name +
        "' is not statically known: it is a shape handle the compiler "
        "has no record of (the session was cleared, its cell was released, "
        "or it came from another session or engine)");
  case HandleKind::ViewHandle:
    throw TypeContractError("variable $" + name +
                            " does not contain a Shape value (it is a "
                            "dynamic view)");
  case HandleKind::Lens:
    throw TypeContractError("variable $" + name +
                            " does not contain a Shape value (it is a lens)");
  case HandleKind::None:
    break;
  }
  throw std::logic_error("shapeOfHandle was given a plain cell");
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
      if (!callReturnsShape(**call))
        throw TypeContractError("function '" + (*call)->name +
                                "' did not return a Shape value");
      appendShapeRules(result, staticShapeOfCall(**call));
    } else {
      if (result.rules.size() >= shapeLimits_.maxRules)
        refuseShape(std::format("shape value exceeds maximum size {} rules",
                                shapeLimits_.maxRules));
      result.rules.push_back(entry);
    }
  }
  return result;
}

ShapeLiteral VQLCompiler::staticShapeOfCall(const FunctionInvocation &call) {
  const auto found = definitions_.find(call.name);
  if (found == definitions_.end() || !callReturnsShape(call))
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
    const CellRef handle = registerShape(std::move(value), ShapeMint::Binding);
    functorBindings_.erase(name);
    streamBindings_[name] = {handle};
    varBindings_[name]    = handle;
  }
  // A literal body expands; a body that names a parameter or calls another
  // shape-returning definition resolves in the scope just bound.
  if (const auto *literal = shapeBody(definition)) return expandShape(*literal);
  const auto &expression =
      *std::get<std::shared_ptr<ValueExpr>>(definition.body);
  if (auto resolved = resolveShapeValue(expression))
    return std::move(*resolved);
  throw TypeContractError("function '" + call.name +
                          "' did not return a Shape value");
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
    if (callReturnsShape(**call)) return staticShapeOfCall(**call);
  }
  return std::nullopt;
}

std::optional<CellRef>
VQLCompiler::compileShapeCall(const FunctionInvocation &call) {
  if (!callReturnsShape(call)) return std::nullopt;
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
