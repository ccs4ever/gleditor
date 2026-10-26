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
};
using CompilerBindings = std::map<std::string, CompilerBinding, std::less<>>;

enum class ResultUse { Value, Discard };

struct ScalarCompilerLimits {
  std::size_t maxNodes{65'536};
  std::size_t maxDepth{256};
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
// block locals and if. A block is one nested expression in this API, not a
// plan-body sequencer: failure rolls the whole statement back. Each child
// statement takes its first result. Conditional conditions are pure and test
// success, not Bool truth. Value-producing conditional arms share a scalar
// type (or fail); discarded arms may differ and produce Unit on success.
// Unsupported syntax is refused; this is not an agent runner or a full
// compiler.
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
