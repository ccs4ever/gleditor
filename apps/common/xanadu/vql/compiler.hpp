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
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include <gleditor/cpp26.hpp>

#include "common/xanadu/store.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/ast.hpp"
#include "common/xanadu/vql/dynamic_view_registry.hpp"
#include "common/xanadu/vql/frame_eligibility.hpp"
#include "common/xanadu/vql/io_match.hpp"
#include "common/xanadu/vql/limits.hpp"
#include "common/xanadu/vql/query_error.hpp"
#include "common/xanadu/vql/static_functor_program.hpp"

namespace xanadu {
class MultiStoreCoordinator;
}

namespace xanadu::vql {

/// The relations the outcome path functions compile to; the I/O script host
/// answers them (ScriptRunner), since the VM and the compiler name no I/O
/// schema. An outcome's payload is answered only where the shared terminal
/// status gate exposes one.
inline constexpr const char *outcomeTagRelation       = "io_outcome_tag";
inline constexpr const char *outcomePayloadRelation   = "io_outcome_payload";
inline constexpr const char *outcomeBytesRelation     = "io_outcome_bytes";
inline constexpr const char *outcomeErrorRelation     = "io_outcome_error";
inline constexpr const char *outcomeDetailRelation    = "io_outcome_detail";
inline constexpr const char *outcomeUncertainRelation = "io_outcome_uncertain";
/// Why a Partial outcome stopped (`io_cause`): the typed terminal cause.
inline constexpr const char *outcomeCauseRelation = "io_outcome_cause";
/// Which arm an action's handle is: 0 a request still to await, 1 an outcome
/// (a refusal or a finished request) that is already the value.
inline constexpr const char *handleArmRelation = "io_handle_arm";
/// The same, after asking the handle's request to stop.
inline constexpr const char *handleCancelRelation = "io_handle_cancel";
/// Admit an open of a path in a mode (IoOpenMode): answers the request, or
/// the Rejected outcome.
inline constexpr const char *openRelation = "io_open";
/// The capability an Complete open outcome minted, when it minted one.
inline constexpr const char *openCapabilityRelation = "io_open_capability";
/// Copy a finished read's outcome into a script-owned record and retire its
/// request, so a named outcome does not hold a service slot (M7-VQL-c §12.1).
/// Answers the record, or nothing when the host keeps the service's outcome.
inline constexpr const char *ownOutcomeRelation = "io_own_outcome";

/// Copy a finished outcome into a script-owned record and leave its request
/// admitted (`await($h) as $r`: the handle stays awaitable). Answers the
/// record, or nothing when the host keeps the service's outcome.
inline constexpr const char *ownCopyRelation = "io_own_copy";
/// The ordinal of the tag of the outcome a `match` is aimed at (the numbering
/// of `IoMatchTag`); a value that is not an outcome is the host's typed
/// refusal, never an arm.
inline constexpr const char *matchTagRelation = "io_match_tag";

/// A capture as three relations over one loop (design §6.2): begin prepares
/// the capture and requests its append (the second form carries an offset and
/// a length of the payload), arm answers whether the program awaits a request
/// (arm 0) or holds the value (arm 1), step takes the next step once the
/// awaited request ended. The host answers all of them; the VM and the compiler
/// name no capture schema.
inline constexpr const char *captureBeginRelation = "io_capture_begin";
inline constexpr const char *captureRangeRelation = "io_capture_range";
inline constexpr const char *captureArmRelation   = "io_capture_arm";
inline constexpr const char *captureStepRelation  = "io_capture_step";
/// The cells that are a receipt's captured bytes, one per translated piece.
inline constexpr const char *outcomeCapturedRelation = "io_outcome_captured";

/// The relation an outcome path function (`io_tag`, `io_payload`, `io_bytes`,
/// `io_error`, `io_detail`, `io_uncertain`) compiles to; nullptr for any other
/// name.
[[nodiscard]] constexpr const char *
outcomeFunctionRelation(const std::string_view name) noexcept {
  if (name == "io_tag") return outcomeTagRelation;
  if (name == "io_payload") return outcomePayloadRelation;
  if (name == "io_bytes") return outcomeBytesRelation;
  if (name == "io_error") return outcomeErrorRelation;
  if (name == "io_detail") return outcomeDetailRelation;
  if (name == "io_uncertain") return outcomeUncertainRelation;
  if (name == "io_cause") return outcomeCauseRelation;
  if (name == "io_captured") return outcomeCapturedRelation;
  return nullptr;
}

inline constexpr std::size_t maxCompiledShapePredicateRules  = 4096;
inline constexpr std::size_t maxCompiledShapeProjectionRules = 4096;

struct CompilationOptions {
  struct Limits {
    // Independent per-request ceilings for named frame variants, generated
    // opcodes, frame descriptor records and closed-functor template work. The
    // work ceiling counts template bindings, steps, dimension-name bytes and
    // typed literal-predicate keys; it does not cap all CPU or arena work.
    std::size_t maxNamedFrameVariants{1024};
    std::size_t maxGeneratedOpcodes{100'000};
    std::size_t maxFrameDescriptorRecords{100'000};
    std::size_t maxClosedFunctorTemplateWork{1'000'000};
    // Post-scope-analysis dependency expansion: callsites and callee/capture
    // name bytes. Initial AST/scope walks have their own traversal limits.
    std::size_t maxClosureCaptureWork{1'000'000};
    /// The longest text a `join` may build (see limits.hpp); a join past it
    /// is `VQL_IO_BOUND`.
    std::size_t maxJoinedTextBytes{defaultJoinedTextBytes};
  };

  /// A dimension a session has mounted: a walk along it that finds nothing
  /// continues from `roots` when the step's direction matches (or is both).
  /// Both the short and the `d.`-qualified name are keys, as the oracle's
  /// session table kept them.
  struct Mount {
    zigzag::DimVector direction{zigzag::DimVector::POS};
    std::vector<zigzag::CellRef> roots;
  };

  bool targetLibrary{false};            ///< Emit library store instead of exec
  std::string moduleName{"std:custom"}; ///< Module name for library store
  std::string symbolName{"query"};      ///< Exported symbol name
  bool optimize{false};                 ///< Enable optimizations
  std::vector<zigzag::CellRef> contextCells;
  std::unordered_map<std::string, std::vector<zigzag::CellRef>> initialBindings;
  std::unordered_map<std::string, std::vector<PathStep>> initialFunctors;
  /// The session's mounts, by dimension name; the compiler consults them for
  /// every dimension step and adds the query's own declarations in order.
  std::unordered_map<std::string, Mount> mounts;
  std::size_t maxContractEmissions{65536};
  Limits limits{};
  DynamicViewRegistry *dynamicViews{nullptr};
};

struct CompilationResult {
  struct SessionBinding {
    /// Mount and Unmount are session events in declaration order: `name` is
    /// the dimension, `direction` its direction, and for Mount `slots` are the
    /// root cells resolved while compiling (no run-time cells, no arena cells).
    enum class Kind : std::uint8_t { Stream, Functor, Mount, Unmount };
    std::string name;
    Kind kind{Kind::Stream};
    std::vector<zigzag::CellRef> slots;
    std::vector<PathStep> functor;
    zigzag::DimVector direction{zigzag::DimVector::POS};
  };
  bool success{true};
  std::optional<zigzag::CellRef> entryOpcode;
  std::string errorMessage;
  std::vector<zigzag::CellRef> generatedOpcodes;
  std::string disassembly;
  std::optional<zigzag::CellRef> terminalOpcode;
  std::optional<zigzag::CellRef> cursor;
  std::vector<zigzag::CellRef> resultSlots;
  std::vector<SessionBinding> sessionBindings;
  /// Set exactly when success is false.
  std::optional<QueryErrorCategory> category;
  /// The statement being compiled when a refusal was thrown, when known.
  std::optional<std::size_t> failingStatement;
  /// One lowered script I/O action: its Submit and the Await a parked cursor
  /// stops at, and the index of the statement that action is.
  struct IoActionSite {
    /// The action's first host-facing opcode (a Submit, or the RelationNext
    /// of a handle action), so a failure there names the statement.
    zigzag::CellRef submitOpcode{};
    /// Absent for a detached read, which never waits.
    std::optional<zigzag::CellRef> awaitOpcode;
    /// The slot the Await reads its request from.
    std::optional<zigzag::CellRef> requestSlot;
    std::size_t statement{};
  };
  std::vector<IoActionSite> ioActions;
  /// The last statement is a script I/O action, so its result is an outcome
  /// cell whose payload the host may append as text.
  bool finalIsIoEffect{false};
};

/// Compares two microversions by causal ancestry: `<` is strict ancestor and
/// `<=` ancestor-or-equal; siblings are unordered. Defined in
/// compile_history.cpp.
bool compareVersionsCausally(CompOp op, const xanadu::MicroversionId &lhs,
                             const xanadu::MicroversionId &rhs);

class VQLCompiler {
public:
  VQLCompiler(zigzag::vortex::VortexCore &core, zigzag::vortex::VortexVM &vm);
  VQLCompiler(MultiStoreCoordinator &coordinator, zigzag::vortex::VortexVM &vm);

  /// Compiles a parsed QueryExpression AST into Vortex bytecode.
  CompilationResult compile(const QueryExpression &query,
                            const CompilationOptions &options = {});

  /// Resolves only the mount and unmount declarations of @p query against
  /// the mount table in @p options and returns them as session events. Emits
  /// no operation and mints no cell; throws QueryRefusal like compile().
  [[nodiscard]] std::vector<CompilationResult::SessionBinding>
  declareMounts(const QueryExpression &query,
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

  /// Throws the explicit refusal for a semantic builtin (project, apply_shape,
  /// any, all, none, one, optional_one, first, same_cell) that has no Vortex
  /// graph lowering yet.
  [[noreturn]] static void refuseSemanticBuiltin(std::string_view name);

  /// Which lowering card owns a function name. The dispatch sites test these
  /// and call the matching stub below; each stub lives in its own file.
  [[nodiscard]] static bool isSemanticShapeBuiltin(std::string_view name);
  [[nodiscard]] static bool isSemanticQuantifierBuiltin(std::string_view name);
  [[nodiscard]] static bool isPathFunction(std::string_view name);
  /// lens and its UFCS sugar docs and files (Q5).
  [[nodiscard]] static bool isLensFunction(std::string_view name);
  /// Every name the language specifies but no lowering exists for yet, owned
  /// by a card or not. A refusal for one is Unsupported; any other unbound
  /// name is Unknown.
  [[nodiscard]] static bool isKnownUnloweredFunction(std::string_view name);

  /// Resolves a dimension name to its DimRef, minting if non-existent.
  zigzag::DimRef resolveDimension(std::string_view name);

  /// Exports the compiled ArenaManifold instructions and zzstructure to a
  /// Store.
  xanadu::MicroversionId exportToStore(
      xanadu::Store &store,
      std::optional<xanadu::MicroversionId> parent = std::nullopt) const;

private:
  CompilationResult failCompilation(const std::exception &refused);

  struct ClosedFunctorClampKey {
    std::int64_t start{1};
    std::optional<std::int64_t> end;

    bool operator==(const ClosedFunctorClampKey &) const = default;
  };
  using ClosedFunctorPredicateLiteral =
      std::variant<std::string, std::int64_t, bool, std::uint64_t>;
  struct ClosedFunctorPredicateFactorKey {
    bool negated{false};
    CompOp op{CompOp::Equal};
    bool literalOnLeft{false};
    ClosedFunctorPredicateLiteral literal;

    bool operator==(const ClosedFunctorPredicateFactorKey &) const = default;
  };
  struct ClosedFunctorPredicateTermKey {
    std::vector<ClosedFunctorPredicateFactorKey> factors;

    bool operator==(const ClosedFunctorPredicateTermKey &) const = default;
  };
  struct ClosedFunctorPredicateKey {
    std::vector<ClosedFunctorPredicateTermKey> terms;

    bool operator==(const ClosedFunctorPredicateKey &) const = default;
  };
  struct ClosedFunctorStepKey {
    std::string dimName;
    zigzag::DimVector direction{zigzag::DimVector::POS};
    bool isBidir{false};
    bool derefMaster{false};
    std::optional<ClosedFunctorClampKey> rangeClamp;
    std::vector<ClosedFunctorPredicateKey> predicates;

    bool operator==(const ClosedFunctorStepKey &) const = default;
  };
  struct ClosedFunctorFormalKey {
    std::size_t formalIndex{};
    std::vector<ClosedFunctorStepKey> steps;

    bool operator==(const ClosedFunctorFormalKey &) const = default;
  };
  struct FrameDefinitionKey {
    std::string name;
    // Present only for typed variadic frames. Zero means no rest actual groups;
    // nullopt identifies an ordinary or untyped-rest frame.
    std::optional<std::size_t> restGroupCount;
    std::vector<ClosedFunctorFormalKey> functorTemplates;
    std::vector<StaticFunctorProgramKey> staticFunctorPrograms;

    bool operator==(const FrameDefinitionKey &) const = default;
  };
  struct FrameDefinitionKeyHash {
    std::size_t operator()(const FrameDefinitionKey &key) const noexcept {
      const auto nameHash = std::hash<std::string>{}(key.name);
      const auto countHash =
          std::hash<std::optional<std::size_t>>{}(key.restGroupCount);
      std::size_t result = nameHash ^ countHash;
      const auto combine = [&result](const std::size_t value) {
        result ^=
            value + std::size_t{0x9e3779b9} + (result << 6) + (result >> 2);
      };
      combine(key.functorTemplates.size());
      for (const auto &formal : key.functorTemplates) {
        combine(std::hash<std::size_t>{}(formal.formalIndex));
        combine(formal.steps.size());
        for (const auto &step : formal.steps) {
          combine(std::hash<std::string>{}(step.dimName));
          combine(std::hash<zigzag::DimVector>{}(step.direction));
          combine(std::hash<bool>{}(step.isBidir));
          combine(std::hash<bool>{}(step.derefMaster));
          combine(step.rangeClamp.has_value());
          if (step.rangeClamp) {
            combine(std::hash<std::int64_t>{}(step.rangeClamp->start));
            combine(step.rangeClamp->end.has_value());
            if (step.rangeClamp->end)
              combine(std::hash<std::int64_t>{}(*step.rangeClamp->end));
          }
          combine(step.predicates.size());
          for (const auto &predicate : step.predicates) {
            combine(predicate.terms.size());
            for (const auto &term : predicate.terms) {
              combine(term.factors.size());
              for (const auto &factor : term.factors) {
                combine(std::hash<bool>{}(factor.negated));
                combine(std::hash<CompOp>{}(factor.op));
                combine(std::hash<bool>{}(factor.literalOnLeft));
                combine(factor.literal.index());
                std::visit(
                    [&combine](const auto &literal) {
                      combine(std::hash<std::decay_t<decltype(literal)>>{}(
                          literal));
                    },
                    factor.literal);
              }
            }
          }
        }
      }
      combine(key.staticFunctorPrograms.size());
      for (const auto &program : key.staticFunctorPrograms) {
        combine(std::hash<std::size_t>{}(program.formalIndex));
        combine(std::hash<std::string>{}(program.structuralIdentity));
      }
      return result;
    }
  };
  // Charge both modifiers on every step, including their default-false state.
  static constexpr std::size_t closedFunctorStepModifierWork = 2;

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
  functorArgument(const ValueExpr &argument);
  std::optional<std::vector<PathStep>>
  functorArgument(const PathExpression &path);
  [[nodiscard]] bool isFunctorArgument(const ValueExpr &argument) const;
  [[nodiscard]] std::optional<std::span<const PathStep>>
  closedFunctorArgumentSteps(const ValueExpr &argument) const;
  void chargeClosedFunctorTemplateWork(std::size_t amount);
  [[nodiscard]] static std::size_t closedFunctorClampWork(bool hasEnd) noexcept;
  [[nodiscard]] static std::size_t
  closedFunctorPredicateWork(const BooleanExpr &predicate) noexcept;
  [[nodiscard]] static std::size_t closedFunctorPredicateWork(
      const ClosedFunctorPredicateKey &predicate) noexcept;
  [[nodiscard]] std::size_t closedFunctorTemplateWork(
      std::span<const ClosedFunctorFormalKey> templates) const;
  [[nodiscard]] std::optional<ClosedFunctorFormalKey>
  normalizeClosedFunctorTemplate(std::span<const PathStep> steps,
                                 std::size_t formalIndex);

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
  zigzag::CellRef emitExistingTargetClone(zigzag::CellRef target);
  zigzag::CellRef emitRank(zigzag::CellRef anchor, zigzag::DimRef dimension,
                           zigzag::DimVector direction);
  std::vector<zigzag::CellRef>
  emitMap(std::span<const zigzag::CellRef> inputs,
          gleditor::cpp26::function_ref<
              std::vector<zigzag::CellRef>(zigzag::CellRef)>
              body);
  std::vector<zigzag::CellRef>
  compileSteps(std::span<const PathStep> steps,
               std::vector<zigzag::CellRef> stream, bool hasContext,
               bool allowEmptyCallFallback = false);
  std::vector<zigzag::CellRef> compileSelector(const PathStep &step,
                                               zigzag::CellRef current,
                                               bool hasContext);
  std::unordered_set<zigzag::CellRef> dynamicStreams_;
  /// Mounts in force while compiling: the session's, then the query's own.
  std::unordered_map<std::string, CompilationOptions::Mount> mounts_;
  /// The query's mount and unmount declarations as session events.
  std::vector<CompilationResult::SessionBinding> mountBindings_;
  /// Continues a dimension step's walk from its mount's roots when the walk
  /// found nothing, as a branch on the emitted stream.
  std::vector<zigzag::CellRef>
  emitMountFallback(const SignedDimensionStep &step,
                    std::vector<zigzag::CellRef> walked);
  /// Constant cells that hold a resolved microversion, and the version each
  /// holds. A `let` binds its variable to such a cell, so membership is how a
  /// variable is known to be version-valued (the oracle kept the same map in
  /// versionValues_); the let handling itself is not this file's to edit.
  std::unordered_map<zigzag::CellRef, xanadu::MicroversionId> versionVars_;
  /// Cells written by emitConstant and the session's bound cells: the only
  /// ones whose value is known while compiling.
  std::unordered_set<zigzag::CellRef> constantCells_;

  /// Steps that consume the whole incoming walk instead of one cell at a time
  /// (a per-cell emitMap cannot express a count or a quantifier). Called from
  /// compileSteps ahead of the per-cell dispatch, only for names that are not
  /// user definitions.
  [[nodiscard]] static bool isWholeWalkStep(const FunctionInvocation &fn);
  std::vector<zigzag::CellRef>
  compileWholeWalkStep(const FunctionInvocation &fn,
                       std::span<const zigzag::CellRef> stream,
                       bool hasContext);

  // Lowering stubs, one file per card. Each throws today's refusal until its
  // card fills it in. "Step" stubs are called from compileSelector with the
  // step, the one cell flowing through it and whether a context exists, and
  // return the next stream; "call" stubs are called where a function is used
  // as a value or argument and return the cells it produces.
  /// Q1, compile_path_functions.cpp: sort_into, find, contains, count.
  std::vector<zigzag::CellRef> compilePathFunctionStep(const PathStep &step,
                                                       zigzag::CellRef current,
                                                       bool hasContext);
  std::vector<zigzag::CellRef>
  compilePathFunctionCall(const FunctionInvocation &fn,
                          std::optional<zigzag::CellRef> context);
  std::optional<zigzag::CellRef>
  compilePathFunctionValue(const FunctionInvocation &fn,
                           zigzag::CellRef ctxCell);
  /// Q1, compile_path_functions.cpp: argument-free count() over the whole walk.
  std::vector<zigzag::CellRef>
  compilePathFunctionWalk(const FunctionInvocation &fn,
                          std::span<const zigzag::CellRef> stream);
  /// Q2, compile_root_selectors.cpp: RootSelectorStep, GenesisSelector,
  /// PerspectiveStep, MacroDimensionGroup.
  std::vector<zigzag::CellRef> compileRootSelectorStep(const PathStep &step,
                                                       zigzag::CellRef current,
                                                       bool hasContext);
  /// Q10, compile_path_morphism.cpp: `$p/+d.x => [ ... ]`, in or out of a
  /// perspective block.
  std::vector<zigzag::CellRef> compilePathMorphismStep(const PathStep &step,
                                                       zigzag::CellRef current,
                                                       bool hasContext);
  /// Q3, compile_history.cpp.
  std::vector<zigzag::CellRef>
  compileVersionPinnedStep(const PathStep &step,
                           std::vector<zigzag::CellRef> stream,
                           bool hasContext);
  std::optional<zigzag::CellRef>
  compileVersionPinnedPath(const PathExpression &path, zigzag::CellRef ctxCell);
  std::optional<zigzag::CellRef>
  compileHistoricalAnchor(const AnchorNode &anchor);
  std::optional<zigzag::CellRef> compileVersionValue(const VersionExpr &version,
                                                     zigzag::CellRef ctxCell);
  /// Q4, compile_watch.cpp.
  std::optional<zigzag::CellRef>
  compileWatchOrPerspectiveStatement(const Statement &statement);
  /// Lowers one script I/O action (compile_io_effect.cpp) to MakeTerm, Submit,
  /// a branch on the arm, and Await; the result is the outcome cell.
  std::optional<zigzag::CellRef>
  compileIoEffect(const IoEffectStatement &effect, std::size_t statement);
  /// Lowers a script `match` over an outcome: one relation answers the tag
  /// ordinal, a branch per arm selects, each arm's binders are the field
  /// relations of its tag, and the arm's row expression is the statement's
  /// value. Pure: no Submit, no Await.
  std::optional<zigzag::CellRef> compileIoMatch(const IoMatchStatement &match,
                                                std::size_t statement);
  /// One RelationNext call of the I/O script host: the single answer per
  /// argument is the host's value.
  struct IoRelated {
    zigzag::CellRef opcode;
    zigzag::CellRef found;
    std::vector<zigzag::CellRef> answers;
  };
  IoRelated ioRelate(const char *relation,
                     const std::vector<zigzag::CellRef> &arguments);
  /// The cell a script action's `$name` operand or source names. A name the
  /// compile has no binding for becomes a text cell naming it (inspectable
  /// in `vqueryc`, refused by every Submit). A name this script's own `let`
  /// bound is a stream, so it is narrowed to exactly one value or the script
  /// stops with `VQL_IO_EFFECT_ARGUMENT`.
  zigzag::CellRef ioBoundCell(const std::string &name);
  void compileMountDeclarations(const QueryExpression &query);
  /// Q5, compile_dynamic_view.cpp.
  void compileDynamicShapeProjection(const ShapeLiteral &shape);
  std::vector<zigzag::CellRef> compileLensStep(const PathStep &step,
                                               zigzag::CellRef current,
                                               bool hasContext);
  std::vector<zigzag::CellRef>
  compileLensCall(const FunctionInvocation &fn,
                  std::optional<zigzag::CellRef> context);
  std::optional<zigzag::CellRef> compileLensValue(const FunctionInvocation &fn,
                                                  zigzag::CellRef ctxCell);
  std::optional<zigzag::CellRef> compileShapeValue(const ShapeLiteral &shape,
                                                   zigzag::CellRef ctxCell);
  /// Q9, compile_field_weave.cpp.
  void compileFieldWeave(const FieldWeave &weave, zigzag::CellRef resultStream);
  /// S1, compile_semantic_shape.cpp: project, apply_shape.
  std::vector<zigzag::CellRef> compileSemanticShapeStep(const PathStep &step,
                                                        zigzag::CellRef current,
                                                        bool hasContext);
  std::vector<zigzag::CellRef>
  compileSemanticShapeCall(const FunctionInvocation &fn,
                           std::optional<zigzag::CellRef> context);
  std::optional<zigzag::CellRef>
  compileSemanticShapeValue(const FunctionInvocation &fn,
                            zigzag::CellRef ctxCell);
  /// S2, compile_semantic_quantifiers.cpp: any, all, none, one, optional_one,
  /// first, same_cell.
  std::vector<zigzag::CellRef>
  compileSemanticQuantifierStep(const PathStep &step, zigzag::CellRef current,
                                bool hasContext);
  std::vector<zigzag::CellRef>
  compileSemanticQuantifierCall(const FunctionInvocation &fn,
                                std::optional<zigzag::CellRef> context);
  std::optional<zigzag::CellRef>
  compileSemanticQuantifierValue(const FunctionInvocation &fn,
                                 zigzag::CellRef ctxCell);
  /// Argument-free any(), all(), none(), one(), optional_one(), first() over
  /// the whole incoming walk.
  std::vector<zigzag::CellRef>
  compileSemanticQuantifierWalk(const FunctionInvocation &fn,
                                std::span<const zigzag::CellRef> stream,
                                bool hasContext);

  // AST compilation subroutines
  std::optional<zigzag::CellRef> compileQuery(const QueryExpression &query);
  std::vector<zigzag::CellRef>
  compileCall(const DefDeclaration &definition,
              const std::vector<ValueExpr> &arguments,
              std::optional<zigzag::CellRef> context, bool ufcs,
              bool entersActiveCallStack = true);
  std::vector<zigzag::CellRef>
  compileClosureInvocation(const RuntimeClosureInvocation &invocation,
                           std::optional<zigzag::CellRef> context,
                           zigzag::vortex::ClosureCallMode mode);
  std::vector<zigzag::CellRef> compileReusableFrameCall(
      const DefDeclaration &definition, const std::vector<ValueExpr> &arguments,
      std::optional<zigzag::CellRef> context, bool ufcs,
      std::vector<ClosedFunctorFormalKey> functorTemplates,
      std::vector<StaticFunctorProgram> staticFunctorPrograms,
      std::vector<std::string> extraCaptureNames,
      std::vector<std::string> requiredProgramCaptureNames);
  void compilePendingFrameDefinitions();
  void compileFrameDefinition(const DefDeclaration &definition,
                              const FrameDefinitionKey &key);
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
      std::optional<FrameDefinitionKey> definitionKey = std::nullopt);
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
  std::unordered_set<std::string> readOnlyDefinitions_;
  std::unordered_map<std::string, std::vector<std::string>>
      frameRequiredDefaultCaptureBindings_;
  std::unordered_map<std::string, std::vector<std::string>>
      frameConditionalRequiredDefaultCaptureBindings_;
  std::unordered_map<std::string, std::vector<std::string>>
      frameOmittedRestRequiredDefaultCaptureBindings_;
  std::unordered_map<std::string, std::vector<std::size_t>>
      frameFunctorFormalIndices_;
  std::unordered_map<std::string, std::vector<FunctorApplicationScope>>
      frameFunctorApplicationScopes_;
  struct FrameDefinitionState {
    zigzag::CellRef descriptor;
    std::vector<ClosedFunctorFormalKey> functorTemplates;
    std::vector<StaticFunctorProgram> staticFunctorPrograms;
    std::vector<std::string> extraCaptureNames;
    std::vector<std::string> requiredCaptureNames;
    std::optional<std::size_t> functorVariantOrdinal;
    std::optional<zigzag::CellRef> entry;
    bool queued{false};
    bool compiled{false};
  };
  std::unordered_map<FrameDefinitionKey, FrameDefinitionState,
                     FrameDefinitionKeyHash>
      frameDefinitions_;
  std::unordered_set<std::string> reusableFrameDefinitions_;
  std::vector<FrameDefinitionKey> pendingFrameDefinitions_;
  std::unordered_set<zigzag::CellRef> frameMutableSlots_;
  bool compilingFrameDefinition_{false};
  std::string compilingFrameDefinitionName_;
  std::vector<std::string> activeCalls_;
  std::size_t maxNamedFrameVariants_{};
  std::size_t remainingClosedFunctorTemplateWork_{};
  std::size_t remainingClosureCaptureWork_{};
  std::size_t maxJoinedTextBytes_{};
  std::size_t nextFunctorVariantOrdinal_{};
  std::size_t nextClosureLambdaId_{};
  std::size_t remainingGeneratedOpcodes_{};
  std::size_t remainingFrameDescriptorRecords_{};
  std::optional<zigzag::CellRef> ambientContext_;
  std::vector<zigzag::CellRef> contextInputs_;
  std::unordered_map<std::string, std::vector<zigzag::CellRef>> streamBindings_;
  std::optional<zigzag::CellRef>
  compileExecutionBlock(const ExecutionBlock &block);
  std::optional<zigzag::CellRef>
  compilePathExpression(const PathExpression &path, zigzag::CellRef ctxCell);
  void validateShapeProjection(const ShapeLiteral &shape);
  void compileShapeProjection(const ShapeLiteral &shape, zigzag::CellRef owner);
  std::vector<zigzag::CellRef>
  applyShapeProjection(std::span<const zigzag::CellRef> owners,
                       const ShapeLiteral &shape);
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
  std::optional<zigzag::CellRef>
  compileCandidatePredicate(const BooleanExpr &expr, zigzag::CellRef candidate);
  std::optional<zigzag::CellRef> compileValueExpr(const ValueExpr &expr,
                                                  zigzag::CellRef ctxCell);
  zigzag::CellRef compileClosureLambda(const PathLambda &lambda);
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
  std::vector<CompilationResult::IoActionSite> ioActions_;
  std::optional<std::size_t> failingStatement_;
  bool finalIsIoEffect_{false};
  /// The names a `let` of this program bound, so a script action that reads
  /// one as a source, offset or length narrows it to one value.
  std::unordered_set<std::string> letNames_;

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
  DynamicViewRegistry *dynamicViews_{nullptr};
  DynamicViewRegistry fallbackDynamicViews_{};
  [[nodiscard]] DynamicViewRegistry &dynamicRegistry() noexcept {
    return dynamicViews_ != nullptr ? *dynamicViews_ : fallbackDynamicViews_;
  }
};

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_COMPILER_HPP
