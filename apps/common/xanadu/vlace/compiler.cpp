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
ValuePtr makeCtor(std::string tag, std::vector<ValuePtr> args) {
  auto v  = std::make_shared<Value>();
  v->kind = Value::Kind::Ctor;
  v->tag  = std::move(tag);
  v->args = std::move(args);
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
  case Value::Kind::Ctor:
    return v->tag.empty() ? "<ctor>" : v->tag;
  case Value::Kind::Thunk:
    return "<thunk>";
  }
  return "<?>";
}
std::string Compiler::render(const ValuePtr &vIn, int depthBudget) {
  if (depthBudget <= 0) return "...";
  auto fv = force(vIn);
  if (!fv) return "<error>";
  const ValuePtr v = *fv;
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
  case Value::Kind::Thunk:
    return "<thunk>"; // force() above should have resolved it
  case Value::Kind::Ctor:
    break;
  }
  // Lists: render Cons/Nil chains as [a, b, c].
  if (v->tag == "Nil") return "[]";
  if (v->tag == "Cons" && v->args.size() == 2) {
    std::string out     = "[";
    const ValuePtr *cur = &v;
    bool first          = true;
    int budget          = depthBudget;
    ValuePtr node       = v;
    while (budget-- > 0) {
      if (node->tag == "Cons" && node->args.size() == 2) {
        if (!first) out += ", ";
        first = false;
        out += render(node->args[0], depthBudget - 1);
        auto tf = force(node->args[1]);
        if (!tf) {
          out += ", <error>";
          break;
        }
        node = *tf;
      } else if (node->tag == "Nil") {
        break;
      } else {
        out += " | " + render(node, depthBudget - 1); // improper tail
        break;
      }
    }
    out += "]";
    (void)cur;
    return out;
  }
  // Tuples: (a, b, ...).
  if (v->tag == "Tuple") {
    std::string out = "(";
    for (std::size_t k = 0; k < v->args.size(); ++k) {
      if (k != 0) out += ", ";
      out += render(v->args[k], depthBudget - 1);
    }
    out += ")";
    return out;
  }
  // General constructor: "Ctor a b".
  std::string out = v->tag;
  for (const auto &a : v->args) {
    out += " " + render(a, depthBudget - 1);
  }
  return out;
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

ValuePtr Compiler::makeThunk(const ExprNode *body, const EnvPtr &env) {
  auto th  = std::make_shared<Thunk>();
  th->body = body;
  th->env  = env;
  auto v   = std::make_shared<Value>();
  v->kind  = Value::Kind::Thunk;
  v->thunk = th;
  return v;
}

std::expected<ValuePtr, EvalError> Compiler::force(ValuePtr v) {
  while (v && v->kind == Value::Kind::Thunk) {
    Thunk &th = *v->thunk;
    if (th.forced) {
      v = th.cached;
      continue;
    }
    auto r = eval(*th.body, th.env);
    if (!r) return r;
    th.forced = true;
    th.cached = *r;
    v         = *r;
  }
  return v;
}

void Compiler::registerData(const DataDecl &dd) {
  for (const auto &c : dd.ctors) {
    const std::size_t arity = c.isRecord ? c.fields.size() : c.argTypes.size();
    ctorArity_[c.name]      = arity;
    if (c.isRecord) {
      // Record a record ctor's ordered field names (§5.7) so named-field
      // placement in construction / functional update can map a field name to
      // its Inputs-Wing position, and so each field name becomes a top-level
      // accessor function C -> fieldType.
      std::vector<std::string> names;
      names.reserve(c.fields.size());
      for (const auto &f : c.fields) {
        names.push_back(f.first);
        fieldAccessor_[f.first] = c.name;
      }
      ctorFields_[c.name] = std::move(names);
    }
  }
}

std::expected<ValuePtr, EvalError>
Compiler::evalRecordUpdate(const ERecordUpdate &ru, const EnvPtr &env,
                           SourceLocation loc) {
  // The record subexpr is either a bare constructor (construction
  // `C { f = e, ... }`, parsed as an update over `ECtor C`) or an existing
  // record value (functional update `r { f = e }`). In both cases we end with
  // a Ctor value whose field args are placed by name.
  const bool bareCtor = std::holds_alternative<ECtor>(ru.record->node);
  std::string tag;
  std::vector<ValuePtr> args;
  if (bareCtor) {
    tag                 = std::get<ECtor>(ru.record->node).name;
    const auto arityIt  = ctorArity_.find(tag);
    const std::size_t n = arityIt == ctorArity_.end() ? 0 : arityIt->second;
    // Start from all-unset slots; named fields fill them below. A field left
    // unnamed in a construction is a program error, surfaced as a missing
    // slot after the loop.
    args.assign(n, nullptr);
  } else {
    auto rE = eval(*ru.record, env);
    if (!rE) return rE;
    auto rF = force(*rE);
    if (!rF) return rF;
    const ValuePtr rec = *rF;
    if (!rec || rec->kind != Value::Kind::Ctor) {
      return fail(CompileError::TypeMismatch,
                  "record update applied to a non-record value", loc);
    }
    tag  = rec->tag;
    args = rec->args; // copy; the original is left unchanged (§5.7)
  }

  const auto fieldsIt = ctorFields_.find(tag);
  if (fieldsIt == ctorFields_.end()) {
    return fail(CompileError::TypeMismatch,
                "'" + tag + "' is not a record constructor", loc);
  }
  const std::vector<std::string> &fieldNames = fieldsIt->second;
  if (args.size() < fieldNames.size()) {
    args.resize(fieldNames.size(), nullptr);
  }

  for (const auto &[name, expr] : ru.fields) {
    std::size_t idx = fieldNames.size();
    for (std::size_t k = 0; k < fieldNames.size(); ++k) {
      if (fieldNames[k] == name) {
        idx = k;
        break;
      }
    }
    if (idx == fieldNames.size()) {
      return fail(CompileError::UnboundIdentifier,
                  "record '" + tag + "' has no field '" + name + "'", loc);
    }
    // Fields stay lazy, like constructor arguments (§4.2): a thunk of the
    // field expression in the current environment.
    args[idx] = makeThunk(expr.get(), env);
  }

  if (bareCtor) {
    for (std::size_t k = 0; k < args.size(); ++k) {
      if (!args[k]) {
        return fail(CompileError::NonExhaustiveMatch,
                    "record construction of '" + tag + "' leaves field '" +
                        fieldNames[k] + "' unset",
                    loc);
      }
    }
  }

  auto v       = makeCtor(tag, std::move(args));
  v->ctorArity = v->args.size();
  return v;
}

std::expected<bool, EvalError>
Compiler::matchPattern(const PatternNode &pat, const ValuePtr &scrutinee,
                       std::unordered_map<std::string, ValuePtr> &out) {
  return std::visit(
      [&](const auto &p) -> std::expected<bool, EvalError> {
        using T = std::remove_cvref_t<decltype(p)>;
        if constexpr (std::is_same_v<T, PWildcard>) {
          return true;
        } else if constexpr (std::is_same_v<T, PVar>) {
          if (out.contains(p.name)) {
            return std::unexpected(
                EvalError{.code   = CompileError::NonLinearPattern,
                          .detail = std::string("'") + p.name +
                                    "' bound twice in one pattern",
                          .loc    = pat.loc});
          }
          out[p.name] = scrutinee;
          return true;
        } else if constexpr (std::is_same_v<T, PLiteral>) {
          auto fv = force(scrutinee);
          if (!fv) return std::unexpected(fv.error());
          const Value &v = **fv;
          switch (p.lit.kind) {
          case LiteralKind::Int:
            return v.kind == Value::Kind::Int && v.i == p.lit.intValue;
          case LiteralKind::Float:
            return v.kind == Value::Kind::Float && v.f == p.lit.floatValue;
          case LiteralKind::String:
            return v.kind == Value::Kind::Str && v.s == p.lit.textValue;
          case LiteralKind::Char:
            return v.kind == Value::Kind::Int && v.i == p.lit.intValue;
          }
          return false;
        } else if constexpr (std::is_same_v<T, PConstructor>) {
          auto fv = force(scrutinee);
          if (!fv) return std::unexpected(fv.error());
          const Value &v = **fv;
          if (v.kind == Value::Kind::Bool) {
            if (p.ctor == "True") return v.b && p.args.empty();
            if (p.ctor == "False") return !v.b && p.args.empty();
            return false;
          }
          if (v.kind != Value::Kind::Ctor || v.tag != p.ctor) {
            return false;
          }
          if (v.args.size() != p.args.size()) {
            return false;
          }
          for (std::size_t k = 0; k < p.args.size(); ++k) {
            auto m = matchPattern(*p.args[k], v.args[k], out);
            if (!m) return m;
            if (!*m) return false;
          }
          return true;
        } else if constexpr (std::is_same_v<T, PTuple>) {
          auto fv = force(scrutinee);
          if (!fv) return std::unexpected(fv.error());
          const Value &v = **fv;
          if (v.kind != Value::Kind::Ctor || v.tag != "Tuple" ||
              v.args.size() != p.elems.size()) {
            return false;
          }
          for (std::size_t k = 0; k < p.elems.size(); ++k) {
            auto m = matchPattern(*p.elems[k], v.args[k], out);
            if (!m) return m;
            if (!*m) return false;
          }
          return true;
        } else {
          auto fv = force(scrutinee);
          if (!fv) return std::unexpected(fv.error());
          const Value &v = **fv;
          if (v.kind != Value::Kind::Ctor || v.tag != p.ctor) {
            return false;
          }
          for (std::size_t k = 0; k < p.fields.size() && k < v.args.size();
               ++k) {
            auto m = matchPattern(*p.fields[k].second, v.args[k], out);
            if (!m) return m;
            if (!*m) return false;
          }
          return true;
        }
      },
      pat.node);
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

  // Cons (:) builds a lazy list cell: the head is evaluated to a thunk and the
  // tail is a thunk, so `n : from (n+1)` makes one Cons and suspends the rest
  // (§9.2 infinite streams).
  if (op.op == ":") {
    std::vector<ValuePtr> args;
    args.push_back(makeThunk(op.lhs.get(), env));
    args.push_back(makeThunk(op.rhs.get(), env));
    auto v       = makeCtor("Cons", std::move(args));
    v->ctorArity = 2;
    return v;
  }

  // && and || short-circuit on Bool (force the left operand).
  if (op.op == "&&" || op.op == "||") {
    auto lE = eval(*op.lhs, env);
    if (!lE) return lE;
    auto l = force(*lE);
    if (!l) return l;
    if ((*l)->kind != Value::Kind::Bool) {
      return fail(CompileError::TypeMismatch,
                  std::string(op.op) + " needs Bool operands", loc);
    }
    if (op.op == "&&" && !(*l)->b) return makeBool(false);
    if (op.op == "||" && (*l)->b) return makeBool(true);
    auto rE = eval(*op.rhs, env);
    if (!rE) return rE;
    auto r = force(*rE);
    if (!r) return r;
    if ((*r)->kind != Value::Kind::Bool) {
      return fail(CompileError::TypeMismatch,
                  std::string(op.op) + " needs Bool operands", loc);
    }
    return makeBool((*r)->b);
  }

  // Arithmetic / comparison / ++ : force both operands to WHNF.
  auto lE = eval(*op.lhs, env);
  if (!lE) return lE;
  auto l = force(*lE);
  if (!l) return l;
  auto rE = eval(*op.rhs, env);
  if (!rE) return rE;
  auto r = force(*rE);
  if (!r) return r;
  return numericOp(op.op, *l, *r, loc);
}

std::expected<ValuePtr, EvalError>
Compiler::apply(const ValuePtr &fnRaw, ValuePtr arg, SourceLocation loc) {
  auto fnF = force(fnRaw);
  if (!fnF) return fnF;
  const ValuePtr fn = *fnF;

  // Applying a constructor accumulates an argument; when saturated it yields
  // the finished Ctor value (§4.2). Constructor args stay lazy (not forced).
  if (fn && fn->kind == Value::Kind::Ctor && fn->args.size() < fn->ctorArity) {
    auto nv = std::make_shared<Value>(*fn);
    nv->args.push_back(std::move(arg));
    return nv;
  }

  if (!fn || fn->kind != Value::Kind::Closure) {
    return fail(CompileError::TypeMismatch, "applying a non-function", loc);
  }
  auto clo = std::make_shared<Closure>(*fn->clo); // copy; accumulate one arg
  clo->argsSoFar.push_back(std::move(arg));
  clo->applied += 1;
  const std::size_t arity = clo->params->size();
  if (clo->applied < arity) {
    auto v  = std::make_shared<Value>();
    v->kind = Value::Kind::Closure;
    v->clo  = clo;
    return v;
  }
  // Fully applied: bind params (as patterns) in a fresh frame over the
  // captured env and evaluate the body. Params may now be any irrefutable
  // pattern; a refutable one that fails is a NonExhaustiveMatch.
  EnvPtr frame = extend(clo->env);
  for (std::size_t k = 0; k < arity; ++k) {
    std::unordered_map<std::string, ValuePtr> binds;
    auto m = matchPattern(*(*clo->params)[k], clo->argsSoFar[k], binds);
    if (!m) return std::unexpected(m.error());
    if (!*m) {
      return fail(CompileError::NonExhaustiveMatch,
                  "lambda argument did not match its parameter pattern",
                  (*clo->params)[k]->loc);
    }
    for (auto &[name, val] : binds) {
      frame->vars[name] = val;
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
            return v; // may be a thunk; forced on demand
          }
          return fail(CompileError::UnboundIdentifier,
                      std::string("unbound identifier '") + node.name + "'",
                      e.loc);
        } else if constexpr (std::is_same_v<T, ECtor>) {
          if (node.name == "True") return makeBool(true);
          if (node.name == "False") return makeBool(false);
          // A known constructor: build a (possibly partial) Ctor value whose
          // ctorArity drives saturation under apply().
          const auto it           = ctorArity_.find(node.name);
          const std::size_t arity = it == ctorArity_.end() ? 0 : it->second;
          auto v                  = makeCtor(node.name, {});
          v->ctorArity = arity; // arity 0 => already a complete value
          return v;
        } else if constexpr (std::is_same_v<T, EBinOp>) {
          return evalBinOp(node, env, e.loc);
        } else if constexpr (std::is_same_v<T, EIf>) {
          auto cE = eval(*node.cond, env);
          if (!cE) return cE;
          auto c = force(*cE);
          if (!c) return c;
          if ((*c)->kind != Value::Kind::Bool) {
            return fail(CompileError::TypeMismatch, "if condition must be Bool",
                        e.loc);
          }
          return (*c)->b ? eval(*node.then, env) : eval(*node.elze, env);
        } else if constexpr (std::is_same_v<T, ELet>) {
          // Bindings become thunks in a fresh frame; letrec and let alike
          // pre-create the frame so a binding's thunk can see its siblings and
          // itself (recursion). Only simple-variable bindings in Stage 4.
          EnvPtr frame = extend(env);
          for (const auto &b : node.bindings) {
            if (!std::holds_alternative<PVar>(b.pattern->node)) {
              return fail(CompileError::NonExhaustiveMatch,
                          "let binds a simple variable (destructuring let is a "
                          "later stage)",
                          b.pattern->loc);
            }
            frame->vars[std::get<PVar>(b.pattern->node).name] =
                makeThunk(b.value.get(), frame);
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
          // A record field accessor `fieldName record` (§5.7): if the callee
          // is a bare field name that is not shadowed by a binding, force the
          // argument to its record Ctor and return the named field's arg.
          if (std::holds_alternative<EVar>(node.fn->node)) {
            const std::string &fn = std::get<EVar>(node.fn->node).name;
            if (!lookup(env, fn)) {
              const auto accIt = fieldAccessor_.find(fn);
              if (accIt != fieldAccessor_.end()) {
                auto aE = eval(*node.arg, env);
                if (!aE) return aE;
                auto aF = force(*aE);
                if (!aF) return aF;
                const ValuePtr rec = *aF;
                if (!rec || rec->kind != Value::Kind::Ctor ||
                    rec->tag != accIt->second) {
                  return fail(CompileError::TypeMismatch,
                              "field accessor '" + fn + "' applied to a non-" +
                                  accIt->second + " value",
                              e.loc);
                }
                const auto &names = ctorFields_[accIt->second];
                for (std::size_t k = 0;
                     k < names.size() && k < rec->args.size(); ++k) {
                  if (names[k] == fn) return rec->args[k];
                }
                return fail(CompileError::UnboundIdentifier,
                            "record has no field '" + fn + "'", e.loc);
              }
            }
          }
          auto fn = eval(*node.fn, env);
          if (!fn) return fn;
          // The argument is passed lazily as a thunk (call-by-need, §6.1).
          ValuePtr arg = makeThunk(node.arg.get(), env);
          return apply(*fn, std::move(arg), e.loc);
        } else if constexpr (std::is_same_v<T, ECase>) {
          // Try each alt in order; the scrutinee is forced only as far as a
          // pattern demands (matchPattern forces per layer). First alt whose
          // pattern matches and whose guard holds wins (§5.4).
          ValuePtr scrut = makeThunk(node.scrutinee.get(), env);
          for (const auto &alt : node.alts) {
            std::unordered_map<std::string, ValuePtr> binds;
            auto m = matchPattern(*alt.pattern, scrut, binds);
            if (!m) return std::unexpected(m.error());
            if (!*m) continue;
            EnvPtr frame = extend(env);
            for (auto &[name, val] : binds) {
              frame->vars[name] = val;
            }
            if (alt.guard.has_value()) {
              auto gE = eval(*alt.guard.value(), frame);
              if (!gE) return gE;
              auto g = force(*gE);
              if (!g) return g;
              if ((*g)->kind != Value::Kind::Bool) {
                return fail(CompileError::TypeMismatch,
                            "case guard must be Bool", e.loc);
              }
              if (!(*g)->b) continue; // guard failed: next alt
            }
            return eval(*alt.body, frame);
          }
          return fail(CompileError::NonExhaustiveMatch,
                      "no case alternative matched the scrutinee", e.loc);
        } else if constexpr (std::is_same_v<T, ETuple>) {
          std::vector<ValuePtr> args;
          args.reserve(node.elems.size());
          for (const auto &el : node.elems) {
            args.push_back(makeThunk(el.get(), env));
          }
          auto v       = makeCtor("Tuple", std::move(args));
          v->ctorArity = node.elems.size();
          return v;
        } else if constexpr (std::is_same_v<T, ERecordUpdate>) {
          return evalRecordUpdate(node, env, e.loc);
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
  // First register all data declarations so constructors resolve regardless of
  // declaration order, then bind top-level value declarations into globals
  // (letrec-style: define the name before evaluating, so functions recurse).
  for (const auto &d : prog.decls) {
    if (std::holds_alternative<DataDecl>(d)) {
      registerData(std::get<DataDecl>(d));
    }
  }
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
