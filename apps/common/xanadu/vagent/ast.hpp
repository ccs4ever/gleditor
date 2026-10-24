/**
 * @file ast.hpp
 * @brief AST for vagent: one node per production of design §8.9.
 *
 * The parser builds exactly what the grammar says and nothing the checker
 * owns: `parent`, `occurrence` and the like are plain names with a flag, a
 * `PascalCase` constraint is not verified, and an `effect` method in a skill
 * parses. Those are the static restrictions of §8.9, later work.
 */
#ifndef COMMON_XANADU_VAGENT_AST_HPP
#define COMMON_XANADU_VAGENT_AST_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "common/xanadu/vagent/token.hpp"

namespace xanadu::vagent {

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

// -- Types ------------------------------------------------------------------

/// `Name`, `Name<A, B>`, `[T]` or `()`.
struct TypeExpr {
  enum class Kind : std::uint8_t { Named, List, Unit } kind{Kind::Named};
  std::string name;           ///< Named only
  std::vector<TypeExpr> args; ///< Named: type arguments. List: the element.
  SourceLocation loc{};
};

struct Param {
  std::string name;
  TypeExpr type;
  SourceLocation loc{};
};

// -- Terms ------------------------------------------------------------------

/// `?x` (a logic variable) or an ordinary expression.
struct TermArg {
  std::optional<std::string> logicVar;
  ExprPtr expr; ///< null when logicVar is set
};

/// `name(args)`: a belief or trigger term.
struct Term {
  std::string name;
  std::vector<TermArg> args;
  SourceLocation loc{};
};

/// `Name(args)`: the operand of `adopt` and `achieve`.
struct GoalCall {
  std::string name;
  std::vector<ExprPtr> args;
  SourceLocation loc{};
};

// -- Expressions ------------------------------------------------------------

struct ELiteral {
  enum class Kind : std::uint8_t {
    Int,
    Float,
    Str,
    Bool,
    Duration,
    Unit, ///< `()`
    None
  } kind{Kind::Int};
  std::int64_t intValue{0};
  double floatValue{0.0};
  bool boolValue{false};
  std::int64_t durationMs{0};
  std::string text;
};

struct EName {
  std::string name;
};

/// `self`, or one of the names §8.8 puts in scope. They are ordinary
/// identifiers to the lexer; the parser tags them so the checker need not
/// compare spellings.
struct ESpecial {
  enum class Kind : std::uint8_t {
    Self,
    Parent,
    Inner,
    Outer,
    Occurrence
  } kind{Kind::Self};
};

struct EUnary {
  enum class Op : std::uint8_t { Neg, Generate, Size, Not } op{Op::Neg};
  ExprPtr operand;
};

struct EBinary {
  enum class Op : std::uint8_t {
    Mul,
    Div,
    Mod,
    Add,
    Sub,
    Concat,
    Lt,
    Le,
    Gt,
    Ge,
    Eq,
    Ne,
    Alt, ///< `|`
    Scan,
    Assign,    ///< `:=`
    RevAssign, ///< `<-`
    And        ///< `&`
  } op{Op::Add};
  ExprPtr left;
  ExprPtr right;
};

/// `lo to hi [by step]`
struct ERange {
  ExprPtr lo;
  ExprPtr hi;
  ExprPtr by; ///< may be null
};

struct ECall {
  ExprPtr callee;
  std::vector<ExprPtr> args;
};
struct EIndex {
  ExprPtr base;
  ExprPtr index;
};
struct EField {
  ExprPtr base;
  std::string name;
};
struct EList {
  std::vector<ExprPtr> elems;
};

struct RecordField {
  std::string name;
  ExprPtr value;
};
struct ERecord {
  std::string type;
  std::vector<RecordField> fields;
};

struct Stmt {
  /// `let name [: type] := value` when `isLet`, otherwise just `value`.
  bool isLet{false};
  std::string name;
  std::optional<TypeExpr> type;
  ExprPtr value;
  SourceLocation loc{};
};
struct EBlock {
  std::vector<Stmt> stmts;
};

struct EIf {
  ExprPtr cond;
  ExprPtr thenBranch;
  ExprPtr elseBranch; ///< may be null
};
struct EWhile {
  ExprPtr cond;
  ExprPtr body;
};
/// `for x in e do body`, or `for believes term do body` (binder empty).
struct EFor {
  std::string binder;
  ExprPtr iterable;                 ///< null for the `believes` form
  std::optional<Term> believesTerm; ///< set for the `believes` form
  ExprPtr body;
};

struct EBelieves {
  Term term;
  ExprPtr from; ///< may be null
};
struct EFindall {
  TermArg templ;
  Term goal;
};

// -- Actions (§8.6) ---------------------------------------------------------

struct EEmit {
  ExprPtr event;
  ExprPtr space;      ///< `in`, may be null
  ExprPtr recipients; ///< `for`, may be null
};
struct ETell {
  bool untell{false};
  ExprPtr to;
  Term fact;
};
struct EBeliefChange {
  enum class Kind : std::uint8_t {
    Believe,
    Forget,
    Revise
  } kind{Kind::Believe};
  Term fact;
};
struct ESpawn {
  std::string agent;
  std::vector<ExprPtr> args;
  ExprPtr space; ///< may be null
};
struct EAdopt {
  GoalCall goal;
};
struct EAchieve {
  GoalCall goal;
  ExprPtr by;     ///< may be null; when set, `within` is too
  ExprPtr within; ///< may be null
  ExprPtr orElse; ///< may be null
};
struct EAsk {
  ExprPtr question; ///< a postfix expression: `agent.q(args)`
  ExprPtr within;
  ExprPtr orElse; ///< may be null
};
struct EAwait {
  std::optional<Term> term; ///< `await t ...`
  std::string eventType;    ///< `await E as e ...`
  std::string binder;
  ExprPtr guard; ///< may be null
  ExprPtr within;
  ExprPtr orElse; ///< may be null
};
struct EDrop {
  ExprPtr target;
};
struct EStop {
  ExprPtr target;
  bool keepChildren{false}; ///< `keeping children` (§4.5)
};
struct ETerminate {
  bool keepChildren{false};
};
/// `install S(args)`, `uninstall S`, `mount B(args)`, `unmount B`.
struct EInstall {
  enum class Kind : std::uint8_t {
    Install,
    Uninstall,
    Mount,
    Unmount
  } kind{Kind::Install};
  std::string name;
  std::vector<ExprPtr> args;
  bool hasParens{false};
};
struct EReturn {
  ExprPtr value; ///< may be null
};
struct EYield {};
struct EConsume {};
struct EFail {};

struct Expr {
  std::variant<ELiteral, EName, ESpecial, EUnary, EBinary, ERange, ECall,
               EIndex, EField, EList, ERecord, EBlock, EIf, EWhile, EFor,
               EBelieves, EFindall, EEmit, ETell, EBeliefChange, ESpawn, EAdopt,
               EAchieve, EAsk, EAwait, EDrop, EStop, ETerminate, EInstall,
               EReturn, EYield, EConsume, EFail>
      node;
  SourceLocation loc{};

  template <typename T>
  explicit Expr(T &&n, SourceLocation l = {})
      : node(std::forward<T>(n)), loc(l) {}
};

// -- Declarations -----------------------------------------------------------

struct Field {
  std::string name; ///< any word
  TypeExpr type;
  SourceLocation loc{};
};

struct ModuleDecl {
  std::vector<std::string> path;
  SourceLocation loc{};
};
struct ImportDecl {
  std::vector<std::string> path;
  std::optional<std::string> alias;
  std::vector<std::string> names;
  bool hasNames{false};
  SourceLocation loc{};
};

struct EventDecl {
  std::string name;
  std::optional<std::string> extends;
  std::vector<Field> fields;
  bool hasBody{false};
  SourceLocation loc{};
};
struct RecordDecl {
  std::string name;
  std::vector<Field> fields;
  SourceLocation loc{};
};
struct PredParam {
  bool key{false};
  std::string name;
  TypeExpr type;
};
struct PredicateDecl {
  std::string name;
  std::vector<PredParam> params;
  SourceLocation loc{};
};
struct QuestionDecl {
  std::string name;
  std::vector<Param> params;
  TypeExpr result;
  SourceLocation loc{};
};

struct Contract {
  enum class Kind : std::uint8_t { Requires, Ensures } kind{Kind::Requires};
  ExprPtr condition;
};
struct MethodSig {
  bool isEffect{false};
  bool preempts{false};
  std::string name;
  std::vector<Param> params;
  std::optional<TypeExpr> result;
  std::optional<std::int64_t> withinMs;
  std::vector<Contract> contracts;
  SourceLocation loc{};
};
struct MethodDef {
  MethodSig sig;
  ExprPtr body; ///< an EBlock
};
struct AnswerDef {
  std::string name;
  std::vector<Param> params;
  TypeExpr result;
  ExprPtr body;
  SourceLocation loc{};
};
struct CapacityDecl {
  std::string name;
  std::vector<std::string> extends;
  std::vector<MethodSig> methods;
  SourceLocation loc{};
};

struct UsesDecl {
  std::vector<std::string> capacities;
  SourceLocation loc{};
};
struct VarDecl {
  std::string name;
  TypeExpr type;
  ExprPtr init;
  SourceLocation loc{};
};
struct LifecycleDef {
  bool uninstall{false};
  ExprPtr body;
  SourceLocation loc{};
};
struct InstallDecl {
  std::string name;
  std::vector<ExprPtr> args;
  bool hasParens{false};
  SourceLocation loc{};
};
struct BehaviorUse {
  std::string name;
  std::vector<ExprPtr> args;
  bool hasParens{false};
  SourceLocation loc{};
};
struct BeliefDecl {
  Term fact;
  SourceLocation loc{};
};

struct EventTrigger {
  std::string eventType;
  std::optional<std::string> binder; ///< after `as`
};
struct BeliefTrigger {
  bool added{true}; ///< `+` or `-`
  Term fact;
};
struct HandlerDef {
  std::variant<EventTrigger, BeliefTrigger> trigger;
  ExprPtr guard; ///< may be null
  std::optional<std::int64_t> priority;
  ExprPtr body;
  SourceLocation loc{};
};

struct GoalClause {
  enum class Kind : std::uint8_t { SucceedWhen, DropWhen, Holds } kind{};
  ExprPtr condition;
};
struct GoalDecl {
  enum class Kind : std::uint8_t { Achieve, Perform, Maintain } kind{};
  std::string name;
  std::vector<Param> params;
  std::vector<GoalClause> clauses;
  bool hasBody{false};
  SourceLocation loc{};
};
struct PlanDecl {
  std::string name;
  std::string goal;
  std::vector<std::string> params; ///< names of the goal's parameters used
  bool hasParams{false};
  ExprPtr where; ///< may be null
  ExprPtr body;
  SourceLocation loc{};
};

/// A body member of a skill, behavior or agent. The parser enforces which
/// alternatives each container admits (§8.9: AgentOnly is agents only).
using Member =
    std::variant<UsesDecl, VarDecl, HandlerDef, MethodDef, AnswerDef, GoalDecl,
                 PlanDecl, InstallDecl, BehaviorUse, BeliefDecl, LifecycleDef>;

struct SkillDecl {
  bool native{false};
  std::string name;
  std::vector<Param> params;
  bool hasParams{false};
  std::vector<std::string> implements;
  std::vector<Member> members;
  SourceLocation loc{};
};
struct BehaviorDecl {
  std::string name;
  std::vector<Param> params;
  bool hasParams{false};
  std::vector<Member> members;
  SourceLocation loc{};
};
struct AgentDecl {
  std::string name;
  std::vector<Param> params;
  bool hasParams{false};
  std::vector<Member> members;
  SourceLocation loc{};
};

using TopDecl = std::variant<EventDecl, RecordDecl, PredicateDecl, QuestionDecl,
                             CapacityDecl, SkillDecl, BehaviorDecl, AgentDecl,
                             GoalDecl, PlanDecl>;

/// One source file: `[module] {import} {declaration}`.
struct Unit {
  std::optional<ModuleDecl> module;
  std::vector<ImportDecl> imports;
  std::vector<TopDecl> decls;
};

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_AST_HPP
