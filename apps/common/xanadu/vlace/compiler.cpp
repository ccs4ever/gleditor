/**
 * @file compiler.cpp
 * @brief vlace Stage 3 evaluator over the Vortex core.
 */
#include "common/xanadu/vlace/compiler.hpp"

#include <format>
#include <istream>
#include <optional>
#include <ostream>
#include <utility>

#include "common/xanadu/zigzag/filesystem_lens.hpp"
#include "common/xanadu/zigzag/projection_session.hpp"

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
  case Value::Kind::Native:
    return v->nativeName.empty() ? "<native>"
                                 : "<native " + v->nativeName + ">";
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
  case Value::Kind::Native:
    return v->nativeName.empty() ? "<native>"
                                 : "<native " + v->nativeName + ">";
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
    core_.arena().consumeWork();
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

  // Monadic sequencing (§6.5): >>= is bind, >> is then. Both build an IO
  // *description* (a tagged Ctor), performing nothing here -- runIO performs it
  // at the boundary. The operands are evaluated to WHNF IO values eagerly (an
  // IO value is a constructor cell), but their effects do not run.
  if (op.op == ">>=" || op.op == ">>") {
    auto lE = eval(*op.lhs, env);
    if (!lE) return lE;
    auto rE = eval(*op.rhs, env);
    if (!rE) return rE;
    auto v       = makeCtor(op.op == ">>=" ? "IO.Bind" : "IO.Then", {*lE, *rE});
    v->ctorArity = 2;
    return v;
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

  // Applying a native built-in accumulates an argument; when saturated it
  // invokes the C++ body with the collected args (§13 std:xanadu verbs).
  if (fn && fn->kind == Value::Kind::Native) {
    auto nv = std::make_shared<Value>(*fn);
    nv->args.push_back(std::move(arg));
    if (nv->args.size() < nv->nativeArity) return nv; // still partial
    return (*nv->native)(*this, nv->args);
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
  core_.arena().consumeWork();
  if (core_.arena().executionLimited() && evaluationDepth_ >= 512)
    throw std::runtime_error("Vlace evaluation nesting budget exhausted");
  struct DepthGuard {
    std::size_t &depth;
    explicit DepthGuard(std::size_t &value) : depth(value) { ++depth; }
    ~DepthGuard() { --depth; }
  } guard(evaluationDepth_);
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
  // Install the typed-IO primitives and Option/Result constructors into the
  // global environment before user declarations, so a program may reference
  // pure/bind/putStr/... and None/Some/Err/Ok without an explicit import.
  installBuiltins();
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

namespace {
// Build a native built-in value of the given arity.
ValuePtr makeNative(std::string name, std::size_t arity, NativeFn fn) {
  auto v         = std::make_shared<Value>();
  v->kind        = Value::Kind::Native;
  v->nativeName  = std::move(name);
  v->nativeArity = arity;
  v->native      = std::make_shared<NativeFn>(std::move(fn));
  return v;
}
// Unwrap a `Cell n` / `Dim n` tagged Ctor to its numeric ref.
std::optional<std::uint32_t> refOfWrapped(const ValuePtr &v,
                                          std::string_view tag) {
  if (!v || v->kind != Value::Kind::Ctor || v->tag != tag ||
      v->args.size() != 1)
    return std::nullopt;
  const ValuePtr &inner = v->args[0];
  if (!inner || inner->kind != Value::Kind::Int) return std::nullopt;
  return static_cast<std::uint32_t>(inner->i);
}
} // namespace

void Compiler::installXanadu() {
  using zigzag::CellRef;
  using zigzag::DimRef;
  // A CellRef/DimRef is surfaced to vlace as a `Cell n` / `Dim n` value so the
  // type system (and pattern matching) keep them distinct from a bare Int.
  auto wrapCell = [](CellRef r) {
    auto inner   = std::make_shared<Value>();
    inner->kind  = Value::Kind::Int;
    inner->i     = static_cast<std::int64_t>(r);
    auto v       = makeCtor("Cell", {inner});
    v->ctorArity = 1;
    return v;
  };
  auto wrapDim = [](DimRef r) {
    auto inner   = std::make_shared<Value>();
    inner->kind  = Value::Kind::Int;
    inner->i     = static_cast<std::int64_t>(r);
    auto v       = makeCtor("Dim", {inner});
    v->ctorArity = 1;
    return v;
  };
  ctorArity_["Cell"] = 1;
  ctorArity_["Dim"]  = 1;

  // makeText :: String -> Cell  -- mint an arena text cell (pure authoring:
  // builds in the ephemeral arena, mints no persistent op, §13.1).
  globals_->vars["makeText"] =
      makeNative("makeText", 1,
                 [wrapCell](Compiler &c, std::vector<ValuePtr> &args)
                     -> std::expected<ValuePtr, EvalError> {
                   auto s = c.force(args[0]);
                   if (!s) return s;
                   if ((*s)->kind != Value::Kind::Str)
                     return fail(CompileError::TypeMismatch,
                                 "makeText: argument is not a String", {});
                   CellRef ref = c.core().arena().makeCell((*s)->s);
                   return wrapCell(ref);
                 });

  // makeDim :: String -> Dim  -- ensure a named dimension (idempotent).
  globals_->vars["makeDim"] =
      makeNative("makeDim", 1,
                 [wrapDim](Compiler &c, std::vector<ValuePtr> &args)
                     -> std::expected<ValuePtr, EvalError> {
                   auto s = c.force(args[0]);
                   if (!s) return s;
                   if ((*s)->kind != Value::Kind::Str)
                     return fail(CompileError::TypeMismatch,
                                 "makeDim: argument is not a String", {});
                   DimRef d = c.core().arena().ensureDimension((*s)->s);
                   return wrapDim(d);
                 });

  // dimensionNamed :: String -> Option Dim  -- look up an existing dimension.
  globals_->vars["dimensionNamed"] = makeNative(
      "dimensionNamed", 1,
      [wrapDim](Compiler &c, std::vector<ValuePtr> &args)
          -> std::expected<ValuePtr, EvalError> {
        auto s = c.force(args[0]);
        if (!s) return s;
        if ((*s)->kind != Value::Kind::Str)
          return fail(CompileError::TypeMismatch,
                      "dimensionNamed: argument is not a String", {});
        DimRef d = c.core().arena().ensureDimension((*s)->s);
        // ensureDimension always yields a dim; treat noCell as absence.
        if (d == zigzag::noCell) {
          auto none       = makeCtor("None", {});
          none->ctorArity = 0;
          return none;
        }
        auto some       = makeCtor("Some", {wrapDim(d)});
        some->ctorArity = 1;
        return some;
      });

  // setLink :: Cell -> Dim -> Cell -> Unit  -- link `from` posward along `dim`
  // to `to`. The engine writes BOTH reciprocal edges (bidirectional by
  // construction, §13.4 example 2), so no second write is needed.
  globals_->vars["setLink"] = makeNative(
      "setLink", 3,
      [](Compiler &c,
         std::vector<ValuePtr> &args) -> std::expected<ValuePtr, EvalError> {
        auto fromV = c.force(args[0]);
        if (!fromV) return fromV;
        auto dimV = c.force(args[1]);
        if (!dimV) return dimV;
        auto toV = c.force(args[2]);
        if (!toV) return toV;
        auto from = refOfWrapped(*fromV, "Cell");
        auto dim  = refOfWrapped(*dimV, "Dim");
        auto to   = refOfWrapped(*toV, "Cell");
        if (!from || !dim || !to)
          return fail(CompileError::TypeMismatch,
                      "setLink: expects (Cell) (Dim) (Cell)", {});
        auto lr = c.core().arena().link(*from, *dim, zigzag::Posward,
                                        std::optional<CellRef>(*to));
        if (!lr)
          return fail(CompileError::TypeMismatch,
                      "setLink: arena refused the link", {});
        auto u       = makeCtor("Unit", {});
        u->ctorArity = 0;
        return u;
      });

  // cellText :: Cell -> String  -- read a cell's content as text (a read of a
  // frozen arena cell is pure, §13.3; local scratch content resolves at once).
  globals_->vars["cellText"] = makeNative(
      "cellText", 1,
      [](Compiler &c,
         std::vector<ValuePtr> &args) -> std::expected<ValuePtr, EvalError> {
        auto cv = c.force(args[0]);
        if (!cv) return cv;
        auto ref = refOfWrapped(*cv, "Cell");
        if (!ref)
          return fail(CompileError::TypeMismatch,
                      "cellText: argument is not a Cell", {});
        auto out  = std::make_shared<Value>();
        out->kind = Value::Kind::Str;
        out->s    = c.core().arena().textOf(*ref);
        return out;
      });

  // linked :: Cell -> Dim -> Option Cell  -- the posward neighbour on `dim`,
  // the one-step read that `walk` and navigation build on (§13.4 example 5).
  globals_->vars["linked"] =
      makeNative("linked", 2,
                 [wrapCell](Compiler &c, std::vector<ValuePtr> &args)
                     -> std::expected<ValuePtr, EvalError> {
                   auto fromV = c.force(args[0]);
                   if (!fromV) return fromV;
                   auto dimV = c.force(args[1]);
                   if (!dimV) return dimV;
                   auto from = refOfWrapped(*fromV, "Cell");
                   auto dim  = refOfWrapped(*dimV, "Dim");
                   if (!from || !dim)
                     return fail(CompileError::TypeMismatch,
                                 "linked: expects (Cell) (Dim)", {});
                   auto nb =
                       c.core().arena().linked(*from, *dim, zigzag::Posward);
                   if (!nb.has_value()) {
                     auto none       = makeCtor("None", {});
                     none->ctorArity = 0;
                     return none;
                   }
                   auto some       = makeCtor("Some", {wrapCell(*nb)});
                   some->ctorArity = 1;
                   return some;
                 });

  // childrenAll :: Cell -> List Cell  -- the contained children of a cell: its
  // d.contains first child, then every d.seq sibling after it. One native
  // because it walks two ranks; a program folds over the resulting list. (The
  // rank model is one posward neighbour per dim, so children are the first
  // child plus the sibling chain, §7.4.)
  globals_->vars["childrenAll"] = makeNative(
      "childrenAll", 1,
      [wrapCell](Compiler &c, std::vector<ValuePtr> &args)
          -> std::expected<ValuePtr, EvalError> {
        auto cv = c.force(args[0]);
        if (!cv) return cv;
        auto parent = refOfWrapped(*cv, "Cell");
        if (!parent)
          return fail(CompileError::TypeMismatch,
                      "childrenAll: argument is not a Cell", {});
        auto &arena      = c.core().arena();
        DimRef dContains = c.core().findOrMintDimension(std::string(
            zigzag::genericDimName(zigzag::GenericDim::Containment)));
        DimRef dSeq      = c.core().findOrMintDimension(
            std::string(zigzag::genericDimName(zigzag::GenericDim::Sequence)));
        std::vector<zigzag::CellRef> kids;
        auto first = arena.linked(*parent, dContains, zigzag::Posward);
        for (auto cur = first; cur.has_value();
             cur      = arena.linked(*cur, dSeq, zigzag::Posward)) {
          kids.push_back(*cur);
          if (kids.size() > 1'000'000) break; // guard a cyclic rank
        }
        // Build a vlace list [c0, c1, ...] from the back (Nil then Cons).
        auto nil       = makeCtor("Nil", {});
        nil->ctorArity = 0;
        ValuePtr acc   = nil;
        for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
          std::vector<ValuePtr> consArgs;
          consArgs.push_back(wrapCell(*it));
          consArgs.push_back(acc);
          auto cons       = makeCtor("Cons", std::move(consArgs));
          cons->ctorArity = 2;
          acc             = cons;
        }
        return acc;
      });

  // words :: String -> List String  -- split on ASCII whitespace, dropping
  // empties. The one tokenization primitive the prelude cannot derive because
  // vlace has no char-level string access (§7.4). Lowercases nothing; a program
  // composes it with other string ops.
  globals_->vars["words"] = makeNative(
      "words", 1,
      [](Compiler &c,
         std::vector<ValuePtr> &args) -> std::expected<ValuePtr, EvalError> {
        auto sv = c.force(args[0]);
        if (!sv) return sv;
        if ((*sv)->kind != Value::Kind::Str)
          return fail(CompileError::TypeMismatch,
                      "words: argument is not a String", {});
        const std::string &s = (*sv)->s;
        std::vector<std::string> toks;
        std::string cur;
        auto isWs = [](char ch) {
          return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
                 ch == '\f' || ch == '\v';
        };
        for (char ch : s) {
          if (isWs(ch)) {
            if (!cur.empty()) {
              toks.push_back(cur);
              cur.clear();
            }
          } else {
            cur.push_back(ch);
          }
        }
        if (!cur.empty()) toks.push_back(cur);
        auto nil       = makeCtor("Nil", {});
        nil->ctorArity = 0;
        ValuePtr acc   = nil;
        for (auto it = toks.rbegin(); it != toks.rend(); ++it) {
          auto strv  = std::make_shared<Value>();
          strv->kind = Value::Kind::Str;
          strv->s    = *it;
          std::vector<ValuePtr> consArgs;
          consArgs.push_back(strv);
          consArgs.push_back(acc);
          auto cons       = makeCtor("Cons", std::move(consArgs));
          cons->ctorArity = 2;
          acc             = cons;
        }
        return acc;
      });

  // promote :: Cell -> IO Int  -- THE authoring effect (§13.1): an IO
  // description that, run at the boundary, crosses the pure arena to
  // persistence. A driverless (no-Store) run reports the reachable subgraph
  // size (the op count a real promote would write) rather than performing a
  // store write; a Store-backed runner performs the real zigzag::promote.
  {
    auto v                    = makeCtor("IO.Promote", {});
    v->ctorArity              = 1;
    globals_->vars["promote"] = v;
  }

  // openLens :: String -> IO Cell  -- project an external source (a directory
  // path) through its lens into THIS evaluator's arena, returning the root
  // cell (§7.4). An IO effect because it reads the external world; performed by
  // runIO, which builds the lens + session and keeps the session alive.
  {
    auto v                     = makeCtor("IO.OpenLens", {});
    v->ctorArity               = 1;
    globals_->vars["openLens"] = v;
  }
}

void Compiler::installBuiltins() {
  // IO primitives are description-builders: a tagged Ctor whose arity is the
  // number of arguments it collects via apply() saturation (§4.2). Building
  // one performs no effect; runIO interprets the resulting tree at the
  // boundary (§6.5). A fresh Ctor with the given tag + arity, args empty.
  auto prim = [](const std::string &tag, std::size_t arity) {
    auto v       = makeCtor(tag, {});
    v->ctorArity = arity;
    return v;
  };
  // pure :: a -> IO a ; bind (>>=) :: IO a -> (a -> IO b) -> IO b ;
  // then (>>) :: IO a -> IO b -> IO b .
  globals_->vars["pure"]     = prim("IO.Pure", 1);
  globals_->vars["bind"]     = prim("IO.Bind", 2);
  globals_->vars["then"]     = prim("IO.Then", 2);
  globals_->vars["putStr"]   = prim("IO.PutStr", 1);
  globals_->vars["putStrLn"] = prim("IO.PutStrLn", 1);
  globals_->vars["getLine"]  = prim("IO.GetLine", 0); // arity 0 => a value
  // Option / Result (§6.6). None/Ok etc. are ordinary ADT constructors; expose
  // them as (possibly partial) Ctor values so a program need not `data`-declare
  // them. ctorArity_ entries let pattern matching check arity.
  globals_->vars["None"] = prim("None", 0);
  globals_->vars["Some"] = prim("Some", 1);
  globals_->vars["Err"]  = prim("Err", 1);
  globals_->vars["Ok"]   = prim("Ok", 1);
  ctorArity_["None"]     = 0;
  ctorArity_["Some"]     = 1;
  ctorArity_["Err"]      = 1;
  ctorArity_["Ok"]       = 1;

  installXanadu();
}

std::expected<ValuePtr, EvalError>
Compiler::runIO(const ValuePtr &action, std::istream &in, std::ostream &out) {
  // Interpret the IO description tree, performing effects left to right. The
  // result of running an action is its "wrapped" value a (what `pure a`
  // carries); bind threads it into the continuation. This is the one place
  // effects happen (§6.5); everything upstream only built the description.
  auto cur = force(action);
  if (!cur) return cur;
  ValuePtr v = *cur;
  if (!v || v->kind != Value::Kind::Ctor) {
    return fail(CompileError::TypeMismatch,
                "runIO: not an IO action (did the program return an IO value?)",
                {});
  }
  const std::string &tag = v->tag;
  if (tag == "IO.Pure") {
    return v->args.empty() ? makeCtor("Unit", {}) : v->args[0];
  }
  if (tag == "IO.PutStr" || tag == "IO.PutStrLn") {
    auto sE = force(v->args.at(0));
    if (!sE) return sE;
    if ((*sE)->kind != Value::Kind::Str) {
      return fail(CompileError::TypeMismatch,
                  "putStr: argument is not a String", {});
    }
    out << (*sE)->s;
    if (tag == "IO.PutStrLn") out << '\n';
    return makeCtor("Unit", {});
  }
  if (tag == "IO.GetLine") {
    std::string line;
    std::getline(in, line);
    return makeStr(std::move(line));
  }
  if (tag == "IO.Promote") {
    // THE authoring effect (§13.1). Without a backing Store (the driver's REPL
    // arena is store-less), performing it cannot write the spool; it reports
    // the op count a real promote would emit -- the number of arena cells
    // currently minted -- as a faithful dry-run. A Store-backed embedder
    // overrides this to call zigzag::promote and return the new version.
    (void)v->args; // the root cell; a store-backed run walks from it
    const auto n = static_cast<std::int64_t>(core_.arena().cells().size());
    return makeInt(n);
  }
  if (tag == "IO.OpenLens") {
    // Project an external source into this evaluator's arena and return the
    // root cell (§7.4). The path is the forced string argument. Currently the
    // only lens is the FilesystemLens (a directory path); a URI scheme could
    // dispatch others. The session is retained so the projection outlives the
    // program that reads it.
    auto pE = force(v->args.at(0));
    if (!pE) return pE;
    if ((*pE)->kind != Value::Kind::Str) {
      return fail(CompileError::TypeMismatch,
                  "openLens: argument is not a String", {});
    }
    auto session = std::make_shared<zigzag::ProjectionSession>(
        std::make_unique<zigzag::FilesystemLens>((*pE)->s), core_);
    sessions_.push_back(session);
    if (session->roots().empty()) {
      // An unreadable / empty source: None would be more honest, but openLens
      // is typed IO Cell; return a bare cell so a program can detect emptiness
      // via cellText == "". A richer IO (Option Cell) is a later refinement.
      auto inner      = std::make_shared<Value>();
      inner->kind     = Value::Kind::Int;
      inner->i        = 0;
      auto cell       = makeCtor("Cell", {inner});
      cell->ctorArity = 1;
      return cell;
    }
    auto inner      = std::make_shared<Value>();
    inner->kind     = Value::Kind::Int;
    inner->i        = static_cast<std::int64_t>(session->roots().front());
    auto cell       = makeCtor("Cell", {inner});
    cell->ctorArity = 1;
    return cell;
  }
  if (tag == "IO.Then") {
    // Perform the first action for its effect, discard its value, run second.
    auto first = runIO(v->args.at(0), in, out);
    if (!first) return first;
    return runIO(v->args.at(1), in, out);
  }
  if (tag == "IO.Bind") {
    // Perform the first action, feed its value to the continuation, run that.
    auto first = runIO(v->args.at(0), in, out);
    if (!first) return first;
    auto next = apply(v->args.at(1), *first, {});
    if (!next) return next;
    return runIO(*next, in, out);
  }
  return fail(CompileError::TypeMismatch,
              std::string("runIO: unknown IO node '") + tag + "'", {});
}

} // namespace xanadu::vlace
