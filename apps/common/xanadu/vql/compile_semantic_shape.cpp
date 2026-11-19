// S1: semantic builtins project and apply_shape.
//
// apply_shape is lowered as the explicit form of the in-place shape mask: the
// assignment targets are resolved first, the mask is applied through the
// existing projection helpers, and the reciprocal links are written last, all
// inside one mutation group so a refusal leaves the subject untouched.
//
// project is lowered to one ProjectTopology op: its result is a detached copy
// of the selected topology whose cells share the source's content spans and
// keep its quote binding, which only the engine's copyTopology can mint. The
// shape's rules are folded to an edge selection here, so the op reads no AST.
#include "common/xanadu/vql/compiler.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace xanadu::vql {
using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;

namespace {

[[noreturn]] void refuse(const std::string &message) {
  throw QueryRefusal(QueryErrorCategory::Unsupported, message);
}

} // namespace

std::vector<CellRef>
VQLCompiler::compileProject(const FunctionInvocation &fn,
                            const ShapeLiteral &shape,
                            const std::optional<CellRef> context) {
  // The rule walk of the oracle, kept as a fold: a wildcard resets what came
  // before, a dimension rule adds or removes its signed edge, and `~` is both
  // signs. What is left is "everything but `remove`" or just `add`.
  bool everything = false;
  using Edge      = std::pair<std::string, DimVector>;
  std::vector<Edge> add;
  std::vector<Edge> remove;
  const auto forget = [](std::vector<Edge> &edges, const Edge &edge) {
    std::erase(edges, edge);
  };
  for (const ShapeRule &entry : shape.rules) {
    if (const auto *wildcard = std::get_if<ShapeWildcard>(&entry.rule)) {
      everything = *wildcard == ShapeWildcard::Everything;
      add.clear();
      remove.clear();
      continue;
    }
    const auto *dimension = std::get_if<DimensionShapeRule>(&entry.rule);
    if (!dimension)
      refuse("project supports only wildcard and dimension shape rules; "
             "pivot, computed-dimension, variable and nested rules are not "
             "lowered");
    if (dimension->targetValue)
      refuse("project accepts dimension masks; use apply_shape for target "
             "assignments");
    const auto fold = [&](const DimVector direction) {
      const Edge edge{dimension->dimName, direction};
      forget(add, edge);
      forget(remove, edge);
      (dimension->negated ? remove : add).push_back(edge);
    };
    fold(dimension->direction);
    if (dimension->isBidir)
      fold(dimension->direction == DimVector::POS ? DimVector::NEG
                                                  : DimVector::POS);
  }

  const auto subjects = compileArgument(fn.args[0], context);
  const CellRef input = normalizeFrameStream(subjects);
  const CellRef op = emitOp(OpcodeKind::ProjectTopology, "#PROJECT_TOPOLOGY");
  emitInput(op, input);
  emitInputConstant(op, static_cast<std::int64_t>(everything ? 1 : 0));
  emitInputConstant(op, static_cast<std::int64_t>(add.size()));
  const auto emitEdges = [&](const std::vector<Edge> &edges) {
    for (const auto &[name, direction] : edges) {
      emitInputConstant(op, static_cast<std::int64_t>(resolveDimension(name)));
      emitInputConstant(op, static_cast<std::int64_t>(toSign(direction)));
    }
  };
  emitEdges(add);
  emitEdges(remove);
  const CellRef result = core_.arena().makeCell();
  dynamicStreams_.insert(result);
  emitOutput(op, result);
  return {result};
}

std::vector<CellRef> VQLCompiler::compileSemanticShapeStep(
    const PathStep &step, const CellRef current, const bool hasContext) {
  const auto &fn = std::get<FunctionInvocation>(step.selector);
  return compileSemanticShapeCall(
      fn, hasContext ? std::optional<CellRef>{current} : std::nullopt);
}

std::vector<CellRef>
VQLCompiler::compileSemanticShapeCall(const FunctionInvocation &fn,
                                      const std::optional<CellRef> context) {
  if (fn.args.size() != 2) refuse(fn.name + " requires 2 argument(s)");
  // A literal, a shape variable or a shape-returning call, expanded while
  // compiling: the graph never carries a shape (compile_shape_value.cpp).
  const auto resolved = resolveShapeValue(fn.args[1]);
  if (!resolved)
    refuse(fn.name + " requires a shape literal, a shape variable or a call "
                     "of a function whose body is a shape literal");
  const ShapeLiteral &shape = *resolved;

  if (fn.name == "project") return compileProject(fn, shape, context);

  // Rules are walked in order exactly as the direct evaluator did: a wildcard
  // drops every pending assignment, and a later rule on the same signed edge
  // replaces an earlier one.
  using Edge = std::pair<std::string, DimVector>;
  std::vector<std::pair<Edge, const DimensionShapeRule *>> assignments;
  ShapeLiteral maskShape;
  for (const ShapeRule &entry : shape.rules) {
    if (std::holds_alternative<ShapeWildcard>(entry.rule)) {
      assignments.clear();
      maskShape.rules.push_back(entry);
      continue;
    }
    const auto *dimension = std::get_if<DimensionShapeRule>(&entry.rule);
    if (!dimension)
      refuse("apply_shape supports only wildcard and dimension shape rules; "
             "pivot, computed-dimension, variable and nested rules are not "
             "lowered");
    if (dimension->targetValue && (dimension->negated || dimension->isBidir))
      refuse("shape assignments require one signed dimension rule");
    const Edge edge{dimension->dimName, dimension->direction};
    std::erase_if(assignments,
                  [&](const auto &held) { return held.first == edge; });
    if (dimension->targetValue && !dimension->negated)
      assignments.emplace_back(edge, dimension);
    ShapeRule stripped                                      = entry;
    std::get<DimensionShapeRule>(stripped.rule).targetValue = nullptr;
    maskShape.rules.push_back(std::move(stripped));
  }

  // Nested shapes and shape-returning definitions are refused before any
  // target or mask is compiled, let alone evaluated.
  std::unordered_set<std::string> visiting;
  const auto rejectNested = [&](const auto &self, const ValueExpr &value,
                                const std::string &dimName) -> void {
    if (std::holds_alternative<std::shared_ptr<ShapeLiteral>>(value.kind))
      refuse("nested shape assignment is unsupported (target of " + dimName +
             ")");
    const auto *function =
        std::get_if<std::shared_ptr<FunctionInvocation>>(&value.kind);
    if (!function || !*function) return;
    const FunctionInvocation &call = **function;
    const auto definition          = definitions_.find(call.name);
    for (std::size_t index = 0; index < call.args.size(); ++index) {
      const bool shapeConsumer =
          index == 1 && definition == definitions_.end() &&
          (call.name == "project" || call.name == "apply_shape" ||
           call.name == "any" || call.name == "all" || call.name == "none");
      if (shapeConsumer &&
          std::holds_alternative<std::shared_ptr<ShapeLiteral>>(
              call.args[index].kind))
        continue;
      self(self, call.args[index], dimName);
    }
    if (definition != definitions_.end() && visiting.insert(call.name).second) {
      if (const auto *body =
              std::get_if<std::shared_ptr<ValueExpr>>(&definition->second.body);
          body && *body)
        self(self, **body, dimName);
    }
  };
  for (const auto &[edge, rule] : assignments)
    rejectNested(rejectNested, *rule->targetValue, rule->dimName);

  // Everything below is decided from the AST alone, so it refuses before the
  // subject or any target has emitted an op.
  for (const auto &[edge, rule] : assignments)
    if (resolveDimension(rule->dimName) == core_.dims().clone)
      refuse("shape assignment cannot assign the clone dimension");

  // The subject's cardinality decides the lowering. compileArgument only emits
  // graph ops; nothing executes when the compile throws, and this runs before
  // beginMutationGroup and before any op that writes the store or arena.
  const auto subjects = compileArgument(fn.args[0], context);
  const bool singleStaticSubject =
      subjects.size() == 1 && !dynamicStreams_.contains(subjects.front());

  // One static subject, one dimension, both directions assigned: the two
  // reciprocal writes land on the subject's own slots when exactly one target
  // is the subject itself, and the second Link would silently overwrite the
  // first. Naming the same variable on both sides keeps both targets equal,
  // which is collision-free. Several or dynamic subjects take the run-time
  // pairwise check below instead, which covers this case too, but the single
  // static form keeps its compile-time refusal so its graph stays loop-free.
  const auto variableName =
      [](const ValueExpr &value) -> std::optional<std::string_view> {
    if (const auto *name = std::get_if<std::string>(&value.kind))
      return std::string_view{*name};
    return std::nullopt;
  };
  if (singleStaticSubject)
    for (const auto &[edge, rule] : assignments)
      for (const auto &[other, otherRule] : assignments) {
        if (rule->dimName != otherRule->dimName ||
            edge.second != DimVector::POS || other.second != DimVector::NEG)
          continue;
        const auto left  = variableName(*rule->targetValue);
        const auto right = variableName(*otherRule->targetValue);
        if (!left || !right || *left != *right)
          refuse("apply_shape reciprocal collision on " + rule->dimName +
                 ": assigning both directions can overwrite the subject's own "
                 "link when one target is the subject; a single-subject form "
                 "does not emit the run-time cell-identity comparison that "
                 "tells the two apart, so name the same variable on both "
                 "sides");
      }

  // A throw from here to endMutationGroup leaves the group open, which is
  // harmless: the compiler is per-compile, so no open group reaches a later
  // compile, and neighbouring lowerings unwind the same way.
  const CellRef group = beginMutationGroup("#APPLY_SHAPE_GROUP");
  TypeExpr cellType;
  cellType.unionType.members.push_back(
      PrimaryType{.kind = BasicType{.kind = BasicType::Kind::Cell}});

  // The one cell a target expression names for one subject, as the contract
  // requires. A literal compiles to a single constant cell that every
  // evaluation of the same graph would share, so two subjects, or two turns of
  // a loop around this call, would name one partner and the second Link would
  // take the reciprocal slot from the first. Boxing it mints one cell per
  // evaluation, which is what a scalar target always meant.
  const auto resolveTarget = [&](const DimensionShapeRule &rule,
                                 const CellRef subject) {
    const auto arguments = compileArgument(*rule.targetValue, subject);
    if (std::holds_alternative<ScalarLiteral>(rule.targetValue->kind) &&
        arguments.size() == 1) {
      const auto box = emitOp(OpcodeKind::VqlBoxScalar, "#SHAPE_TARGET_BOX");
      emitInput(box, arguments.front());
      const auto boxed = core_.arena().makeCell();
      emitOutput(box, boxed);
      return boxed;
    }
    const CellRef targetStream = normalizeFrameStream(arguments);
    const std::array<CellRef, 1> held{targetStream};
    emitRequiredContractMatch(cellType, held,
                              "shape assignment target " + rule.dimName);
    return emitFirstCell(targetStream);
  };

  if (singleStaticSubject) {
    std::vector<CellRef> targets;
    for (const auto &[edge, rule] : assignments)
      targets.push_back(resolveTarget(*rule, subjects.front()));
    const auto masked = applyShapeProjection(subjects, maskShape);
    for (std::size_t index = 0; index < assignments.size(); ++index) {
      const auto &[edge, rule] = assignments[index];
      const CellRef link = emitOp(OpcodeKind::Link, "#APPLY_SHAPE_ASSIGN");
      emitInput(link, subjects.front());
      emitInputConstant(
          link, static_cast<std::int64_t>(resolveDimension(rule->dimName)));
      emitInputConstant(link, static_cast<std::int64_t>(toSign(edge.second)));
      emitInput(link, targets[index]);
    }
    endMutationGroup(group, "#APPLY_SHAPE_GROUP_END");
    return masked;
  }

  if (assignments.empty()) {
    const auto masked = applyShapeProjection(subjects, maskShape);
    endMutationGroup(group, "#APPLY_SHAPE_GROUP_END");
    return masked;
  }

  // Several or run-time subjects. Everything is resolved and checked before the
  // first mask or write, so a refusal leaves no partial effect. Subjects are
  // gathered into one stream so every later loop walks the same order.
  const CellRef subjectStream = emitStreamClear();
  emitAppendStream(subjectStream, subjects);
  const std::array<CellRef, 1> subjectInput{subjectStream};

  // Each target is resolved with its own subject as context and must be
  // exactly one cell; the stream stays parallel to the subject stream.
  std::vector<CellRef> targetStreams;
  for (const auto &[edge, rule] : assignments) {
    const auto resolved = emitMap(subjectInput, [&](const CellRef subject) {
      return std::vector<CellRef>{resolveTarget(*rule, subject)};
    });
    targetStreams.push_back(resolved.front());
  }

  // Walks the subject stream and every target stream in step; the streams have
  // one entry per subject, so none ends before the lead.
  std::vector<CellRef> lockstepStreams{subjectStream};
  lockstepStreams.insert(lockstepStreams.end(), targetStreams.begin(),
                         targetStreams.end());
  const auto lockstep = [&](const auto &body) {
    std::vector<CellRef> iterators;
    for (const CellRef stream : lockstepStreams) {
      const auto begin = emitOp(OpcodeKind::StreamBegin, "#SHAPE_ZIP_BEGIN");
      emitInput(begin, stream);
      const auto iterator = core_.arena().makeCell();
      emitOutput(begin, iterator);
      iterators.push_back(iterator);
    }
    emitIteratorLoop(iterators.front(), [&](const CellRef lead) {
      std::vector<CellRef> items{lead};
      for (std::size_t index = 1; index < iterators.size(); ++index) {
        const auto next = emitOp(OpcodeKind::IteratorNext, "#SHAPE_ZIP_NEXT");
        emitInput(next, iterators[index]);
        const auto item    = core_.arena().makeCell();
        const auto present = core_.arena().makeCell();
        emitOutput(next, item);
        emitOutput(next, present);
        items.push_back(item);
      }
      body(std::span<const CellRef>{items});
    });
  };
  const auto sameCell = [&](const CellRef left, const CellRef right) {
    const auto op = emitOp(OpcodeKind::SameCell, "#SHAPE_SAME_CELL");
    emitInput(op, left);
    emitInput(op, right);
    const auto out = core_.arena().makeCell();
    emitOutput(op, out);
    return out;
  };
  const auto logical = [&](const OpcodeKind kind, const CellRef left,
                           const CellRef right) {
    const auto op = emitOp(kind, "#SHAPE_LOGIC");
    emitInput(op, left);
    emitInput(op, right);
    const auto out = core_.arena().makeCell();
    emitOutput(op, out);
    return out;
  };
  const auto negate = [&](const CellRef value) {
    const auto op = emitOp(OpcodeKind::Not, "#SHAPE_LOGIC_NOT");
    emitInput(op, value);
    const auto out = core_.arena().makeCell();
    emitOutput(op, out);
    return out;
  };
  // Two claims collide exactly when one slot match holds without the other:
  // `left` says the claims name the same slot, `right` that they name the
  // same partner. Asserting both implications is asserting the equivalence.
  const auto requireEquivalent = [&](const CellRef left, const CellRef right) {
    for (const auto &[premise, conclusion] :
         {std::pair{left, right}, std::pair{right, left}}) {
      const auto holds   = logical(OpcodeKind::Or, negate(premise), conclusion);
      const auto require = emitOp(OpcodeKind::Assert, "#SHAPE_COLLISION");
      emitInput(require, holds);
      emitInputConstant(require,
                        std::string{"shape assignment collision: incompatible "
                                    "reciprocal targets"});
    }
  };

  // Which pairs of claims can share a slot is fixed by the AST: a rule against
  // itself (forward-forward and reverse-reverse together) and a forward rule
  // against a reverse rule of the same dimension (forward-reverse, both ways).
  // Subject pairs are only known at run time, so every pair is tested.
  lockstep([&](std::span<const CellRef> outer) {
    lockstep([&](std::span<const CellRef> inner) {
      for (std::size_t i = 0; i < assignments.size(); ++i) {
        const auto &[edgeI, ruleI] = assignments[i];
        const CellRef subjectA = outer[0], targetA = outer[1 + i];
        for (std::size_t j = 0; j < assignments.size(); ++j) {
          const auto &[edgeJ, ruleJ] = assignments[j];
          if (resolveDimension(ruleI->dimName) !=
              resolveDimension(ruleJ->dimName))
            continue;
          const CellRef subjectB = inner[0], targetB = inner[1 + j];
          if (i == j)
            requireEquivalent(sameCell(subjectA, subjectB),
                              sameCell(targetA, targetB));
          else if (edgeI.second == DimVector::POS &&
                   edgeJ.second == DimVector::NEG)
            requireEquivalent(sameCell(subjectA, targetB),
                              sameCell(targetA, subjectB));
        }
      }
    });
  });

  const auto masked = applyShapeProjection(subjects, maskShape);

  lockstep([&](std::span<const CellRef> row) {
    for (std::size_t index = 0; index < assignments.size(); ++index) {
      const auto &[edge, rule] = assignments[index];
      const CellRef link = emitOp(OpcodeKind::Link, "#APPLY_SHAPE_ASSIGN");
      emitInput(link, row[0]);
      emitInputConstant(
          link, static_cast<std::int64_t>(resolveDimension(rule->dimName)));
      emitInputConstant(link, static_cast<std::int64_t>(toSign(edge.second)));
      emitInput(link, row[1 + index]);
    }
  });
  endMutationGroup(group, "#APPLY_SHAPE_GROUP_END");
  return masked;
}

std::optional<CellRef>
VQLCompiler::compileSemanticShapeValue(const FunctionInvocation &fn,
                                       const CellRef ctxCell) {
  const auto results = compileSemanticShapeCall(fn, ctxCell);
  if (results.empty()) return std::nullopt;
  return emitFirstCell(results.front());
}

} // namespace xanadu::vql
