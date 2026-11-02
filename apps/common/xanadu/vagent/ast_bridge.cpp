#include "common/xanadu/vagent/ast_bridge.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "common/xanadu/vagent/runtime/compiler.hpp"

namespace xanadu::vagent {
namespace {

std::vector<std::string> splitModulePath(std::string_view str) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start < str.size()) {
    auto pos = str.find(':', start);
    if (pos == std::string_view::npos) {
      parts.emplace_back(str.substr(start));
      break;
    }
    parts.emplace_back(str.substr(start, pos - start));
    start = pos + 1;
  }
  return parts;
}

std::string joinModulePath(const std::vector<std::string> &parts) {
  std::string res;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) res += ':';
    res += parts[i];
  }
  return res;
}

std::int64_t parseInteger(std::string text) {
  std::erase(text, '_');
  std::string_view sv = text;
  int radix           = 10;
  bool negative       = false;
  if (sv.starts_with('-')) {
    negative = true;
    sv.remove_prefix(1);
  } else if (sv.starts_with('+')) {
    sv.remove_prefix(1);
  }
  if (sv.starts_with("0x") || sv.starts_with("0X")) {
    radix = 16;
    sv.remove_prefix(2);
  } else if (sv.starts_with("0b") || sv.starts_with("0B")) {
    radix = 2;
    sv.remove_prefix(2);
  } else if (sv.starts_with("0o") || sv.starts_with("0O")) {
    radix = 8;
    sv.remove_prefix(2);
  }
  std::int64_t val = 0;
  auto res = std::from_chars(sv.data(), sv.data() + sv.size(), val, radix);
  if (res.ec == std::errc{}) {
    return negative ? -val : val;
  }
  return 0;
}

double parseFloat(std::string text) {
  std::erase(text, '_');
  double val = 0.0;
  auto res   = std::from_chars(text.data(), text.data() + text.size(), val);
  if (res.ec == std::errc{}) {
    return val;
  }
  return 0.0;
}

std::int64_t parseDurationMs(std::string text) {
  std::erase(text, '_');
  std::size_t unitPos = text.find_first_not_of("0123456789.+-eE");
  if (unitPos == std::string::npos) return 0;
  std::string numStr  = text.substr(0, unitPos);
  std::string unitStr = text.substr(unitPos);
  double number       = 0.0;
  auto res =
      std::from_chars(numStr.data(), numStr.data() + numStr.size(), number);
  if (res.ec != std::errc{}) return 0;
  if (unitStr == "ms") {
    return static_cast<std::int64_t>(std::llround(number));
  }
  if (unitStr == "s") {
    return static_cast<std::int64_t>(std::llround(number * 1000.0));
  }
  if (unitStr == "min") {
    return static_cast<std::int64_t>(std::llround(number * 60000.0));
  }
  return static_cast<std::int64_t>(std::llround(number));
}

std::string formatDurationMs(std::int64_t ms) {
  if (ms > 0 && ms % 60000 == 0) {
    return std::to_string(ms / 60000) + "min";
  }
  if (ms > 0 && ms % 1000 == 0) {
    return std::to_string(ms / 1000) + "s";
  }
  return std::to_string(ms) + "ms";
}

std::optional<runtime::EBinary::Op> toRuntimeBinaryOp(std::string_view op) {
  if (op == "+") return runtime::EBinary::Op::Add;
  if (op == "-") return runtime::EBinary::Op::Sub;
  if (op == "*") return runtime::EBinary::Op::Mul;
  if (op == "/") return runtime::EBinary::Op::Div;
  if (op == "%") return runtime::EBinary::Op::Mod;
  if (op == "++") return runtime::EBinary::Op::Concat;
  if (op == "<") return runtime::EBinary::Op::Lt;
  if (op == "<=") return runtime::EBinary::Op::Le;
  if (op == ">") return runtime::EBinary::Op::Gt;
  if (op == ">=") return runtime::EBinary::Op::Ge;
  if (op == "==") return runtime::EBinary::Op::Eq;
  if (op == "!=") return runtime::EBinary::Op::Ne;
  if (op == "|") return runtime::EBinary::Op::Alt;
  if (op == "?" || op == "scan") return runtime::EBinary::Op::Scan;
  if (op == ":=") return runtime::EBinary::Op::Assign;
  if (op == "<-") return runtime::EBinary::Op::RevAssign;
  if (op == "&") return runtime::EBinary::Op::And;
  return std::nullopt;
}

std::string_view toFrontendBinaryOp(runtime::EBinary::Op op) {
  switch (op) {
  case runtime::EBinary::Op::Add:
    return "+";
  case runtime::EBinary::Op::Sub:
    return "-";
  case runtime::EBinary::Op::Mul:
    return "*";
  case runtime::EBinary::Op::Div:
    return "/";
  case runtime::EBinary::Op::Mod:
    return "%";
  case runtime::EBinary::Op::Concat:
    return "++";
  case runtime::EBinary::Op::Lt:
    return "<";
  case runtime::EBinary::Op::Le:
    return "<=";
  case runtime::EBinary::Op::Gt:
    return ">";
  case runtime::EBinary::Op::Ge:
    return ">=";
  case runtime::EBinary::Op::Eq:
    return "==";
  case runtime::EBinary::Op::Ne:
    return "!=";
  case runtime::EBinary::Op::Alt:
    return "|";
  case runtime::EBinary::Op::Scan:
    return "?";
  case runtime::EBinary::Op::Assign:
    return ":=";
  case runtime::EBinary::Op::RevAssign:
    return "<-";
  case runtime::EBinary::Op::And:
    return "&";
  }
  return "";
}

std::optional<runtime::EUnary::Op> toRuntimeUnaryOp(std::string_view op) {
  if (op == "-") return runtime::EUnary::Op::Neg;
  if (op == "^" || op == "generate") return runtime::EUnary::Op::Generate;
  if (op == "~" || op == "size") return runtime::EUnary::Op::Size;
  if (op == "not" || op == "!") return runtime::EUnary::Op::Not;
  return std::nullopt;
}

std::string_view toFrontendUnaryOp(runtime::EUnary::Op op) {
  switch (op) {
  case runtime::EUnary::Op::Neg:
    return "-";
  case runtime::EUnary::Op::Generate:
    return "^";
  case runtime::EUnary::Op::Size:
    return "size";
  case runtime::EUnary::Op::Not:
    return "not";
  }
  return "";
}

std::optional<runtime::ESpecial::Kind> toRuntimeSpecial(std::string_view name) {
  if (name == "self") return runtime::ESpecial::Kind::Self;
  if (name == "parent") return runtime::ESpecial::Kind::Parent;
  if (name == "inner") return runtime::ESpecial::Kind::Inner;
  if (name == "outer") return runtime::ESpecial::Kind::Outer;
  if (name == "occurrence") return runtime::ESpecial::Kind::Occurrence;
  return std::nullopt;
}

std::string_view toFrontendSpecial(runtime::ESpecial::Kind kind) {
  switch (kind) {
  case runtime::ESpecial::Kind::Self:
    return "self";
  case runtime::ESpecial::Kind::Parent:
    return "parent";
  case runtime::ESpecial::Kind::Inner:
    return "inner";
  case runtime::ESpecial::Kind::Outer:
    return "outer";
  case runtime::ESpecial::Kind::Occurrence:
    return "occurrence";
  }
  return "";
}

} // namespace

// ============================================================================
// Types
// ============================================================================

runtime::TypeExpr lowerType(const Type &type) {
  if (type.name == "()") {
    return runtime::TypeExpr{.kind = runtime::TypeExpr::Kind::Unit,
                             .name = "",
                             .args = {},
                             .loc  = {}};
  }
  if (type.name == "List" && type.arguments.size() == 1) {
    std::vector<runtime::TypeExpr> args;
    args.push_back(lowerType(type.arguments[0]));
    return runtime::TypeExpr{.kind = runtime::TypeExpr::Kind::List,
                             .name = "",
                             .args = std::move(args),
                             .loc  = {}};
  }
  std::vector<runtime::TypeExpr> args;
  args.reserve(type.arguments.size());
  for (const auto &arg : type.arguments) {
    args.push_back(lowerType(arg));
  }
  return runtime::TypeExpr{.kind = runtime::TypeExpr::Kind::Named,
                           .name = type.name,
                           .args = std::move(args),
                           .loc  = {}};
}

Type liftType(const runtime::TypeExpr &typeExpr) {
  if (typeExpr.kind == runtime::TypeExpr::Kind::Unit) {
    return Type{"()", {}};
  }
  if (typeExpr.kind == runtime::TypeExpr::Kind::List) {
    std::vector<Type> args;
    if (!typeExpr.args.empty()) {
      args.push_back(liftType(typeExpr.args[0]));
    }
    return Type{"List", std::move(args)};
  }
  std::vector<Type> args;
  args.reserve(typeExpr.args.size());
  for (const auto &arg : typeExpr.args) {
    args.push_back(liftType(arg));
  }
  return Type{typeExpr.name, std::move(args)};
}

bool areEquivalent(const Type &frontend, const runtime::TypeExpr &runtime) {
  return liftType(runtime) == frontend;
}

// ============================================================================
// Terms and Goal Calls
// ============================================================================

std::expected<runtime::Term, Diagnostic> lowerTerm(const Expr &expr) {
  if (expr.kind != ExprKind::Term && expr.kind != ExprKind::Call &&
      expr.kind != ExprKind::Identifier) {
    return std::unexpected(Diagnostic{DiagnosticCode::Syntax,
                                      "expected a predicate term", expr.loc});
  }
  runtime::Term term;
  term.loc = expr.loc;
  if (expr.kind == ExprKind::Identifier) {
    term.name = expr.text;
    return term;
  }
  if (expr.kind == ExprKind::Call) {
    if (expr.children.empty()) {
      return std::unexpected(
          Diagnostic{DiagnosticCode::Syntax, "empty call in term", expr.loc});
    }
    term.name = expr.children[0]->text;
    for (std::size_t i = 1; i < expr.children.size(); ++i) {
      const auto &child = expr.children[i];
      if (child->kind == ExprKind::LogicVariable) {
        term.args.push_back(
            runtime::TermArg{.logicVar = child->text, .expr = nullptr});
      } else {
        auto lowerArg = lowerExpr(*child);
        if (!lowerArg) return std::unexpected(lowerArg.error());
        term.args.push_back(runtime::TermArg{.logicVar = std::nullopt,
                                             .expr     = std::move(*lowerArg)});
      }
    }
    return term;
  }
  // ExprKind::Term
  term.name = expr.text;
  for (const auto &child : expr.children) {
    if (child->kind == ExprKind::LogicVariable) {
      term.args.push_back(
          runtime::TermArg{.logicVar = child->text, .expr = nullptr});
    } else {
      auto lowerArg = lowerExpr(*child);
      if (!lowerArg) return std::unexpected(lowerArg.error());
      term.args.push_back(runtime::TermArg{.logicVar = std::nullopt,
                                           .expr     = std::move(*lowerArg)});
    }
  }
  return term;
}

ExprPtr liftTerm(const runtime::Term &term) {
  auto expr  = std::make_unique<Expr>();
  expr->kind = ExprKind::Term;
  expr->text = term.name;
  expr->loc  = term.loc;
  for (const auto &arg : term.args) {
    if (arg.logicVar.has_value()) {
      auto varExpr  = std::make_unique<Expr>();
      varExpr->kind = ExprKind::LogicVariable;
      varExpr->text = *arg.logicVar;
      expr->children.push_back(std::move(varExpr));
    } else if (arg.expr) {
      auto lifted = liftExpr(*arg.expr);
      if (lifted) {
        expr->children.push_back(std::move(*lifted));
      }
    }
  }
  return expr;
}

std::expected<runtime::GoalCall, Diagnostic> lowerGoalCall(const Expr &expr) {
  runtime::GoalCall gc;
  gc.loc = expr.loc;
  if (expr.kind == ExprKind::Identifier) {
    gc.name = expr.text;
    return gc;
  }
  if (expr.kind == ExprKind::Call) {
    if (expr.children.empty()) {
      return std::unexpected(
          Diagnostic{DiagnosticCode::Syntax, "empty goal call", expr.loc});
    }
    gc.name = expr.children[0]->text;
    for (std::size_t i = 1; i < expr.children.size(); ++i) {
      auto arg = lowerExpr(*expr.children[i]);
      if (!arg) return std::unexpected(arg.error());
      gc.args.push_back(std::move(*arg));
    }
    return gc;
  }
  return std::unexpected(
      Diagnostic{DiagnosticCode::Syntax, "expected a goal call", expr.loc});
}

ExprPtr liftGoalCall(const runtime::GoalCall &call) {
  auto expr  = std::make_unique<Expr>();
  expr->kind = ExprKind::Call;
  expr->loc  = call.loc;
  auto id    = std::make_unique<Expr>();
  id->kind   = ExprKind::Identifier;
  id->text   = call.name;
  id->loc    = call.loc;
  expr->children.push_back(std::move(id));
  for (const auto &arg : call.args) {
    if (arg) {
      auto lifted = liftExpr(*arg);
      if (lifted) {
        expr->children.push_back(std::move(*lifted));
      }
    }
  }
  return expr;
}

// ============================================================================
// Expressions
// ============================================================================

std::expected<runtime::ExprPtr, Diagnostic> lowerExpr(const Expr &expr) {
  switch (expr.kind) {
  case ExprKind::Integer: {
    runtime::ELiteral lit;
    lit.kind     = runtime::ELiteral::Kind::Int;
    lit.intValue = parseInteger(expr.text);
    lit.text     = expr.text;
    return std::make_unique<runtime::Expr>(lit, expr.loc);
  }
  case ExprKind::Float: {
    runtime::ELiteral lit;
    lit.kind       = runtime::ELiteral::Kind::Float;
    lit.floatValue = parseFloat(expr.text);
    lit.text       = expr.text;
    return std::make_unique<runtime::Expr>(lit, expr.loc);
  }
  case ExprKind::String: {
    runtime::ELiteral lit;
    lit.kind = runtime::ELiteral::Kind::Str;
    lit.text = expr.text;
    return std::make_unique<runtime::Expr>(lit, expr.loc);
  }
  case ExprKind::Boolean: {
    runtime::ELiteral lit;
    lit.kind      = runtime::ELiteral::Kind::Bool;
    lit.boolValue = (expr.text == "true");
    lit.text      = expr.text;
    return std::make_unique<runtime::Expr>(lit, expr.loc);
  }
  case ExprKind::Duration: {
    runtime::ELiteral lit;
    lit.kind       = runtime::ELiteral::Kind::Duration;
    lit.durationMs = parseDurationMs(expr.text);
    lit.text       = expr.text;
    return std::make_unique<runtime::Expr>(lit, expr.loc);
  }
  case ExprKind::Unit: {
    runtime::ELiteral lit;
    lit.kind = runtime::ELiteral::Kind::Unit;
    return std::make_unique<runtime::Expr>(lit, expr.loc);
  }
  case ExprKind::None: {
    runtime::ELiteral lit;
    lit.kind = runtime::ELiteral::Kind::None;
    return std::make_unique<runtime::Expr>(lit, expr.loc);
  }
  case ExprKind::Identifier: {
    if (auto spec = toRuntimeSpecial(expr.text)) {
      return std::make_unique<runtime::Expr>(runtime::ESpecial{*spec},
                                             expr.loc);
    }
    return std::make_unique<runtime::Expr>(runtime::EName{expr.text}, expr.loc);
  }
  case ExprKind::Unary: {
    auto op = toRuntimeUnaryOp(expr.text);
    if (!op)
      return std::unexpected(Diagnostic{DiagnosticCode::Syntax,
                                        "unknown unary operator: " + expr.text,
                                        expr.loc});
    if (expr.children.empty())
      return std::unexpected(Diagnostic{DiagnosticCode::Syntax,
                                        "missing unary operand", expr.loc});
    auto operand = lowerExpr(*expr.children[0]);
    if (!operand) return std::unexpected(operand.error());
    return std::make_unique<runtime::Expr>(
        runtime::EUnary{*op, std::move(*operand)}, expr.loc);
  }
  case ExprKind::Binary: {
    auto op = toRuntimeBinaryOp(expr.text);
    if (!op)
      return std::unexpected(Diagnostic{DiagnosticCode::Syntax,
                                        "unknown binary operator: " + expr.text,
                                        expr.loc});
    if (expr.children.size() < 2)
      return std::unexpected(Diagnostic{DiagnosticCode::Syntax,
                                        "missing binary operands", expr.loc});
    auto left = lowerExpr(*expr.children[0]);
    if (!left) return std::unexpected(left.error());
    auto right = lowerExpr(*expr.children[1]);
    if (!right) return std::unexpected(right.error());
    return std::make_unique<runtime::Expr>(
        runtime::EBinary{*op, std::move(*left), std::move(*right)}, expr.loc);
  }
  case ExprKind::Range: {
    if (expr.children.size() < 2)
      return std::unexpected(Diagnostic{DiagnosticCode::Syntax,
                                        "range requires lower and upper bounds",
                                        expr.loc});
    auto lo = lowerExpr(*expr.children[0]);
    if (!lo) return std::unexpected(lo.error());
    auto hi = lowerExpr(*expr.children[1]);
    if (!hi) return std::unexpected(hi.error());
    runtime::ExprPtr by = nullptr;
    if (expr.children.size() > 2) {
      auto b = lowerExpr(*expr.children[2]);
      if (!b) return std::unexpected(b.error());
      by = std::move(*b);
    }
    return std::make_unique<runtime::Expr>(
        runtime::ERange{std::move(*lo), std::move(*hi), std::move(by)},
        expr.loc);
  }
  case ExprKind::Call: {
    if (expr.children.empty())
      return std::unexpected(Diagnostic{DiagnosticCode::Syntax,
                                        "empty call expression", expr.loc});
    auto callee = lowerExpr(*expr.children[0]);
    if (!callee) return std::unexpected(callee.error());
    std::vector<runtime::ExprPtr> args;
    args.reserve(expr.children.size() - 1);
    for (std::size_t i = 1; i < expr.children.size(); ++i) {
      auto a = lowerExpr(*expr.children[i]);
      if (!a) return std::unexpected(a.error());
      args.push_back(std::move(*a));
    }
    return std::make_unique<runtime::Expr>(
        runtime::ECall{std::move(*callee), std::move(args)}, expr.loc);
  }
  case ExprKind::Index: {
    if (expr.children.size() < 2)
      return std::unexpected(Diagnostic{
          DiagnosticCode::Syntax, "index requires base and index", expr.loc});
    auto base = lowerExpr(*expr.children[0]);
    if (!base) return std::unexpected(base.error());
    auto idx = lowerExpr(*expr.children[1]);
    if (!idx) return std::unexpected(idx.error());
    return std::make_unique<runtime::Expr>(
        runtime::EIndex{std::move(*base), std::move(*idx)}, expr.loc);
  }
  case ExprKind::Field: {
    if (expr.children.empty())
      return std::unexpected(Diagnostic{
          DiagnosticCode::Syntax, "field access requires base", expr.loc});
    auto base = lowerExpr(*expr.children[0]);
    if (!base) return std::unexpected(base.error());
    return std::make_unique<runtime::Expr>(
        runtime::EField{std::move(*base), expr.text}, expr.loc);
  }
  case ExprKind::List: {
    std::vector<runtime::ExprPtr> elems;
    elems.reserve(expr.children.size());
    for (const auto &c : expr.children) {
      auto e = lowerExpr(*c);
      if (!e) return std::unexpected(e.error());
      elems.push_back(std::move(*e));
    }
    return std::make_unique<runtime::Expr>(runtime::EList{std::move(elems)},
                                           expr.loc);
  }
  case ExprKind::Record: {
    std::vector<runtime::RecordField> fields;
    fields.reserve(expr.children.size());
    for (std::size_t i = 0; i < expr.children.size(); ++i) {
      auto v = lowerExpr(*expr.children[i]);
      if (!v) return std::unexpected(v.error());
      fields.push_back(runtime::RecordField{
          i < expr.names.size() ? expr.names[i] : "", std::move(*v)});
    }
    return std::make_unique<runtime::Expr>(
        runtime::ERecord{expr.text, std::move(fields)}, expr.loc);
  }
  case ExprKind::Block: {
    std::vector<runtime::Stmt> stmts;
    stmts.reserve(expr.children.size());
    for (const auto &c : expr.children) {
      if (c->kind == ExprKind::Let) {
        runtime::Stmt s;
        s.isLet = true;
        s.name  = c->text;
        s.loc   = c->loc;
        if (c->annotation) s.type = lowerType(*c->annotation);
        if (!c->children.empty()) {
          auto v = lowerExpr(*c->children[0]);
          if (!v) return std::unexpected(v.error());
          s.value = std::move(*v);
        }
        stmts.push_back(std::move(s));
      } else {
        auto v = lowerExpr(*c);
        if (!v) return std::unexpected(v.error());
        stmts.push_back(runtime::Stmt{.isLet = false,
                                      .name  = "",
                                      .type  = std::nullopt,
                                      .value = std::move(*v),
                                      .loc   = c->loc});
      }
    }
    return std::make_unique<runtime::Expr>(runtime::EBlock{std::move(stmts)},
                                           expr.loc);
  }
  case ExprKind::If: {
    if (expr.children.size() < 2)
      return std::unexpected(Diagnostic{DiagnosticCode::Syntax,
                                        "if requires condition and then arm",
                                        expr.loc});
    auto cond = lowerExpr(*expr.children[0]);
    if (!cond) return std::unexpected(cond.error());
    auto thenBranch = lowerExpr(*expr.children[1]);
    if (!thenBranch) return std::unexpected(thenBranch.error());
    runtime::ExprPtr elseBranch = nullptr;
    if (expr.children.size() > 2) {
      auto el = lowerExpr(*expr.children[2]);
      if (!el) return std::unexpected(el.error());
      elseBranch = std::move(*el);
    }
    return std::make_unique<runtime::Expr>(runtime::EIf{std::move(*cond),
                                                        std::move(*thenBranch),
                                                        std::move(elseBranch)},
                                           expr.loc);
  }
  case ExprKind::While: {
    if (expr.children.size() < 2)
      return std::unexpected(Diagnostic{DiagnosticCode::Syntax,
                                        "while requires condition and body",
                                        expr.loc});
    auto cond = lowerExpr(*expr.children[0]);
    if (!cond) return std::unexpected(cond.error());
    auto body = lowerExpr(*expr.children[1]);
    if (!body) return std::unexpected(body.error());
    return std::make_unique<runtime::Expr>(
        runtime::EWhile{std::move(*cond), std::move(*body)}, expr.loc);
  }
  case ExprKind::For: {
    if (expr.children.size() < 2)
      return std::unexpected(Diagnostic{
          DiagnosticCode::Syntax, "for requires iterable and body", expr.loc});
    auto iter = lowerExpr(*expr.children[0]);
    if (!iter) return std::unexpected(iter.error());
    auto body = lowerExpr(*expr.children[1]);
    if (!body) return std::unexpected(body.error());
    return std::make_unique<runtime::Expr>(
        runtime::EFor{expr.binding.value_or(""), std::move(*iter), std::nullopt,
                      nullptr, std::move(*body)},
        expr.loc);
  }
  case ExprKind::ForBelieves: {
    if (expr.children.size() < 2)
      return std::unexpected(Diagnostic{DiagnosticCode::Syntax,
                                        "for believes requires term and body",
                                        expr.loc});
    auto term = lowerTerm(*expr.children[0]);
    if (!term) return std::unexpected(term.error());
    runtime::ExprPtr scope = nullptr;
    if (expr.scope) {
      auto sc = lowerExpr(*expr.scope);
      if (!sc) return std::unexpected(sc.error());
      scope = std::move(*sc);
    }
    auto body = lowerExpr(*expr.children[1]);
    if (!body) return std::unexpected(body.error());
    return std::make_unique<runtime::Expr>(
        runtime::EFor{"", nullptr, std::move(*term), std::move(scope),
                      std::move(*body)},
        expr.loc);
  }
  case ExprKind::Believes: {
    if (expr.children.empty())
      return std::unexpected(Diagnostic{DiagnosticCode::Syntax,
                                        "believes requires term", expr.loc});
    auto term = lowerTerm(*expr.children[0]);
    if (!term) return std::unexpected(term.error());
    runtime::ExprPtr scope = nullptr;
    if (expr.scope) {
      auto sc = lowerExpr(*expr.scope);
      if (!sc) return std::unexpected(sc.error());
      scope = std::move(*sc);
    }
    return std::make_unique<runtime::Expr>(
        runtime::EBelieves{std::move(*term), std::move(scope)}, expr.loc);
  }
  case ExprKind::FindAll: {
    if (expr.children.size() < 2)
      return std::unexpected(Diagnostic{DiagnosticCode::Syntax,
                                        "findall requires template and goal",
                                        expr.loc});
    runtime::TermArg templ;
    if (expr.children[0]->kind == ExprKind::LogicVariable) {
      templ.logicVar = expr.children[0]->text;
    } else {
      auto te = lowerExpr(*expr.children[0]);
      if (!te) return std::unexpected(te.error());
      templ.expr = std::move(*te);
    }
    auto goal = lowerTerm(*expr.children[1]);
    if (!goal) return std::unexpected(goal.error());
    runtime::ExprPtr scope = nullptr;
    if (expr.scope) {
      auto sc = lowerExpr(*expr.scope);
      if (!sc) return std::unexpected(sc.error());
      scope = std::move(*sc);
    }
    return std::make_unique<runtime::Expr>(
        runtime::EFindall{std::move(templ), std::move(*goal), std::move(scope)},
        expr.loc);
  }
  case ExprKind::Emit: {
    if (expr.children.empty())
      return std::unexpected(Diagnostic{
          DiagnosticCode::Syntax, "emit requires event expression", expr.loc});
    auto event = lowerExpr(*expr.children[0]);
    if (!event) return std::unexpected(event.error());
    runtime::ExprPtr scope = nullptr;
    if (expr.scope) {
      auto sc = lowerExpr(*expr.scope);
      if (!sc) return std::unexpected(sc.error());
      scope = std::move(*sc);
    }
    runtime::ExprPtr rec = nullptr;
    if (expr.recipients) {
      auto rc = lowerExpr(*expr.recipients);
      if (!rc) return std::unexpected(rc.error());
      rec = std::move(*rc);
    }
    return std::make_unique<runtime::Expr>(
        runtime::EEmit{std::move(*event), std::move(scope), std::move(rec)},
        expr.loc);
  }
  case ExprKind::Tell:
  case ExprKind::Untell: {
    if (expr.children.size() < 2)
      return std::unexpected(
          Diagnostic{DiagnosticCode::Syntax,
                     "tell/untell requires recipient and fact", expr.loc});
    auto to = lowerExpr(*expr.children[0]);
    if (!to) return std::unexpected(to.error());
    auto fact = lowerTerm(*expr.children[1]);
    if (!fact) return std::unexpected(fact.error());
    return std::make_unique<runtime::Expr>(
        runtime::ETell{expr.kind == ExprKind::Untell, std::move(*to),
                       std::move(*fact)},
        expr.loc);
  }
  case ExprKind::Believe:
  case ExprKind::Forget:
  case ExprKind::Revise: {
    if (expr.children.empty())
      return std::unexpected(Diagnostic{
          DiagnosticCode::Syntax, "belief change requires fact", expr.loc});
    runtime::EBeliefChange::Kind kind =
        expr.kind == ExprKind::Believe
            ? runtime::EBeliefChange::Kind::Believe
            : (expr.kind == ExprKind::Forget
                   ? runtime::EBeliefChange::Kind::Forget
                   : runtime::EBeliefChange::Kind::Revise);
    auto fact = lowerTerm(*expr.children[0]);
    if (!fact) return std::unexpected(fact.error());
    runtime::ExprPtr scope = nullptr;
    if (expr.scope) {
      auto sc = lowerExpr(*expr.scope);
      if (!sc) return std::unexpected(sc.error());
      scope = std::move(*sc);
    }
    return std::make_unique<runtime::Expr>(
        runtime::EBeliefChange{kind, std::move(*fact), std::move(scope)},
        expr.loc);
  }
  case ExprKind::Spawn: {
    std::string agent;
    std::vector<runtime::ExprPtr> args;
    if (!expr.children.empty()) {
      if (expr.children[0]->kind == ExprKind::Call &&
          !expr.children[0]->children.empty()) {
        agent = expr.children[0]->children[0]->text;
        for (std::size_t i = 1; i < expr.children[0]->children.size(); ++i) {
          auto a = lowerExpr(*expr.children[0]->children[i]);
          if (!a) return std::unexpected(a.error());
          args.push_back(std::move(*a));
        }
      } else {
        agent = expr.children[0]->text;
      }
    }
    runtime::ExprPtr scope = nullptr;
    if (expr.scope) {
      auto sc = lowerExpr(*expr.scope);
      if (!sc) return std::unexpected(sc.error());
      scope = std::move(*sc);
    }
    return std::make_unique<runtime::Expr>(
        runtime::ESpawn{agent, std::move(args), std::move(scope)}, expr.loc);
  }
  case ExprKind::Adopt: {
    if (expr.children.empty())
      return std::unexpected(
          Diagnostic{DiagnosticCode::Syntax, "adopt requires goal", expr.loc});
    auto gc = lowerGoalCall(*expr.children[0]);
    if (!gc) return std::unexpected(gc.error());
    return std::make_unique<runtime::Expr>(runtime::EAdopt{std::move(*gc)},
                                           expr.loc);
  }
  case ExprKind::Achieve: {
    if (expr.children.empty())
      return std::unexpected(Diagnostic{DiagnosticCode::Syntax,
                                        "achieve requires goal", expr.loc});
    auto gc = lowerGoalCall(*expr.children[0]);
    if (!gc) return std::unexpected(gc.error());
    runtime::ExprPtr by = nullptr;
    if (expr.children.size() > 1) {
      auto b = lowerExpr(*expr.children[1]);
      if (!b) return std::unexpected(b.error());
      by = std::move(*b);
    }
    runtime::ExprPtr within = nullptr;
    if (expr.deadline) {
      auto w = lowerExpr(*expr.deadline);
      if (!w) return std::unexpected(w.error());
      within = std::move(*w);
    }
    runtime::ExprPtr orElse = nullptr;
    if (expr.recovery) {
      auto o = lowerExpr(*expr.recovery);
      if (!o) return std::unexpected(o.error());
      orElse = std::move(*o);
    }
    return std::make_unique<runtime::Expr>(
        runtime::EAchieve{std::move(*gc), std::move(by), std::move(within),
                          std::move(orElse)},
        expr.loc);
  }
  case ExprKind::Ask: {
    if (expr.children.empty())
      return std::unexpected(Diagnostic{DiagnosticCode::Syntax,
                                        "ask requires question expression",
                                        expr.loc});
    auto q = lowerExpr(*expr.children[0]);
    if (!q) return std::unexpected(q.error());
    runtime::ExprPtr within = nullptr;
    if (expr.deadline) {
      auto w = lowerExpr(*expr.deadline);
      if (!w) return std::unexpected(w.error());
      within = std::move(*w);
    }
    runtime::ExprPtr orElse = nullptr;
    if (expr.recovery) {
      auto o = lowerExpr(*expr.recovery);
      if (!o) return std::unexpected(o.error());
      orElse = std::move(*o);
    }
    return std::make_unique<runtime::Expr>(
        runtime::EAsk{std::move(*q), std::move(within), std::move(orElse)},
        expr.loc);
  }
  case ExprKind::AwaitTerm: {
    if (expr.children.empty())
      return std::unexpected(Diagnostic{DiagnosticCode::Syntax,
                                        "await term requires term", expr.loc});
    auto term = lowerTerm(*expr.children[0]);
    if (!term) return std::unexpected(term.error());
    runtime::ExprPtr guard = nullptr;
    if (expr.guard) {
      auto g = lowerExpr(*expr.guard);
      if (!g) return std::unexpected(g.error());
      guard = std::move(*g);
    }
    runtime::ExprPtr within = nullptr;
    if (expr.deadline) {
      auto w = lowerExpr(*expr.deadline);
      if (!w) return std::unexpected(w.error());
      within = std::move(*w);
    }
    runtime::ExprPtr orElse = nullptr;
    if (expr.recovery) {
      auto o = lowerExpr(*expr.recovery);
      if (!o) return std::unexpected(o.error());
      orElse = std::move(*o);
    }
    return std::make_unique<runtime::Expr>(
        runtime::EAwait{std::move(*term), "", "", std::move(guard),
                        std::move(within), std::move(orElse)},
        expr.loc);
  }
  case ExprKind::AwaitEvent: {
    runtime::ExprPtr guard = nullptr;
    if (expr.guard) {
      auto g = lowerExpr(*expr.guard);
      if (!g) return std::unexpected(g.error());
      guard = std::move(*g);
    }
    runtime::ExprPtr within = nullptr;
    if (expr.deadline) {
      auto w = lowerExpr(*expr.deadline);
      if (!w) return std::unexpected(w.error());
      within = std::move(*w);
    }
    runtime::ExprPtr orElse = nullptr;
    if (expr.recovery) {
      auto o = lowerExpr(*expr.recovery);
      if (!o) return std::unexpected(o.error());
      orElse = std::move(*o);
    }
    return std::make_unique<runtime::Expr>(
        runtime::EAwait{std::nullopt, expr.text, expr.binding.value_or(""),
                        std::move(guard), std::move(within), std::move(orElse)},
        expr.loc);
  }
  case ExprKind::Drop: {
    if (expr.children.empty())
      return std::unexpected(
          Diagnostic{DiagnosticCode::Syntax, "drop requires target", expr.loc});
    auto target = lowerExpr(*expr.children[0]);
    if (!target) return std::unexpected(target.error());
    return std::make_unique<runtime::Expr>(runtime::EDrop{std::move(*target)},
                                           expr.loc);
  }
  case ExprKind::Stop: {
    if (expr.children.empty())
      return std::unexpected(
          Diagnostic{DiagnosticCode::Syntax, "stop requires target", expr.loc});
    auto target = lowerExpr(*expr.children[0]);
    if (!target) return std::unexpected(target.error());
    return std::make_unique<runtime::Expr>(runtime::EStop{std::move(*target)},
                                           expr.loc);
  }
  case ExprKind::Terminate:
    return std::make_unique<runtime::Expr>(runtime::ETerminate{}, expr.loc);
  case ExprKind::Install:
  case ExprKind::Uninstall:
  case ExprKind::Mount:
  case ExprKind::Unmount: {
    runtime::EInstall::Kind kind =
        expr.kind == ExprKind::Install
            ? runtime::EInstall::Kind::Install
            : (expr.kind == ExprKind::Uninstall
                   ? runtime::EInstall::Kind::Uninstall
                   : (expr.kind == ExprKind::Mount
                          ? runtime::EInstall::Kind::Mount
                          : runtime::EInstall::Kind::Unmount));
    std::string name;
    std::vector<runtime::ExprPtr> args;
    bool hasParens = false;
    if (!expr.children.empty()) {
      if (expr.children[0]->kind == ExprKind::Call &&
          !expr.children[0]->children.empty()) {
        hasParens = true;
        name      = expr.children[0]->children[0]->text;
        for (std::size_t i = 1; i < expr.children[0]->children.size(); ++i) {
          auto a = lowerExpr(*expr.children[0]->children[i]);
          if (!a) return std::unexpected(a.error());
          args.push_back(std::move(*a));
        }
      } else {
        name = expr.children[0]->text;
      }
    } else {
      name = expr.text;
    }
    return std::make_unique<runtime::Expr>(
        runtime::EInstall{kind, name, std::move(args), hasParens}, expr.loc);
  }
  case ExprKind::Return: {
    runtime::ExprPtr val = nullptr;
    if (!expr.children.empty()) {
      auto v = lowerExpr(*expr.children[0]);
      if (!v) return std::unexpected(v.error());
      val = std::move(*v);
    }
    return std::make_unique<runtime::Expr>(runtime::EReturn{std::move(val)},
                                           expr.loc);
  }
  case ExprKind::Yield:
    return std::make_unique<runtime::Expr>(runtime::EYield{}, expr.loc);
  case ExprKind::Consume:
    return std::make_unique<runtime::Expr>(runtime::EConsume{}, expr.loc);
  case ExprKind::Fail:
    return std::make_unique<runtime::Expr>(runtime::EFail{}, expr.loc);
  case ExprKind::Term: {
    auto term = lowerTerm(expr);
    if (!term) return std::unexpected(term.error());
    return std::make_unique<runtime::Expr>(
        runtime::EBelieves{std::move(*term), nullptr}, expr.loc);
  }
  case ExprKind::Let:
  case ExprKind::LogicVariable:
    break;
  }
  return std::unexpected(Diagnostic{DiagnosticCode::Syntax,
                                    "unsupported expression kind in lowering",
                                    expr.loc});
}

std::expected<ExprPtr, Diagnostic> liftExpr(const runtime::Expr &expr) {
  return std::visit(
      [&](const auto &node) -> std::expected<ExprPtr, Diagnostic> {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, runtime::ELiteral>) {
          auto e = std::make_unique<Expr>();
          e->loc = expr.loc;
          switch (node.kind) {
          case runtime::ELiteral::Kind::Int:
            e->kind = ExprKind::Integer;
            e->text =
                node.text.empty() ? std::to_string(node.intValue) : node.text;
            return e;
          case runtime::ELiteral::Kind::Float:
            e->kind = ExprKind::Float;
            e->text =
                node.text.empty() ? std::to_string(node.floatValue) : node.text;
            return e;
          case runtime::ELiteral::Kind::Str:
            e->kind = ExprKind::String;
            e->text = node.text;
            return e;
          case runtime::ELiteral::Kind::Bool:
            e->kind = ExprKind::Boolean;
            e->text = node.boolValue ? "true" : "false";
            return e;
          case runtime::ELiteral::Kind::Duration:
            e->kind = ExprKind::Duration;
            e->text = node.text.empty() ? formatDurationMs(node.durationMs)
                                        : node.text;
            return e;
          case runtime::ELiteral::Kind::Unit:
            e->kind = ExprKind::Unit;
            return e;
          case runtime::ELiteral::Kind::None:
            e->kind = ExprKind::None;
            return e;
          }
        } else if constexpr (std::is_same_v<T, runtime::EName>) {
          auto e  = std::make_unique<Expr>();
          e->kind = ExprKind::Identifier;
          e->text = node.name;
          e->loc  = expr.loc;
          return e;
        } else if constexpr (std::is_same_v<T, runtime::ESpecial>) {
          auto e  = std::make_unique<Expr>();
          e->kind = ExprKind::Identifier;
          e->text = std::string(toFrontendSpecial(node.kind));
          e->loc  = expr.loc;
          return e;
        } else if constexpr (std::is_same_v<T, runtime::EUnary>) {
          auto e  = std::make_unique<Expr>();
          e->kind = ExprKind::Unary;
          e->text = std::string(toFrontendUnaryOp(node.op));
          e->loc  = expr.loc;
          if (node.operand) {
            auto lifted = liftExpr(*node.operand);
            if (!lifted) return std::unexpected(lifted.error());
            e->children.push_back(std::move(*lifted));
          }
          return e;
        } else if constexpr (std::is_same_v<T, runtime::EBinary>) {
          auto e  = std::make_unique<Expr>();
          e->kind = ExprKind::Binary;
          e->text = std::string(toFrontendBinaryOp(node.op));
          e->loc  = expr.loc;
          if (node.left) {
            auto l = liftExpr(*node.left);
            if (!l) return std::unexpected(l.error());
            e->children.push_back(std::move(*l));
          }
          if (node.right) {
            auto r = liftExpr(*node.right);
            if (!r) return std::unexpected(r.error());
            e->children.push_back(std::move(*r));
          }
          return e;
        } else if constexpr (std::is_same_v<T, runtime::ERange>) {
          auto e  = std::make_unique<Expr>();
          e->kind = ExprKind::Range;
          e->loc  = expr.loc;
          if (node.lo) {
            auto lo = liftExpr(*node.lo);
            if (!lo) return std::unexpected(lo.error());
            e->children.push_back(std::move(*lo));
          }
          if (node.hi) {
            auto hi = liftExpr(*node.hi);
            if (!hi) return std::unexpected(hi.error());
            e->children.push_back(std::move(*hi));
          }
          if (node.by) {
            auto by = liftExpr(*node.by);
            if (!by) return std::unexpected(by.error());
            e->children.push_back(std::move(*by));
          }
          return e;
        } else if constexpr (std::is_same_v<T, runtime::ECall>) {
          auto e  = std::make_unique<Expr>();
          e->kind = ExprKind::Call;
          e->loc  = expr.loc;
          if (node.callee) {
            auto c = liftExpr(*node.callee);
            if (!c) return std::unexpected(c.error());
            e->children.push_back(std::move(*c));
          }
          for (const auto &arg : node.args) {
            if (arg) {
              auto a = liftExpr(*arg);
              if (!a) return std::unexpected(a.error());
              e->children.push_back(std::move(*a));
            }
          }
          return e;
        } else if constexpr (std::is_same_v<T, runtime::EIndex>) {
          auto e  = std::make_unique<Expr>();
          e->kind = ExprKind::Index;
          e->loc  = expr.loc;
          if (node.base) {
            auto b = liftExpr(*node.base);
            if (!b) return std::unexpected(b.error());
            e->children.push_back(std::move(*b));
          }
          if (node.index) {
            auto idx = liftExpr(*node.index);
            if (!idx) return std::unexpected(idx.error());
            e->children.push_back(std::move(*idx));
          }
          return e;
        } else if constexpr (std::is_same_v<T, runtime::EField>) {
          auto e  = std::make_unique<Expr>();
          e->kind = ExprKind::Field;
          e->text = node.name;
          e->loc  = expr.loc;
          if (node.base) {
            auto b = liftExpr(*node.base);
            if (!b) return std::unexpected(b.error());
            e->children.push_back(std::move(*b));
          }
          return e;
        } else if constexpr (std::is_same_v<T, runtime::EList>) {
          auto e  = std::make_unique<Expr>();
          e->kind = ExprKind::List;
          e->loc  = expr.loc;
          for (const auto &elem : node.elems) {
            if (elem) {
              auto lifted = liftExpr(*elem);
              if (!lifted) return std::unexpected(lifted.error());
              e->children.push_back(std::move(*lifted));
            }
          }
          return e;
        } else if constexpr (std::is_same_v<T, runtime::ERecord>) {
          auto e  = std::make_unique<Expr>();
          e->kind = ExprKind::Record;
          e->text = node.type;
          e->loc  = expr.loc;
          for (const auto &f : node.fields) {
            e->names.push_back(f.name);
            if (f.value) {
              auto v = liftExpr(*f.value);
              if (!v) return std::unexpected(v.error());
              e->children.push_back(std::move(*v));
            }
          }
          return e;
        } else if constexpr (std::is_same_v<T, runtime::EBlock>) {
          auto e  = std::make_unique<Expr>();
          e->kind = ExprKind::Block;
          e->loc  = expr.loc;
          for (const auto &stmt : node.stmts) {
            if (stmt.isLet) {
              auto letExpr  = std::make_unique<Expr>();
              letExpr->kind = ExprKind::Let;
              letExpr->text = stmt.name;
              letExpr->loc  = stmt.loc;
              if (stmt.type) letExpr->annotation = liftType(*stmt.type);
              if (stmt.value) {
                auto v = liftExpr(*stmt.value);
                if (!v) return std::unexpected(v.error());
                letExpr->children.push_back(std::move(*v));
              }
              e->children.push_back(std::move(letExpr));
            } else if (stmt.value) {
              auto v = liftExpr(*stmt.value);
              if (!v) return std::unexpected(v.error());
              e->children.push_back(std::move(*v));
            }
          }
          return e;
        } else if constexpr (std::is_same_v<T, runtime::EIf>) {
          auto e  = std::make_unique<Expr>();
          e->kind = ExprKind::If;
          e->loc  = expr.loc;
          if (node.cond) {
            auto c = liftExpr(*node.cond);
            if (!c) return std::unexpected(c.error());
            e->children.push_back(std::move(*c));
          }
          if (node.thenBranch) {
            auto t = liftExpr(*node.thenBranch);
            if (!t) return std::unexpected(t.error());
            e->children.push_back(std::move(*t));
          }
          if (node.elseBranch) {
            auto el = liftExpr(*node.elseBranch);
            if (!el) return std::unexpected(el.error());
            e->children.push_back(std::move(*el));
          }
          return e;
        } else if constexpr (std::is_same_v<T, runtime::EWhile>) {
          auto e  = std::make_unique<Expr>();
          e->kind = ExprKind::While;
          e->loc  = expr.loc;
          if (node.cond) {
            auto c = liftExpr(*node.cond);
            if (!c) return std::unexpected(c.error());
            e->children.push_back(std::move(*c));
          }
          if (node.body) {
            auto b = liftExpr(*node.body);
            if (!b) return std::unexpected(b.error());
            e->children.push_back(std::move(*b));
          }
          return e;
        } else if constexpr (std::is_same_v<T, runtime::EFor>) {
          auto e = std::make_unique<Expr>();
          e->loc = expr.loc;
          if (node.believesTerm) {
            e->kind = ExprKind::ForBelieves;
            e->children.push_back(liftTerm(*node.believesTerm));
            if (node.believesFrom) {
              auto f = liftExpr(*node.believesFrom);
              if (!f) return std::unexpected(f.error());
              e->scope = std::move(*f);
            }
          } else {
            e->kind    = ExprKind::For;
            e->binding = node.binder;
            if (node.iterable) {
              auto it = liftExpr(*node.iterable);
              if (!it) return std::unexpected(it.error());
              e->children.push_back(std::move(*it));
            }
          }
          if (node.body) {
            auto b = liftExpr(*node.body);
            if (!b) return std::unexpected(b.error());
            e->children.push_back(std::move(*b));
          }
          return e;
        } else if constexpr (std::is_same_v<T, runtime::EBelieves>) {
          auto e  = std::make_unique<Expr>();
          e->kind = ExprKind::Believes;
          e->loc  = expr.loc;
          e->children.push_back(liftTerm(node.term));
          if (node.from) {
            auto f = liftExpr(*node.from);
            if (!f) return std::unexpected(f.error());
            e->scope = std::move(*f);
          }
          return e;
        } else if constexpr (std::is_same_v<T, runtime::EFindall>) {
          auto e  = std::make_unique<Expr>();
          e->kind = ExprKind::FindAll;
          e->loc  = expr.loc;
          if (node.templ.logicVar) {
            auto var  = std::make_unique<Expr>();
            var->kind = ExprKind::LogicVariable;
            var->text = *node.templ.logicVar;
            e->children.push_back(std::move(var));
          } else if (node.templ.expr) {
            auto te = liftExpr(*node.templ.expr);
            if (!te) return std::unexpected(te.error());
            e->children.push_back(std::move(*te));
          }
          e->children.push_back(liftTerm(node.goal));
          if (node.from) {
            auto f = liftExpr(*node.from);
            if (!f) return std::unexpected(f.error());
            e->scope = std::move(*f);
          }
          return e;
        } else if constexpr (std::is_same_v<T, runtime::EEmit>) {
          auto e  = std::make_unique<Expr>();
          e->kind = ExprKind::Emit;
          e->loc  = expr.loc;
          if (node.event) {
            auto ev = liftExpr(*node.event);
            if (!ev) return std::unexpected(ev.error());
            e->children.push_back(std::move(*ev));
          }
          if (node.space) {
            auto sp = liftExpr(*node.space);
            if (!sp) return std::unexpected(sp.error());
            e->scope = std::move(*sp);
          }
          if (node.recipients) {
            auto rc = liftExpr(*node.recipients);
            if (!rc) return std::unexpected(rc.error());
            e->recipients = std::move(*rc);
          }
          return e;
        } else if constexpr (std::is_same_v<T, runtime::ETell>) {
          auto e  = std::make_unique<Expr>();
          e->kind = node.untell ? ExprKind::Untell : ExprKind::Tell;
          e->loc  = expr.loc;
          if (node.to) {
            auto to = liftExpr(*node.to);
            if (!to) return std::unexpected(to.error());
            e->children.push_back(std::move(*to));
          }
          e->children.push_back(liftTerm(node.fact));
          return e;
        } else if constexpr (std::is_same_v<T, runtime::EBeliefChange>) {
          auto e  = std::make_unique<Expr>();
          e->kind = node.kind == runtime::EBeliefChange::Kind::Believe
                        ? ExprKind::Believe
                        : (node.kind == runtime::EBeliefChange::Kind::Forget
                               ? ExprKind::Forget
                               : ExprKind::Revise);
          e->loc  = expr.loc;
          e->children.push_back(liftTerm(node.fact));
          if (node.from) {
            auto f = liftExpr(*node.from);
            if (!f) return std::unexpected(f.error());
            e->scope = std::move(*f);
          }
          return e;
        } else if constexpr (std::is_same_v<T, runtime::ESpawn>) {
          auto e     = std::make_unique<Expr>();
          e->kind    = ExprKind::Spawn;
          e->loc     = expr.loc;
          auto call  = std::make_unique<Expr>();
          call->kind = ExprKind::Call;
          call->loc  = expr.loc;
          auto id    = std::make_unique<Expr>();
          id->kind   = ExprKind::Identifier;
          id->text   = node.agent;
          id->loc    = expr.loc;
          call->children.push_back(std::move(id));
          for (const auto &a : node.args) {
            if (a) {
              auto lifted = liftExpr(*a);
              if (!lifted) return std::unexpected(lifted.error());
              call->children.push_back(std::move(*lifted));
            }
          }
          e->children.push_back(std::move(call));
          if (node.space) {
            auto sp = liftExpr(*node.space);
            if (!sp) return std::unexpected(sp.error());
            e->scope = std::move(*sp);
          }
          return e;
        } else if constexpr (std::is_same_v<T, runtime::EAdopt>) {
          auto e  = std::make_unique<Expr>();
          e->kind = ExprKind::Adopt;
          e->loc  = expr.loc;
          e->children.push_back(liftGoalCall(node.goal));
          return e;
        } else if constexpr (std::is_same_v<T, runtime::EAchieve>) {
          auto e  = std::make_unique<Expr>();
          e->kind = ExprKind::Achieve;
          e->loc  = expr.loc;
          e->children.push_back(liftGoalCall(node.goal));
          if (node.by) {
            auto by = liftExpr(*node.by);
            if (!by) return std::unexpected(by.error());
            e->children.push_back(std::move(*by));
          }
          if (node.within) {
            auto w = liftExpr(*node.within);
            if (!w) return std::unexpected(w.error());
            e->deadline = std::move(*w);
          }
          if (node.orElse) {
            auto o = liftExpr(*node.orElse);
            if (!o) return std::unexpected(o.error());
            e->recovery = std::move(*o);
          }
          return e;
        } else if constexpr (std::is_same_v<T, runtime::EAsk>) {
          auto e  = std::make_unique<Expr>();
          e->kind = ExprKind::Ask;
          e->loc  = expr.loc;
          if (node.question) {
            auto q = liftExpr(*node.question);
            if (!q) return std::unexpected(q.error());
            e->children.push_back(std::move(*q));
          }
          if (node.within) {
            auto w = liftExpr(*node.within);
            if (!w) return std::unexpected(w.error());
            e->deadline = std::move(*w);
          }
          if (node.orElse) {
            auto o = liftExpr(*node.orElse);
            if (!o) return std::unexpected(o.error());
            e->recovery = std::move(*o);
          }
          return e;
        } else if constexpr (std::is_same_v<T, runtime::EAwait>) {
          auto e = std::make_unique<Expr>();
          e->loc = expr.loc;
          if (node.term) {
            e->kind = ExprKind::AwaitTerm;
            e->children.push_back(liftTerm(*node.term));
          } else {
            e->kind = ExprKind::AwaitEvent;
            e->text = node.eventType;
            if (!node.binder.empty()) e->binding = node.binder;
          }
          if (node.guard) {
            auto g = liftExpr(*node.guard);
            if (!g) return std::unexpected(g.error());
            e->guard = std::move(*g);
          }
          if (node.within) {
            auto w = liftExpr(*node.within);
            if (!w) return std::unexpected(w.error());
            e->deadline = std::move(*w);
          }
          if (node.orElse) {
            auto o = liftExpr(*node.orElse);
            if (!o) return std::unexpected(o.error());
            e->recovery = std::move(*o);
          }
          return e;
        } else if constexpr (std::is_same_v<T, runtime::EDrop>) {
          auto e  = std::make_unique<Expr>();
          e->kind = ExprKind::Drop;
          e->loc  = expr.loc;
          if (node.target) {
            auto tgt = liftExpr(*node.target);
            if (!tgt) return std::unexpected(tgt.error());
            e->children.push_back(std::move(*tgt));
          }
          return e;
        } else if constexpr (std::is_same_v<T, runtime::EStop>) {
          auto e  = std::make_unique<Expr>();
          e->kind = ExprKind::Stop;
          e->loc  = expr.loc;
          if (node.target) {
            auto tgt = liftExpr(*node.target);
            if (!tgt) return std::unexpected(tgt.error());
            e->children.push_back(std::move(*tgt));
          }
          return e;
        } else if constexpr (std::is_same_v<T, runtime::ETerminate>) {
          auto e  = std::make_unique<Expr>();
          e->kind = ExprKind::Terminate;
          e->loc  = expr.loc;
          return e;
        } else if constexpr (std::is_same_v<T, runtime::EInstall>) {
          auto e  = std::make_unique<Expr>();
          e->kind = node.kind == runtime::EInstall::Kind::Install
                        ? ExprKind::Install
                        : (node.kind == runtime::EInstall::Kind::Uninstall
                               ? ExprKind::Uninstall
                               : (node.kind == runtime::EInstall::Kind::Mount
                                      ? ExprKind::Mount
                                      : ExprKind::Unmount));
          e->loc  = expr.loc;
          e->text = node.name;
          if (node.hasParens || !node.args.empty()) {
            auto call  = std::make_unique<Expr>();
            call->kind = ExprKind::Call;
            call->loc  = expr.loc;
            auto id    = std::make_unique<Expr>();
            id->kind   = ExprKind::Identifier;
            id->text   = node.name;
            id->loc    = expr.loc;
            call->children.push_back(std::move(id));
            for (const auto &a : node.args) {
              if (a) {
                auto lifted = liftExpr(*a);
                if (!lifted) return std::unexpected(lifted.error());
                call->children.push_back(std::move(*lifted));
              }
            }
            e->children.push_back(std::move(call));
          } else {
            auto id  = std::make_unique<Expr>();
            id->kind = ExprKind::Identifier;
            id->text = node.name;
            id->loc  = expr.loc;
            e->children.push_back(std::move(id));
          }
          return e;
        } else if constexpr (std::is_same_v<T, runtime::EReturn>) {
          auto e  = std::make_unique<Expr>();
          e->kind = ExprKind::Return;
          e->loc  = expr.loc;
          if (node.value) {
            auto v = liftExpr(*node.value);
            if (!v) return std::unexpected(v.error());
            e->children.push_back(std::move(*v));
          }
          return e;
        } else if constexpr (std::is_same_v<T, runtime::EYield>) {
          auto e  = std::make_unique<Expr>();
          e->kind = ExprKind::Yield;
          e->loc  = expr.loc;
          return e;
        } else if constexpr (std::is_same_v<T, runtime::EConsume>) {
          auto e  = std::make_unique<Expr>();
          e->kind = ExprKind::Consume;
          e->loc  = expr.loc;
          return e;
        } else if constexpr (std::is_same_v<T, runtime::EFail>) {
          auto e  = std::make_unique<Expr>();
          e->kind = ExprKind::Fail;
          e->loc  = expr.loc;
          return e;
        }
        return std::unexpected(Diagnostic{DiagnosticCode::Syntax,
                                          "unknown runtime expression node",
                                          expr.loc});
      },
      expr.node);
}

namespace {

bool areExpressionsEquivalent(const Expr &a, const Expr &b) {
  if (a.kind != b.kind) return false;
  if (a.text != b.text) {
    if (a.kind == ExprKind::Integer) {
      if (parseInteger(a.text) != parseInteger(b.text)) return false;
    } else if (a.kind == ExprKind::Duration) {
      if (parseDurationMs(a.text) != parseDurationMs(b.text)) return false;
    } else if (a.kind == ExprKind::Float) {
      if (std::abs(parseFloat(a.text) - parseFloat(b.text)) > 1e-9)
        return false;
    } else {
      return false;
    }
  }
  if (a.binding != b.binding) return false;
  if (a.children.size() != b.children.size()) return false;
  for (std::size_t i = 0; i < a.children.size(); ++i) {
    if (!areExpressionsEquivalent(*a.children[i], *b.children[i])) return false;
  }
  return true;
}

} // namespace

bool areEquivalent(const Expr &frontend, const runtime::Expr &runtime) {
  auto lifted = liftExpr(runtime);
  if (!lifted) return false;
  return areExpressionsEquivalent(frontend, **lifted);
}

// ============================================================================
// Members & Declarations
// ============================================================================

std::expected<runtime::Member, Diagnostic> lowerMember(const Decl &decl) {
  switch (decl.kind) {
  case DeclKind::Uses: {
    runtime::UsesDecl u;
    u.capacities = decl.names;
    u.loc        = decl.loc;
    return u;
  }
  case DeclKind::Field: {
    runtime::VarDecl v;
    v.name = decl.name;
    v.type = lowerType(decl.result.value_or(Type{"Int", {}}));
    v.loc  = decl.loc;
    if (decl.initializer) {
      auto init = lowerExpr(*decl.initializer);
      if (!init) return std::unexpected(init.error());
      v.init = std::move(*init);
    }
    return v;
  }
  case DeclKind::Handler: {
    runtime::HandlerDef h;
    h.loc      = decl.loc;
    h.priority = decl.priority;
    if (decl.guard) {
      auto g = lowerExpr(*decl.guard);
      if (!g) return std::unexpected(g.error());
      h.guard = std::move(*g);
    }
    if (decl.body) {
      auto b = lowerExpr(*decl.body);
      if (!b) return std::unexpected(b.error());
      h.body = std::move(*b);
    }
    if (decl.name == "+" || decl.name == "-") {
      if (!decl.trigger)
        return std::unexpected(Diagnostic{
            DiagnosticCode::Syntax, "missing belief trigger fact", decl.loc});
      auto fact = lowerTerm(*decl.trigger);
      if (!fact) return std::unexpected(fact.error());
      h.trigger = runtime::BeliefTrigger{.added = (decl.name == "+"),
                                         .fact  = std::move(*fact)};
    } else {
      h.trigger =
          runtime::EventTrigger{.eventType = decl.name, .binder = decl.binding};
    }
    return h;
  }
  case DeclKind::Method: {
    runtime::MethodDef m;
    m.sig.isEffect = decl.effect;
    m.sig.preempts = decl.preempts;
    m.sig.name     = decl.name;
    m.sig.loc      = decl.loc;
    if (decl.result) m.sig.result = lowerType(*decl.result);
    if (decl.deadline) m.sig.withinMs = parseDurationMs(decl.deadline->text);
    for (const auto &p : decl.parameters) {
      m.sig.params.push_back(runtime::Param{
          .name = p.name, .type = lowerType(p.type), .loc = p.loc});
    }
    for (const auto &c : decl.contracts) {
      if (c.condition) {
        auto cond = lowerExpr(*c.condition);
        if (!cond) return std::unexpected(cond.error());
        m.sig.contracts.push_back(runtime::Contract{
            .kind      = c.postcondition ? runtime::Contract::Kind::Ensures
                                         : runtime::Contract::Kind::Requires,
            .condition = std::move(*cond)});
      }
    }
    if (decl.body) {
      auto b = lowerExpr(*decl.body);
      if (!b) return std::unexpected(b.error());
      m.body = std::move(*b);
    }
    return m;
  }
  case DeclKind::Answer: {
    runtime::AnswerDef a;
    a.name   = decl.name;
    a.loc    = decl.loc;
    a.result = lowerType(decl.result.value_or(Type{"()", {}}));
    for (const auto &p : decl.parameters) {
      a.params.push_back(runtime::Param{
          .name = p.name, .type = lowerType(p.type), .loc = p.loc});
    }
    if (decl.body) {
      auto b = lowerExpr(*decl.body);
      if (!b) return std::unexpected(b.error());
      a.body = std::move(*b);
    }
    return a;
  }
  case DeclKind::Goal: {
    auto top = lowerTopDecl(decl);
    if (!top) return std::unexpected(top.error());
    return std::get<runtime::GoalDecl>(std::move(*top));
  }
  case DeclKind::Plan: {
    auto top = lowerTopDecl(decl);
    if (!top) return std::unexpected(top.error());
    return std::get<runtime::PlanDecl>(std::move(*top));
  }
  case DeclKind::Install: {
    runtime::InstallDecl inst;
    inst.name = decl.name;
    inst.loc  = decl.loc;
    if (decl.initializer && decl.initializer->kind == ExprKind::Call) {
      inst.hasParens = true;
      for (std::size_t i = 1; i < decl.initializer->children.size(); ++i) {
        auto arg = lowerExpr(*decl.initializer->children[i]);
        if (!arg) return std::unexpected(arg.error());
        inst.args.push_back(std::move(*arg));
      }
    }
    return inst;
  }
  case DeclKind::BehaviorUse: {
    runtime::BehaviorUse bu;
    bu.name = decl.name;
    bu.loc  = decl.loc;
    if (decl.initializer && decl.initializer->kind == ExprKind::Call) {
      bu.hasParens = true;
      for (std::size_t i = 1; i < decl.initializer->children.size(); ++i) {
        auto arg = lowerExpr(*decl.initializer->children[i]);
        if (!arg) return std::unexpected(arg.error());
        bu.args.push_back(std::move(*arg));
      }
    }
    return bu;
  }
  case DeclKind::Belief: {
    runtime::BeliefDecl bd;
    bd.loc = decl.loc;
    if (decl.initializer) {
      auto term = lowerTerm(*decl.initializer);
      if (!term) return std::unexpected(term.error());
      bd.fact = std::move(*term);
    }
    return bd;
  }
  case DeclKind::Lifecycle: {
    runtime::LifecycleDef lc;
    lc.loc  = decl.loc;
    lc.kind = decl.name == "uninstall" ? runtime::LifecycleDef::Kind::Uninstall
                                       : runtime::LifecycleDef::Kind::Install;
    lc.uninstall = (decl.name == "uninstall");
    lc.param     = decl.binding;
    if (decl.body) {
      auto b = lowerExpr(*decl.body);
      if (!b) return std::unexpected(b.error());
      lc.body = std::move(*b);
    }
    return lc;
  }
  default:
    break;
  }
  return std::unexpected(Diagnostic{DiagnosticCode::Syntax,
                                    "decl is not a valid container member",
                                    decl.loc});
}

std::expected<Decl, Diagnostic> liftGoalDecl(const runtime::GoalDecl &node) {
  Decl d;
  d.kind     = DeclKind::Goal;
  d.name     = node.name;
  d.loc      = node.loc;
  d.goalKind = node.kind == runtime::GoalDecl::Kind::Achieve
                   ? GoalKind::Achieve
                   : (node.kind == runtime::GoalDecl::Kind::Perform
                          ? GoalKind::Perform
                          : GoalKind::Maintain);
  for (const auto &p : node.params) {
    d.parameters.push_back(Parameter{
        .name = p.name, .type = liftType(p.type), .key = false, .loc = p.loc});
  }
  for (const auto &c : node.clauses) {
    if (c.condition) {
      auto cond = liftExpr(*c.condition);
      if (!cond) return std::unexpected(cond.error());
      GoalClause::Kind k = GoalClause::Kind::Succeed;
      if (c.kind == runtime::GoalClause::Kind::DropWhen)
        k = GoalClause::Kind::Drop;
      else if (c.kind == runtime::GoalClause::Kind::Holds)
        k = GoalClause::Kind::Holds;
      d.goalClauses.push_back(GoalClause{
          .kind = k, .condition = std::move(*cond), .loc = node.loc});
    }
  }
  return d;
}

std::expected<Decl, Diagnostic> liftPlanDecl(const runtime::PlanDecl &node) {
  Decl d;
  d.kind   = DeclKind::Plan;
  d.name   = node.name;
  d.target = node.goal;
  d.names  = node.params;
  d.loc    = node.loc;
  if (node.where) {
    auto w = liftExpr(*node.where);
    if (!w) return std::unexpected(w.error());
    d.guard = std::move(*w);
  }
  if (node.body) {
    auto b = liftExpr(*node.body);
    if (!b) return std::unexpected(b.error());
    d.body = std::move(*b);
  }
  return d;
}

std::expected<Decl, Diagnostic> liftMember(const runtime::Member &member) {
  return std::visit(
      [&](const auto &node) -> std::expected<Decl, Diagnostic> {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, runtime::UsesDecl>) {
          Decl d;
          d.kind  = DeclKind::Uses;
          d.names = node.capacities;
          d.loc   = node.loc;
          return d;
        } else if constexpr (std::is_same_v<T, runtime::VarDecl>) {
          Decl d;
          d.kind   = DeclKind::Field;
          d.name   = node.name;
          d.result = liftType(node.type);
          d.loc    = node.loc;
          if (node.init) {
            auto init = liftExpr(*node.init);
            if (!init) return std::unexpected(init.error());
            d.initializer = std::move(*init);
          }
          return d;
        } else if constexpr (std::is_same_v<T, runtime::HandlerDef>) {
          Decl d;
          d.kind     = DeclKind::Handler;
          d.loc      = node.loc;
          d.priority = node.priority;
          if (node.guard) {
            auto g = liftExpr(*node.guard);
            if (!g) return std::unexpected(g.error());
            d.guard = std::move(*g);
          }
          if (node.body) {
            auto b = liftExpr(*node.body);
            if (!b) return std::unexpected(b.error());
            d.body = std::move(*b);
          }
          if (std::holds_alternative<runtime::EventTrigger>(node.trigger)) {
            const auto &et = std::get<runtime::EventTrigger>(node.trigger);
            d.name         = et.eventType;
            d.binding      = et.binder;
            auto trig      = std::make_unique<Expr>();
            trig->kind     = ExprKind::Identifier;
            trig->text     = et.eventType;
            trig->loc      = node.loc;
            d.trigger      = std::move(trig);
          } else {
            const auto &bt = std::get<runtime::BeliefTrigger>(node.trigger);
            d.name         = bt.added ? "+" : "-";
            d.trigger      = liftTerm(bt.fact);
          }
          return d;
        } else if constexpr (std::is_same_v<T, runtime::MethodDef>) {
          Decl d;
          d.kind     = DeclKind::Method;
          d.name     = node.sig.name;
          d.effect   = node.sig.isEffect;
          d.preempts = node.sig.preempts;
          d.loc      = node.sig.loc;
          if (node.sig.result) d.result = liftType(*node.sig.result);
          if (node.sig.withinMs) {
            auto dl    = std::make_unique<Expr>();
            dl->kind   = ExprKind::Duration;
            dl->text   = formatDurationMs(*node.sig.withinMs);
            dl->loc    = node.sig.loc;
            d.deadline = std::move(dl);
          }
          for (const auto &p : node.sig.params) {
            d.parameters.push_back(Parameter{.name = p.name,
                                             .type = liftType(p.type),
                                             .key  = false,
                                             .loc  = p.loc});
          }
          for (const auto &c : node.sig.contracts) {
            if (c.condition) {
              auto cond = liftExpr(*c.condition);
              if (!cond) return std::unexpected(cond.error());
              d.contracts.push_back(Contract{
                  .postcondition = (c.kind == runtime::Contract::Kind::Ensures),
                  .condition     = std::move(*cond),
                  .loc           = node.sig.loc});
            }
          }
          if (node.body) {
            auto b = liftExpr(*node.body);
            if (!b) return std::unexpected(b.error());
            d.body = std::move(*b);
          }
          return d;
        } else if constexpr (std::is_same_v<T, runtime::AnswerDef>) {
          Decl d;
          d.kind   = DeclKind::Answer;
          d.name   = node.name;
          d.result = liftType(node.result);
          d.loc    = node.loc;
          for (const auto &p : node.params) {
            d.parameters.push_back(Parameter{.name = p.name,
                                             .type = liftType(p.type),
                                             .key  = false,
                                             .loc  = p.loc});
          }
          if (node.body) {
            auto b = liftExpr(*node.body);
            if (!b) return std::unexpected(b.error());
            d.body = std::move(*b);
          }
          return d;
        } else if constexpr (std::is_same_v<T, runtime::GoalDecl>) {
          return liftGoalDecl(node);
        } else if constexpr (std::is_same_v<T, runtime::PlanDecl>) {
          return liftPlanDecl(node);
        } else if constexpr (std::is_same_v<T, runtime::InstallDecl>) {
          Decl d;
          d.kind     = DeclKind::Install;
          d.name     = node.name;
          d.loc      = node.loc;
          auto call  = std::make_unique<Expr>();
          call->kind = ExprKind::Call;
          call->loc  = node.loc;
          auto id    = std::make_unique<Expr>();
          id->kind   = ExprKind::Identifier;
          id->text   = node.name;
          id->loc    = node.loc;
          call->children.push_back(std::move(id));
          for (const auto &arg : node.args) {
            if (arg) {
              auto a = liftExpr(*arg);
              if (!a) return std::unexpected(a.error());
              call->children.push_back(std::move(*a));
            }
          }
          d.initializer = std::move(call);
          return d;
        } else if constexpr (std::is_same_v<T, runtime::BehaviorUse>) {
          Decl d;
          d.kind     = DeclKind::BehaviorUse;
          d.name     = node.name;
          d.loc      = node.loc;
          auto call  = std::make_unique<Expr>();
          call->kind = ExprKind::Call;
          call->loc  = node.loc;
          auto id    = std::make_unique<Expr>();
          id->kind   = ExprKind::Identifier;
          id->text   = node.name;
          id->loc    = node.loc;
          call->children.push_back(std::move(id));
          for (const auto &arg : node.args) {
            if (arg) {
              auto a = liftExpr(*arg);
              if (!a) return std::unexpected(a.error());
              call->children.push_back(std::move(*a));
            }
          }
          d.initializer = std::move(call);
          return d;
        } else if constexpr (std::is_same_v<T, runtime::BeliefDecl>) {
          Decl d;
          d.kind        = DeclKind::Belief;
          d.name        = node.fact.name;
          d.loc         = node.loc;
          d.initializer = liftTerm(node.fact);
          return d;
        } else if constexpr (std::is_same_v<T, runtime::LifecycleDef>) {
          Decl d;
          d.kind    = DeclKind::Lifecycle;
          d.name    = node.uninstall ? "uninstall" : "install";
          d.binding = node.param;
          d.loc     = node.loc;
          if (node.body) {
            auto b = liftExpr(*node.body);
            if (!b) return std::unexpected(b.error());
            d.body = std::move(*b);
          }
          return d;
        }
        return std::unexpected(
            Diagnostic{DiagnosticCode::Syntax, "unknown member node", {}});
      },
      member);
}

std::expected<runtime::TopDecl, Diagnostic> lowerTopDecl(const Decl &decl) {
  switch (decl.kind) {
  case DeclKind::Event: {
    runtime::EventDecl ev;
    ev.name    = decl.name;
    ev.extends = decl.target;
    ev.hasBody = !decl.parameters.empty();
    ev.loc     = decl.loc;
    for (const auto &p : decl.parameters) {
      ev.fields.push_back(runtime::Field{
          .name = p.name, .type = lowerType(p.type), .loc = p.loc});
    }
    return ev;
  }
  case DeclKind::Record: {
    runtime::RecordDecl rec;
    rec.name = decl.name;
    rec.loc  = decl.loc;
    for (const auto &p : decl.parameters) {
      rec.fields.push_back(runtime::Field{
          .name = p.name, .type = lowerType(p.type), .loc = p.loc});
    }
    return rec;
  }
  case DeclKind::Predicate: {
    runtime::PredicateDecl pred;
    pred.name = decl.name;
    pred.loc  = decl.loc;
    for (const auto &p : decl.parameters) {
      pred.params.push_back(runtime::PredParam{
          .key = p.key, .name = p.name, .type = lowerType(p.type)});
    }
    return pred;
  }
  case DeclKind::Question: {
    runtime::QuestionDecl q;
    q.name   = decl.name;
    q.loc    = decl.loc;
    q.result = lowerType(decl.result.value_or(Type{"()", {}}));
    for (const auto &p : decl.parameters) {
      q.params.push_back(runtime::Param{
          .name = p.name, .type = lowerType(p.type), .loc = p.loc});
    }
    return q;
  }
  case DeclKind::Capacity: {
    runtime::CapacityDecl cap;
    cap.name    = decl.name;
    cap.extends = decl.names;
    cap.loc     = decl.loc;
    for (const auto &m : decl.members) {
      runtime::MethodSig sig;
      sig.isEffect = m.effect;
      sig.preempts = m.preempts;
      sig.name     = m.name;
      sig.loc      = m.loc;
      if (m.result) sig.result = lowerType(*m.result);
      if (m.deadline) sig.withinMs = parseDurationMs(m.deadline->text);
      for (const auto &p : m.parameters) {
        sig.params.push_back(runtime::Param{
            .name = p.name, .type = lowerType(p.type), .loc = p.loc});
      }
      for (const auto &c : m.contracts) {
        if (c.condition) {
          auto cond = lowerExpr(*c.condition);
          if (!cond) return std::unexpected(cond.error());
          sig.contracts.push_back(runtime::Contract{
              .kind      = c.postcondition ? runtime::Contract::Kind::Ensures
                                           : runtime::Contract::Kind::Requires,
              .condition = std::move(*cond)});
        }
      }
      cap.methods.push_back(std::move(sig));
    }
    return cap;
  }
  case DeclKind::Goal: {
    runtime::GoalDecl g;
    g.name = decl.name;
    g.loc  = decl.loc;
    if (decl.goalKind == GoalKind::Achieve)
      g.kind = runtime::GoalDecl::Kind::Achieve;
    else if (decl.goalKind == GoalKind::Perform)
      g.kind = runtime::GoalDecl::Kind::Perform;
    else
      g.kind = runtime::GoalDecl::Kind::Maintain;
    g.hasBody = !decl.goalClauses.empty();
    for (const auto &p : decl.parameters) {
      g.params.push_back(runtime::Param{
          .name = p.name, .type = lowerType(p.type), .loc = p.loc});
    }
    for (const auto &c : decl.goalClauses) {
      if (c.condition) {
        auto cond = lowerExpr(*c.condition);
        if (!cond) return std::unexpected(cond.error());
        runtime::GoalClause::Kind k = runtime::GoalClause::Kind::SucceedWhen;
        if (c.kind == GoalClause::Kind::Drop)
          k = runtime::GoalClause::Kind::DropWhen;
        else if (c.kind == GoalClause::Kind::Holds)
          k = runtime::GoalClause::Kind::Holds;
        g.clauses.push_back(
            runtime::GoalClause{.kind = k, .condition = std::move(*cond)});
      }
    }
    return g;
  }
  case DeclKind::Plan: {
    runtime::PlanDecl pl;
    pl.name      = decl.name;
    pl.goal      = decl.target.value_or("");
    pl.params    = decl.names;
    pl.hasParams = !decl.names.empty() || decl.planBindings.has_value();
    pl.loc       = decl.loc;
    if (decl.guard) {
      auto w = lowerExpr(*decl.guard);
      if (!w) return std::unexpected(w.error());
      pl.where = std::move(*w);
    }
    if (decl.body) {
      auto b = lowerExpr(*decl.body);
      if (!b) return std::unexpected(b.error());
      pl.body = std::move(*b);
    }
    return pl;
  }
  case DeclKind::Skill: {
    runtime::SkillDecl sk;
    sk.native     = decl.native;
    sk.name       = decl.name;
    sk.implements = decl.names;
    sk.loc        = decl.loc;
    sk.hasParams  = !decl.parameters.empty();
    for (const auto &p : decl.parameters) {
      sk.params.push_back(runtime::Param{
          .name = p.name, .type = lowerType(p.type), .loc = p.loc});
    }
    for (const auto &m : decl.members) {
      auto member = lowerMember(m);
      if (!member) return std::unexpected(member.error());
      sk.members.push_back(std::move(*member));
    }
    return sk;
  }
  case DeclKind::Behavior: {
    runtime::BehaviorDecl bh;
    bh.name      = decl.name;
    bh.loc       = decl.loc;
    bh.hasParams = !decl.parameters.empty();
    for (const auto &p : decl.parameters) {
      bh.params.push_back(runtime::Param{
          .name = p.name, .type = lowerType(p.type), .loc = p.loc});
    }
    for (const auto &m : decl.members) {
      auto member = lowerMember(m);
      if (!member) return std::unexpected(member.error());
      bh.members.push_back(std::move(*member));
    }
    return bh;
  }
  case DeclKind::Agent: {
    runtime::AgentDecl ag;
    ag.name      = decl.name;
    ag.loc       = decl.loc;
    ag.hasParams = !decl.parameters.empty();
    for (const auto &p : decl.parameters) {
      ag.params.push_back(runtime::Param{
          .name = p.name, .type = lowerType(p.type), .loc = p.loc});
    }
    for (const auto &m : decl.members) {
      auto member = lowerMember(m);
      if (!member) return std::unexpected(member.error());
      ag.members.push_back(std::move(*member));
    }
    return ag;
  }
  default:
    break;
  }
  return std::unexpected(Diagnostic{
      DiagnosticCode::Syntax, "decl is not a top-level declaration", decl.loc});
}

std::expected<Decl, Diagnostic> liftTopDecl(const runtime::TopDecl &topDecl) {
  return std::visit(
      [&](const auto &node) -> std::expected<Decl, Diagnostic> {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, runtime::EventDecl>) {
          Decl d;
          d.kind   = DeclKind::Event;
          d.name   = node.name;
          d.target = node.extends;
          d.loc    = node.loc;
          for (const auto &f : node.fields) {
            d.parameters.push_back(Parameter{.name = f.name,
                                             .type = liftType(f.type),
                                             .key  = false,
                                             .loc  = f.loc});
          }
          return d;
        } else if constexpr (std::is_same_v<T, runtime::RecordDecl>) {
          Decl d;
          d.kind = DeclKind::Record;
          d.name = node.name;
          d.loc  = node.loc;
          for (const auto &f : node.fields) {
            d.parameters.push_back(Parameter{.name = f.name,
                                             .type = liftType(f.type),
                                             .key  = false,
                                             .loc  = f.loc});
          }
          return d;
        } else if constexpr (std::is_same_v<T, runtime::PredicateDecl>) {
          Decl d;
          d.kind = DeclKind::Predicate;
          d.name = node.name;
          d.loc  = node.loc;
          for (const auto &p : node.params) {
            d.parameters.push_back(Parameter{.name = p.name,
                                             .type = liftType(p.type),
                                             .key  = p.key,
                                             .loc  = node.loc});
          }
          return d;
        } else if constexpr (std::is_same_v<T, runtime::QuestionDecl>) {
          Decl d;
          d.kind   = DeclKind::Question;
          d.name   = node.name;
          d.result = liftType(node.result);
          d.loc    = node.loc;
          for (const auto &p : node.params) {
            d.parameters.push_back(Parameter{.name = p.name,
                                             .type = liftType(p.type),
                                             .key  = false,
                                             .loc  = p.loc});
          }
          return d;
        } else if constexpr (std::is_same_v<T, runtime::CapacityDecl>) {
          Decl d;
          d.kind  = DeclKind::Capacity;
          d.name  = node.name;
          d.names = node.extends;
          d.loc   = node.loc;
          for (const auto &m : node.methods) {
            Decl md;
            md.kind     = DeclKind::Method;
            md.name     = m.name;
            md.effect   = m.isEffect;
            md.preempts = m.preempts;
            md.loc      = m.loc;
            if (m.result) md.result = liftType(*m.result);
            if (m.withinMs) {
              auto dl     = std::make_unique<Expr>();
              dl->kind    = ExprKind::Duration;
              dl->text    = formatDurationMs(*m.withinMs);
              dl->loc     = m.loc;
              md.deadline = std::move(dl);
            }
            for (const auto &p : m.params) {
              md.parameters.push_back(Parameter{.name = p.name,
                                                .type = liftType(p.type),
                                                .key  = false,
                                                .loc  = p.loc});
            }
            for (const auto &c : m.contracts) {
              if (c.condition) {
                auto cond = liftExpr(*c.condition);
                if (!cond) return std::unexpected(cond.error());
                md.contracts.push_back(
                    Contract{.postcondition =
                                 (c.kind == runtime::Contract::Kind::Ensures),
                             .condition = std::move(*cond),
                             .loc       = m.loc});
              }
            }
            d.members.push_back(std::move(md));
          }
          return d;
        } else if constexpr (std::is_same_v<T, runtime::GoalDecl>) {
          return liftGoalDecl(node);
        } else if constexpr (std::is_same_v<T, runtime::PlanDecl>) {
          return liftPlanDecl(node);
        } else if constexpr (std::is_same_v<T, runtime::SkillDecl>) {
          Decl d;
          d.kind   = DeclKind::Skill;
          d.native = node.native;
          d.name   = node.name;
          d.names  = node.implements;
          d.loc    = node.loc;
          for (const auto &p : node.params) {
            d.parameters.push_back(Parameter{.name = p.name,
                                             .type = liftType(p.type),
                                             .key  = false,
                                             .loc  = p.loc});
          }
          for (const auto &m : node.members) {
            auto lifted = liftMember(m);
            if (!lifted) return std::unexpected(lifted.error());
            d.members.push_back(std::move(*lifted));
          }
          return d;
        } else if constexpr (std::is_same_v<T, runtime::BehaviorDecl>) {
          Decl d;
          d.kind = DeclKind::Behavior;
          d.name = node.name;
          d.loc  = node.loc;
          for (const auto &p : node.params) {
            d.parameters.push_back(Parameter{.name = p.name,
                                             .type = liftType(p.type),
                                             .key  = false,
                                             .loc  = p.loc});
          }
          for (const auto &m : node.members) {
            auto lifted = liftMember(m);
            if (!lifted) return std::unexpected(lifted.error());
            d.members.push_back(std::move(*lifted));
          }
          return d;
        } else if constexpr (std::is_same_v<T, runtime::AgentDecl>) {
          Decl d;
          d.kind = DeclKind::Agent;
          d.name = node.name;
          d.loc  = node.loc;
          for (const auto &p : node.params) {
            d.parameters.push_back(Parameter{.name = p.name,
                                             .type = liftType(p.type),
                                             .key  = false,
                                             .loc  = p.loc});
          }
          for (const auto &m : node.members) {
            auto lifted = liftMember(m);
            if (!lifted) return std::unexpected(lifted.error());
            d.members.push_back(std::move(*lifted));
          }
          return d;
        }
        return std::unexpected(
            Diagnostic{DiagnosticCode::Syntax, "unknown top decl node", {}});
      },
      topDecl);
}

// ============================================================================
// Program / Unit Lowering and Lifting
// ============================================================================

std::expected<runtime::Unit, Diagnostic>
lowerProgramToUnit(const Program &program) {
  runtime::Unit unit;
  if (program.module) {
    unit.module = runtime::ModuleDecl{.path = splitModulePath(*program.module),
                                      .loc  = {}};
  }
  unit.imports.reserve(program.imports.size());
  for (const auto &imp : program.imports) {
    unit.imports.push_back(
        runtime::ImportDecl{.path     = splitModulePath(imp.path),
                            .alias    = imp.alias,
                            .names    = imp.symbols,
                            .hasNames = !imp.symbols.empty(),
                            .loc      = imp.loc});
  }
  unit.decls.reserve(program.declarations.size());
  for (const auto &decl : program.declarations) {
    auto top = lowerTopDecl(decl);
    if (!top) return std::unexpected(top.error());
    unit.decls.push_back(std::move(*top));
  }
  return unit;
}

std::expected<Program, Diagnostic>
liftUnitToProgram(const runtime::Unit &unit) {
  Program program;
  if (unit.module) {
    program.module = joinModulePath(unit.module->path);
  }
  program.imports.reserve(unit.imports.size());
  for (const auto &imp : unit.imports) {
    program.imports.push_back(Import{.path    = joinModulePath(imp.path),
                                     .alias   = imp.alias,
                                     .symbols = imp.names,
                                     .loc     = imp.loc});
  }
  program.declarations.reserve(unit.decls.size());
  for (const auto &decl : unit.decls) {
    auto lifted = liftTopDecl(decl);
    if (!lifted) return std::unexpected(lifted.error());
    program.declarations.push_back(std::move(*lifted));
  }
  return program;
}

bool areEquivalent(const Decl &frontend, const runtime::TopDecl &runtime) {
  auto lifted = liftTopDecl(runtime);
  if (!lifted) return false;
  if (frontend.kind != lifted->kind) return false;
  if (frontend.name != lifted->name) return false;
  if (frontend.parameters.size() != lifted->parameters.size()) return false;
  for (std::size_t i = 0; i < frontend.parameters.size(); ++i) {
    if (frontend.parameters[i].name != lifted->parameters[i].name) return false;
    if (frontend.parameters[i].type != lifted->parameters[i].type) return false;
    if (frontend.parameters[i].key != lifted->parameters[i].key) return false;
  }
  if (frontend.members.size() != lifted->members.size()) return false;
  return true;
}

bool areEquivalent(const Decl &frontend, const runtime::Member &runtime) {
  auto lifted = liftMember(runtime);
  if (!lifted) return false;
  if (frontend.kind != lifted->kind) return false;
  if (frontend.name != lifted->name) return false;
  return true;
}

bool areEquivalent(const Program &frontend, const runtime::Unit &runtime) {
  if (frontend.module.has_value() != runtime.module.has_value()) return false;
  if (frontend.module.has_value()) {
    if (*frontend.module != joinModulePath(runtime.module->path)) return false;
  }
  if (frontend.imports.size() != runtime.imports.size()) return false;
  for (std::size_t i = 0; i < frontend.imports.size(); ++i) {
    if (frontend.imports[i].path != joinModulePath(runtime.imports[i].path))
      return false;
    if (frontend.imports[i].alias != runtime.imports[i].alias) return false;
    if (frontend.imports[i].symbols != runtime.imports[i].names) return false;
  }
  if (frontend.declarations.size() != runtime.decls.size()) return false;
  for (std::size_t i = 0; i < frontend.declarations.size(); ++i) {
    if (!areEquivalent(frontend.declarations[i], runtime.decls[i]))
      return false;
  }
  return true;
}

// ============================================================================
// Compilation Bridge
// ============================================================================

std::expected<runtime::CompiledAgent, runtime::CompileFailure>
compileProgramAgent(zigzag::vortex::VortexCore &core, const Program &program,
                    std::string_view agentType) {
  auto unit = lowerProgramToUnit(program);
  if (!unit) {
    return std::unexpected(
        runtime::CompileFailure{.code   = runtime::CompileError::Unsupported,
                                .detail = unit.error().detail,
                                .loc    = unit.error().loc});
  }
  return runtime::Compiler(core).compileAgent(*unit, agentType);
}

} // namespace xanadu::vagent
