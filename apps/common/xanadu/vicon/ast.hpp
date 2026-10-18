/**
 * @file ast.hpp
 * @brief AST for vicon: every node is a generator (spec §2).
 *
 * An expression compiles to an opcode chain that produces zero or more
 * result cells; there is no boolean node anywhere because comparisons
 * produce their right operand or fail.
 */
#ifndef COMMON_XANADU_VICON_AST_HPP
#define COMMON_XANADU_VICON_AST_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "common/xanadu/scanner_base.hpp"

namespace xanadu::vicon {

using ExprPtr = std::unique_ptr<struct Expr>;

// -- Expression node kinds ----------------------------------------------------

/// Integer / real / string / cset literal.
struct ELiteral {
  enum class Kind : std::uint8_t { Int, Real, Str, Cset } kind{Kind::Int};
  std::int64_t intValue{0};
  double realValue{0.0};
  std::string text;
};

/// A plain variable name.
struct EIdent {
  std::string name;
};

/// An `&name` keyword: &subject, &pos, &null, &fail, &time, ...
struct EKeyword {
  std::string name; ///< without the leading '&'
};

/// Unary operators: Bang `!`, Neg `-`, Not `not`.
struct EUnary {
  enum class Op : std::uint8_t { Bang, Neg, Not } op{Op::Bang};
  ExprPtr operand;
};

/// Binary operators. Comparisons produce their right operand or fail.
struct EBinary {
  enum class Op : std::uint8_t {
    Add, Sub, Mul, Div, Mod, Pow,
    Concat,                      // ||
    Lt, Le, Eq, Ne, Gt, Ge,      // numeric comparisons (produce right operand)
    StrLt, StrLe, StrEq, StrNe, StrGt, StrGe,
    Assign, RevAssign,           // := and <-
    Scan,                        // s ? e
  } op{Op::Add};
  ExprPtr left;
  ExprPtr right;
};

/// `e1 | e2` alternation: all of e1's results, then all of e2's.
struct EAlternation {
  ExprPtr first;
  ExprPtr second;
};

/// `i to j [by k]` integer-sequence generator.
struct ETo {
  ExprPtr lo;
  ExprPtr hi;
  ExprPtr by; ///< may be null (defaults to 1)
};

/// `e \ n` limitation.
struct ELimit {
  ExprPtr expr;
  ExprPtr count;
};

/// `name(args...)` call of a built-in or user procedure.
struct ECall {
  std::string name;
  std::vector<ExprPtr> args;
};

/// `s[i]` subscript / `s[i:j]` slice.
struct ESubscript {
  ExprPtr base;
  ExprPtr index;
};
struct ESlice {
  ExprPtr base;
  ExprPtr from;
  ExprPtr to;
};

/// An explicit `{ e1; e2; ... }` sequence or a statement list: the
/// expression's results are the last element's; earlier elements are bounded.
struct ESequence {
  std::vector<ExprPtr> exprs;
};

/// `if c then t [else f]`.
struct EIf {
  ExprPtr cond;
  ExprPtr thenBranch;
  ExprPtr elseBranch; ///< may be null
};

/// `while c do b` / `until c do b` (`do` may be implicit at statement level).
struct EWhile {
  ExprPtr cond;
  ExprPtr body; ///< may be null
};
struct EUntil {
  ExprPtr cond;
  ExprPtr body;
};

/// `every gen do body` — drives gen to exhaustion.
struct EEvery {
  ExprPtr gen;
  ExprPtr body;
};

/// `break [e]` / `next` — only valid inside a loop; the parser records the
/// node and the compiler refuses it outside a loop context.
struct EBreak {
  ExprPtr result; ///< may be null
};
struct ENext {};

/// `return e`, `fail`, `suspend e`, `repeat e`.
struct EReturn {
  ExprPtr expr;
};
struct EFail {};
struct ESuspend {
  ExprPtr expr;
};
struct ERepeat {
  ExprPtr expr;
};

/// `case e of { m1 : b1; m2 : b2; default : bd }`.
struct ECaseArm {
  ExprPtr match;
  ExprPtr body;
};
struct ECase {
  ExprPtr subject;
  std::vector<ECaseArm> arms;
  ExprPtr otherwise; ///< `default:` arm, may be null
};

/// One `procedure name(p, ...) ... end` declaration.
struct ProcDecl {
  std::string name;
  std::vector<std::string> params;
  ExprPtr body; ///< an ESequence
  SourceLocation loc{};
};

struct Expr {
  std::variant<ELiteral, EIdent, EKeyword, EUnary, EBinary, EAlternation, ETo,
               ELimit, ECall, ESubscript, ESlice, ESequence, EIf, EWhile,
               EUntil, EEvery, EBreak, ENext, EReturn, EFail, ESuspend,
               ERepeat, ECase>
      node;
  SourceLocation loc{};

  template <typename T> explicit Expr(T &&n, SourceLocation l = {})
      : node(std::forward<T>(n)), loc(l) {}
};

struct Program {
  std::vector<ProcDecl> procedures;
  std::vector<ExprPtr> statements; ///< top-level (initial/main) statements
};

} // namespace xanadu::vicon

#endif // COMMON_XANADU_VICON_AST_HPP
