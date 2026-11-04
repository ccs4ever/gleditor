#ifndef COMMON_XANADU_VAGENT_COMPILER_HPP
#define COMMON_XANADU_VAGENT_COMPILER_HPP

#include <expected>
#include <map>
#include <optional>
#include <string>

#include "common/xanadu/vagent/ast.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"

namespace xanadu::vagent {

struct CompilerBinding {
  zigzag::CellRef cell;
  Type type;
  // §10.1's state-side marker bit: set by the binding-table driver when
  // the cell is a state-side checkpointed slot rather than a frame-bound
  // capture — the provenance phase-3 fragment emission records for
  // `closed` re-verification (a linked capture is discoverable by edge;
  // `cells"$cap_N"` anchors resolve only on stable captures).
  bool stateSide{false};
};
using CompilerBindings = std::map<std::string, CompilerBinding, std::less<>>;

enum class ResultUse { Value, Discard };

struct ScalarCompilerLimits {
  std::size_t maxNodes{65'536};
  std::size_t maxDepth{256};
  // §5.5's staged-evaluation budget: the sibling of CheckLimits' bounds —
  // a divergent `static` would otherwise loop the compile forever. Passed
  // to VortexVM::run as the operand's cycle bound per `static` node.
  std::size_t staticFuel{100'000};
};

struct CompiledStatement {
  zigzag::CellRef entry;
  zigzag::CellRef result;
  zigzag::CellRef produced;
  // Absence means statically non-producing (fail), not an untyped value.
  std::optional<Type> resultType;
};

// First Vortex-lowering iteration: scalar literals/typed bindings, arithmetic,
// string concatenation, comparisons, &, |, -/not, scalar :=/<-, fail, lexical
// block locals, if, and §5.5 `static` staged evaluation (the operand is
// lowered, run in the compile arena under staticFuel with no mark
// outstanding, and folded in as a literal). A block is one nested expression
// in this API, not a plan-body sequencer: failure rolls the whole statement
// back. Each child statement takes its first result. Conditional conditions
// are pure and test success, not Bool truth. Value-producing conditional
// arms share a scalar type (or fail); discarded arms may differ and produce
// Unit on success. Surviving code"…"/splice nodes are named compile
// refusals — expansion completes before lowering. Unsupported syntax is
// refused; this is not an agent runner or a full compiler.
class Compiler {
public:
  Compiler(zigzag::vortex::VortexCore &core, zigzag::vortex::VortexVM &vm,
           ScalarCompilerLimits limits = {})
      : core_(core), vm_(vm), limits_(limits) {}

  // All graph cells must be assembled before turn marks or choice points exist.
  // Binding cells must outlive the graph and retain their declared scalar
  // types. Unsupported/name/type/shape errors mint nothing. Allocation-budget
  // failures propagate as resource faults; there is no arena rollback of VM
  // registrations. Discard affects result use, independently of whether the
  // position allows bounded-only operations; successful discarded results are
  // Unit, while failures still produce no value.
  [[nodiscard]] std::expected<CompiledStatement, Diagnostic>
  compileStatement(const Expr &statement, const CompilerBindings &bindings = {},
                   ResultUse use = ResultUse::Value);

  // Run entry on the owner's existing cursor. Only after normal Halt may the
  // caller inspect produced (Bool); result is meaningful only when it is true.
  // The graph bounds the first result and preserves the caller's choice depth.
  // A core numeric contract fault must be backtracked/resumed by the adapter,
  // as specified in §4.2; run failure is not a completed statement result.

private:
  zigzag::vortex::VortexCore &core_;
  zigzag::vortex::VortexVM &vm_;
  ScalarCompilerLimits limits_;
};

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_COMPILER_HPP
