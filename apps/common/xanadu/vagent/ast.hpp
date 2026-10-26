#ifndef COMMON_XANADU_VAGENT_AST_HPP
#define COMMON_XANADU_VAGENT_AST_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
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
};

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

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
};

struct Parameter {
  std::string name;
  Type type;
  bool key{false};
  SourceLocation loc{};
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
