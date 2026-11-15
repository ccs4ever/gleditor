// Q2: root selectors, perspective read steps, macro-group selectors.
#include "common/xanadu/vql/compiler.hpp"

#include <array>
#include <stdexcept>

namespace xanadu::vql {
using zigzag::CellRef;
using zigzag::vortex::OpcodeKind;

std::vector<CellRef> VQLCompiler::compileRootSelectorStep(
    const PathStep &step, const CellRef current, const bool hasContext) {
  if (std::holds_alternative<GenesisSelector>(step.selector)) {
    return {current};
  }

  if (const auto *root = std::get_if<RootSelectorStep>(&step.selector)) {
    switch (root->kind) {
    case RootSelectorStep::Kind::Genesis:
      return {current};

    case RootSelectorStep::Kind::ActiveCursor: {
      const auto rank =
          emitRank(current, core_.dims().cursors, zigzag::DimVector::POS);
      const std::array<CellRef, 1> rankInputs{rank};
      return emitClamp(rankInputs, RangeClamp{.start = 1, .end = 1});
    }

    case RootSelectorStep::Kind::AllCursors:
      return {emitRank(current, core_.dims().cursors, zigzag::DimVector::POS)};

    case RootSelectorStep::Kind::NamedCursor: {
      const auto rank =
          emitRank(current, core_.dims().cursors, zigzag::DimVector::POS);
      const auto expectedName = emitConstant(root->name);
      const std::array<CellRef, 1> rankInputs{rank};
      return emitMap(rankInputs, [&](CellRef candidate) {
        const auto result = core_.arena().makeCell();
        const auto clear =
            emitOp(OpcodeKind::ClearCellRefs, "#CURSOR_NAME_CLEAR");
        emitOutput(clear, result);

        const auto eq = emitOp(OpcodeKind::Eq, "#CURSOR_NAME_EQ");
        emitInput(eq, candidate);
        emitInput(eq, expectedName);
        const auto match = core_.arena().makeCell();
        emitOutput(eq, match);

        const auto end = mintOpcode(OpcodeKind::Nop, "#CURSOR_FILTER_END");
        const auto branch =
            emitOp(OpcodeKind::BranchFalseCell, "#CURSOR_FILTER_BRANCH");
        emitInput(branch, match);
        emitInput(branch, end);

        const auto copy =
            emitOp(OpcodeKind::CopyCellRefs, "#CURSOR_FILTER_COPY");
        emitInput(copy, candidate);
        emitOutput(copy, result);
        appendOpcode(end);

        return std::vector<CellRef>{result};
      });
    }

    case RootSelectorStep::Kind::RootMetacells:
    case RootSelectorStep::Kind::AllNamedCursors:
      return {current};
    }
  }

  if (const auto *persp = std::get_if<PerspectiveStep>(&step.selector)) {
    if (persp->targetStep) {
      if (const auto *sd =
              std::get_if<SignedDimensionStep>(&persp->targetStep->selector)) {
        std::string perspDimName = sd->dimName;
        if (!persp->perspective.empty()) {
          std::string normPersp = persp->perspective;
          if (normPersp.starts_with('@')) normPersp = normPersp.substr(1);
          if (normPersp.ends_with(':'))
            normPersp = normPersp.substr(0, normPersp.size() - 1);
          perspDimName += "@" + normPersp;
        }
        const auto perspDim = resolveDimension(perspDimName);
        if (const auto linked =
                core_.arena().linked(current, perspDim, sd->direction)) {
          return {*linked};
        }
        const auto canonDim = resolveDimension(sd->dimName);
        if (const auto fallback =
                core_.arena().linked(current, canonDim, sd->direction)) {
          return {*fallback};
        }
        throw QueryRefusal(
            QueryErrorCategory::Unsupported,
            "this VQL path selector does not have Vortex graph lowering yet");
      }
      auto res = compileSelector(*persp->targetStep, current, hasContext);
      if (res.empty()) {
        throw QueryRefusal(
            QueryErrorCategory::Unsupported,
            "this VQL path selector does not have Vortex graph lowering yet");
      }
      return res;
    }
    return {current};
  }

  if (const auto *group =
          std::get_if<std::shared_ptr<MacroDimensionGroup>>(&step.selector)) {
    if (!*group) return {current};
    const auto walked = compileSteps((*group)->steps, {current}, hasContext);
    if ((*group)->repetition == Repetition::ZeroOrOne ||
        (*group)->repetition == Repetition::ZeroOrMore) {
      const auto result = emitStreamClear();
      emitAppendStream(result, walked);
      const auto present = emitStreamPresence(walked);
      const auto absent  = core_.arena().makeCell();
      const auto negate  = emitOp(OpcodeKind::Not, "#MACRO_FALLBACK_NOT");
      emitInput(negate, present);
      emitOutput(negate, absent);
      const auto end = mintOpcode(OpcodeKind::Nop, "#MACRO_FALLBACK_END");
      const auto branch =
          emitOp(OpcodeKind::BranchFalseCell, "#MACRO_FALLBACK");
      emitInput(branch, absent);
      emitInput(branch, end);
      const auto append =
          emitOp(OpcodeKind::StreamAppend, "#MACRO_YIELD_CURRENT");
      emitInput(append, current);
      emitOutput(append, result);
      appendOpcode(end);
      return {result};
    }
    return walked;
  }

  throw std::invalid_argument(
      "this VQL path selector does not have Vortex graph lowering yet");
}

} // namespace xanadu::vql
