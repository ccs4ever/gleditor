#include "common/xanadu/vpl/graph_emit.hpp"

#include <stdexcept>

namespace xanadu::vpl::emit {

namespace {

// Card V2 replaces each body. Until then a lowering that reaches a helper is
// refused by name instead of emitting a wrong graph.
[[noreturn]] void notYet(std::string_view helper) {
  throw std::invalid_argument("VPL graph emit helper '" + std::string(helper) +
                              "' is not implemented yet (card V2)");
}

} // namespace

CellRef mintLabel(LowerContext &, std::string_view) { notYet("mintLabel"); }

void placeLabel(LowerContext &, CellRef) { notYet("placeLabel"); }

void branchIfFalse(LowerContext &, CellRef, CellRef, std::string_view) {
  notYet("branchIfFalse");
}

void branchAlways(LowerContext &, CellRef, std::string_view) {
  notYet("branchAlways");
}

void iteratorLoop(LowerContext &, CellRef,
                  gleditor::cpp26::function_ref<void(CellRef)>) {
  notYet("iteratorLoop");
}

void walkRank(LowerContext &, CellRef, DimRef, DimVector,
              gleditor::cpp26::function_ref<void(CellRef)>) {
  notYet("walkRank");
}

CellRef extreme(LowerContext &, CellRef, DimRef, DimVector) {
  notYet("extreme");
}

void countedLoop(LowerContext &, CellRef,
                 gleditor::cpp26::function_ref<void(CellRef)>) {
  notYet("countedLoop");
}

CellRef mintAfter(LowerContext &, CellRef, DimRef, DimVector) {
  notYet("mintAfter");
}

void copyValue(LowerContext &, CellRef, CellRef) { notYet("copyValue"); }

CellRef aliasRegister(LowerContext &, CellRef) { notYet("aliasRegister"); }

void mergeRankInputDependencies(LowerContext &, CellRef,
                                std::span<const CellRef>) {
  notYet("mergeRankInputDependencies");
}

} // namespace xanadu::vpl::emit
