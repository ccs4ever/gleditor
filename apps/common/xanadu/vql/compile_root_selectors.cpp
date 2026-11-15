// Q2: root selectors, perspective read steps, macro-group selectors.
#include "common/xanadu/vql/compiler.hpp"

#include <array>
#include <span>
#include <string>
#include <utility>
#include <vector>

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

    case RootSelectorStep::Kind::RootMetacells:
      throw QueryRefusal(
          QueryErrorCategory::Unsupported,
          "root metacell enumeration has no graph-visible root registry");

    case RootSelectorStep::Kind::ActiveCursor:
      return {emitRank(current, core_.dims().cursors, zigzag::DimVector::POS)};

    case RootSelectorStep::Kind::AllCursors:
    case RootSelectorStep::Kind::NamedCursor:
    case RootSelectorStep::Kind::AllNamedCursors: {
      const bool includePinned =
          root->kind == RootSelectorStep::Kind::AllCursors ||
          root->kind == RootSelectorStep::Kind::AllNamedCursors;
      const bool filterName =
          root->kind == RootSelectorStep::Kind::NamedCursor ||
          root->kind == RootSelectorStep::Kind::AllNamedCursors;

      CellRef cursors =
          emitRank(current, core_.dims().cursors, zigzag::DimVector::POS);
      if (includePinned) {
        const CellRef pinned   = emitRank(current, core_.dims().pinningCursors,
                                          zigzag::DimVector::POS);
        const CellRef combined = emitStreamClear();
        emitAppendStream(combined, std::span{&cursors, std::size_t{1}});
        emitAppendStream(combined, std::span{&pinned, std::size_t{1}});
        cursors = combined;
      }

      if (!filterName) return {cursors};

      const CellRef expectedName = emitConstant(root->name);
      return emitMap(
          std::span{&cursors, std::size_t{1}}, [&](const CellRef candidate) {
            // A cursor may carry multiple names; return it once if
            // any member of its d.name rank matches.
            const CellRef matched = core_.arena().makeCell();
            const CellRef clear =
                emitOp(OpcodeKind::ClearCellRefs, "#CURSOR_NAME_MATCH_CLEAR");
            emitOutput(clear, matched);

            const CellRef names =
                emitRank(candidate, core_.dims().name, zigzag::DimVector::POS);
            const auto nameWalk = emitMap(
                std::span{&names, std::size_t{1}}, [&](const CellRef nameCell) {
                  const CellRef equal =
                      emitOp(OpcodeKind::Eq, "#CURSOR_NAME_EQ");
                  emitInput(equal, nameCell);
                  emitInput(equal, expectedName);
                  const CellRef matches = core_.arena().makeCell();
                  emitOutput(equal, matches);

                  const CellRef end =
                      mintOpcode(OpcodeKind::Nop, "#CURSOR_NAME_MATCH_END");
                  const CellRef branch = emitOp(OpcodeKind::BranchFalseCell,
                                                "#CURSOR_NAME_MATCH_BRANCH");
                  emitInput(branch, matches);
                  emitInput(branch, end);
                  const CellRef copy = emitOp(OpcodeKind::CopyCellRefs,
                                              "#CURSOR_NAME_MATCH_COPY");
                  emitInput(copy, candidate);
                  emitOutput(copy, matched);
                  appendOpcode(end);
                  return std::vector<CellRef>{};
                });
            static_cast<void>(nameWalk);

            const CellRef present = emitCellPresence(matched);
            const CellRef result  = core_.arena().makeCell();
            const CellRef clearResult =
                emitOp(OpcodeKind::ClearCellRefs, "#CURSOR_FILTER_CLEAR");
            emitOutput(clearResult, result);
            const CellRef end =
                mintOpcode(OpcodeKind::Nop, "#CURSOR_FILTER_END");
            const CellRef branch =
                emitOp(OpcodeKind::BranchFalseCell, "#CURSOR_FILTER_BRANCH");
            emitInput(branch, present);
            emitInput(branch, end);
            const CellRef copy =
                emitOp(OpcodeKind::CopyCellRefs, "#CURSOR_FILTER_COPY");
            emitInput(copy, candidate);
            emitOutput(copy, result);
            appendOpcode(end);
            return std::vector<CellRef>{result};
          });
    }
    }
  }

  if (const auto *perspective = std::get_if<PerspectiveStep>(&step.selector)) {
    if (!perspective->targetStep) {
      throw QueryRefusal(QueryErrorCategory::Unsupported,
                         "perspective traversal has no target step");
    }

    const auto *dimension =
        std::get_if<SignedDimensionStep>(&perspective->targetStep->selector);
    if (dimension == nullptr || !dimension->creates.empty()) {
      throw QueryRefusal(
          QueryErrorCategory::Unsupported,
          "perspective traversal supports only read-only dimension steps");
    }

    std::string name = perspective->perspective;
    if (name.starts_with('@')) name.erase(0, 1);
    if (name.ends_with(':')) name.pop_back();
    if (name.empty()) {
      throw QueryRefusal(QueryErrorCategory::Unsupported,
                         "perspective traversal requires a named axis");
    }

    // Qualified dimensions are the graph-visible representation currently
    // used by perspective writes. The full clone-map perspective semantics
    // remain unavailable to the VM compiler.
    PathStep qualifiedStep  = *perspective->targetStep;
    auto qualifiedDimension = *dimension;
    qualifiedDimension.dimName += "@" + name;
    qualifiedStep.selector = std::move(qualifiedDimension);

    const std::vector<CellRef> input{current};
    const std::array<PathStep, 1> qualifiedSteps{std::move(qualifiedStep)};
    const auto qualified = compileSteps(qualifiedSteps, input, hasContext);
    const auto qualifiedPresent = emitStreamPresence(qualified);

    const CellRef result = emitStreamClear();
    const CellRef useCanonical =
        mintOpcode(OpcodeKind::Nop, "#PERSPECTIVE_CANONICAL");
    const CellRef done = mintOpcode(OpcodeKind::Nop, "#PERSPECTIVE_DONE");
    const CellRef choose =
        emitOp(OpcodeKind::BranchFalseCell, "#PERSPECTIVE_FALLBACK");
    emitInput(choose, qualifiedPresent);
    emitInput(choose, useCanonical);
    emitAppendStream(result, qualified);

    const CellRef skipFallback =
        emitOp(OpcodeKind::BranchFalseCell, "#PERSPECTIVE_SKIP_FALLBACK");
    emitInputConstant(skipFallback, false);
    emitInput(skipFallback, done);

    appendOpcode(useCanonical);
    const std::array<PathStep, 1> canonicalSteps{*perspective->targetStep};
    const auto canonical = compileSteps(canonicalSteps, input, hasContext);
    emitAppendStream(result, canonical);
    appendOpcode(done);
    return {result};
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

  throw QueryRefusal(QueryErrorCategory::Unsupported,
                     "this VQL path selector does not have Vortex graph "
                     "lowering yet");
}

} // namespace xanadu::vql
