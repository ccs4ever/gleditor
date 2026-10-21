/**
 * @file ast.hpp
 * @brief Abstract Syntax Tree (AST) node representations for VQL v15.1.
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
  Home,            ///< "##"
  NamedStore,      ///< "##NAME"
  Root,            ///< "#"
  Cursor,          ///< "^"
  NamedCursor,     ///< "^NAME"
  AllCursors,      ///< "^^"
  AllNamedCursors, ///< "^^NAME"
  Variable,        ///< "$var"
  LiteralCellId,
  Context,        ///< "."
  Create,         ///< "%" or "%VALUE"
  StoreGenerator, ///< "current()", "local(...) ", "topic(...)", etc.
  StoreAnchor,    ///< "##btpk(...)", "##topic(...)", etc.
  Microversion,   ///< "@2a9", "@88"
};

enum class ShapeWildcard : std::uint8_t {
  Everything, ///< "*"
  Nothing,    ///< "!*"
};

// -- Forward Declarations ----------------------------------------------------

struct PathExpression;
struct FunctionInvocation;
struct ActionClause;
struct EffectClause;
struct ShapeLiteral;
struct VersionExpr;
struct PathLambda;
struct Statement;
struct ScriptBody;

// -- Literals and Value Expressions ------------------------------------------

struct ScalarLiteral {
  std::variant<std::string, double, std::int64_t, bool> value;
};

struct VersionExpr {
  enum class Op : std::uint8_t {
    Literal,  ///< raw microversion string
    Variable, ///< $var
    Next,     ///< + 1
    Parent,   ///< - 1
    Branch,   ///< / "branch"
    Range,    ///< ..
  };
  Op op{Op::Literal};
  std::string versionStr;
  std::shared_ptr<VersionExpr> left{nullptr};
  std::shared_ptr<VersionExpr> right{nullptr};
  std::int64_t stepOffset{0};
  std::string branchName;
};

struct VersionPin {
  std::vector<VersionExpr> versions;
  bool isRange{false}; ///< whether specified with ".."
};

struct BooleanExpr;

struct ValueExpr {
  std::variant<std::shared_ptr<PathExpression>, ScalarLiteral,
               std::string /*VariableRef*/, std::shared_ptr<FunctionInvocation>,
               std::shared_ptr<ShapeLiteral>, std::shared_ptr<VersionExpr>,
               std::shared_ptr<PathLambda>, std::shared_ptr<BooleanExpr>>
      kind;
};

struct FunctionInvocation {
  std::string name;
  std::vector<ValueExpr> args;
};

// -- First-Class Shapes -------------------------------------------------------

struct DimensionShapeRule {
  bool negated{false}; ///< "!"
  bool isBidir{false}; ///< "~"
  zigzag::DimVector direction{zigzag::DimVector::POS};
  std::string dimName;
  std::shared_ptr<ValueExpr> targetValue{nullptr}; ///< optional ": expr"
};

struct DimensionPivotRule {
  std::string fromDim;
  std::string toDim;
};

struct VirtualDimensionDefRule {
  std::string dimName;
  zigzag::DimVector direction{zigzag::DimVector::POS};
  std::shared_ptr<ValueExpr> expr{nullptr};
};

struct ShapeRule {
  std::variant<ShapeWildcard, std::string /*VariableRef $shape*/,
               std::shared_ptr<FunctionInvocation>, DimensionShapeRule,
               DimensionPivotRule, VirtualDimensionDefRule,
               std::shared_ptr<ShapeLiteral>>
      rule;
};

struct ShapeLiteral {
  std::vector<ShapeRule> rules;
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
               std::shared_ptr<FunctionInvocation>,
               std::shared_ptr<ShapeLiteral> /*ShapePredicate*/>
      kind;
};

struct BooleanFactor {
  bool negated{false};
  std::variant<ComparisonExpr, PredicateTest> test;
};

struct BooleanTerm {
  std::vector<BooleanFactor> factors; ///< AND conjunction
};

struct BooleanExpr {
  std::vector<BooleanTerm> terms; ///< OR disjunction
};

// -- Path Steps & Creation ----------------------------------------------------

struct CreateValue {
  enum class Kind : std::uint8_t { Empty, Literal, Expression };
  Kind kind{Kind::Empty};
  std::string literal;
  std::shared_ptr<ValueExpr> expr{nullptr};
};

// -- Anchors & Range Clamps --------------------------------------------------

struct StoreGeneratorCall {
  std::string opName; ///< "current", "local", "topic", "btpk", "citing",
                      ///< "backlinks", "find", "system", "btih", "ledger"
  std::vector<ValueExpr> args;
};

struct StoreAnchor {
  std::string method; ///< "btpk", "topic", etc.
  std::vector<ValueExpr> args;
  std::string sliceName;
  bool derefMaster{false};
};
using StoreAnchorSpec = StoreAnchor;

struct AnchorNode {
  AnchorKind kind{AnchorKind::Home};
  std::string name;
  std::uint64_t cellId{0};
  bool derefMaster{false}; ///< Suffixed with ">"
  std::optional<CreateValue> createValue{std::nullopt};
  std::optional<StoreGeneratorCall> generator{std::nullopt};
  std::optional<StoreAnchorSpec> storeAnchor{std::nullopt};
  std::optional<VersionPin> versionPin{std::nullopt};
  std::string microversionId;
};

struct RangeClamp {
  std::int64_t start{1};
  std::optional<std::int64_t> end{std::nullopt};
};

struct SignedDimensionStep {
  std::string dimName;
  zigzag::DimVector direction{zigzag::DimVector::POS};
  bool isBidir{false}; ///< "~d.tag"
  Placement placement{Placement::Default};
  std::vector<CreateValue> creates;
};

struct MacroDimensionGroup;

struct GenesisSelector {}; ///< "/##"

struct SliceSelectorStep {
  std::optional<std::string> nameFilter{std::nullopt};
  std::shared_ptr<BooleanExpr> predicateFilter{nullptr};
};

struct DocSelectorStep {
  std::optional<std::string> nameFilter{std::nullopt};
  std::shared_ptr<BooleanExpr> predicateFilter{nullptr};
};

struct PathMorphism {
  enum class Op : std::uint8_t { Arrow, Transclude };
  Op op{Op::Arrow};
  std::string perspective; ///< optional "@persp:"
  ValueExpr subject;       ///< StringLiteral, SignedDimension, VariableRef
  std::vector<ValueExpr> targets; ///< Sprout list or single ValueExpr
  bool isSproutList{false};
};

struct DimensionPivotStep {
  std::string fromDim;
  std::string toDim;
};

struct VirtualDimensionStep {
  SignedDimensionStep dimStep;
  ValueExpr expr;
};

struct PathStep;

struct RootSelectorStep {
  enum class Kind : std::uint8_t {
    Genesis,
    RootMetacells,
    ActiveCursor,
    AllCursors,
    NamedCursor,
    AllNamedCursors,
  };
  Kind kind{Kind::Genesis};
  std::string name;
};

struct PerspectiveStep {
  std::string perspective;
  std::shared_ptr<PathStep> targetStep{nullptr};
};

using StepSelector =
    std::variant<SignedDimensionStep, std::shared_ptr<MacroDimensionGroup>,
                 FunctionInvocation, GenesisSelector, SliceSelectorStep,
                 DocSelectorStep, PathMorphism, DimensionPivotStep,
                 VirtualDimensionStep, RootSelectorStep, PerspectiveStep,
                 std::shared_ptr<PathLambda>>;

struct PathStep {
  StepSelector selector;
  bool derefMaster{false}; ///< Suffixed with ">"
  std::vector<BooleanExpr> predicates;
  std::vector<ShapeLiteral> shapePredicates; ///< "[{ ... }]"
  std::optional<RangeClamp> rangeClamp{std::nullopt};
  std::optional<VersionPin> versionPin{std::nullopt};
  YieldMode yieldMode{YieldMode::Default};
};

struct MacroDimensionGroup {
  std::vector<PathStep> steps;
  Repetition repetition{Repetition::Once};
};

struct CloneOperand {
  std::shared_ptr<PathExpression> path{nullptr};
  std::optional<CreateValue> bareCreate{std::nullopt};
};

struct CloneTail {
  std::vector<CloneOperand> operands;
};

struct PathExpression {
  AnchorNode anchor{};
  /// `#[...]`: predicates on the anchor's own cells, before any step.
  std::vector<BooleanExpr> anchorPredicates;
  std::vector<PathStep> steps;
  std::optional<ShapeLiteral> shapeProjection{std::nullopt}; ///< "$path{ ... }"
  std::optional<CloneTail> cloneTail{std::nullopt};
};

// -- FLWOR & Action Clauses --------------------------------------------------

struct FieldWeave {
  std::vector<PathStep> steps;
};

struct ReturnItem {
  std::variant<PathExpression, FieldWeave> item;
};

struct ReturnClause {
  std::vector<ReturnItem> items;
};

struct DestructureField {
  bool isContext{false}; ///< "."
  std::string dimName;   ///< e.g. "d.tag"
  zigzag::DimVector direction{zigzag::DimVector::POS};
  bool isBidir{false};
  std::string varName; ///< "$t"
};

struct DestructurePattern {
  std::vector<DestructureField> fields;
};

struct ForClause {
  std::string varName;
  std::optional<DestructurePattern> destructurePattern{std::nullopt};
  PathExpression inPath{};
};

struct LetClause {
  std::string varName;
  std::variant<PathExpression, ValueExpr> target;
};

struct EffectItem {
  std::variant<LetClause, PathExpression,
               std::pair<ForClause, std::shared_ptr<EffectClause>>>
      item;
};

struct EffectClause {
  std::vector<EffectItem> items;
};

struct ConditionalClause {
  BooleanExpr condition{};
  std::shared_ptr<ActionClause> thenClause{nullptr};
  std::shared_ptr<ActionClause> elseClause{nullptr};
};

struct ActionClause {
  std::variant<ReturnClause, EffectClause, ConditionalClause> clause;
};

struct WhereClause {
  BooleanExpr condition{};
};

struct ExecutionBlock {
  std::vector<std::variant<ForClause, LetClause>> bindings;
  std::optional<WhereClause> where{std::nullopt};
  ActionClause action{};
};

struct WatchStatement {
  PathExpression targetPath;
  std::optional<std::string> asVar{std::nullopt};
  std::optional<DestructurePattern> asPattern{std::nullopt};
  std::optional<VersionExpr> sinceVersion{std::nullopt};
  std::optional<BooleanExpr> untilCondition{std::nullopt};
  std::shared_ptr<ScriptBody> body{nullptr};
};

struct PerspectiveBlock {
  std::string perspectiveName;
  std::shared_ptr<ScriptBody> body{nullptr};
};

struct MountDeclaration {
  StoreAnchor storeAnchor;
  std::string dimName;
  zigzag::DimVector direction{zigzag::DimVector::POS};
};

struct WeaveStatement {
  EffectClause effect;
};

struct Statement : std::variant<ExecutionBlock, WatchStatement, WeaveStatement,
                                PerspectiveBlock, PathExpression> {
  using variant::variant;
};

struct ScriptBody {
  std::vector<Statement> statements;
};

struct LambdaParam {
  std::variant<std::string /*$var*/, bool /*ContextDot*/, DestructurePattern>
      target;
  std::shared_ptr<ValueExpr> defaultValue{nullptr};
  bool isVariadic{false};
};

struct PathLambda {
  std::vector<LambdaParam> params;
  std::variant<std::shared_ptr<ValueExpr>, std::shared_ptr<ScriptBody>> body;
};

struct DefDeclaration {
  enum class Effect : std::uint8_t { Inferred, Pure, Weave };
  Effect effect{Effect::Inferred};
  std::string name;
  std::vector<LambdaParam> params;
  std::variant<std::shared_ptr<ValueExpr>, std::shared_ptr<ScriptBody>> body;
};

struct QueryExpression {
  std::vector<DefDeclaration> defs;
  std::vector<MountDeclaration> mounts;
  ScriptBody body;
  std::variant<ExecutionBlock, PathExpression> expr;
};

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_AST_HPP
