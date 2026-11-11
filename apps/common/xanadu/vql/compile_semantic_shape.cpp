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
#include "common/xanadu/vql/compiler.hpp"

#include <array>
#include <optional>
#include <string>
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

  if (fn.name == "project")
    refuse("VQL 'project' is not compiled to Vortex: it must copy the "
           "selected topology into detached cells that share the source "
           "content spans and quote bindings, and no Vortex opcode creates "
           "such a cell");

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

  const auto subjects = compileArgument(fn.args[0], context);

  if (!assignments.empty()) {
    // Two subjects sharing a target, or a repeated subject, can only be told
    // apart by cell identity, which VqlCompare (a value comparison) cannot
    // supply. A single statically known subject has nothing to collide with.
    if (subjects.size() != 1 || dynamicStreams_.contains(subjects.front()))
      refuse("apply_shape with target assignments requires a single "
             "statically known subject: rejecting reciprocal collisions "
             "between subjects needs a cell-identity comparison that no "
             "Vortex opcode provides");
    for (const auto &[edge, rule] : assignments)
      if (resolveDimension(rule->dimName) == core_.dims().clone)
        refuse("shape assignment cannot assign the clone dimension");
  }

  const CellRef group = beginMutationGroup("#APPLY_SHAPE_GROUP");
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
