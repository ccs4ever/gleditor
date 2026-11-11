/**
 * @file logic_compile_core.cpp
 * @brief Card P1: predicate frames, clause chains, head unification, body
 * goals, recursion, the solution driver and the query entry. Every definition
 * below is a stub that refuses until P1 fills it in.
 */
#include "common/xanadu/vortex/logic_compile.hpp"

namespace zigzag::vortex::logic {

namespace {
constexpr std::string_view kCard = "P1";
} // namespace

CellRef appendOp(CompileContext &, Chain &, OpcodeKind,
                 std::span<const CellRef>, std::span<const CellRef>) {
  refuseUnlowered(kCard, "appendOp");
}

CellRef newLocal(CompileContext &, FrameScope &) {
  refuseUnlowered(kCard, "newLocal");
}

CellRef variableSlot(CompileContext &, FrameScope &, Chain &, CellRef) {
  refuseUnlowered(kCard, "variableSlot");
}

CellRef emitTerm(CompileContext &, FrameScope &, Chain &, CellRef) {
  refuseUnlowered(kCard, "emitTerm");
}

CellRef emitConstant(CompileContext &, Chain &, const ScalarConstant &) {
  refuseUnlowered(kCard, "emitConstant");
}

std::optional<CellRef> argumentPort(const FrameScope &, std::size_t) {
  refuseUnlowered(kCard, "argumentPort");
}

bool bindArgumentPort(FrameScope &, CellRef, std::size_t) {
  refuseUnlowered(kCard, "bindArgumentPort");
}

void emitUnifyOrFail(CompileContext &, FrameScope &, Chain &, CellRef,
                     CellRef) {
  refuseUnlowered(kCard, "emitUnifyOrFail");
}

void emitFailUnless(CompileContext &, FrameScope &, Chain &, CellRef) {
  refuseUnlowered(kCard, "emitFailUnless");
}

Label newLabel(CompileContext &) { refuseUnlowered(kCard, "newLabel"); }

void placeLabel(CompileContext &, Chain &, Label) {
  refuseUnlowered(kCard, "placeLabel");
}

CellRef emitChoice(CompileContext &, Chain &, Label) {
  refuseUnlowered(kCard, "emitChoice");
}

CellRef emitJump(CompileContext &, Chain &, Label) {
  refuseUnlowered(kCard, "emitJump");
}

CellRef emitBranchIf(CompileContext &, Chain &, CellRef, Label) {
  refuseUnlowered(kCard, "emitBranchIf");
}

CellRef emitBranchUnless(CompileContext &, Chain &, CellRef, Label) {
  refuseUnlowered(kCard, "emitBranchUnless");
}

CellRef emitCut(CompileContext &, FrameScope &, Chain &) {
  refuseUnlowered(kCard, "emitCut");
}

NegationScope openNegationScope(CompileContext &, FrameScope &, Chain &) {
  refuseUnlowered(kCard, "openNegationScope");
}

void closeNegationScope(FrameScope &, const NegationScope &) {
  refuseUnlowered(kCard, "closeNegationScope");
}

CellRef emitCutTo(CompileContext &, Chain &, CellRef) {
  refuseUnlowered(kCard, "emitCutTo");
}

void emitNonTermGoal(CompileContext &, FrameScope &, Chain &, CellRef) {
  refuseUnlowered(kCard, "goal is not a term");
}

LoweredQuery lowerQuery(CompileContext &, std::span<const CellRef>) {
  refuseUnlowered(kCard, "lowerQuery");
}

struct SolutionDriver::Impl {};

SolutionDriver::SolutionDriver(CompileContext &, const LoweredQuery &) {
  refuseUnlowered(kCard, "SolutionDriver");
}

SolutionDriver::~SolutionDriver() = default;

DriveOutcome SolutionDriver::next() {
  refuseUnlowered(kCard, "SolutionDriver::next");
}

LogicSolution SolutionDriver::solution() const {
  refuseUnlowered(kCard, "SolutionDriver::solution");
}

void SolutionDriver::stop() { refuseUnlowered(kCard, "SolutionDriver::stop"); }

bool compileAndRunQuery(CompileContext &, std::span<const CellRef>,
                        const std::function<bool(const LogicSolution &)> &,
                        std::size_t) {
  refuseUnlowered(kCard, "compileAndRunQuery");
}

bool hasUserClauses(const CompileContext &, const PredicateKey &) {
  refuseUnlowered(kCard, "hasUserClauses");
}

void emitUserCall(CompileContext &, FrameScope &, Chain &,
                  const PredicateKey &key, CellRef) {
  refuseUnlowered(kCard, "call to user predicate " + key.display());
}

std::optional<CellRef> compilePredicateFrame(CompileContext &,
                                             const PredicateKey &key) {
  refuseUnlowered(kCard, "predicate frame " + key.display());
}

void emitUnknownPredicate(CompileContext &, FrameScope &, Chain &,
                          const PredicateKey &key) {
  refuseUnlowered(kCard, "no lowering for predicate " + key.display());
}

void emitClause(CompileContext &, FrameScope &, Chain &, CellRef, bool) {
  refuseUnlowered(kCard, "emitClause");
}

} // namespace zigzag::vortex::logic
