// Q2: root selectors, perspective read steps, macro-group selectors.
#include "common/xanadu/vql/compiler.hpp"

namespace xanadu::vql {
using zigzag::CellRef;

std::vector<CellRef> VQLCompiler::compileRootSelectorStep(
    const PathStep &step, const CellRef current, const bool hasContext) {
  const auto *rootStep = std::get_if<RootSelectorStep>(&step.selector);
  if (!rootStep) {
    if (std::holds_alternative<GenesisSelector>(step.selector)) {
      if (hasContext) {
        return {current};
      }
      return {core_.home()};
    }
    throw QueryRefusal(QueryErrorCategory::Unsupported,
                       "expected root selector step");
  }

  if (rootStep->kind == RootSelectorStep::Kind::Genesis) {
    if (hasContext) {
      return {current};
    }
    return {core_.home()};
  }
  if (rootStep->kind == RootSelectorStep::Kind::ActiveCursor ||
      rootStep->kind == RootSelectorStep::Kind::NamedCursor ||
      rootStep->kind == RootSelectorStep::Kind::AllNamedCursors) {
    const auto cursorsDim = core_.dims().cursors;
    std::vector<CellRef> results;
    const auto contextCell = hasContext ? current : core_.home();
    if (const auto linkedCell = core_.arena().linked(contextCell, cursorsDim,
                                                     zigzag::DimVector::POS)) {
      if (!rootStep->name.empty()) {
        const auto name = core_.arena().textOf(*linkedCell);
        if (name == rootStep->name) {
          results.push_back(*linkedCell);
        }
      } else {
        results.push_back(*linkedCell);
      }
    }
    if (results.empty() && rootStep->name.empty()) {
      results.push_back(contextCell);
    }
    return results;
  }
  throw QueryRefusal(
      QueryErrorCategory::Unsupported,
      "this VQL path selector does not have Vortex graph lowering yet");
}

} // namespace xanadu::vql
