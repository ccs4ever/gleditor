// S1: semantic builtins project and apply_shape.
//
// apply_shape is lowered as the explicit form of the in-place shape mask: the
// assignment targets are resolved first, the mask is applied through the
// existing projection helpers, and the reciprocal links are written last, all
// inside one mutation group so a refusal leaves the subject untouched.
//
// project is refused by name. Its result is a detached copy of the selected
// topology whose cells share the source's content spans and keep its quote
// binding, and no Vortex opcode mints a cell carrying another cell's spans.
#include "common/xanadu/multi_store.hpp"
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
  const auto *shapeArgument =
      std::get_if<std::shared_ptr<ShapeLiteral>>(&fn.args[1].kind);
  if (!shapeArgument || !*shapeArgument)
    refuse(fn.name + " requires a literal shape; a shape held in a variable "
                     "or returned by a function is not lowered");
  const ShapeLiteral &shape = **shapeArgument;

  if (fn.name == "project") {
    for (const ShapeRule &entry : shape.rules) {
      if (!std::holds_alternative<ShapeWildcard>(entry.rule) &&
          !std::holds_alternative<DimensionShapeRule>(entry.rule)) {
        refuse("project supports only wildcard and dimension shape rules");
      }
      if (const auto *dim = std::get_if<DimensionShapeRule>(&entry.rule)) {
        if (dim->targetValue) {
          refuse("project accepts dimension masks; use apply_shape for target "
                 "assignments");
        }
      }
    }

    using Edge = std::pair<zigzag::DimRef, DimVector>;
    std::vector<Edge> selected;
    auto &arena     = core_.arena();
    auto dimensions = arena.dimensions();
    for (const auto &cell : arena.cells()) {
      arena.consumeWork();
      for (const auto &edge : arena.dimensionsOf(cell.birthOp)) {
        if (std::ranges::find(dimensions, edge.dim) == dimensions.end()) {
          dimensions.push_back(edge.dim);
        }
      }
    }
    for (const auto &rule : shape.rules) {
      if (const auto *wildcard = std::get_if<ShapeWildcard>(&rule.rule)) {
        selected.clear();
        if (*wildcard == ShapeWildcard::Everything) {
          for (const auto dim : dimensions) {
            selected.emplace_back(dim, DimVector::POS);
            selected.emplace_back(dim, DimVector::NEG);
          }
        }
      }
      if (const auto *dim = std::get_if<DimensionShapeRule>(&rule.rule)) {
        const auto add = [&](const DimVector direction) {
          const Edge edge{resolveDimension(dim->dimName), direction};
          std::erase(selected, edge);
          if (!dim->negated) {
            selected.push_back(edge);
          }
        };
        if (dim->isBidir) {
          add(DimVector::POS);
          add(DimVector::NEG);
        } else {
          add(dim->direction);
        }
      }
    }

    const auto evaluatePathDirect =
        [&](const PathExpression &pe,
            std::optional<CellRef> ctx) -> std::vector<CellRef> {
      std::vector<CellRef> cur;
      if (pe.anchor.kind == AnchorKind::NamedStore) {
        if (coordinator_) {
          if (const auto h = coordinator_->resolveNamedStore(pe.anchor.name))
            cur = {*h};
        }
      } else if (pe.anchor.kind == AnchorKind::Home ||
                 pe.anchor.kind == AnchorKind::Root) {
        cur = {core_.home()};
      } else if (pe.anchor.kind == AnchorKind::Context) {
        if (ctx)
          cur = {*ctx};
        else if (ambientContext_)
          cur = {*ambientContext_};
      } else if (pe.anchor.kind == AnchorKind::Variable) {
        if (const auto b = streamBindings_.find(pe.anchor.name);
            b != streamBindings_.end())
          cur = b->second;
        else if (const auto b = varBindings_.find(pe.anchor.name);
                 b != varBindings_.end() && b->second)
          cur = {*b->second};
      } else if (pe.anchor.kind == AnchorKind::LiteralCellId) {
        cur = {static_cast<CellRef>(pe.anchor.cellId)};
      }
      for (const auto &step : pe.steps) {
        if (const auto *sd = std::get_if<SignedDimensionStep>(&step.selector)) {
          const auto d = resolveDimension(sd->dimName);
          std::vector<CellRef> next;
          for (const auto c : cur) {
            if (const auto n = arena.linkedMinting(c, d, sd->direction)) {
              next.push_back(*n);
            }
          }
          cur = std::move(next);
        }
      }
      return cur;
    };

    std::vector<CellRef> subjects;
    if (const auto *name = std::get_if<std::string>(&fn.args[0].kind)) {
      if (const auto bound = streamBindings_.find(*name);
          bound != streamBindings_.end()) {
        subjects = bound->second;
      } else if (const auto bound = varBindings_.find(*name);
                 bound != varBindings_.end() && bound->second) {
        subjects = {*bound->second};
      }
    } else if (const auto *path = std::get_if<std::shared_ptr<PathExpression>>(
                   &fn.args[0].kind)) {
      subjects = evaluatePathDirect(**path, context);
    } else {
      subjects = compileArgument(fn.args[0], context);
    }

    std::unordered_map<CellRef, CellRef> copies;
    std::vector<CellRef> pending;
    const auto copy = [&](const CellRef source) {
      if (const auto found = copies.find(source); found != copies.end()) {
        return found->second;
      }
      arena.consumeWork();
      const auto authority =
          arena.cloneMaster(source, core_.dims().clone).value_or(source);
      const auto spans = arena.contentOf(authority);
      std::vector<xanadu::PrimediaSpan> content(spans.begin(), spans.end());
      std::optional<std::uint32_t> quoteSpace;
      if (const auto foreign = arena.foreignOf(authority)) {
        if (const auto s = arena.spaceAt(foreign->space)) {
          if (s->store) {
            quoteSpace = foreign->space;
            if (s->manifold) {
              const auto foreignSpans = s->manifold->contentOf(foreign->index);
              content.assign(foreignSpans.begin(), foreignSpans.end());
            }
          }
        }
      } else if (const auto quote = arena.quotedContent(authority)) {
        for (std::uint32_t s = 1; s <= arena.spaceCount(); ++s) {
          if (const auto attached = arena.spaceAt(s);
              attached && attached->store == quote->store) {
            quoteSpace = s;
            content    = quote->spans;
            break;
          }
        }
      }
      const auto value = core_.render(source);
      CellRef snapshot = zigzag::noCell;
      if (quoteSpace) {
        snapshot = arena.makeQuote(*quoteSpace, content);
        if (value && std::holds_alternative<std::int64_t>(*value)) {
          const auto integer = std::get<std::int64_t>(*value);
          zigzag::expectWritten(
              arena.setValueBits(snapshot, xanadu::ValueKind::Int64,
                                 std::bit_cast<std::uint64_t>(integer)));
        } else if (value && std::holds_alternative<double>(*value)) {
          const auto number = std::get<double>(*value);
          zigzag::expectWritten(
              arena.setValueBits(snapshot, xanadu::ValueKind::Double,
                                 std::bit_cast<std::uint64_t>(number)));
        } else if (value && std::holds_alternative<bool>(*value)) {
          const auto boolean = std::get<bool>(*value);
          zigzag::expectWritten(
              arena.setValueBits(snapshot, xanadu::ValueKind::Bool,
                                 static_cast<std::uint64_t>(boolean)));
        }
      } else if (value && std::holds_alternative<std::int64_t>(*value)) {
        snapshot = arena.makeScalarCell(std::get<std::int64_t>(*value));
      } else if (value && std::holds_alternative<double>(*value)) {
        snapshot = arena.makeScalarCell(std::get<double>(*value));
      } else if (value && std::holds_alternative<bool>(*value)) {
        snapshot = arena.makeScalarCell(std::get<bool>(*value));
      } else if (!content.empty()) {
        snapshot = arena.makeCell();
        zigzag::expectWritten(arena.setContent(snapshot, content));
      } else if (value && std::holds_alternative<std::string>(*value)) {
        snapshot = arena.makeCell(std::get<std::string>(*value));
      } else {
        snapshot = arena.makeCell();
      }
      copies.emplace(source, snapshot);
      pending.push_back(source);
      return snapshot;
    };

    std::vector<CellRef> result;
    for (const auto subject : subjects) {
      result.push_back(copy(subject));
    }
    for (std::size_t index = 0; index < pending.size(); ++index) {
      const auto source   = pending[index];
      const auto snapshot = copies.at(source);
      for (const auto &[dim, direction] : selected) {
        arena.consumeWork();
        if (const auto target = arena.linkedMinting(source, dim, direction)) {
          const auto copiedTarget = copy(*target);
          zigzag::expectWritten(
              arena.link(snapshot, dim, direction, copiedTarget));
        }
      }
    }
    return result;
  }

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

  // One subject, one dimension, both directions assigned: the two reciprocal
  // writes land on the subject's own slots when exactly one target is the
  // subject itself, and the second Link would silently overwrite the first.
  // Naming the same variable on both sides keeps both targets equal, which is
  // collision-free; anything else needs a cell-identity comparison.
  const auto variableName =
      [](const ValueExpr &value) -> std::optional<std::string_view> {
    if (const auto *name = std::get_if<std::string>(&value.kind))
      return std::string_view{*name};
    return std::nullopt;
  };
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
               "link when one target is the subject, and telling that apart "
               "needs a cell-identity comparison that no Vortex opcode "
               "provides");
    }

  // The subject's cardinality is the one refusal that needs the compiled
  // argument. compileArgument only emits graph ops; nothing executes when the
  // compile throws, and this runs before beginMutationGroup and before any
  // op that writes the store or arena.
  const auto subjects = compileArgument(fn.args[0], context);
  if (subjects.empty()) {
    refuse("apply_shape requires at least one subject");
  }

  // A throw from here to endMutationGroup leaves the group open, which is
  // harmless: the compiler is per-compile, so no open group reaches a later
  // compile, and neighbouring lowerings unwind the same way.
  const CellRef group = beginMutationGroup("#APPLY_SHAPE_GROUP");

  if (!assignments.empty()) {
    if (subjects.size() > 1) {
      for (std::size_t i = 1; i < subjects.size(); ++i) {
        const auto same = core_.arena().makeCell();
        const auto sameOp =
            emitOp(OpcodeKind::SameCell, "#APPLY_SHAPE_SAME_CELL");
        emitInput(sameOp, subjects.front());
        emitInput(sameOp, subjects[i]);
        emitOutput(sameOp, same);
        const auto assertOp = emitOp(OpcodeKind::Assert, "#APPLY_SHAPE_ASSERT");
        emitInput(assertOp, same);
        emitInput(assertOp,
                  emitConstant(std::string(
                      "apply_shape with target assignments rejected reciprocal "
                      "collision between "
                      "distinct subjects by cell-identity comparison")));
      }
    } else if (!subjects.empty() &&
               dynamicStreams_.contains(subjects.front())) {
      const auto first = emitFirstCell(subjects.front());
      const auto iter  = core_.arena().makeCell();
      const auto begin =
          emitOp(OpcodeKind::StreamBegin, "#APPLY_SHAPE_CHECK_BEGIN");
      emitInput(begin, subjects.front());
      emitOutput(begin, iter);
      emitIteratorLoop(iter, [&](CellRef item) {
        const auto same = core_.arena().makeCell();
        const auto sameOp =
            emitOp(OpcodeKind::SameCell, "#APPLY_SHAPE_SAME_CELL");
        emitInput(sameOp, first);
        emitInput(sameOp, item);
        emitOutput(sameOp, same);
        const auto assertOp = emitOp(OpcodeKind::Assert, "#APPLY_SHAPE_ASSERT");
        emitInput(assertOp, same);
        emitInput(assertOp,
                  emitConstant(std::string(
                      "apply_shape with target assignments rejected reciprocal "
                      "collision between "
                      "distinct subjects by cell-identity comparison")));
      });
    }
  }
  std::vector<CellRef> targets;
  TypeExpr cellType;
  cellType.unionType.members.push_back(
      PrimaryType{.kind = BasicType{.kind = BasicType::Kind::Cell}});
  for (const auto &[edge, rule] : assignments) {
    const auto arguments =
        compileArgument(*rule->targetValue, subjects.front());
    const CellRef targetStream = normalizeFrameStream(arguments);
    const std::array<CellRef, 1> held{targetStream};
    emitRequiredContractMatch(cellType, held,
                              "shape assignment target " + rule->dimName);
    targets.push_back(emitFirstCell(targetStream));
  }

  const auto masked = applyShapeProjection(subjects, maskShape);

  for (std::size_t index = 0; index < assignments.size(); ++index) {
    const auto &[edge, rule] = assignments[index];
    const CellRef link       = emitOp(OpcodeKind::Link, "#APPLY_SHAPE_ASSIGN");
    emitInput(link, subjects.front());
    emitInputConstant(
        link, static_cast<std::int64_t>(resolveDimension(rule->dimName)));
    emitInputConstant(link, static_cast<std::int64_t>(toSign(edge.second)));
    emitInput(link, targets[index]);
  }
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
