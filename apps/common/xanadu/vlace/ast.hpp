/**
 * @file ast.hpp
 * @brief Abstract syntax tree for vlace (design/vlace-functional-language.md
 *        §5.1). Expression, declaration, and pattern nodes.
 *
 * The tree is owning and immutable-by-convention: expressions and patterns are
 * held by std::unique_ptr through the Expr/Pattern wrappers so the recursive
 * variants can refer to themselves. Every node carries a SourceLocation so the
 * type checker and compiler can render "vlace: file:line:col: Code: detail"
 * diagnostics (§8.3).
 */
#ifndef COMMON_XANADU_VLACE_AST_HPP
#define COMMON_XANADU_VLACE_AST_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "common/xanadu/vlace/token.hpp"

namespace xanadu::vlace {

// -- Literals ----------------------------------------------------------------

enum class LiteralKind : std::uint8_t { Int, Float, String, Char };

struct Literal {
  LiteralKind kind{LiteralKind::Int};
  std::int64_t intValue{0};
  double floatValue{0.0};
  std::string textValue; ///< string / char payload
};

// -- Forward declarations for the recursive tree -----------------------------

struct ExprNode;
struct PatternNode;
using Expr    = std::unique_ptr<ExprNode>;
using Pattern = std::unique_ptr<PatternNode>;

// -- Patterns (§5.1) ---------------------------------------------------------

struct PWildcard {}; ///< _
struct PVar {
  std::string name;
};
struct PLiteral {
  Literal lit;
};
struct PConstructor { ///< C p1 p2 ...  (also cons p:q and list [a,b])
  std::string ctor;
  std::vector<Pattern> args;
};
struct PTuple {
  std::vector<Pattern> elems;
};
struct PRecord { ///< C { f = p, ... }
  std::string ctor;
  std::vector<std::pair<std::string, Pattern>> fields;
};

struct PatternNode {
  std::variant<PWildcard, PVar, PLiteral, PConstructor, PTuple, PRecord> node;
  SourceLocation loc{};
};

// -- Case alternatives (§5.4) ------------------------------------------------

struct Alt {
  Pattern pattern;
  std::optional<Expr> guard; ///< optional `if g`
  Expr body;
};

// -- let / letrec bindings ---------------------------------------------------

struct Binding {
  Pattern pattern; ///< usually a PVar, but any pattern (destructuring let)
  Expr value;
};

// -- Expressions (§5.1) ------------------------------------------------------

struct EVar {
  std::string name; ///< may be qualified "Module.name"
};
struct ECtor {
  std::string name;
};
struct ELiteral {
  Literal lit;
};
struct ELet {
  bool recursive{false}; ///< let vs letrec
  std::vector<Binding> bindings;
  Expr body;
};
struct EIf {
  Expr cond;
  Expr then;
  Expr elze;
};
struct ECase {
  Expr scrutinee;
  std::vector<Alt> alts;
};
struct ELambda {
  std::vector<Pattern> params; ///< fn p1 p2 ... => body
  Expr body;
};
struct EApp { ///< function application f a (left-assoc, built by the parser)
  Expr fn;
  Expr arg;
};
struct EBinOp { ///< a `op` b, resolved by the fixity table (§5.1)
  std::string op;
  Expr lhs;
  Expr rhs;
};
struct EList { ///< [a, b, c]  (sugar for a : b : c : [])
  std::vector<Expr> elems;
};
struct ETuple { ///< (a, b, ...) -- always >= 2 elements
  std::vector<Expr> elems;
};
struct ERecordUpdate { ///< r { f = e, ... }
  Expr record;
  std::vector<std::pair<std::string, Expr>> fields;
};

struct ExprNode {
  std::variant<EVar, ECtor, ELiteral, ELet, EIf, ECase, ELambda, EApp, EBinOp,
               EList, ETuple, ERecordUpdate>
      node;
  SourceLocation loc{};
};

// -- Type expressions (§5.1) -------------------------------------------------

struct TypeNode;
using Type = std::unique_ptr<TypeNode>;

struct TVar {
  std::string name;
}; ///< lowercase a
struct TCon { ///< Int, List a (constructor applied to args)
  std::string name;
  std::vector<Type> args;
};
struct TFun {
  Type from;
  Type to;
};

struct TypeNode {
  std::variant<TVar, TCon, TFun> node;
  SourceLocation loc{};
};

// -- Declarations (§5.1) -----------------------------------------------------

struct CtorDef {
  std::string name;
  std::vector<Type> argTypes;                       ///< positional
  std::vector<std::pair<std::string, Type>> fields; ///< record form
  bool isRecord{false};
};

struct DataDecl {
  std::string name;                ///< type constructor, e.g. Tree
  std::vector<std::string> params; ///< type variables
  std::vector<CtorDef> ctors;
};

struct TypeDecl { ///< type alias
  std::string name;
  std::vector<std::string> params;
  Type aliased;
};

struct ValueDecl { ///< f p... = e  (desugars to f = fn p... => e)
  std::string name;
  std::vector<Pattern> params;
  Expr body;
};

struct ImportDecl {
  std::string modulePath;           ///< "std:fun"
  std::optional<std::string> alias; ///< as C
  std::vector<std::string> names;   ///< selective; empty = all
};

enum class Fixity : std::uint8_t { Left, Right, None };
struct FixityDecl {
  Fixity fixity{Fixity::Left};
  int level{9};
  std::string op;
};

using Decl =
    std::variant<ValueDecl, DataDecl, TypeDecl, ImportDecl, FixityDecl>;

// -- Program -----------------------------------------------------------------

struct Program {
  std::vector<Decl> decls;
  std::optional<Expr> result; ///< the trailing expression, if any
};

// -- Compile errors (§8.2) ---------------------------------------------------

enum class CompileError : std::uint8_t {
  LexicalError,
  UnexpectedToken,
  UnboundIdentifier,
  TypeMismatch,
  OccursCheckFailed,
  NonLinearPattern,
  NonExhaustiveMatch,
  UnknownModule,
};

[[nodiscard]] std::string_view compileErrorName(CompileError err) noexcept;

} // namespace xanadu::vlace

#endif // COMMON_XANADU_VLACE_AST_HPP
