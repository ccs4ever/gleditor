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
#include <unordered_set>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vpl/ast.hpp"
#include "common/xanadu/vpl/view.hpp"

namespace xanadu::vpl {

struct CompilationOptions {
  bool targetLibrary{false};            ///< Emit library store instead of exec
  std::string moduleName{"vpl:custom"}; ///< Module name for library store
  std::string symbolName{"main"};       ///< Exported symbol name
  bool optimize{false};                 ///< Enable optimizations
  std::unordered_map<std::string, VplView> initialBindings;
};

struct CompiledBinding {
  zigzag::CellRef cell;
  std::vector<zigzag::DirectedDim> axes;
  /// Which axes are read in reverse (card V4b); empty when none is.
  AxisReversed reversed;
  std::vector<zigzag::CellRef> dependencies;
  /// The bound value is an enclosed array: a box cell whose content hangs off
  /// `d.enclosure`. Valence stays `axes.size()`; enclosure is static because a
  /// cell cannot carry the flag at run time (card V10).
  bool enclosed{false};
  /// Set when the bound value is a dimension (or a vector of dimensions): such
  /// a value is pure compile-time structure and has no cells (card V4).
  std::optional<std::vector<zigzag::DirectedDim>> dimensionValues{};
};

struct CompilationResult {
  bool success{true};
  std::optional<zigzag::CellRef> entryOpcode;
  std::string errorMessage;
  std::vector<zigzag::CellRef> generatedOpcodes;
  std::string disassembly;
  std::optional<zigzag::CellRef> terminalOpcode;
  std::optional<zigzag::CellRef> cursor;
  std::vector<zigzag::CellRef> resultSlots;
  std::vector<zigzag::DirectedDim> resultAxes;
  /// The result's reversed axes (see CompiledBinding).
  AxisReversed resultReversed;
  std::vector<zigzag::CellRef> resultDependencies;
  std::unordered_map<zigzag::CellRef, std::vector<zigzag::CellRef>>
      cellDependencies;
  std::unordered_map<std::string, CompiledBinding> assignedBindings;
  /// The final result is an enclosed array (see CompiledBinding::enclosed).
  bool resultEnclosed{false};
  /// A `⎕ x` print marker was compiled: the host prints the final result after
  /// the run. The VM itself performs no I/O (card V13).
  bool resultPrint{false};
  /// Set when the final result is a dimension or a vector of dimensions.
  std::optional<std::vector<zigzag::DirectedDim>> resultDimensionValues{};
};

class LowerContext;

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
  disassemble(std::optional<zigzag::CellRef> entryOpcode,
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
  /// The per-card lowering files (lower_*.cpp, graph_emit.cpp) reach the
  /// emission helpers and the static side tables only through LowerContext
  /// (lower_decls.hpp), so a card never edits this header.
  friend class LowerContext;

  // Emission helpers
  zigzag::CellRef mintOpcode(zigzag::vortex::OpcodeKind kind,
                             std::string_view label = {});
  void appendOpcode(zigzag::CellRef op);
  void recordResultFacts(zigzag::CellRef cell);
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
  std::optional<zigzag::CellRef> compileNode(const AstNode &node);
  std::optional<zigzag::CellRef> compileProgram(const Program &prog);
  zigzag::CellRef compileScalar(const ScalarExpr &expr);
  std::optional<zigzag::CellRef> compileVector(const VectorExpr &expr);
  zigzag::CellRef compileDimension(const DimensionExpr &expr);
  std::optional<zigzag::CellRef> compileIdentifier(const IdentifierExpr &expr);
  zigzag::CellRef compileVerb(const VerbExpr &expr);
  std::optional<zigzag::CellRef> compileMonadic(const MonadicExpr &expr);
  zigzag::CellRef compileDyadic(const DyadicExpr &expr);
  zigzag::CellRef compileAdverb(const AdverbExpr &expr);
  zigzag::CellRef compileConjunction(const ConjunctionExpr &expr);
  std::optional<zigzag::CellRef> compileAssign(const AssignExpr &expr);
  std::optional<zigzag::CellRef> compileIndexing(const IndexingExpr &expr);
  std::optional<zigzag::CellRef> compileQuad(const QuadExpr &expr);

  // Optimization helpers
  std::optional<zigzag::vortex::CellValue>
  tryFoldConstant(const AstNode &node) const;

  // Library / Executable structure emission
  zigzag::CellRef linkAsExecutable(zigzag::CellRef entryOp);
  void linkAsLibrary(zigzag::CellRef entryOp, std::string_view moduleName,
                     std::string_view symbolName);

  zigzag::vortex::VortexCore &core_;
  zigzag::vortex::VortexVM &vm_;

  std::optional<zigzag::CellRef> entryOp_;
  std::optional<zigzag::CellRef> currentOp_;
  std::vector<zigzag::CellRef> allOps_;
  std::vector<zigzag::CellRef> lastResultCells_;
  std::vector<zigzag::DirectedDim> lastResultAxes_;
  AxisReversed lastResultReversed_;
  std::vector<zigzag::CellRef> lastResultDependencies_;

  std::unordered_map<std::string, std::optional<zigzag::CellRef>> varBindings_;
  std::unordered_map<std::string, std::vector<zigzag::DirectedDim>> varAxes_;
  std::unordered_map<std::string, AxisReversed> varReversed_;
  std::unordered_map<std::string, CompiledBinding> assignedBindings_;
  std::unordered_map<zigzag::CellRef, std::vector<zigzag::DirectedDim>>
      rankAxes_;
  /// Reversed axes of a view, keyed like rankAxes_; no entry means none is.
  std::unordered_map<zigzag::CellRef, AxisReversed> rankReversed_;
  std::unordered_map<zigzag::CellRef, std::vector<zigzag::CellRef>>
      rankDependencies_;
  std::unordered_map<zigzag::CellRef, std::vector<zigzag::CellRef>>
      cellDependencies_;
  /// Static array facts, keyed like rankAxes_ by the cell a value lives in.
  /// Valence is rankAxes_.at(cell).size(); these two cover what axes cannot.
  std::unordered_set<zigzag::CellRef> enclosedCells_;
  std::unordered_map<zigzag::CellRef, std::vector<zigzag::DirectedDim>>
      dimensionValues_;
  std::unordered_set<std::string> varEnclosed_;
  std::unordered_map<std::string, std::vector<zigzag::DirectedDim>>
      varDimensionValues_;
  bool printRequested_{false};
  bool lastResultEnclosed_{false};
  std::optional<std::vector<zigzag::DirectedDim>> lastResultDimensionValues_;
  bool optimize_{false};

  [[nodiscard]] std::vector<zigzag::CellRef>
  dependenciesOf(zigzag::CellRef cell) const;
  [[nodiscard]] std::vector<zigzag::CellRef>
  rankDependenciesOf(zigzag::CellRef cell) const;
};

} // namespace xanadu::vpl

#endif // COMMON_XANADU_VPL_COMPILER_HPP
