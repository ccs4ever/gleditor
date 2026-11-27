/**
 * @file ast.hpp
 * @brief Abstract Syntax Tree (AST) node representations for VQL v16.0.
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
struct RuntimeClosureInvocation;
struct BinaryValueExpr;
struct Statement;
struct ScriptBody;
struct TypeExpr;

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
               std::shared_ptr<RuntimeClosureInvocation>,
               std::shared_ptr<ShapeLiteral>, std::shared_ptr<VersionExpr>,
               std::shared_ptr<PathLambda>, std::shared_ptr<BooleanExpr>,
               std::shared_ptr<BinaryValueExpr>>
      kind;
};

struct BinaryValueExpr {
  std::shared_ptr<ValueExpr> left;
  std::shared_ptr<ValueExpr> right;
};

struct FunctionInvocation {
  std::string name;
  std::vector<ValueExpr> args;
  std::vector<std::string> argNames;
  bool isSliceRead{false};
};

/// A call through a first-class runtime closure value. The callee is an
/// ordinary expression so bounded walkers can validate it before lowering.
struct RuntimeClosureInvocation {
  ValueExpr callee;
  std::vector<ValueExpr> args;
  std::vector<std::string> argNames;
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
                 std::shared_ptr<PathLambda>,
                 std::shared_ptr<RuntimeClosureInvocation>>;

struct PathStep {
  StepSelector selector;
  bool derefMaster{false}; ///< Suffixed with ">"
  std::vector<BooleanExpr> predicates;
  std::vector<ShapeLiteral> shapePredicates; ///< "[{ ... }]"
  std::optional<ShapeLiteral>
      projectionAfter; ///< Applied before the next step.
  std::optional<RangeClamp> rangeClamp{std::nullopt};
  std::optional<VersionPin> versionPin{std::nullopt};
  YieldMode yieldMode{YieldMode::Default};
  // Internal lexical capture; nested AST nodes are read-only after parsing.
  // Executable export lowers this body to instructions, never serializes it.
  std::shared_ptr<const std::vector<PathStep>> capturedFunctor;
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
  /// `/+d.parent` with no anchor: a PartialPathFunctor (§4.12.3). Evaluated
  /// in place it reads from the context, as it always did; bound by `let` or
  /// passed as an argument it is kept as steps and applied later.
  bool isFunctor{false};
  /// `#[...]`: predicates on the anchor's own cells, before any step.
  std::vector<BooleanExpr> anchorPredicates;
  std::optional<ShapeLiteral>
      anchorProjection; ///< Applied before the first step.
  std::vector<PathStep> steps;
  std::optional<ShapeLiteral> shapeProjection{std::nullopt}; ///< "$path{ ... }"
  std::optional<CloneTail> cloneTail{std::nullopt};
};

// -- FLWOR & Action Clauses --------------------------------------------------

struct FieldWeave {
  std::vector<PathStep> steps;
};

struct ReturnValue {
  ValueExpr expression;
};

struct ReturnItem {
  std::variant<PathExpression, FieldWeave, ReturnValue> item;
};

struct ReturnClause {
  std::vector<ReturnItem> items;
};

/// One field of a shape contract: `+d.fps: $fps: Int := 60`. Every part
/// after the dimension is optional, so the same field serves a pure contract
/// (`+d.fps: Int`), a pure binding (`+d.fps: $fps`) and both (§4.12.2).
struct DestructureField {
  bool isContext{false}; ///< "."
  std::string dimName;   ///< e.g. "d.tag"
  zigzag::DimVector direction{zigzag::DimVector::POS};
  bool isBidir{false};
  std::string varName;                     ///< "$t"; empty binds nothing
  std::shared_ptr<TypeExpr> type{nullptr}; ///< ": String"
  std::shared_ptr<ValueExpr> defaultValue{nullptr}; ///< `:= "note"`
};

struct DestructurePattern {
  std::vector<DestructureField> fields;
};

// -- Gradual Structural Types (§4.12.1) --------------------------------------

struct BasicType {
  enum class Kind : std::uint8_t {
    Cell,
    Dim,
    Store,
    Slice,
    Version,
    Int,
    Float,
    String,
    Bool,
    Any,
    Contextual, ///< a ContextualTypeIdent; `name` holds it
  };
  Kind kind{Kind::Any};
  std::string name;
};

/// `Stream<T>`, or a contextual identifier applied to arguments.
struct GenericType {
  std::string name;
  std::vector<std::shared_ptr<TypeExpr>> args;
};

/// `{ +d.title: String, ... }`: a cell carrying these dimensions. Its fields
/// are destructure fields, which is what lets a contract bind (Mode 4).
struct ShapeType {
  DestructurePattern pattern;
};

struct PrimaryType {
  std::variant<BasicType, GenericType, ShapeType,
               std::shared_ptr<TypeExpr> /*ParenType*/>
      kind;
  /// `Cell { ... }`: a basic or generic type refined by a shape.
  std::optional<ShapeType> refinement{std::nullopt};
  bool isOptional{false}; ///< "?": absence is also accepted
};

struct UnionType {
  std::vector<PrimaryType> members; ///< separated by "|"
};

struct TypeExpr {
  UnionType unionType;
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
  /// The name was written as `$variable`, so perspectiveName is the
  /// variable's name and not a perspective's.
  bool perspectiveIsVariable{false};
  std::shared_ptr<ScriptBody> body{nullptr};
};

struct MountDeclaration {
  StoreAnchor storeAnchor;
  std::string dimName;
  zigzag::DimVector direction{zigzag::DimVector::POS};
};

struct UnmountDeclaration {
  std::string dimName;
  zigzag::DimVector direction{zigzag::DimVector::POS};
};

using NamespaceDeclaration = std::variant<MountDeclaration, UnmountDeclaration>;

struct WeaveStatement {
  EffectClause effect;
};

/// A byte count or offset of a script I/O action: an integer literal, or the
/// name of a session variable bound to a numeric cell.
using IoOperand = std::variant<std::int64_t, std::string>;

/// A script action's time bound: an integer literal or a `$variable` bound to
/// a numeric cell, in @p unitMillis milliseconds each (1 for `ms`, 1000 for
/// `s`).
struct IoDuration {
  IoOperand amount{std::int64_t{0}};
  std::int64_t unitMillis{1};
};

/// The path an `open` action names: a string literal or a `$variable` bound
/// to a text cell.
struct IoOpenPath {
  std::string text;
  bool isVariable{false};
};

/// What an `open` action asks the host for. The host's write grant, never the
/// script, decides whether a mutating mode is allowed; the numbering is the
/// value the compiled `io_open` call carries, so it is part of the contract
/// between the compiler and the runner.
enum class IoOpenMode : std::int64_t {
  Read,
  Write,
  WriteCreate,
  Append,
  AppendCreate,
};

/// The request a `submit` action describes.
enum class IoRequestKind : std::uint8_t {
  ReadBytes,
  WriteBytes,
  AppendBytes,
  Sync,
};

/// What a write or append sends: a string literal, or the name of a session
/// or earlier-action variable bound to a text cell or an outcome (whose
/// payload is copied byte for byte).
struct IoByteSource {
  std::string text;
  bool isVariable{false};
};

/// A script action (plan §10, M7-VQL): it exists only as a top-level script
/// statement, so it is reached once per script run, never per row. Four
/// shapes, all written `effect { ... } [as $name]`:
///
///   Read:   `let $s := submit($cap, read_bytes(off, max)) [within N ms|s];
///            match $s { admitted($q) => await($q) | $q, refused($e) => $e }`
///           The match is fixed in shape -- both arms -- because a refusal is
///           a value the script keeps, not a failure. `=> $q` starts the read
///           and yields the request handle without waiting (a detached read).
///   Await:  `await($h)` waits for a handle's outcome.
///   Cancel: `cancel($h)` asks the backend to stop the handle's request and
///           yields its outcome (Cancelled, or the terminal it had reached).
///   Open:   `open("path")` or `open($path)` opens a file read-only and yields
///           the open outcome; `as $name` binds the capability it minted.
///           `for write|append [create]` asks for a mutating open, which the
///           host's write grant may refuse (a Rejected outcome).
///
/// A submit may also describe `write_bytes(off, src)`, `append_bytes(src)` or
/// `sync()`; all take the same `within`, match arms and `as`.
///
///   Capture: `capture($r)` or `capture($r, off, len)` makes the payload an
///           outcome retained durable in the author's permascroll and yields
///           the capture's receipt, or the outcome that refused or ended it.
///           It is not one request (an append, then durable steps), so it has
///           no submit form: it is its own statement.
struct IoEffectStatement {
  /// `Read` is the submit shape; which request it submits is `request`.
  enum class Kind : std::uint8_t { Read, Await, Cancel, Open, Capture };
  Kind kind{Kind::Read};
  IoRequestKind request{IoRequestKind::ReadBytes};
  /// WriteBytes and AppendBytes: the bytes to send.
  IoByteSource source;
  /// Open: the mode (`for write [create]`, `for append [create]`).
  IoOpenMode openMode{IoOpenMode::Read};

  std::string submissionVar;
  std::string capabilityVar;
  IoOperand offset{std::int64_t{0}};
  IoOperand maxBytes{std::int64_t{0}};
  std::string requestVar;
  std::string refusalVar;
  /// `within N ms|s` on a Read: the request ends as TimedOut after that long.
  std::optional<IoDuration> within;
  /// A Read whose admitted arm is `=> $q`: yields the handle, does not wait.
  bool detached{false};
  /// Await and Cancel: the handle variable (a request, or a refusal outcome).
  std::string handleVar;
  /// Capture: the outcome variable, and the selected sub-range of its payload
  /// (`offset`, `maxBytes` read as offset and length) when @ref captureRanged.
  std::string captureVar;
  bool captureRanged{false};
  /// Open: the path.
  IoOpenPath openPath;
  /// `effect { ... } as $r`: the outcome cell (for Open, the capability the
  /// open minted, or the outcome itself when it minted none), named for the
  /// statements that follow so they can read it with io_tag($r) and friends.
  std::optional<std::string> resultVar;
};

/// The terminal outcome tags a `match` arm can name. The numbering is the
/// value the compiled `io_match_tag` relation answers, so it is part of the
/// contract between the compiler and the runner (as `IoOpenMode` is); it is
/// the order of the shared outcome tags, and the runner maps by name, not by
/// trusting the two enums to agree.
enum class IoMatchTag : std::int64_t {
  Rejected,
  Complete,
  EndOfStream,
  Partial,
  Failed,
  Cancelled,
  TimedOut,
  Overflow,
  MessageTooLarge,
  Stale,
  Conflict,
  Unknown,
};

/// One arm of a script `match`: `complete($p) => $p`. No tag is the `_` arm.
/// A binder that is absent (`_`) is not bound; one that is present is bound
/// to the tag's field in order (see `io_match.hpp`), as a stream that is empty
/// when the outcome carries no such field, as the path functions answer.
struct IoMatchArm {
  std::optional<IoMatchTag> tag;
  std::vector<std::optional<std::string>> binders;
  ReturnItem body;
};

/// `match $r { complete($p) => ..., end_of_stream => ..., _ => ... }` over an
/// outcome a script holds. A pure top-level statement: its arms are row
/// expressions, it submits nothing and cannot park, and it is reached once
/// per script run like an action.
struct IoMatchStatement {
  /// The variable that names the outcome.
  std::string subject;
  std::vector<IoMatchArm> arms;
};

struct Statement : std::variant<ExecutionBlock, WatchStatement, WeaveStatement,
                                PerspectiveBlock, PathExpression,
                                IoEffectStatement, IoMatchStatement> {
  using variant::variant;
};

struct ScriptBody {
  std::vector<Statement> statements;
};

struct LambdaParam {
  std::variant<std::string /*$var*/, bool /*ContextDot*/, DestructurePattern>
      target;
  std::shared_ptr<TypeExpr> type{nullptr}; ///< "$p: Cell { ... }"
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
  std::shared_ptr<TypeExpr> returnType{nullptr}; ///< "): Int :="
  std::variant<std::shared_ptr<ValueExpr>, std::shared_ptr<ScriptBody>> body;
};

struct QueryExpression {
  std::vector<DefDeclaration> defs;
  std::vector<MountDeclaration> mounts;
  std::vector<NamespaceDeclaration> namespaceDeclarations;
  ScriptBody body;
  std::variant<ExecutionBlock, PathExpression> expr;
};

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_AST_HPP
