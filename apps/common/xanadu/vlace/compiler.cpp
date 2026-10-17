/**
 * @file compiler.cpp
 * @brief vlace Stage 3 evaluator over the Vortex core.
 */
#include "common/xanadu/vlace/compiler.hpp"

#include <format>
#include <utility>

namespace xanadu::vlace {

using zigzag::CellRef;
using zigzag::vortex::CellValue;
using zigzag::vortex::OpcodeKind;

namespace {

ValuePtr makeInt(std::int64_t i) {
  auto v  = std::make_shared<Value>();
  v->kind = Value::Kind::Int;
  v->i    = i;
  return v;
}
ValuePtr makeFloat(double f) {
  auto v  = std::make_shared<Value>();
  v->kind = Value::Kind::Float;
  v->f    = f;
  return v;
}
ValuePtr makeStr(std::string s) {
  auto v  = std::make_shared<Value>();
  v->kind = Value::Kind::Str;
  v->s    = std::move(s);
  return v;
}
ValuePtr makeBool(bool b) {
  auto v  = std::make_shared<Value>();
  v->kind = Value::Kind::Bool;
  v->b    = b;
  return v;
}

std::unexpected<EvalError> fail(CompileError code, std::string detail,
                                SourceLocation loc) {
  return std::unexpected(
      EvalError{.code = code, .detail = std::move(detail), .loc = loc});
}

ValuePtr lookup(const EnvPtr &env, const std::string &name) {
  for (const Env *e = env.get(); e != nullptr; e = e->parent.get()) {
    const auto it = e->vars.find(name);
    if (it != e->vars.end()) {
      return it->second;
    }
  }
  return nullptr;
}

EnvPtr extend(const EnvPtr &parent) {
  auto e    = std::make_shared<Env>();
  e->parent = parent;
  return e;
}

} // namespace

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
  }
  return "<?>";
}

std::expected<ValuePtr, EvalError> Compiler::numericOp(std::string_view op,
                                                       const ValuePtr &lhs,
                                                       const ValuePtr &rhs,
                                                       SourceLocation loc) {
  // Comparisons produce Bool; arithmetic produces Int or Float. Both read the
  // operands' scalar bits -- here directly, matching what the VM's arithmetic
  // opcodes compute over R6 scalar cells. (Using the opcode path end to end is
  // what Stage 4+ does once values live as cells; Stage 3 evaluates eagerly.)
  const bool bothInt =
      lhs->kind == Value::Kind::Int && rhs->kind == Value::Kind::Int;
  const bool numeric =
      (lhs->kind == Value::Kind::Int || lhs->kind == Value::Kind::Float) &&
      (rhs->kind == Value::Kind::Int || rhs->kind == Value::Kind::Float);

  auto asF = [](const ValuePtr &v) {
    return v->kind == Value::Kind::Int ? static_cast<double>(v->i) : v->f;
  };

  // Comparisons.
  if (op == "==" || op == "/=" || op == "<" || op == "<=" || op == ">" ||
      op == ">=") {
    if (numeric) {
      const double a = asF(lhs);
      const double b = asF(rhs);
      bool r         = false;
      if (op == "==")
        r = a == b;
      else if (op == "/=")
        r = a != b;
      else if (op == "<")
        r = a < b;
      else if (op == "<=")
        r = a <= b;
      else if (op == ">")
        r = a > b;
      else
        r = a >= b;
      return makeBool(r);
    }
    if (lhs->kind == Value::Kind::Str && rhs->kind == Value::Kind::Str) {
      bool r = (op == "==")   ? (lhs->s == rhs->s)
               : (op == "/=") ? (lhs->s != rhs->s)
                              : false;
      if (op == "==" || op == "/=") return makeBool(r);
    }
    return fail(CompileError::TypeMismatch,
                std::string("comparison '") + std::string(op) +
                    "' needs matching comparable operands",
                loc);
  }

  // String concatenation with ++.
  if (op == "++") {
    if (lhs->kind == Value::Kind::Str && rhs->kind == Value::Kind::Str) {
      return makeStr(lhs->s + rhs->s);
    }
    return fail(CompileError::TypeMismatch, "++ needs two strings", loc);
  }

  if (!numeric) {
    return fail(CompileError::TypeMismatch,
                std::string("operator '") + std::string(op) +
                    "' needs numeric operands",
                loc);
  }

  // Arithmetic through scalar cells + the VM's arithmetic opcode: mint the two
  // operands as scalar cells, assemble the opcode, step it once, read the
  // result. This exercises the Vortex arithmetic path (R6 bits) rather than a
  // bare C++ computation.
  OpcodeKind kind{};
  if (op == "+")
    kind = OpcodeKind::Add;
  else if (op == "-")
    kind = OpcodeKind::Sub;
  else if (op == "*")
    kind = OpcodeKind::Mul;
  else if (op == "/")
    kind = OpcodeKind::Div;
  else if (op == "%")
    kind = OpcodeKind::Mod;
  else
    return fail(CompileError::UnexpectedToken,
                std::string("unknown operator '") + std::string(op) + "'", loc);

  CellRef lc     = bothInt ? core_.arena().makeScalarCell(lhs->i)
                           : core_.arena().makeScalarCell(asF(lhs));
  CellRef rc     = bothInt ? core_.arena().makeScalarCell(rhs->i)
                           : core_.arena().makeScalarCell(asF(rhs));
  CellRef opCell = vm_.mintOpcode(kind, "#vlace_arith");
  core_.bindInput(opCell, lc);
  core_.bindInput(opCell, rc);
  CellRef out = core_.arena().makeCell();
  core_.bindOutput(opCell, out);
  CellRef cursor = vm_.spawnCursor(opCell, "vlace_arith");
  const auto res = vm_.step(cursor);
  if (!res.success) {
    return fail(CompileError::TypeMismatch, "arithmetic opcode failed", loc);
  }
  const CellValue rv = core_.render(out);
  if (std::holds_alternative<std::int64_t>(rv)) {
    return makeInt(std::get<std::int64_t>(rv));
  }
  if (std::holds_alternative<double>(rv)) {
    return makeFloat(std::get<double>(rv));
  }
  return fail(CompileError::TypeMismatch, "arithmetic produced non-number",
              loc);
}

std::expected<ValuePtr, EvalError>
Compiler::evalBinOp(const EBinOp &op, const EnvPtr &env, SourceLocation loc) {
  // $ is low-precedence application: a $ b == a b.
  if (op.op == "$") {
    auto fn = eval(*op.lhs, env);
    if (!fn) return fn;
    auto arg = eval(*op.rhs, env);
    if (!arg) return arg;
    return apply(*fn, *arg, loc);
  }
  // && and || short-circuit on Bool.
  if (op.op == "&&" || op.op == "||") {
    auto l = eval(*op.lhs, env);
    if (!l) return l;
    if ((*l)->kind != Value::Kind::Bool) {
      return fail(CompileError::TypeMismatch,
                  std::string(op.op) + " needs Bool operands", loc);
    }
    if (op.op == "&&" && !(*l)->b) return makeBool(false);
    if (op.op == "||" && (*l)->b) return makeBool(true);
    auto r = eval(*op.rhs, env);
    if (!r) return r;
    if ((*r)->kind != Value::Kind::Bool) {
      return fail(CompileError::TypeMismatch,
                  std::string(op.op) + " needs Bool operands", loc);
    }
    return makeBool((*r)->b);
  }

  auto l = eval(*op.lhs, env);
  if (!l) return l;
  auto r = eval(*op.rhs, env);
  if (!r) return r;
  return numericOp(op.op, *l, *r, loc);
}

std::expected<ValuePtr, EvalError>
Compiler::apply(const ValuePtr &fn, ValuePtr arg, SourceLocation loc) {
  if (!fn || fn->kind != Value::Kind::Closure) {
    return fail(CompileError::TypeMismatch, "applying a non-function", loc);
  }
  auto clo = std::make_shared<Closure>(*fn->clo); // copy; accumulate one arg
  clo->argsSoFar.push_back(std::move(arg));
  clo->applied += 1;
  const std::size_t arity = clo->params->size();
  if (clo->applied < arity) {
    // Partial application: return a new closure value.
    auto v  = std::make_shared<Value>();
    v->kind = Value::Kind::Closure;
    v->clo  = clo;
    return v;
  }
  // Fully applied: bind params in a fresh frame over the captured env and
  // evaluate the body. Stage 3 params are simple variables or wildcards.
  EnvPtr frame = extend(clo->env);
  for (std::size_t k = 0; k < arity; ++k) {
    const PatternNode &p = *(*clo->params)[k];
    if (std::holds_alternative<PVar>(p.node)) {
      frame->vars[std::get<PVar>(p.node).name] = clo->argsSoFar[k];
    } else if (std::holds_alternative<PWildcard>(p.node)) {
      // discard
    } else {
      return fail(CompileError::NonExhaustiveMatch,
                  "Stage 3 lambda params must be variables or _", p.loc);
    }
  }
  return eval(*clo->body, frame);
}

std::expected<ValuePtr, EvalError> Compiler::eval(const ExprNode &e,
                                                  const EnvPtr &env) {
  return std::visit(
      [&](const auto &node) -> std::expected<ValuePtr, EvalError> {
        using T = std::remove_cvref_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ELiteral>) {
          switch (node.lit.kind) {
          case LiteralKind::Int:
            return makeInt(node.lit.intValue);
          case LiteralKind::Float:
            return makeFloat(node.lit.floatValue);
          case LiteralKind::String:
            return makeStr(node.lit.textValue);
          case LiteralKind::Char:
            return makeInt(node.lit.intValue);
          }
          return makeInt(0);
        } else if constexpr (std::is_same_v<T, EVar>) {
          if (ValuePtr v = lookup(env, node.name)) {
            return v;
          }
          // Built-in constructors True / False are represented as Bool.
          return fail(CompileError::UnboundIdentifier,
                      std::string("unbound identifier '") + node.name + "'",
                      e.loc);
        } else if constexpr (std::is_same_v<T, ECtor>) {
          if (node.name == "True") return makeBool(true);
          if (node.name == "False") return makeBool(false);
          return fail(CompileError::UnboundIdentifier,
                      std::string("unknown constructor '") + node.name +
                          "' (ADTs arrive in Stage 4)",
                      e.loc);
        } else if constexpr (std::is_same_v<T, EBinOp>) {
          return evalBinOp(node, env, e.loc);
        } else if constexpr (std::is_same_v<T, EIf>) {
          auto c = eval(*node.cond, env);
          if (!c) return c;
          if ((*c)->kind != Value::Kind::Bool) {
            return fail(CompileError::TypeMismatch, "if condition must be Bool",
                        e.loc);
          }
          return (*c)->b ? eval(*node.then, env) : eval(*node.elze, env);
        } else if constexpr (std::is_same_v<T, ELet>) {
          EnvPtr frame = extend(env);
          // letrec: pre-bind names so bodies can refer to each other. Stage 3
          // supports recursive *functions* (the common case); a recursive
          // non-function value is not forced here.
          for (const auto &b : node.bindings) {
            if (!std::holds_alternative<PVar>(b.pattern->node)) {
              return fail(CompileError::NonExhaustiveMatch,
                          "Stage 3 let binds a simple variable",
                          b.pattern->loc);
            }
          }
          for (const auto &b : node.bindings) {
            auto val = eval(*b.value, frame);
            if (!val) return val;
            frame->vars[std::get<PVar>(b.pattern->node).name] = *val;
          }
          return eval(*node.body, frame);
        } else if constexpr (std::is_same_v<T, ELambda>) {
          auto clo    = std::make_shared<Closure>();
          clo->params = &node.params;
          clo->body   = node.body.get();
          clo->env    = env;
          auto v      = std::make_shared<Value>();
          v->kind     = Value::Kind::Closure;
          v->clo      = clo;
          return v;
        } else if constexpr (std::is_same_v<T, EApp>) {
          auto fn = eval(*node.fn, env);
          if (!fn) return fn;
          auto arg = eval(*node.arg, env);
          if (!arg) return arg;
          return apply(*fn, *arg, e.loc);
        } else {
          return fail(CompileError::UnexpectedToken,
                      "this expression form arrives in a later stage", e.loc);
        }
      },
      e.node);
}

std::expected<ValuePtr, EvalError> Compiler::evaluate(const Expr &expr) {
  return eval(*expr, globals_);
}

std::expected<ValuePtr, EvalError>
Compiler::evaluateProgram(const Program &prog) {
  // Bind top-level value declarations into globals (letrec-style: define the
  // name before evaluating, so functions can recurse / refer to each other).
  for (const auto &d : prog.decls) {
    if (std::holds_alternative<ValueDecl>(d)) {
      const auto &vd = std::get<ValueDecl>(d);
      // f p... = body  ==>  f = fn p... => body
      if (vd.params.empty()) {
        auto val = eval(*vd.body, globals_);
        if (!val) return val;
        globals_->vars[vd.name] = *val;
      } else {
        auto clo    = std::make_shared<Closure>();
        clo->params = &vd.params;
        clo->body   = vd.body.get();
        clo->env    = globals_; // captures globals, so recursion resolves
        auto v      = std::make_shared<Value>();
        v->kind     = Value::Kind::Closure;
        v->clo      = clo;
        globals_->vars[vd.name] = v;
      }
    }
    // data/type/import/fixity decls carry no Stage 3 runtime effect.
  }
  if (prog.result.has_value()) {
    return eval(*prog.result.value(), globals_);
  }
  return makeInt(0);
}

} // namespace xanadu::vlace
