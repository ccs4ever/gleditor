// Q10: PathMorphism steps (`$p/+d.comment => [ ... ]`), with or without a
// perspective qualifier.
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu::vql {
using zigzag::CellRef;
using zigzag::vortex::OpcodeKind;

std::vector<CellRef> VQLCompiler::compilePathMorphismStep(
    const PathStep &step, const CellRef current, const bool hasContext) {
  const auto *morphism = std::get_if<PathMorphism>(&step.selector);
  if (!morphism) {
    throw QueryRefusal(QueryErrorCategory::Unsupported,
                       "expected path morphism selector");
  }
  if (morphism->op != PathMorphism::Op::Arrow) {
    throw QueryRefusal(QueryErrorCategory::Unsupported,
                       "this VQL path selector does not have Vortex graph "
                       "lowering yet");
  }

  std::string dimName     = "d.content";
  auto direction          = zigzag::DimVector::POS;
  const auto parseDimText = [&](std::string_view raw) {
    if (raw.starts_with('+')) {
      direction = zigzag::DimVector::POS;
      dimName   = std::string(raw.substr(1));
    } else if (raw.starts_with('-')) {
      direction = zigzag::DimVector::NEG;
      dimName   = std::string(raw.substr(1));
    } else {
      direction = zigzag::DimVector::POS;
      dimName   = std::string(raw);
    }
  };

  if (const auto *literal =
          std::get_if<ScalarLiteral>(&morphism->subject.kind)) {
    if (const auto *dimension = std::get_if<std::string>(&literal->value)) {
      parseDimText(*dimension);
    } else {
      throw QueryRefusal(QueryErrorCategory::Unsupported,
                         "VQL dimension morphism requires a dimension subject");
    }
  } else if (const auto *dimension =
                 std::get_if<std::string>(&morphism->subject.kind)) {
    parseDimText(*dimension);
  } else if (const auto *p = std::get_if<std::shared_ptr<PathExpression>>(
                 &morphism->subject.kind);
             p && *p && !(*p)->steps.empty() &&
             std::holds_alternative<SignedDimensionStep>(
                 (*p)->steps[0].selector)) {
    const auto &sd = std::get<SignedDimensionStep>((*p)->steps[0].selector);
    dimName        = sd.dimName;
    direction      = sd.direction;
  } else if (!std::holds_alternative<std::shared_ptr<PathExpression>>(
                 morphism->subject.kind) ||
             std::get<std::shared_ptr<PathExpression>>(
                 morphism->subject.kind)) {
    throw QueryRefusal(QueryErrorCategory::Unsupported,
                       "VQL dimension morphism requires a dimension subject");
  }

  if (!morphism->perspective.empty()) {
    std::string perspective = morphism->perspective;
    if (perspective.starts_with('@')) perspective.erase(0, 1);
    if (perspective.ends_with(':')) perspective.pop_back();
    dimName += "@" + perspective;
  }
  const auto linkDimension = resolveDimension(dimName);
  const auto subject       = hasContext ? current : core_.home();
  const auto attachPoint   = emitExtreme(subject, linkDimension, direction);
  const auto group         = beginMutationGroup("#MORPHISM_GROUP");

  const auto clearRef = [&](const CellRef destination, const char *label) {
    const auto clear = emitOp(OpcodeKind::ClearCellRefs, label);
    emitOutput(clear, destination);
  };
  const auto copyRef = [&](const CellRef source, const CellRef destination,
                           const char *label) {
    const auto copy = emitOp(OpcodeKind::CopyCellRefs, label);
    emitInput(copy, source);
    emitOutput(copy, destination);
  };
  const auto branchFalse = [&](const CellRef condition, const CellRef target,
                               const char *label) {
    const auto branch = emitOp(OpcodeKind::BranchFalseCell, label);
    emitInput(branch, condition);
    emitInput(branch, target);
  };

  const CellRef carrier = core_.arena().makeCell();
  clearRef(carrier, "#MORPHISM_CARRIER_CLEAR");
  copyRef(attachPoint, carrier, "#MORPHISM_CARRIER_INIT");

  std::vector<CellRef> spawned;
  for (const auto &target : morphism->targets) {
    const auto value = compileValueExpr(target, current);
    if (!value) continue;

    const CellRef targetCell = core_.arena().makeCell();
    clearRef(targetCell, "#MORPHISM_TARGET_CLEAR");
    const CellRef present = emitCellPresence(*value);
    const CellRef end     = mintOpcode(OpcodeKind::Nop, "#MORPHISM_TARGET_END");
    branchFalse(present, end, "#MORPHISM_TARGET_BRANCH");

    if (std::holds_alternative<ScalarLiteral>(target.kind)) {
      const auto create = emitOp(OpcodeKind::New, "#MORPHISM_NEW");
      emitInput(create, carrier);
      emitInputConstant(create, static_cast<std::int64_t>(linkDimension));
      emitInputConstant(create,
                        static_cast<std::int64_t>(
                            direction == zigzag::DimVector::POS ? 1 : -1));

      const auto read = emitOp(OpcodeKind::Link, "#MORPHISM_RESULT");
      emitInput(read, carrier);
      emitInputConstant(read, static_cast<std::int64_t>(linkDimension));
      emitInputConstant(read,
                        static_cast<std::int64_t>(
                            direction == zigzag::DimVector::POS ? 1 : -1));
      emitOutput(read, targetCell);

      const auto write = emitOp(OpcodeKind::Value, "#MORPHISM_VALUE");
      emitInput(write, targetCell);
      emitInputConstant(write, std::int64_t{0});
      emitInputConstant(write, std::int64_t{-1});
      emitInput(write, *value);
    } else {
      const auto link = emitOp(OpcodeKind::Link, "#MORPHISM_LINK");
      emitInput(link, carrier);
      emitInputConstant(link, static_cast<std::int64_t>(linkDimension));
      emitInputConstant(link,
                        static_cast<std::int64_t>(
                            direction == zigzag::DimVector::POS ? 1 : -1));
      emitInput(link, *value);
      copyRef(*value, targetCell, "#MORPHISM_EXISTING_TARGET");
    }

    copyRef(targetCell, carrier, "#MORPHISM_CARRIER_ADVANCE");
    appendOpcode(end);
    spawned.push_back(targetCell);
  }
  endMutationGroup(group, "#MORPHISM_GROUP_END");
  return spawned;
}

} // namespace xanadu::vql
