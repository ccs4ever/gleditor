// V5, the data half: a shape value's rules as cells (shape_cells.hpp).
//
// The compile-time table still holds the expanded ShapeLiteral and still feeds
// the static lowering (compile_shape_predicate.cpp, compile_semantic_shape.cpp
// and compile_projection.cpp), so a program built from a known shape is the
// one it always was. The cells are written beside it, for a reader that has
// only the handle: a stored shape, a result, a value that crossed a query.
#include "common/xanadu/vql/compiler.hpp"

#include <format>
#include <stdexcept>
#include <variant>

namespace xanadu::vql {
using zigzag::CellRef;
using zigzag::vortex::OpcodeKind;

namespace {

[[noreturn]] void refuseTarget(const std::string &message) {
  throw QueryRefusal(QueryErrorCategory::Unsupported, message);
}

std::string bareName(const std::string &spelled) {
  return spelled.starts_with('$') ? spelled.substr(1) : spelled;
}

bool relativeToSubject(const ValueExpr &value);

bool relativeToSubject(const PathExpression &path) {
  // A partial path is applied to whatever it is given, so it is the subject's
  // by construction.
  if (path.isFunctor || path.anchor.kind == AnchorKind::Context) return true;
  for (const PathStep &step : path.steps)
    if (const auto *call = std::get_if<FunctionInvocation>(&step.selector))
      for (const ValueExpr &argument : call->args)
        if (relativeToSubject(argument)) return true;
  return false;
}

bool relativeToSubject(const FunctionInvocation &call) {
  for (const ValueExpr &argument : call.args)
    if (relativeToSubject(argument)) return true;
  return false;
}

bool relativeToSubject(const BooleanExpr &condition) {
  for (const BooleanTerm &term : condition.terms)
    for (const BooleanFactor &factor : term.factors) {
      if (const auto *comparison = std::get_if<ComparisonExpr>(&factor.test)) {
        if ((comparison->left && relativeToSubject(*comparison->left)) ||
            (comparison->right && relativeToSubject(*comparison->right)))
          return true;
        continue;
      }
      const auto &test = std::get<PredicateTest>(factor.test);
      if (const auto *dot = std::get_if<bool>(&test.kind)) {
        if (*dot) return true;
      } else if (const auto *path =
                     std::get_if<std::shared_ptr<PathExpression>>(&test.kind)) {
        if (*path && relativeToSubject(**path)) return true;
      } else if (const auto *value =
                     std::get_if<std::shared_ptr<ValueExpr>>(&test.kind)) {
        if (*value && relativeToSubject(**value)) return true;
      } else if (const auto *call =
                     std::get_if<std::shared_ptr<FunctionInvocation>>(
                         &test.kind)) {
        if (*call && relativeToSubject(**call)) return true;
      }
    }
  return false;
}

bool relativeToSubject(const ValueExpr &value) {
  if (const auto *path =
          std::get_if<std::shared_ptr<PathExpression>>(&value.kind))
    return *path && relativeToSubject(**path);
  if (const auto *call =
          std::get_if<std::shared_ptr<FunctionInvocation>>(&value.kind))
    return *call && relativeToSubject(**call);
  if (const auto *binary =
          std::get_if<std::shared_ptr<BinaryValueExpr>>(&value.kind))
    return *binary && (*binary)->left && (*binary)->right &&
           (relativeToSubject(*(*binary)->left) ||
            relativeToSubject(*(*binary)->right));
  if (const auto *boolean =
          std::get_if<std::shared_ptr<BooleanExpr>>(&value.kind))
    return *boolean && relativeToSubject(**boolean);
  return false;
}

/// One rule as cells. The rule cell's text is never empty because DimByName
/// refuses an empty name.
CellRef mintRule(zigzag::ArenaManifold &arena, const ShapeDimensions &dims,
                 const std::string &dimension, const bool negated,
                 const bool bidirectional, const zigzag::DimVector direction) {
  const CellRef rule = arena.makeCell(dimension);
  zigzag::expectWritten(
      arena.link(rule, dims.neg, false, arena.makeScalarCell(negated)));
  zigzag::expectWritten(
      arena.link(rule, dims.bidir, false, arena.makeScalarCell(bidirectional)));
  zigzag::expectWritten(
      arena.link(rule, dims.dir, false,
                 arena.makeScalarCell(
                     static_cast<std::int64_t>(zigzag::toSign(direction)))));
  return rule;
}

/// The kind goes on last: whether a dimension rule is a nested one is known
/// only once its target has been looked at.
void linkKind(zigzag::ArenaManifold &arena, const ShapeDimensions &dims,
              const CellRef rule, const std::string_view kind) {
  zigzag::expectWritten(
      arena.link(rule, dims.kind, false, arena.makeCell(std::string{kind})));
}

} // namespace

CellRef VQLCompiler::registerShape(ShapeLiteral expanded,
                                   const ShapeMint mint) {
  auto &arena          = core_.arena();
  const CellRef handle = arena.makeCell();
  // A one-cell closed rank on the marker dimension: a handle is told from a
  // plain cell by the cell itself, at no cost in cells (a rank holds one
  // neighbour a side, so handles could not share an anchor). The dimension is
  // named when the first shape is made.
  const ShapeDimensions dims = ensureShapeDimensions(core_);
  zigzag::expectWritten(
      arena.link(handle, dims.marker, zigzag::DimVector::POS, handle));
  std::size_t rulesLeft = shapeLimits_.maxRules;
  lateTargetMade_       = false;
  fillShapeCells(handle, expanded, dims, mint, 0, rulesLeft);
  if (lateTargetMade_) lateHandles_.insert(handle);
  shapeHandles_.emplace(handle, std::move(expanded));
  return handle;
}

void VQLCompiler::fillShapeCells(const CellRef shape,
                                 const ShapeLiteral &expanded,
                                 const ShapeDimensions &dims,
                                 const ShapeMint mint, const std::size_t depth,
                                 std::size_t &rulesLeft) {
  if (depth > shapeLimits_.maxDepth)
    refuseTarget(std::format("VQL shape value exceeds maximum nesting depth {}",
                             shapeLimits_.maxDepth));
  auto &arena = core_.arena();
  const std::string none{viewNoDimensionName};
  CellRef previous = shape;
  for (const ShapeRule &entry : expanded.rules) {
    if (rulesLeft == 0)
      refuseTarget(std::format("VQL shape value exceeds maximum size {} rules",
                               shapeLimits_.maxRules));
    --rulesLeft;
    CellRef rule{};
    if (const auto *wildcard = std::get_if<ShapeWildcard>(&entry.rule)) {
      rule = mintRule(arena, dims, none, *wildcard == ShapeWildcard::Nothing,
                      false, zigzag::DimVector::POS);
      linkKind(arena, dims, rule, shapeKindWildcard);
    } else if (const auto *spec =
                   std::get_if<DimensionShapeRule>(&entry.rule)) {
      rule = mintRule(arena, dims, spec->dimName, spec->negated, spec->isBidir,
                      spec->direction);
      const bool nested =
          captureShapeTarget(rule, *spec, dims, mint, depth, rulesLeft);
      linkKind(arena, dims, rule, nested ? shapeKindNested : shapeKindDim);
    } else if (const auto *pivot =
                   std::get_if<DimensionPivotRule>(&entry.rule)) {
      rule = mintRule(arena, dims, pivot->fromDim, false, false,
                      zigzag::DimVector::POS);
      linkKind(arena, dims, rule, viewKindPivot);
      zigzag::expectWritten(
          arena.link(rule, dims.to, false, arena.makeCell(pivot->toDim)));
    } else if (const auto *computed =
                   std::get_if<VirtualDimensionDefRule>(&entry.rule)) {
      // The expression is syntax the compiler lowers where the view is
      // stepped; only the fact that the rule is computed is data.
      rule = mintRule(arena, dims, computed->dimName, false, false,
                      computed->direction);
      linkKind(arena, dims, rule, viewKindComputed);
    } else {
      throw std::invalid_argument(
          "a shape's rules are expanded before they are written as cells");
    }
    zigzag::expectWritten(arena.link(previous, dims.rule, false, rule));
    previous = rule;
  }
  recordShapeWitness(arena, dims, shape);
}

bool VQLCompiler::captureShapeTarget(const CellRef rule,
                                     const DimensionShapeRule &spec,
                                     const ShapeDimensions &dims,
                                     const ShapeMint mint,
                                     const std::size_t depth,
                                     std::size_t &rulesLeft) {
  if (!spec.targetValue) return false;
  auto &arena             = core_.arena();
  const ValueExpr &target = *spec.targetValue;
  const auto nest         = [&](const ShapeLiteral &inner) {
    const CellRef nested = arena.makeCell();
    fillShapeCells(nested, inner, dims, mint, depth + 1, rulesLeft);
    zigzag::expectWritten(arena.link(rule, dims.nested, false, nested));
  };

  if (const auto *literal = std::get_if<ScalarLiteral>(&target.kind)) {
    // A fresh scalar per shape, so a consumer that links a copy to each
    // subject never takes a cell some other shape holds.
    const CellRef scalar = std::visit(
        [&](const auto &value) -> CellRef {
          if constexpr (std::same_as<std::remove_cvref_t<decltype(value)>,
                                     std::string>)
            return arena.makeCell(value);
          else
            return arena.makeScalarCell(value);
        },
        literal->value);
    zigzag::expectWritten(arena.link(rule, dims.target, false, scalar));
    return false;
  }
  if (const auto *inner =
          std::get_if<std::shared_ptr<ShapeLiteral>>(&target.kind)) {
    if (*inner) nest(expandShape(**inner));
    return true;
  }
  if (std::holds_alternative<std::shared_ptr<PathLambda>>(target.kind) ||
      std::holds_alternative<std::shared_ptr<RuntimeClosureInvocation>>(
          target.kind))
    refuseTarget("VQL shape rule '" + spec.dimName +
                 "' has a closure as its target: a shape value holds data, and "
                 "a closure is code (a path or functor target, d.vql.via, is "
                 "not built)");
  if (const auto *path =
          std::get_if<std::shared_ptr<PathExpression>>(&target.kind);
      path && *path && (*path)->isFunctor)
    refuseTarget("VQL shape rule '" + spec.dimName +
                 "' has a rootless partial path as its target: a path or "
                 "functor target (d.vql.via) is not built");
  if (const auto *spelled = std::get_if<std::string>(&target.kind)) {
    const std::string name = bareName(*spelled);
    if (functorBindings_.contains(name))
      refuseTarget("VQL shape rule '" + spec.dimName + "' names $" + name +
                   ", a path functor: a path or functor target (d.vql.via) is "
                   "not built");
    const bool bound =
        varBindings_.contains(name) || streamBindings_.contains(name);
    if (!bound) {
      // Not a read: the rule binds what it matches to this name.
      // The name is kept as `$name` text on d.vql.to, which a pivot rule uses
      // for its dimension; the kind tells them apart. That is enough for a
      // plain binder and deliberately no more: a binder that needs a pattern,
      // a type or a scope would want cells of its own, so this is the place a
      // richer representation would grow from.
      zigzag::expectWritten(
          arena.link(rule, dims.to, false, arena.makeCell("$" + name)));
      return false;
    }
    if (variableHoldsHandle(name)) {
      nest(shapeOfVariable(name));
      return true;
    }
  }
  if (relativeToSubject(target))
    refuseTarget(
        "VQL shape rule '" + spec.dimName +
        "' has a target relative to the subject: a shape value has no subject "
        "to read it from when it is made. Write the shape where it is "
        "applied, or use a rootless path (d.vql.via, not built)");
  if (mint == ShapeMint::Binding) return false;
  emitShapeTargetCapture(rule, spec, dims);
  return false;
}

void VQLCompiler::emitShapeTargetCapture(const CellRef rule,
                                         const DimensionShapeRule &spec,
                                         const ShapeDimensions &dims) {
  // The handle is one cell for every call of a body or turn of a loop that
  // makes it, so a target written to it at run time would be overwritten by
  // the next one and the earlier shape would silently change. Nothing is
  // written there: the consumers in this query (apply_shape, a predicate,
  // project, a returned handle) read the expression from the compile-time
  // table and evaluate it where they are, as they did before shapes were
  // cells, and the rule says so (d.vql.late) so a reader of the cells alone
  // refuses it by name instead of taking it for a presence-only rule.
  if (compilingFrameDefinition_ || !activeCalls_.empty() || loopDepth_ > 0) {
    zigzag::expectWritten(core_.arena().link(rule, dims.late, false, rule));
    lateTargetMade_ = true;
    return;
  }
  auto &arena       = core_.arena();
  const auto values = compileArgument(*spec.targetValue, std::nullopt);

  const auto begin = emitOp(OpcodeKind::StreamBegin, "#SHAPE_TARGET_BEGIN");
  for (const CellRef cell : values) emitInput(begin, cell);
  const CellRef iterator = arena.makeCell();
  emitOutput(begin, iterator);
  const auto next = emitOp(OpcodeKind::IteratorNext, "#SHAPE_TARGET_FIRST");
  emitInput(next, iterator);
  const CellRef item    = arena.makeCell();
  const CellRef present = arena.makeCell();
  emitOutput(next, item);
  emitOutput(next, present);
  const auto extra = emitOp(OpcodeKind::IteratorNext, "#SHAPE_TARGET_EXTRA");
  emitInput(extra, iterator);
  const CellRef extraItem    = arena.makeCell();
  const CellRef extraPresent = arena.makeCell();
  emitOutput(extra, extraItem);
  emitOutput(extra, extraPresent);
  const auto single = emitOp(OpcodeKind::Not, "#SHAPE_TARGET_SINGLE");
  emitInput(single, extraPresent);
  const CellRef exactlyOne = arena.makeCell();
  emitOutput(single, exactlyOne);
  const auto assertion = emitOp(OpcodeKind::Assert, "#SHAPE_TARGET_ASSERT");
  emitInput(assertion, exactlyOne);
  emitInputConstant(assertion,
                    "one cardinality violation: the target of shape rule '" +
                        spec.dimName + "' yields several values; expected one");

  const CellRef empty = mintOpcode(OpcodeKind::Nop, "#SHAPE_TARGET_EMPTY");
  const CellRef done  = mintOpcode(OpcodeKind::Nop, "#SHAPE_TARGET_DONE");
  const auto skip = emitOp(OpcodeKind::BranchFalseCell, "#SHAPE_TARGET_SKIP");
  emitInput(skip, present);
  emitInput(skip, empty);

  // A box takes a raw scalar slot, not the reference the iterator hands over.
  const CellRef raw = emitRawCopy(item);
  const auto box    = emitOp(OpcodeKind::VqlBoxScalar, "#SHAPE_TARGET_COPY");
  emitInput(box, raw);
  const CellRef copy = arena.makeCell();
  emitOutput(box, copy);
  const auto write = emitOp(OpcodeKind::Link, "#SHAPE_TARGET_WRITE");
  emitInput(write, rule);
  emitInputConstant(write, static_cast<std::int64_t>(dims.target));
  emitInputConstant(write, static_cast<std::int64_t>(1));
  emitInput(write, copy);
  const auto jump = emitOp(OpcodeKind::BranchFalseCell, "#SHAPE_TARGET_JUMP");
  emitInputConstant(jump, false);
  emitInput(jump, done);

  appendOpcode(empty);
  // The rule is its own marker, as a handle is its own shape marker.
  const auto mark = emitOp(OpcodeKind::Link, "#SHAPE_TARGET_VOID");
  emitInput(mark, rule);
  emitInputConstant(mark, static_cast<std::int64_t>(dims.voided));
  emitInputConstant(mark, static_cast<std::int64_t>(1));
  emitInput(mark, rule);
  appendOpcode(done);
}

} // namespace xanadu::vql
