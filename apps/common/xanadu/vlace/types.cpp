/**
 * @file types.cpp
 * @brief Hindley-Milner inference for vlace (§5.5).
 */
#include "common/xanadu/vlace/types.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <ranges>
#include <utility>

namespace xanadu::vlace {

namespace {
std::unexpected<TypeError> terr(CompileError code, std::string detail,
                                SourceLocation loc) {
  return std::unexpected(
      TypeError{.code = code, .detail = std::move(detail), .loc = loc});
}
} // namespace

TypeChecker::TypeChecker() {
  // Built-in list + bool constructors so Nil/Cons/True/False type-check even
  // without an explicit `data` decl (they are the prelude ADTs, §4.6/§7.4).
  // List a = Nil | Cons a (List a)
  const int a        = nextVar_++;
  auto av            = std::make_shared<Ty>();
  av->kind           = Ty::Kind::Var;
  av->varId          = a;
  dataCtors_["List"] = {"Nil", "Cons"};
  ctorData_["Nil"]   = "List";
  ctorData_["Cons"]  = "List";
  // Nil : forall a. List a
  {
    Scheme s;
    s.vars        = {a};
    s.body        = tyList(av);
    ctors_["Nil"] = s;
  }
  // Cons : forall a. a -> List a -> List a
  {
    Scheme s;
    s.vars         = {a};
    s.body         = tyFun(av, tyFun(tyList(av), tyList(av)));
    ctors_["Cons"] = s;
  }
  dataCtors_["Bool"] = {"False", "True"};
  ctorData_["False"] = "Bool";
  ctorData_["True"]  = "Bool";
  ctors_["False"]    = Scheme{.vars = {}, .body = tyBool()};
  ctors_["True"]     = Scheme{.vars = {}, .body = tyBool()};

  // Option a = None | Some a   (§6.6 recoverable absence)
  {
    const int o          = nextVar_++;
    auto ov              = std::make_shared<Ty>();
    ov->kind             = Ty::Kind::Var;
    ov->varId            = o;
    dataCtors_["Option"] = {"None", "Some"};
    ctorData_["None"]    = "Option";
    ctorData_["Some"]    = "Option";
    ctors_["None"]       = Scheme{.vars = {o}, .body = tyOption(ov)};
    ctors_["Some"]       = Scheme{.vars = {o}, .body = tyFun(ov, tyOption(ov))};
  }
  // Result e a = Err e | Ok a   (§6.6 recoverable failure)
  {
    const int ev         = nextVar_++;
    const int av         = nextVar_++;
    auto evt             = std::make_shared<Ty>();
    evt->kind            = Ty::Kind::Var;
    evt->varId           = ev;
    auto avt             = std::make_shared<Ty>();
    avt->kind            = Ty::Kind::Var;
    avt->varId           = av;
    dataCtors_["Result"] = {"Err", "Ok"};
    ctorData_["Err"]     = "Result";
    ctorData_["Ok"]      = "Result";
    ctors_["Err"] =
        Scheme{.vars = {ev, av}, .body = tyFun(evt, tyResult(evt, avt))};
    ctors_["Ok"] =
        Scheme{.vars = {ev, av}, .body = tyFun(avt, tyResult(evt, avt))};
  }

  // Typed IO primitives (§6.5), as value-level builtins:
  //   pure     :: a -> IO a
  //   bind     :: IO a -> (a -> IO b) -> IO b
  //   then     :: IO a -> IO b -> IO b
  //   putStr   :: String -> IO Unit
  //   putStrLn :: String -> IO Unit
  //   getLine  :: IO String
  {
    const int a       = nextVar_++;
    auto av           = std::make_shared<Ty>();
    av->kind          = Ty::Kind::Var;
    av->varId         = a;
    builtins_["pure"] = Scheme{.vars = {a}, .body = tyFun(av, tyIO(av))};
  }
  {
    const int a = nextVar_++;
    const int b = nextVar_++;
    auto av     = std::make_shared<Ty>();
    av->kind    = Ty::Kind::Var;
    av->varId   = a;
    auto bv     = std::make_shared<Ty>();
    bv->kind    = Ty::Kind::Var;
    bv->varId   = b;
    // IO a -> (a -> IO b) -> IO b
    builtins_["bind"] =
        Scheme{.vars = {a, b},
               .body = tyFun(tyIO(av), tyFun(tyFun(av, tyIO(bv)), tyIO(bv)))};
    // IO a -> IO b -> IO b
    builtins_["then"] = Scheme{
        .vars = {a, b}, .body = tyFun(tyIO(av), tyFun(tyIO(bv), tyIO(bv)))};
  }
  builtins_["putStr"] =
      Scheme{.vars = {}, .body = tyFun(tyStr(), tyIO(tyUnit()))};
  builtins_["putStrLn"] =
      Scheme{.vars = {}, .body = tyFun(tyStr(), tyIO(tyUnit()))};
  builtins_["getLine"] = Scheme{.vars = {}, .body = tyIO(tyStr())};

  // std:xanadu authoring verbs (§13). Cell and Dim are opaque reference types;
  // Unit is the result of a side-effecting-looking arena write (still pure --
  // it builds in the arena, mints no op). promote is the one effect: IO Int.
  //   makeText       :: String -> Cell
  //   makeDim        :: String -> Dim
  //   dimensionNamed :: String -> Option Dim
  //   setLink        :: Cell -> Dim -> Cell -> Unit
  //   cellText       :: Cell -> String
  //   linked         :: Cell -> Dim -> Option Cell
  //   promote        :: Cell -> IO Int
  const auto tyCell     = tycon("Cell");
  const auto tyDim      = tycon("Dim");
  builtins_["makeText"] = Scheme{.vars = {}, .body = tyFun(tyStr(), tyCell)};
  builtins_["makeDim"]  = Scheme{.vars = {}, .body = tyFun(tyStr(), tyDim)};
  builtins_["dimensionNamed"] =
      Scheme{.vars = {}, .body = tyFun(tyStr(), tyOption(tyDim))};
  builtins_["setLink"] = Scheme{
      .vars = {}, .body = tyFun(tyCell, tyFun(tyDim, tyFun(tyCell, tyUnit())))};
  builtins_["cellText"] = Scheme{.vars = {}, .body = tyFun(tyCell, tyStr())};
  builtins_["linked"] =
      Scheme{.vars = {}, .body = tyFun(tyCell, tyFun(tyDim, tyOption(tyCell)))};
  builtins_["promote"] =
      Scheme{.vars = {}, .body = tyFun(tyCell, tyIO(tyInt()))};
}

TyPtr TypeChecker::tycon(std::string name, std::vector<TyPtr> args) {
  auto t  = std::make_shared<Ty>();
  t->kind = Ty::Kind::Con;
  t->con  = std::move(name);
  t->args = std::move(args);
  return t;
}
TyPtr TypeChecker::tyInt() { return tycon("Int"); }
TyPtr TypeChecker::tyFloat() { return tycon("Float"); }
TyPtr TypeChecker::tyBool() { return tycon("Bool"); }
TyPtr TypeChecker::tyStr() { return tycon("String"); }
TyPtr TypeChecker::tyChar() { return tycon("Char"); }
TyPtr TypeChecker::tyFun(TyPtr from, TyPtr to) {
  return tycon("->", {std::move(from), std::move(to)});
}
TyPtr TypeChecker::tyList(TyPtr elem) {
  return tycon("List", {std::move(elem)});
}
TyPtr TypeChecker::tyIO(TyPtr inner) { return tycon("IO", {std::move(inner)}); }
TyPtr TypeChecker::tyOption(TyPtr inner) {
  return tycon("Option", {std::move(inner)});
}
TyPtr TypeChecker::tyResult(TyPtr err, TyPtr ok) {
  return tycon("Result", {std::move(err), std::move(ok)});
}
TyPtr TypeChecker::tyUnit() { return tycon("Unit"); }

TyPtr TypeChecker::freshVar() {
  auto t   = std::make_shared<Ty>();
  t->kind  = Ty::Kind::Var;
  t->varId = nextVar_++;
  return t;
}

TyPtr TypeChecker::prune(TyPtr t) {
  // Follow a variable's instance chain, path-compressing as we go.
  while (t && t->kind == Ty::Kind::Var && t->instance) {
    t->instance = prune(t->instance);
    t           = t->instance;
  }
  return t;
}

bool TypeChecker::occurs(int varId, const TyPtr &tIn) {
  TyPtr t = prune(const_cast<TypeChecker *>(this)->prune(tIn));
  if (t->kind == Ty::Kind::Var) {
    return t->varId == varId;
  }
  for (const auto &a : t->args) {
    if (occurs(varId, a)) return true;
  }
  return false;
}

std::expected<void, TypeError> TypeChecker::unify(TyPtr aIn, TyPtr bIn,
                                                  SourceLocation loc) {
  TyPtr a = prune(aIn);
  TyPtr b = prune(bIn);
  if (a->kind == Ty::Kind::Var) {
    if (a->varId != (b->kind == Ty::Kind::Var ? b->varId : -1)) {
      if (occurs(a->varId, b)) {
        return terr(CompileError::OccursCheckFailed,
                    "cannot construct the infinite type " + renderType(a) +
                        " ~ " + renderType(b),
                    loc);
      }
      a->instance = b;
    }
    return {};
  }
  if (b->kind == Ty::Kind::Var) {
    return unify(b, a, loc);
  }
  // Both constructors: names and arities must match, then unify args.
  if (a->con != b->con || a->args.size() != b->args.size()) {
    return terr(CompileError::TypeMismatch,
                "cannot unify " + renderType(a) + " with " + renderType(b),
                loc);
  }
  for (std::size_t i = 0; i < a->args.size(); ++i) {
    auto r = unify(a->args[i], b->args[i], loc);
    if (!r) return r;
  }
  return {};
}

void TypeChecker::freeVars(const TyPtr &tIn, std::vector<int> &out) {
  TyPtr t = prune(tIn);
  if (t->kind == Ty::Kind::Var) {
    if (std::ranges::find(out, t->varId) == out.end()) {
      out.push_back(t->varId);
    }
    return;
  }
  for (const auto &a : t->args) {
    freeVars(a, out);
  }
}

void TypeChecker::freeVarsOfEnv(const EnvPtr &env, std::vector<int> &out) {
  for (const TypeEnv *e = env.get(); e != nullptr; e = e->parent.get()) {
    for (const auto &[name, scheme] : e->vars) {
      std::vector<int> fv;
      freeVars(scheme.body, fv);
      // A variable quantified by the scheme is not free in the environment.
      for (int v : fv) {
        if (std::ranges::find(scheme.vars, v) == scheme.vars.end() &&
            std::ranges::find(out, v) == out.end()) {
          out.push_back(v);
        }
      }
    }
  }
}

TyPtr TypeChecker::instantiate(const Scheme &s) {
  // Replace each quantified variable with a fresh one.
  std::unordered_map<int, TyPtr> subst;
  for (int v : s.vars) {
    subst[v] = freshVar();
  }
  // Recursively copy the body, swapping quantified vars.
  std::function<TyPtr(const TyPtr &)> copy = [&](const TyPtr &tIn) -> TyPtr {
    TyPtr t = prune(tIn);
    if (t->kind == Ty::Kind::Var) {
      const auto it = subst.find(t->varId);
      return it == subst.end() ? t : it->second;
    }
    std::vector<TyPtr> args;
    args.reserve(t->args.size());
    for (const auto &a : t->args) {
      args.push_back(copy(a));
    }
    return tycon(t->con, std::move(args));
  };
  return copy(s.body);
}

Scheme TypeChecker::generalize(const TyPtr &t, const EnvPtr &env) {
  std::vector<int> envFree;
  freeVarsOfEnv(env, envFree);
  std::vector<int> tFree;
  freeVars(t, tFree);
  Scheme s;
  s.body = prune(t);
  for (int v : tFree) {
    if (std::ranges::find(envFree, v) == envFree.end()) {
      s.vars.push_back(v);
    }
  }
  return s;
}

std::string TypeChecker::renderType(const TyPtr &tIn) {
  // prune() is non-static; replicate the follow here without this.
  TyPtr t = tIn;
  while (t && t->kind == Ty::Kind::Var && t->instance) {
    t = t->instance;
  }
  if (!t) return "?";
  if (t->kind == Ty::Kind::Var) {
    // Render variables as a, b, c, ... by id.
    std::string name;
    int id = t->varId;
    name += static_cast<char>('a' + (id % 26));
    if (id >= 26) name += std::to_string(id / 26);
    return name;
  }
  if (t->con == "->" && t->args.size() == 2) {
    const std::string from = renderType(t->args[0]);
    const std::string to   = renderType(t->args[1]);
    // Parenthesize a function on the left of an arrow.
    TyPtr lf = t->args[0];
    while (lf && lf->kind == Ty::Kind::Var && lf->instance) lf = lf->instance;
    const bool lhsFun = lf && lf->kind == Ty::Kind::Con && lf->con == "->";
    return (lhsFun ? "(" + from + ")" : from) + " -> " + to;
  }
  if (t->con == "List" && t->args.size() == 1) {
    return "[" + renderType(t->args[0]) + "]";
  }
  if (t->args.empty()) {
    return t->con;
  }
  std::string out = t->con;
  for (const auto &a : t->args) {
    out += " " + renderType(a);
  }
  return out;
}

void TypeChecker::registerData(const DataDecl &dd) {
  // Build the result type Tname a b ... with fresh vars for the params.
  std::unordered_map<std::string, TyPtr> paramVars;
  std::vector<int> schemeVars;
  std::vector<TyPtr> resultArgs;
  for (const auto &p : dd.params) {
    auto v       = freshVar();
    paramVars[p] = v;
    schemeVars.push_back(v->varId);
    resultArgs.push_back(v);
  }
  const TyPtr resultTy = tycon(dd.name, resultArgs);

  // Translate an AST Type into a Ty using the data decl's param vars.
  std::function<TyPtr(const Type &)> conv = [&](const Type &at) -> TyPtr {
    const TypeNode &n = *at;
    if (std::holds_alternative<TVar>(n.node)) {
      const auto &name = std::get<TVar>(n.node).name;
      const auto it    = paramVars.find(name);
      return it == paramVars.end() ? freshVar() : it->second;
    }
    if (std::holds_alternative<TCon>(n.node)) {
      const auto &c = std::get<TCon>(n.node);
      std::vector<TyPtr> args;
      for (const auto &a : c.args) args.push_back(conv(a));
      return tycon(c.name, std::move(args));
    }
    const auto &fn = std::get<TFun>(n.node);
    return tyFun(conv(fn.from), conv(fn.to));
  };

  std::vector<std::string> names;
  for (const auto &c : dd.ctors) {
    names.push_back(c.name);
    ctorData_[c.name] = dd.name;
    // Build the constructor's function type: arg1 -> arg2 -> ... -> result.
    std::vector<TyPtr> argTys;
    if (c.isRecord) {
      for (const auto &f : c.fields) argTys.push_back(conv(f.second));
    } else {
      for (const auto &a : c.argTypes) argTys.push_back(conv(a));
    }
    TyPtr ty = resultTy;
    for (auto it = argTys.rbegin(); it != argTys.rend(); ++it) {
      ty = tyFun(*it, ty);
    }
    ctors_[c.name] = Scheme{.vars = schemeVars, .body = ty};
    if (c.isRecord) {
      // Each field name is a top-level accessor function C -> fieldType, and
      // the ctor's field names/types drive ERecordUpdate checking (§5.7). The
      // accessor shares the data type's quantified params so it is polymorphic
      // where the data type is.
      std::vector<std::string> fieldNames;
      fieldNames.reserve(c.fields.size());
      for (std::size_t k = 0; k < c.fields.size(); ++k) {
        fieldNames.push_back(c.fields[k].first);
        fieldAccessors_[c.fields[k].first] =
            Scheme{.vars = schemeVars, .body = tyFun(resultTy, argTys[k])};
      }
      recordFields_[c.name] = std::move(fieldNames);
    }
  }
  dataCtors_[dd.name] = names;
}

std::expected<TyPtr, TypeError>
TypeChecker::inferPattern(const PatternNode &p, const EnvPtr &env,
                          TyPtr expected,
                          std::unordered_map<std::string, Scheme> &binds) {
  return std::visit(
      [&](const auto &pat) -> std::expected<TyPtr, TypeError> {
        using T = std::remove_cvref_t<decltype(pat)>;
        if constexpr (std::is_same_v<T, PWildcard>) {
          return expected;
        } else if constexpr (std::is_same_v<T, PVar>) {
          binds[pat.name] = Scheme{.vars = {}, .body = expected};
          return expected;
        } else if constexpr (std::is_same_v<T, PLiteral>) {
          TyPtr lt = pat.lit.kind == LiteralKind::Int      ? tyInt()
                     : pat.lit.kind == LiteralKind::Float  ? tyFloat()
                     : pat.lit.kind == LiteralKind::String ? tyStr()
                                                           : tyChar();
          auto u   = unify(expected, lt, p.loc);
          if (!u) return std::unexpected(u.error());
          return expected;
        } else if constexpr (std::is_same_v<T, PConstructor>) {
          const auto it = ctors_.find(pat.ctor);
          if (it == ctors_.end()) {
            return terr(CompileError::UnboundIdentifier,
                        "unknown constructor '" + pat.ctor + "' in pattern",
                        p.loc);
          }
          TyPtr ct = instantiate(it->second);
          // Peel one function arrow per sub-pattern; the tail is the result.
          for (const auto &sub : pat.args) {
            TyPtr pruned = prune(ct);
            if (pruned->kind != Ty::Kind::Con || pruned->con != "->") {
              return terr(CompileError::TypeMismatch,
                          "constructor '" + pat.ctor +
                              "' applied to too many patterns",
                          p.loc);
            }
            auto sm = inferPattern(*sub, env, pruned->args[0], binds);
            if (!sm) return sm;
            ct = pruned->args[1];
          }
          auto u = unify(expected, ct, p.loc);
          if (!u) return std::unexpected(u.error());
          return expected;
        } else if constexpr (std::is_same_v<T, PTuple>) {
          std::vector<TyPtr> elemTys;
          for (const auto &sub : pat.elems) {
            TyPtr ev = freshVar();
            auto sm  = inferPattern(*sub, env, ev, binds);
            if (!sm) return sm;
            elemTys.push_back(ev);
          }
          auto u = unify(expected, tycon("Tuple", elemTys), p.loc);
          if (!u) return std::unexpected(u.error());
          return expected;
        } else { // PRecord: treat positionally for Stage 5
          const auto it = ctors_.find(pat.ctor);
          if (it == ctors_.end()) {
            return terr(CompileError::UnboundIdentifier,
                        "unknown record constructor '" + pat.ctor + "'", p.loc);
          }
          TyPtr ct = instantiate(it->second);
          for (const auto &f : pat.fields) {
            TyPtr pruned = prune(ct);
            if (pruned->kind != Ty::Kind::Con || pruned->con != "->") break;
            auto sm = inferPattern(*f.second, env, pruned->args[0], binds);
            if (!sm) return sm;
            ct = pruned->args[1];
          }
          auto u = unify(expected, ct, p.loc);
          if (!u) return std::unexpected(u.error());
          return expected;
        }
      },
      p.node);
}

std::expected<TyPtr, TypeError>
TypeChecker::inferRecordUpdate(const ERecordUpdate &ru, const EnvPtr &env,
                               SourceLocation loc) {
  // Determine the record constructor: a bare constructor subexpr is record
  // construction `C { ... }`; otherwise it is a functional update over an
  // existing record value, whose constructor is read from its data type.
  std::string ctorName;
  const bool bareCtor = std::holds_alternative<ECtor>(ru.record->node);
  TyPtr recordTy;
  if (bareCtor) {
    ctorName = std::get<ECtor>(ru.record->node).name;
  } else {
    auto rTy = infer(*ru.record, env);
    if (!rTy) return rTy;
    recordTy = prune(*rTy);
    // The record's data type names its (single) record constructor.
    if (recordTy->kind != Ty::Kind::Con) {
      return terr(CompileError::TypeMismatch,
                  "record update applied to a non-record value", loc);
    }
    const auto dcIt = dataCtors_.find(recordTy->con);
    if (dcIt == dataCtors_.end() || dcIt->second.empty()) {
      return terr(CompileError::TypeMismatch,
                  "'" + recordTy->con + "' is not a record type", loc);
    }
    ctorName = dcIt->second.front();
  }

  const auto fieldsIt = recordFields_.find(ctorName);
  if (fieldsIt == recordFields_.end()) {
    return terr(CompileError::TypeMismatch,
                "'" + ctorName + "' is not a record constructor", loc);
  }
  const std::vector<std::string> &fieldNames = fieldsIt->second;

  // Instantiate the ctor scheme fresh: fieldTy1 -> ... -> fieldTyN -> result.
  const auto ctIt = ctors_.find(ctorName);
  if (ctIt == ctors_.end()) {
    return terr(CompileError::UnboundIdentifier,
                "unknown record constructor '" + ctorName + "'", loc);
  }
  TyPtr walk = instantiate(ctIt->second);
  std::vector<TyPtr> fieldTys;
  fieldTys.reserve(fieldNames.size());
  for (std::size_t k = 0; k < fieldNames.size(); ++k) {
    TyPtr pruned = prune(walk);
    if (pruned->kind != Ty::Kind::Con || pruned->con != "->") {
      return terr(CompileError::TypeMismatch,
                  "record constructor '" + ctorName + "' has fewer fields",
                  loc);
    }
    fieldTys.push_back(pruned->args[0]);
    walk = pruned->args[1];
  }
  const TyPtr resultTy = prune(walk); // the data type, after peeling fields

  if (!bareCtor) {
    auto u = unify(recordTy, resultTy, loc);
    if (!u) return std::unexpected(u.error());
  }

  // Each named field's expression must have that field's type.
  for (const auto &[name, expr] : ru.fields) {
    std::size_t idx = fieldNames.size();
    for (std::size_t k = 0; k < fieldNames.size(); ++k) {
      if (fieldNames[k] == name) {
        idx = k;
        break;
      }
    }
    if (idx == fieldNames.size()) {
      return terr(CompileError::UnboundIdentifier,
                  "record '" + ctorName + "' has no field '" + name + "'", loc);
    }
    auto fTy = infer(*expr, env);
    if (!fTy) return fTy;
    auto u = unify(*fTy, fieldTys[idx], expr->loc);
    if (!u) return std::unexpected(u.error());
  }

  return resultTy;
}

std::expected<TyPtr, TypeError> TypeChecker::infer(const ExprNode &e,
                                                   const EnvPtr &env) {
  auto lookup = [&](const std::string &name) -> const Scheme * {
    for (const TypeEnv *x = env.get(); x != nullptr; x = x->parent.get()) {
      const auto it = x->vars.find(name);
      if (it != x->vars.end()) return &it->second;
    }
    return nullptr;
  };

  return std::visit(
      [&](const auto &node) -> std::expected<TyPtr, TypeError> {
        using T = std::remove_cvref_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ELiteral>) {
          switch (node.lit.kind) {
          case LiteralKind::Int:
            return tyInt();
          case LiteralKind::Float:
            return tyFloat();
          case LiteralKind::String:
            return tyStr();
          case LiteralKind::Char:
            return tyChar();
          }
          return tyInt();
        } else if constexpr (std::is_same_v<T, EVar>) {
          if (const Scheme *s = lookup(node.name)) {
            return instantiate(*s);
          }
          if (const auto it = ctors_.find(node.name); it != ctors_.end()) {
            return instantiate(it->second);
          }
          if (const auto it = fieldAccessors_.find(node.name);
              it != fieldAccessors_.end()) {
            return instantiate(it->second);
          }
          if (const auto it = builtins_.find(node.name);
              it != builtins_.end()) {
            return instantiate(it->second);
          }
          return terr(CompileError::UnboundIdentifier,
                      "unbound identifier '" + node.name + "'", e.loc);
        } else if constexpr (std::is_same_v<T, ECtor>) {
          const auto it = ctors_.find(node.name);
          if (it == ctors_.end()) {
            return terr(CompileError::UnboundIdentifier,
                        "unknown constructor '" + node.name + "'", e.loc);
          }
          return instantiate(it->second);
        } else if constexpr (std::is_same_v<T, ELambda>) {
          EnvPtr frame  = std::make_shared<TypeEnv>();
          frame->parent = env;
          std::vector<TyPtr> paramTys;
          for (const auto &p : node.params) {
            TyPtr pv = freshVar();
            std::unordered_map<std::string, Scheme> binds;
            auto pm = inferPattern(*p, frame, pv, binds);
            if (!pm) return std::unexpected(pm.error());
            for (auto &[n, sc] : binds) frame->vars[n] = sc;
            paramTys.push_back(pv);
          }
          auto bodyTy = infer(*node.body, frame);
          if (!bodyTy) return bodyTy;
          TyPtr ty = *bodyTy;
          for (auto it = paramTys.rbegin(); it != paramTys.rend(); ++it) {
            ty = tyFun(*it, ty);
          }
          return ty;
        } else if constexpr (std::is_same_v<T, EApp>) {
          auto fnTy = infer(*node.fn, env);
          if (!fnTy) return fnTy;
          auto argTy = infer(*node.arg, env);
          if (!argTy) return argTy;
          TyPtr resultTy = freshVar();
          auto u         = unify(*fnTy, tyFun(*argTy, resultTy), e.loc);
          if (!u) return std::unexpected(u.error());
          return resultTy;
        } else if constexpr (std::is_same_v<T, EIf>) {
          auto c = infer(*node.cond, env);
          if (!c) return c;
          auto u1 = unify(*c, tyBool(), e.loc);
          if (!u1) return std::unexpected(u1.error());
          auto th = infer(*node.then, env);
          if (!th) return th;
          auto el = infer(*node.elze, env);
          if (!el) return el;
          auto u2 = unify(*th, *el, e.loc);
          if (!u2) return std::unexpected(u2.error());
          return *th;
        } else if constexpr (std::is_same_v<T, ELet>) {
          EnvPtr frame  = std::make_shared<TypeEnv>();
          frame->parent = env;
          // letrec: pre-bind each name to a fresh monomorphic var so bodies
          // can reference each other, infer values, then generalize (§5.5).
          std::vector<TyPtr> placeholders;
          for (const auto &b : node.bindings) {
            if (!std::holds_alternative<PVar>(b.pattern->node)) {
              return terr(CompileError::TypeMismatch,
                          "let binds a simple variable", b.pattern->loc);
            }
            TyPtr pv = freshVar();
            frame->vars[std::get<PVar>(b.pattern->node).name] =
                Scheme{.vars = {}, .body = pv};
            placeholders.push_back(pv);
          }
          for (std::size_t k = 0; k < node.bindings.size(); ++k) {
            auto vt = infer(*node.bindings[k].value, frame);
            if (!vt) return vt;
            auto u = unify(placeholders[k], *vt, node.bindings[k].value->loc);
            if (!u) return std::unexpected(u.error());
          }
          // Generalize each binding over variables not free in the outer env.
          for (const auto &b : node.bindings) {
            const std::string &nm = std::get<PVar>(b.pattern->node).name;
            frame->vars[nm]       = generalize(frame->vars[nm].body, env);
          }
          return infer(*node.body, frame);
        } else if constexpr (std::is_same_v<T, EBinOp>) {
          const std::string &op = node.op;
          if (op == "$") {
            auto fnTy = infer(*node.lhs, env);
            if (!fnTy) return fnTy;
            auto argTy = infer(*node.rhs, env);
            if (!argTy) return argTy;
            TyPtr r = freshVar();
            auto u  = unify(*fnTy, tyFun(*argTy, r), e.loc);
            if (!u) return std::unexpected(u.error());
            return r;
          }
          if (op == ":") {
            auto h = infer(*node.lhs, env);
            if (!h) return h;
            auto t = infer(*node.rhs, env);
            if (!t) return t;
            auto u = unify(*t, tyList(*h), e.loc);
            if (!u) return std::unexpected(u.error());
            return tyList(*h);
          }
          auto l = infer(*node.lhs, env);
          if (!l) return l;
          auto r = infer(*node.rhs, env);
          if (!r) return r;
          if (op == ">>=") {
            // IO a >>= (a -> IO b) : IO b
            TyPtr a = freshVar();
            TyPtr b = freshVar();
            auto ul = unify(*l, tyIO(a), e.loc);
            if (!ul) return std::unexpected(ul.error());
            auto ur = unify(*r, tyFun(a, tyIO(b)), e.loc);
            if (!ur) return std::unexpected(ur.error());
            return tyIO(b);
          }
          if (op == ">>") {
            // IO a >> IO b : IO b
            TyPtr a = freshVar();
            TyPtr b = freshVar();
            auto ul = unify(*l, tyIO(a), e.loc);
            if (!ul) return std::unexpected(ul.error());
            auto ur = unify(*r, tyIO(b), e.loc);
            if (!ur) return std::unexpected(ur.error());
            return tyIO(b);
          }
          if (op == "&&" || op == "||") {
            auto u1 = unify(*l, tyBool(), e.loc);
            if (!u1) return std::unexpected(u1.error());
            auto u2 = unify(*r, tyBool(), e.loc);
            if (!u2) return std::unexpected(u2.error());
            return tyBool();
          }
          if (op == "++") {
            auto u = unify(*l, *r, e.loc);
            if (!u) return std::unexpected(u.error());
            return *l; // String or List a
          }
          if (op == "==" || op == "/=" || op == "<" || op == "<=" ||
              op == ">" || op == ">=") {
            auto u = unify(*l, *r, e.loc);
            if (!u) return std::unexpected(u.error());
            return tyBool();
          }
          // Arithmetic + - * / %: operands and result share one numeric type.
          // Int and Float are distinct and do not unify, so 1 + 2.0 fails
          // (§4.6).
          auto u = unify(*l, *r, e.loc);
          if (!u) return std::unexpected(u.error());
          return *l;
        } else if constexpr (std::is_same_v<T, ECase>) {
          auto scrut = infer(*node.scrutinee, env);
          if (!scrut) return scrut;
          TyPtr resultTy   = freshVar();
          bool hasCatchAll = false;
          for (const auto &alt : node.alts) {
            EnvPtr frame  = std::make_shared<TypeEnv>();
            frame->parent = env;
            std::unordered_map<std::string, Scheme> binds;
            auto pm = inferPattern(*alt.pattern, frame, *scrut, binds);
            if (!pm) return std::unexpected(pm.error());
            for (auto &[n, sc] : binds) frame->vars[n] = sc;
            if (std::holds_alternative<PWildcard>(alt.pattern->node) ||
                std::holds_alternative<PVar>(alt.pattern->node)) {
              hasCatchAll = true;
            }
            if (alt.guard.has_value()) {
              auto g = infer(*alt.guard.value(), frame);
              if (!g) return g;
              auto ug = unify(*g, tyBool(), e.loc);
              if (!ug) return std::unexpected(ug.error());
            }
            auto bt = infer(*alt.body, frame);
            if (!bt) return bt;
            auto u = unify(resultTy, *bt, e.loc);
            if (!u) return std::unexpected(u.error());
          }
          (void)hasCatchAll;
          return resultTy;
        } else if constexpr (std::is_same_v<T, ERecordUpdate>) {
          return inferRecordUpdate(node, env, e.loc);
        } else {
          return terr(CompileError::TypeMismatch,
                      "this expression form is not yet typed", e.loc);
        }
      },
      e.node);
}

namespace {
// Render a type with its free variables renamed to a, b, c, ... in order of
// first appearance, so inferred polymorphic types read canonically regardless
// of the internal fresh-variable ids.
std::string renderCanonical(const TyPtr &root) {
  std::unordered_map<int, std::string> names;
  int next                                  = 0;
  std::function<TyPtr(const TyPtr &)> prune = [&](const TyPtr &tIn) -> TyPtr {
    TyPtr t = tIn;
    while (t && t->kind == Ty::Kind::Var && t->instance) t = t->instance;
    return t;
  };
  std::function<std::string(const TyPtr &)> go =
      [&](const TyPtr &tIn) -> std::string {
    TyPtr t = prune(tIn);
    if (!t) return "?";
    if (t->kind == Ty::Kind::Var) {
      auto it = names.find(t->varId);
      if (it == names.end()) {
        std::string nm;
        nm += static_cast<char>('a' + (next % 26));
        if (next >= 26) nm += std::to_string(next / 26);
        names[t->varId] = nm;
        ++next;
        return nm;
      }
      return it->second;
    }
    if (t->con == "->" && t->args.size() == 2) {
      TyPtr lf          = prune(t->args[0]);
      const bool lhsFun = lf && lf->kind == Ty::Kind::Con && lf->con == "->";
      const std::string from = go(t->args[0]);
      return (lhsFun ? "(" + from + ")" : from) + " -> " + go(t->args[1]);
    }
    if (t->con == "List" && t->args.size() == 1) {
      return "[" + go(t->args[0]) + "]";
    }
    if (t->args.empty()) return t->con;
    std::string out = t->con;
    for (const auto &a : t->args) out += " " + go(a);
    return out;
  };
  return go(root);
}
} // namespace

std::expected<std::string, TypeError>
TypeChecker::checkExpression(const Expr &expr) {
  auto t = infer(*expr, globals_);
  if (!t) return std::unexpected(t.error());
  return renderCanonical(*t);
}

std::expected<std::string, TypeError>
TypeChecker::checkProgram(const Program &prog) {
  for (const auto &d : prog.decls) {
    if (std::holds_alternative<DataDecl>(d)) {
      registerData(std::get<DataDecl>(d));
    }
  }
  // Bind top-level value decls letrec-style, then generalize.
  std::vector<std::pair<std::string, TyPtr>> placeholders;
  for (const auto &d : prog.decls) {
    if (std::holds_alternative<ValueDecl>(d)) {
      const auto &vd          = std::get<ValueDecl>(d);
      TyPtr pv                = freshVar();
      globals_->vars[vd.name] = Scheme{.vars = {}, .body = pv};
      placeholders.emplace_back(vd.name, pv);
    }
  }
  for (const auto &d : prog.decls) {
    if (!std::holds_alternative<ValueDecl>(d)) continue;
    const auto &vd = std::get<ValueDecl>(d);
    // f p... = body  ==>  infer fn p... => body
    EnvPtr frame  = std::make_shared<TypeEnv>();
    frame->parent = globals_;
    std::vector<TyPtr> paramTys;
    for (const auto &p : vd.params) {
      TyPtr pv = freshVar();
      std::unordered_map<std::string, Scheme> binds;
      auto pm = inferPattern(*p, frame, pv, binds);
      if (!pm) return std::unexpected(pm.error());
      for (auto &[n, sc] : binds) frame->vars[n] = sc;
      paramTys.push_back(pv);
    }
    auto bodyTy = infer(*vd.body, frame);
    if (!bodyTy) return std::unexpected(bodyTy.error());
    TyPtr ty = *bodyTy;
    for (auto it = paramTys.rbegin(); it != paramTys.rend(); ++it) {
      ty = tyFun(*it, ty);
    }
    // Unify with the placeholder, then generalize at the top level.
    for (auto &[nm, pv] : placeholders) {
      if (nm == vd.name) {
        auto u = unify(pv, ty, vd.body->loc);
        if (!u) return std::unexpected(u.error());
      }
    }
  }
  for (auto &[nm, pv] : placeholders) {
    globals_->vars[nm] = generalize(pv, std::make_shared<TypeEnv>());
  }
  if (prog.result.has_value()) {
    auto t = infer(*prog.result.value(), globals_);
    if (!t) return std::unexpected(t.error());
    return renderCanonical(*t);
  }
  return std::string("()");
}

} // namespace xanadu::vlace
