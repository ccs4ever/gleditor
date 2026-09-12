/**
 * @file compiler.hpp
 * @brief VQL Compiler translating VQL AST into Vortex bytecode and zzstructure
 * graphs.
 *
 * Compiles VQL queries into Vortex opcodes chained along +d.spin with dual-wing
 * parameter binding (+d.grab inputs, -d.grab outputs, chained on +d.step).
 * Supports emitting executable stores (home/d.spin ending in Halt) and library
 * stores (home/d.stdlib with exported symbols along +d.vars/+d.values ending in
 * Return).
 */
#ifndef COMMON_XANADU_VQL_COMPILER_HPP
#define COMMON_XANADU_VQL_COMPILER_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/ast.hpp"

namespace xanadu::vql {

struct CompilationOptions {
  bool targetLibrary{false};            ///< Emit library store instead of exec
  std::string moduleName{"std:custom"}; ///< Module name for library store
  std::string symbolName{"query"};      ///< Exported symbol name
  bool optimize{false};                 ///< Enable optimizations
};

struct CompilationResult {
  bool success{true};
  zigzag::CellRef entryOpcode{zigzag::noCell};
  std::string errorMessage{};
  std::vector<zigzag::CellRef> generatedOpcodes;
  std::string disassembly{};
};

class VQLCompiler {
public:
  VQLCompiler(zigzag::vortex::VortexCore &core, zigzag::vortex::VortexVM &vm);

  /// Compiles a parsed QueryExpression AST into Vortex bytecode.
  CompilationResult compile(const QueryExpression &query,
                            const CompilationOptions &options = {});

  /// Lexes, parses, and compiles a query string into Vortex bytecode.
  CompilationResult compile(std::string_view queryString,
                            const CompilationOptions &options = {});

  /// Generates a human-readable disassembly listing of opcodes along +d.spin.
  [[nodiscard]] std::string
  disassemble(zigzag::CellRef entryOpcode,
              std::size_t maxInstructions = 1000) const;

  /// Returns the opcode mnemonic for an OpcodeKind.
  [[nodiscard]] static std::string_view
  opcodeMnemonic(zigzag::vortex::OpcodeKind kind) noexcept;

  /// Resolves a dimension name to its DimRef, minting if non-existent.
  zigzag::DimRef resolveDimension(std::string_view name);

  /// Exports the compiled ArenaManifold instructions and zzstructure to a
  /// Store.
  xanadu::MicroversionId
  exportToStore(xanadu::Store &store,
                const xanadu::MicroversionId &parent = {}) const;

private:
  // Emission helpers
  zigzag::CellRef emitOp(zigzag::vortex::OpcodeKind kind,
                         std::string_view label = {});
  void emitInput(zigzag::CellRef op, zigzag::CellRef operand);
  void emitOutput(zigzag::CellRef op, zigzag::CellRef target);
  zigzag::CellRef
  emitConstant(const zigzag::vortex::CellValue &val,
               std::optional<zigzag::CellRef> designatedCell = std::nullopt);
  zigzag::CellRef emitInputConstant(zigzag::CellRef op,
                                    const zigzag::vortex::CellValue &val);

  // AST compilation subroutines
  zigzag::CellRef compileQuery(const QueryExpression &query);
  zigzag::CellRef compileExecutionBlock(const ExecutionBlock &block);
  zigzag::CellRef compilePathExpression(const PathExpression &path,
                                        zigzag::CellRef ctxCell);
  zigzag::CellRef compileAnchor(const AnchorNode &anchor);
  zigzag::CellRef compilePathStep(const PathStep &step,
                                  zigzag::CellRef inStreamCell);
  zigzag::CellRef compileBooleanExpr(const BooleanExpr &expr,
                                     zigzag::CellRef ctxCell);
  zigzag::CellRef compileValueExpr(const ValueExpr &expr,
                                   zigzag::CellRef ctxCell);
  zigzag::CellRef compileComparison(const ComparisonExpr &comp,
                                    zigzag::CellRef ctxCell);

  // Library / Executable structure emission
  void linkAsExecutable(zigzag::CellRef entryOp);
  void linkAsLibrary(zigzag::CellRef entryOp, std::string_view moduleName,
                     std::string_view symbolName);

  zigzag::vortex::VortexCore &core_;
  zigzag::vortex::VortexVM &vm_;

  zigzag::CellRef entryOp_{zigzag::noCell};
  zigzag::CellRef currentOp_{zigzag::noCell};
  std::vector<zigzag::CellRef> allOps_;
  std::vector<zigzag::CellRef> lastResultCells_;

  std::unordered_map<std::string, zigzag::CellRef> varBindings_;
};

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_COMPILER_HPP
