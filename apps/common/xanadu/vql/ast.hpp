/**
 * @file ast.hpp
 * @brief Abstract Syntax Tree (AST) node representations for VQL v13.0.
 */
#ifndef COMMON_XANADU_VQL_AST_HPP
#define COMMON_XANADU_VQL_AST_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "common/xanadu/zigzag/dim_vector.hpp"

namespace xanadu::vql {

// -- Enums -------------------------------------------------------------------

enum class Placement : std::uint8_t {
  Default,
  From,
  Rank,
  Head,
  Tail,
};

enum class YieldMode : std::uint8_t {
  Default, ///< "!new"
  New,     ///< "!new"
  Last,    ///< "!last"
  Both,    ///< "!both"
  Keep,    ///< "!keep" or "!"
};

enum class Repetition : std::uint8_t {
  Once,
  ZeroOrMore, ///< "*"
  OneOrMore,  ///< "+"
  ZeroOrOne,  ///< "?"
};

enum class CompOp : std::uint8_t {
  Equal,
  NotEqual,
  LessThan,
  GreaterThan,
  LessEqual,
  GreaterEqual,
};

enum class AnchorKind : std::uint8_t {
  Home,        ///< "##"
  NamedStore,  ///< "##NAME"
  Root,        ///< "#"
  Cursor,      ///< "^"
  NamedCursor, ///< "^NAME"
  Variable,    ///< "$var"
  LiteralCellId,
  Context, ///< "."
  Create,  ///< "%" or "%VALUE"
};

// -- Forward Declarations ----------------------------------------------------

struct PathExpression;
struct FunctionInvocation;
struct ActionClause;
struct EffectClause;

// -- Literals and Value Expressions ------------------------------------------

struct ScalarLiteral {
  std::variant<std::string, double, std::int64_t, bool> value{};
};

struct ValueExpr {
  std::variant<std::shared_ptr<PathExpression>, ScalarLiteral,
               std::string /*VariableRef*/, std::shared_ptr<FunctionInvocation>>
      kind{};
};

struct FunctionInvocation {
  std::string name{};
  std::vector<ValueExpr> args{};
};

// -- Boolean & Predicate Logic -----------------------------------------------

struct ComparisonExpr {
  std::shared_ptr<ValueExpr> left{nullptr};
  CompOp op{CompOp::Equal};
  std::shared_ptr<ValueExpr> right{nullptr};
};

struct PredicateTest {
  std::variant<std::shared_ptr<PathExpression>, bool /*ContextDot*/,
               std::shared_ptr<ValueExpr> /*ExtendedTruthiness*/,
               std::shared_ptr<FunctionInvocation>>
      kind{};
};

struct BooleanFactor {
  bool negated{false};
  std::variant<ComparisonExpr, PredicateTest> test{};
};

struct BooleanTerm {
  std::vector<BooleanFactor> factors{}; ///< AND conjunction
};

struct BooleanExpr {
  std::vector<BooleanTerm> terms{}; ///< OR disjunction
};

// -- Path Steps & Creation ----------------------------------------------------

struct CreateValue {
  enum class Kind : std::uint8_t { Empty, Literal, Expression };
  Kind kind{Kind::Empty};
  std::string literal{};
  std::shared_ptr<ValueExpr> expr{nullptr};
};

// -- Anchors & Range Clamps --------------------------------------------------

struct AnchorNode {
  AnchorKind kind{AnchorKind::Home};
  std::string name{};
  std::uint64_t cellId{0};
  bool derefMaster{false}; ///< Suffixed with ">"
  std::optional<CreateValue> createValue{std::nullopt};
};

struct RangeClamp {
  std::int64_t start{1};
  std::optional<std::int64_t> end{std::nullopt};
};

struct SignedDimensionStep {
  std::string dimName{};
  zigzag::DimVector direction{zigzag::DimVector::POS};
  Placement placement{Placement::Default};
  std::vector<CreateValue> creates{};
};

struct MacroDimensionGroup;

using StepSelector =
    std::variant<SignedDimensionStep, std::shared_ptr<MacroDimensionGroup>,
                 FunctionInvocation>;

struct PathStep {
  StepSelector selector{};
  bool derefMaster{false}; ///< Suffixed with ">"
  std::vector<BooleanExpr> predicates{};
  std::optional<RangeClamp> rangeClamp{std::nullopt};
  YieldMode yieldMode{YieldMode::Default};
};

struct MacroDimensionGroup {
  std::vector<PathStep> steps{};
  Repetition repetition{Repetition::Once};
};

struct CloneOperand {
  std::shared_ptr<PathExpression> path{nullptr};
  std::optional<CreateValue> bareCreate{std::nullopt};
};

struct CloneTail {
  std::vector<CloneOperand> operands{};
};

struct PathExpression {
  AnchorNode anchor{};
  std::vector<PathStep> steps{};
  std::optional<CloneTail> cloneTail{std::nullopt};
};

// -- FLWOR & Action Clauses --------------------------------------------------

struct FieldWeave {
  std::vector<PathStep> steps{};
};

struct ReturnItem {
  std::variant<PathExpression, FieldWeave> item{};
};

struct ReturnClause {
  std::vector<ReturnItem> items{};
};

struct ForClause {
  std::string varName{};
  PathExpression inPath{};
};

struct LetClause {
  std::string varName{};
  std::variant<PathExpression, ValueExpr> target{};
};

struct EffectItem {
  std::variant<LetClause, PathExpression,
               std::pair<ForClause, std::shared_ptr<EffectClause>>>
      item{};
};

struct EffectClause {
  std::vector<EffectItem> items{};
};

struct ConditionalClause {
  BooleanExpr condition{};
  std::shared_ptr<ActionClause> thenClause{nullptr};
  std::shared_ptr<ActionClause> elseClause{nullptr};
};

struct ActionClause {
  std::variant<ReturnClause, EffectClause, ConditionalClause> clause{};
};

struct WhereClause {
  BooleanExpr condition{};
};

struct ExecutionBlock {
  std::vector<std::variant<ForClause, LetClause>> bindings{};
  std::optional<WhereClause> where{std::nullopt};
  ActionClause action{};
};

struct QueryExpression {
  std::variant<ExecutionBlock, PathExpression> expr{};
};

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_AST_HPP
