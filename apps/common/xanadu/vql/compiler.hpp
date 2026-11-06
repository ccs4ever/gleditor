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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <gleditor/cpp26.hpp>

#include "common/xanadu/store.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/ast.hpp"

namespace xanadu {
class MultiStoreCoordinator;
}

namespace xanadu::vql {

inline constexpr std::size_t maxCompiledShapePredicateRules  = 4096;
inline constexpr std::size_t maxCompiledShapeProjectionRules = 4096;

struct CompilationOptions {
  bool targetLibrary{false};            ///< Emit library store instead of exec
  std::string moduleName{"std:custom"}; ///< Module name for library store
  std::string symbolName{"query"};      ///< Exported symbol name
  bool optimize{false};                 ///< Enable optimizations
  std::vector<zigzag::CellRef> contextCells;
  std::unordered_map<std::string, std::vector<zigzag::CellRef>> initialBindings;
  std::size_t maxContractEmissions{65536};
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
};

class VQLCompiler {
public:
  VQLCompiler(zigzag::vortex::VortexCore &core, zigzag::vortex::VortexVM &vm);
  VQLCompiler(MultiStoreCoordinator &coordinator, zigzag::vortex::VortexVM &vm);

  /// Compiles a parsed QueryExpression AST into Vortex bytecode.
  CompilationResult compile(const QueryExpression &query,
                            const CompilationOptions &options = {});

  /// Lexes, parses, and compiles a query string into Vortex bytecode.
  CompilationResult compile(std::string_view queryString,
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
  xanadu::MicroversionId exportToStore(
      xanadu::Store &store,
      std::optional<xanadu::MicroversionId> parent = std::nullopt) const;

private:
  // Emission helpers
  zigzag::CellRef mintOpcode(zigzag::vortex::OpcodeKind kind,
                             std::string_view label = {});
  zigzag::CellRef emitOp(zigzag::vortex::OpcodeKind kind,
                         std::string_view label = {});
  void emitInput(zigzag::CellRef op, zigzag::CellRef operand);
  void emitOutput(zigzag::CellRef op, zigzag::CellRef target);
  zigzag::CellRef
  emitConstant(const zigzag::vortex::CellValue &val,
               std::optional<zigzag::CellRef> designatedCell = std::nullopt);
  zigzag::CellRef emitInputConstant(zigzag::CellRef op,
                                    const zigzag::vortex::CellValue &val);

  std::optional<std::vector<PathStep>>
  functorArgument(const ValueExpr &argument) const;
  std::optional<std::vector<PathStep>>
  functorArgument(const PathExpression &path) const;

  void appendOpcode(zigzag::CellRef opcode);
  zigzag::CellRef emitStreamClear();
  void emitAppendStream(zigzag::CellRef output,
                        std::span<const zigzag::CellRef> inputs);
  std::vector<zigzag::CellRef>
  emitClamp(std::span<const zigzag::CellRef> inputs, const RangeClamp &clamp);
  void
  emitIteratorLoop(zigzag::CellRef iterator,
                   gleditor::cpp26::function_ref<void(zigzag::CellRef)> body);
  zigzag::CellRef emitReverse(std::span<const zigzag::CellRef> inputs);
  zigzag::CellRef emitDereference(zigzag::CellRef candidate);
  zigzag::CellRef emitExtreme(zigzag::CellRef anchor, zigzag::DimRef dimension,
                              zigzag::DimVector direction);
  zigzag::CellRef emitRank(zigzag::CellRef anchor, zigzag::DimRef dimension,
                           zigzag::DimVector direction);
  std::vector<zigzag::CellRef>
  emitMap(std::span<const zigzag::CellRef> inputs,
          gleditor::cpp26::function_ref<
              std::vector<zigzag::CellRef>(zigzag::CellRef)>
              body);
  std::vector<zigzag::CellRef> compileSteps(std::span<const PathStep> steps,
                                            std::vector<zigzag::CellRef> stream,
                                            bool hasContext);
  std::vector<zigzag::CellRef> compileSelector(const PathStep &step,
                                               zigzag::CellRef current,
                                               bool hasContext);
  std::unordered_set<zigzag::CellRef> dynamicStreams_;

  // AST compilation subroutines
  std::optional<zigzag::CellRef> compileQuery(const QueryExpression &query);
  std::vector<zigzag::CellRef>
  compileCall(const DefDeclaration &definition,
              const std::vector<ValueExpr> &arguments,
              std::optional<zigzag::CellRef> context, bool ufcs);
  std::vector<zigzag::CellRef>
  compileReusableFrameCall(const DefDeclaration &definition,
                           const std::vector<ValueExpr> &arguments,
                           std::optional<zigzag::CellRef> context, bool ufcs);
  void compilePendingFrameDefinitions();
  void compileFrameDefinition(const DefDeclaration &definition);
  class IsolatedCompileState;
  struct PreparedContractValidator {
    zigzag::CellRef descriptor;
    std::size_t id;
    std::vector<zigzag::CellRef> frozenStreams;
  };
  PreparedContractValidator
  prepareContractValidator(const TypeExpr &type,
                           std::span<const zigzag::CellRef> subjects);
  zigzag::CellRef
  invokeContractValidator(const PreparedContractValidator &prepared);
  zigzag::CellRef
  compileContractValidator(const TypeExpr &type,
                           std::span<const zigzag::CellRef> subjects);
  zigzag::CellRef openGuardScope(std::string_view label);
  void closeGuardScope(zigzag::CellRef token, std::string_view label);
  void pushContractGuard(const PreparedContractValidator &prepared,
                         std::string_view diagnostic, std::string_view label);
  zigzag::CellRef beginMutationGroup(std::string_view label);
  void endMutationGroup(zigzag::CellRef token, std::string_view label);
  void finalizeNativeFrame(
      zigzag::CellRef descriptor, zigzag::vortex::FrameDescriptorSpec &spec,
      std::optional<std::string_view> definitionName = std::nullopt);
  zigzag::CellRef emitNativeFrameReturn(zigzag::CellRef descriptor,
                                        std::string_view label);
  [[nodiscard]] zigzag::CellRef
  normalizeFrameStream(std::span<const zigzag::CellRef> cells);
  std::vector<zigzag::CellRef>
  compileArgument(const ValueExpr &argument,
                  std::optional<zigzag::CellRef> context);
  std::optional<zigzag::CellRef>
  compileBuiltinInvocation(const FunctionInvocation &invocation,
                           std::optional<zigzag::CellRef> context);
  std::vector<zigzag::CellRef> emitContractDefaultSlots(const TypeExpr &type);
  zigzag::CellRef emitContractMatch(const TypeExpr &type,
                                    std::span<const zigzag::CellRef> subjects,
                                    bool bindCaptures);
  zigzag::CellRef
  emitContractGraphMatch(const TypeExpr &type,
                         std::span<const zigzag::CellRef> subjects,
                         bool bindCaptures);
  void validateContractAliases(const TypeExpr &type);
  void emitRequiredContractMatch(const TypeExpr &type,
                                 std::span<const zigzag::CellRef> subjects,
                                 std::string_view functionName);
  void chargeContractEmission();
  zigzag::CellRef mintContractLabel(std::string_view label);
  class ContractGraphBackend;
  std::unordered_map<std::string, DefDeclaration> definitions_;
  std::unordered_map<std::string, std::vector<std::string>>
      frameFreeCaptureNames_;
  std::unordered_map<std::string, std::vector<std::string>>
      frameRequiredDefaultCaptureBindings_;
  struct FrameDefinitionState {
    zigzag::CellRef descriptor;
    std::optional<zigzag::CellRef> entry;
    bool queued{false};
    bool compiled{false};
  };
  std::unordered_map<std::string, FrameDefinitionState> frameDefinitions_;
  std::unordered_set<std::string> reusableFrameDefinitions_;
  std::vector<std::string> pendingFrameDefinitions_;
  std::unordered_set<zigzag::CellRef> frameMutableSlots_;
  bool compilingFrameDefinition_{false};
  std::string compilingFrameDefinitionName_;
  std::vector<std::string> activeCalls_;
  std::optional<zigzag::CellRef> ambientContext_;
  std::vector<zigzag::CellRef> contextInputs_;
  std::unordered_map<std::string, std::vector<zigzag::CellRef>> streamBindings_;
  std::optional<zigzag::CellRef>
  compileExecutionBlock(const ExecutionBlock &block);
  std::optional<zigzag::CellRef>
  compilePathExpression(const PathExpression &path, zigzag::CellRef ctxCell);
  void validateShapeProjection(const ShapeLiteral &shape);
  void compileShapeProjection(const ShapeLiteral &shape, zigzag::CellRef owner);
  std::optional<zigzag::CellRef> compileAnchor(const AnchorNode &anchor);
  std::optional<zigzag::CellRef> compilePathStep(const PathStep &step,
                                                 zigzag::CellRef inStreamCell);
  zigzag::CellRef emitFirstCell(zigzag::CellRef input);
  zigzag::CellRef emitCellPresence(zigzag::CellRef candidate);
  zigzag::CellRef
  emitContextFallback(zigzag::CellRef candidate,
                      std::optional<zigzag::CellRef> previousContext);
  zigzag::CellRef emitStreamPresence(std::span<const zigzag::CellRef> inputs);
  std::optional<zigzag::CellRef> compileBooleanExpr(const BooleanExpr &expr,
                                                    zigzag::CellRef ctxCell);
  std::optional<zigzag::CellRef> compileValueExpr(const ValueExpr &expr,
                                                  zigzag::CellRef ctxCell);
  std::optional<zigzag::CellRef> compileComparison(const ComparisonExpr &comp,
                                                   zigzag::CellRef ctxCell);
  zigzag::CellRef compileShapePredicate(const ShapeLiteral &shape,
                                        zigzag::CellRef candidate);

  // Library / Executable structure emission
  zigzag::CellRef linkAsExecutable(zigzag::CellRef entryOp);
  void linkAsLibrary(zigzag::CellRef entryOp, std::string_view moduleName,
                     std::string_view symbolName);

  zigzag::vortex::VortexCore &core_;
  zigzag::vortex::VortexVM &vm_;
  gleditor::cpp26::optional<MultiStoreCoordinator &> coordinator_;

  std::optional<zigzag::CellRef> entryOp_;
  std::optional<zigzag::CellRef> currentOp_;
  std::vector<zigzag::CellRef> allOps_;
  std::vector<zigzag::CellRef> lastResultCells_;

  std::unordered_map<std::string, std::optional<zigzag::CellRef>> varBindings_;
  std::unordered_map<std::string, std::vector<PathStep>> functorBindings_;
  std::size_t remainingShapeProjectionRules_{maxCompiledShapeProjectionRules};
  std::size_t remainingShapePredicateRules_{maxCompiledShapePredicateRules};
  std::size_t remainingShapePredicateValidationNodes_{
      maxCompiledShapePredicateRules};
  std::size_t remainingContractEmissions_{};
  std::size_t nextContractValidatorId_{};
  bool compilingGraphContract_{false};
  std::unordered_map<const ValueExpr *, zigzag::CellRef> shapeTargetConstants_;
  zigzag::CellRef compileShapePredicateImpl(const ShapeLiteral &shape,
                                            zigzag::CellRef candidate);
};

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_COMPILER_HPP
