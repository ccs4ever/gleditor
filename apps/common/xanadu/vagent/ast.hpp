#ifndef COMMON_XANADU_VAGENT_AST_HPP
#define COMMON_XANADU_VAGENT_AST_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "common/xanadu/vagent/token.hpp"

namespace xanadu::vagent {

struct Type {
  std::string name;
  std::vector<Type> arguments;
  bool operator==(const Type &) const = default;
};

[[nodiscard]] inline std::string typeName(const Type &type) {
  if (type.name == "List" && type.arguments.size() == 1) {
    return "[" + typeName(type.arguments.front()) + "]";
  }
  std::string result = type.name;
  if (!type.arguments.empty()) {
    result += '<';
    for (std::size_t i = 0; i < type.arguments.size(); ++i) {
      if (i != 0) result += ", ";
      result += typeName(type.arguments[i]);
    }
    result += '>';
  }
  return result;
}

enum class ExprKind : std::uint8_t {
  Integer,
  Float,
  String,
  Duration,
  Boolean,
  None,
  Unit,
  Identifier,
  LogicVariable,
  Unary,
  Binary,
  Range,
  Call,
  Index,
  Field,
  Block,
  List,
  Cells,
  Record,
  If,
  While,
  For,
  ForBelieves,
  Let,
  Term,
  Believes,
  FindAll,
  Emit,
  Tell,
  Untell,
  Believe,
  Forget,
  Revise,
  Spawn,
  Adopt,
  Achieve,
  Ask,
  AwaitTerm,
  AwaitEvent,
  Drop,
  Install,
  Uninstall,
  Mount,
  Unmount,
  Stop,
  Terminate,
  Return,
  Yield,
  Consume,
  Fail,
  Code,
  Splice,
  Static,
};

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

// The resolve-only pass's binding-identity stamp (§5.6 of
// design/vagent-metaprogramming.md). `scope` identifies the scope the node
// resolved in — carried on every Expr, not only Identifier, so expand can
// seed a code"…" body subtree from its literal's own scope — and `ordinal`
// identifies one binding within that scope, present on Identifier and
// LogicVariable nodes. A Code node's ordinal is a *fragment* ordinal
// instead — minted program-wide, it distinguishes two literals that share
// a SourceLocation coordinate space (nested literals in different bodies)
// — so (loc, ordinal) stays a unique fragment identity. Absent until
// resolve runs; cloneExpr and cloneDecl preserve it verbatim because
// kernel side tables key on `const Expr *` — a pointer-keyed stamp table
// would die under cloning.
struct ScopeStamp {
  std::uint64_t scope{0};
  std::optional<std::uint64_t> ordinal;
  bool operator==(const ScopeStamp &) const = default;
};

// The §5.6 fragment-table key: a code"…" literal's SourceLocation plus the
// fragment ordinal its ScopeStamp carries (absent for a literal resolve
// never env-walked — a stampScopeOnly'd subtree — where loc alone is the
// key). cloneExpr preserves both fields, so every clone of one literal
// resolves to the original's record.
struct FragmentKey {
  SourceLocation loc{};
  std::optional<std::uint64_t> ordinal;
};
[[nodiscard]] inline bool operator<(const FragmentKey &left,
                                    const FragmentKey &right) {
  return std::tie(left.loc.line, left.loc.column, left.loc.offset,
                  left.ordinal) < std::tie(right.loc.line, right.loc.column,
                                           right.loc.offset, right.ordinal);
}

// Fixed-role optional expressions avoid positional sentinels for scope,
// recipients, deadline, guard and recovery in actions. Ordinary operands are
// ordered in children; record names are parallel to their value children.
struct Expr {
  ExprKind kind{ExprKind::Unit};
  std::string text;
  SourceLocation loc{};
  std::vector<ExprPtr> children;
  std::vector<std::string> names;
  std::optional<Type> annotation;
  ExprPtr scope;
  ExprPtr recipients;
  ExprPtr deadline;
  ExprPtr guard;
  ExprPtr recovery;
  std::optional<std::string> binding;
  std::optional<ScopeStamp> stamp;
};

struct Parameter {
  std::string name;
  Type type;
  bool key{false};
  SourceLocation loc{};
};

// §5.1's CodeBody (design/vagent-metaprogramming.md) — the parsed inner
// text of a code"…" literal, produced by Parser::parseCodeBody. `label`
// is the optional '#'-prefixed user label on the fragment head (§6.7);
// `parameters` the optional 'Ident : Type {,…} |' template signature;
// `body` the fragment expression. Every SourceLocation inside is a
// literal-relative inner offset — the src:m:l:c+o:c convention qualifies
// them with the literal's own loc at report time (code_path's
// innerDiagnostic).
struct CodeBody {
  std::optional<std::string> label;
  std::vector<Parameter> parameters;
  ExprPtr body;
};

struct Contract {
  bool postcondition{false};
  ExprPtr condition;
  SourceLocation loc{};
};

struct GoalClause {
  enum class Kind : std::uint8_t { Succeed, Drop, Holds };
  Kind kind{Kind::Succeed};
  ExprPtr condition;
  SourceLocation loc{};
};

enum class DeclKind : std::uint8_t {
  Event,
  Record,
  Predicate,
  Question,
  Capacity,
  Skill,
  Behavior,
  Agent,
  Goal,
  Plan,
  Method,
  Answer,
  Handler,
  Field,
  Uses,
  Install,
  BehaviorUse,
  Belief,
  Lifecycle,
};

enum class GoalKind : std::uint8_t { Achieve, Perform, Maintain };

struct Decl {
  DeclKind kind{DeclKind::Event};
  std::string name;
  SourceLocation loc{};
  std::vector<Parameter> parameters;
  std::optional<Type> result;
  std::vector<std::string> names;
  std::optional<std::string> target;
  std::optional<GoalKind> goalKind;
  std::optional<std::vector<std::string>> planBindings;
  std::vector<Contract> contracts;
  std::vector<GoalClause> goalClauses;
  std::vector<Decl> members;
  ExprPtr body;
  ExprPtr guard;
  ExprPtr initializer;
  ExprPtr trigger;
  std::optional<std::string> binding;
  std::optional<std::int64_t> priority;
  ExprPtr deadline;
  bool native{false};
  bool effect{false};
  bool preempts{false};
};

struct Import {
  std::string path;
  std::optional<std::string> alias;
  std::vector<std::string> symbols;
  SourceLocation loc{};
};

struct Program {
  std::optional<std::string> module;
  std::vector<Import> imports;
  std::vector<Decl> declarations;
};

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_AST_HPP
