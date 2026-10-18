/**
 * @file compiler.hpp
 * @brief Lowers a vicon AST to Vortex opcode streams (spec §8.2).
 *
 * Every expression compiles to an opcode graph that produces zero or more
 * result cells. The emission idioms — the retry `altOp` loop, the CP-exit
 * loop boundary, the kill-flag for `\` and bounded evaluation — are pinned
 * in design/vicon-implementation.md; this file is the contract the compiler
 * and its tests are written against.
 *
 * Conventions a compiled program obeys:
 *  - Each generator mints `#Choice` on entry and re-pushes the same `altOp`
 *    per production; exhaustion falls to `#Fail` which pops the next outer
 *    choice point.
 *  - "Produce" writes the result cell and jumps to a shared `Halt` opcode —
 *    reaching `Halt` is Icon's suspension.
 *  - `Driver::resume()` lands the cursor on a `#Fail` opcode, which is Icon's
 *    resumption.
 *  - Variables live on the cursor's `+d.vars`/`+d.values` scope via `Bind`/
 *    `Resolve` opcodes; `<-` is an ordinary trailed write, `:=` the same
 *    write past the live mark.
 */
#ifndef COMMON_XANADU_VICON_COMPILER_HPP
#define COMMON_XANADU_VICON_COMPILER_HPP

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/xanadu/vicon/ast.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_stdlib.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"

namespace xanadu::vicon {

/// One compiled vicon program: its entry opcode, the result and suspension
/// plumbing the driver needs, and the procedures it exported.
struct CompiledProgram {
  zigzag::CellRef entry{zigzag::noCell};
  zigzag::CellRef resultCell{zigzag::noCell}; ///< each produce writes here
  zigzag::CellRef haltOp{zigzag::noCell};     ///< the shared suspension point
  zigzag::CellRef failOp{zigzag::noCell};     ///< shared `#Fail` for resume()
  std::vector<std::pair<std::string, zigzag::CellRef>> procedures;
};

/// Why a source expression could not be compiled — named, so a REPL can say
/// which, rather than returning an empty program (spec §8.2).
enum class CompileError : std::uint8_t {
  LexicalError,
  UnexpectedToken,
  UnboundIdentifier,
  UnbalancedScan,
  BreakOutsideLoop,
  NextOutsideLoop,
  UnsupportedConstruct,
};

struct CompileFailure {
  CompileError code{CompileError::UnexpectedToken};
  std::string detail;
  SourceLocation loc{};
};

class Compiler {
public:
  explicit Compiler(zigzag::vortex::VortexCore &core);

  /// Compile a whole program; std::expected so a failure names its reason
  /// rather than throwing or returning a half-built stream.
  [[nodiscard]] std::expected<CompiledProgram, CompileFailure>
  compile(const Program &program);

  /// Parse and compile one source string into a runnable program.
  [[nodiscard]] std::expected<CompiledProgram, CompileFailure>
  compileSource(std::string_view source);

  /// Parse and compile a single expression (REPL path). The program's entry
  /// produces the expression's results in order.
  [[nodiscard]] std::expected<CompiledProgram, CompileFailure>
  compileExpression(std::string_view source);

  /// Setters chain (AGENTS.md).
  Compiler *withScanningSubject(zigzag::CellRef subject) noexcept {
    scanSubject_ = subject;
    return this;
  }

  [[nodiscard]] zigzag::vortex::VortexCore &core() noexcept { return core_; }
  [[nodiscard]] zigzag::vortex::VortexVM &vm() noexcept { return vm_; }
  [[nodiscard]] zigzag::vortex::VortexStdLib &stdlib() noexcept {
    return stdlib_;
  }

private:
  // Emission helpers shared by every node kind. Each emit* writes opcode
  // cells into the arena and returns the entry cell of that fragment.
  struct EmitResult {
    zigzag::CellRef entry{zigzag::noCell};
    zigzag::CellRef produceCont{zigzag::noCell}; ///< where produce jumps
  };

  /// Loop context: flags consulted on every produce path in the loop's
  /// dynamic extent (design doc, break/next idiom).
  struct LoopContext {
    zigzag::CellRef breakFlag{zigzag::noCell};
    zigzag::CellRef nextFlag{zigzag::noCell};
    zigzag::CellRef afterLoop{zigzag::noCell};
    zigzag::CellRef loopHead{zigzag::noCell};
    zigzag::CellRef resultCell{zigzag::noCell};
  };

  std::expected<zigzag::CellRef, CompileFailure> emitExpr(const Expr &e);
  std::expected<zigzag::CellRef, CompileFailure>
  emitLiteral(const ELiteral &lit);
  std::expected<zigzag::CellRef, CompileFailure> emitTo(const ETo &to);
  std::expected<zigzag::CellRef, CompileFailure>
  emitAlternation(const EAlternation &alt);
  std::expected<zigzag::CellRef, CompileFailure> emitLimit(const ELimit &lim);
  std::expected<zigzag::CellRef, CompileFailure> emitBinary(const EBinary &b);
  std::expected<zigzag::CellRef, CompileFailure> emitIf(const EIf &e);
  std::expected<zigzag::CellRef, CompileFailure> emitEvery(const EEvery &e);
  std::expected<zigzag::CellRef, CompileFailure> emitWhile(const EWhile &e);
  std::expected<zigzag::CellRef, CompileFailure> emitUntil(const EUntil &e);
  std::expected<zigzag::CellRef, CompileFailure> emitCall(const ECall &c);
  std::expected<zigzag::CellRef, CompileFailure> emitScan(const EBinary &s);
  std::expected<zigzag::CellRef, CompileFailure>
  emitBounded(const Expr &e, zigzag::CellRef resultCell,
              zigzag::CellRef contOp);

  zigzag::CellRef mintOp(zigzag::vortex::OpcodeKind kind,
                         std::string_view label = {});
  zigzag::CellRef mintScalar(std::int64_t v);
  void spin(zigzag::CellRef from, zigzag::CellRef to);

  zigzag::vortex::VortexCore &core_;
  zigzag::vortex::VortexVM vm_;
  zigzag::vortex::VortexStdLib stdlib_;
  std::vector<LoopContext> loopStack_;
  std::unordered_map<std::string, zigzag::CellRef> procedures_;
  zigzag::CellRef scanSubject_{zigzag::noCell};
};

} // namespace xanadu::vicon

#endif // COMMON_XANADU_VICON_COMPILER_HPP
