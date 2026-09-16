/**
 * @file compiler.hpp
 * @brief VPL Bytecode Compiler translating VPL AST into Vortex bytecode.
 *
 * Compiles VPL array expressions into Vortex opcodes chained along +d.spin with
 * dual-wing parameter binding (+d.grab inputs, -d.grab outputs, chained on
 * +d.step). Supports emitting executable stores (home/d.spin ending in Halt)
 * and library stores (home/d.stdlib with exported symbols along
 * +d.vars/+d.values ending in Return).
 */
#ifndef COMMON_XANADU_VPL_COMPILER_HPP
#define COMMON_XANADU_VPL_COMPILER_HPP

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
#include "common/xanadu/vpl/ast.hpp"

namespace xanadu::vpl {

struct CompilationOptions {
  bool targetLibrary{false};            ///< Emit library store instead of exec
  std::string moduleName{"vpl:custom"}; ///< Module name for library store
  std::string symbolName{"main"};       ///< Exported symbol name
  bool optimize{false};                 ///< Enable optimizations
};

struct CompilationResult {
  bool success{true};
  zigzag::CellRef entryOpcode{zigzag::noCell};
  std::string errorMessage{};
  std::vector<zigzag::CellRef> generatedOpcodes;
  std::string disassembly{};
};

class VPLCompiler {
public:
  VPLCompiler(zigzag::vortex::VortexCore &core, zigzag::vortex::VortexVM &vm);

  /// Compiles a parsed Program AST into Vortex bytecode.
  CompilationResult compile(const Program &program,
                            const CompilationOptions &options = {});

  /// Compiles a single AST node into Vortex bytecode.
  CompilationResult compile(const AstNode &ast,
                            const CompilationOptions &options = {});

  /// Lexes, parses, and compiles a VPL source string into Vortex bytecode.
  CompilationResult compile(std::string_view source,
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

  /// Pretty-prints the AST hierarchy in human-readable ASCII tree format.
  [[nodiscard]] static std::string renderAST(const AstNode &node);

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
  zigzag::CellRef compileNode(const AstNode &node);
  zigzag::CellRef compileProgram(const Program &prog);
  zigzag::CellRef compileScalar(const ScalarExpr &expr);
  zigzag::CellRef compileVector(const VectorExpr &expr);
  zigzag::CellRef compileDimension(const DimensionExpr &expr);
  zigzag::CellRef compileIdentifier(const IdentifierExpr &expr);
  zigzag::CellRef compileVerb(const VerbExpr &expr);
  zigzag::CellRef compileMonadic(const MonadicExpr &expr);
  zigzag::CellRef compileDyadic(const DyadicExpr &expr);
  zigzag::CellRef compileAdverb(const AdverbExpr &expr);
  zigzag::CellRef compileConjunction(const ConjunctionExpr &expr);
  zigzag::CellRef compileAssign(const AssignExpr &expr);
  zigzag::CellRef compileIndexing(const IndexingExpr &expr);
  zigzag::CellRef compileQuad(const QuadExpr &expr);

  // Optimization helpers
  std::optional<zigzag::vortex::CellValue>
  tryFoldConstant(const AstNode &node) const;

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
  bool optimize_{false};
};

} // namespace xanadu::vpl

#endif // COMMON_XANADU_VPL_COMPILER_HPP
