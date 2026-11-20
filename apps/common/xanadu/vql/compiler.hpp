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
#include <deque>
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
#include "common/xanadu/vql/frame_eligibility.hpp"
#include "common/xanadu/vql/query_error.hpp"
#include "common/xanadu/vql/shape_cells.hpp"
#include "common/xanadu/vql/shape_limits.hpp"
#include "common/xanadu/vql/static_functor_program.hpp"
#include "common/xanadu/vql/view_cells.hpp"

namespace xanadu {
class MultiStoreCoordinator;
}

namespace xanadu::vql {

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
    /// Rule-count and nesting-depth ceilings of a shape; system://settings
    /// holds the values a host passes (see shape_limits.hpp).
    ShapeLimits shape{};
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
  /// The shape handles earlier queries of the session left, handle cell to
  /// expanded shape, so a shape bound once can be projected by a later query.
  CompiledHandleTable initialShapes;
  /// The session's mounts, by dimension name; the compiler consults them for
  /// every dimension step and adds the query's own declarations in order.
  std::unordered_map<std::string, Mount> mounts;
  std::size_t maxContractEmissions{65536};
  Limits limits{};
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
  /// The compile-time shape table at the end of the query: `initialShapes`
  /// plus the handles this query minted. Cells here are program cells until
  /// the executor settles the program, which may re-mint the live ones.
  CompiledHandleTable shapes;
  /// Set exactly when success is false.
  std::optional<QueryErrorCategory> category;
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
    // Named static frames specialize typed rest by supplied group count.
    std::optional<std::size_t> restGroupCount;
    // Runtime closures keep one Groups port and loop over any tail length.
    bool runtimeRestGroups{false};
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
      result ^= std::hash<bool>{}(key.runtimeRestGroups) +
                std::size_t{0x9e3779b9} + (result << 6) + (result >> 2);
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
  std::vector<zigzag::CellRef> compileSteps(std::span<const PathStep> steps,
                                            std::vector<zigzag::CellRef> stream,
                                            bool hasContext,
                                            bool allowInitialEmptyCall = false);
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
  /// find(needle): a RelationNext scan over the host's find provider, one
  /// stream of hits; an absent needle answers an empty stream.
  std::vector<zigzag::CellRef>
  compileFind(const FunctionInvocation &fn,
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
                           std::vector<zigzag::CellRef> stream, bool hasContext,
                           bool allowInitialEmptyCall = false);
  std::optional<zigzag::CellRef>
  compileVersionPinnedPath(const PathExpression &path, zigzag::CellRef ctxCell);
  /// Refuses a watch baseline no attached store has: a pin at such a version
  /// answers nothing, but a baseline that is an empty snapshot would report
  /// every live cell as new.
  void requireWatchBaseline(const VersionExpr &since);
  std::optional<zigzag::CellRef>
  compileHistoricalAnchor(const AnchorNode &anchor);
  std::optional<zigzag::CellRef> compileVersionValue(const VersionExpr &version,
                                                     zigzag::CellRef ctxCell);
  /// Q4, compile_watch.cpp.
  std::optional<zigzag::CellRef>
  compileWatchOrPerspectiveStatement(const Statement &statement);
  void compileMountDeclarations(const QueryExpression &query);
  /// Q5, compile_dynamic_view.cpp.
  void compileDynamicShapeProjection(const ShapeLiteral &shape);
  /// V12, compile_dynamic_view.cpp and compile_view_steps.cpp. A dynamic view
  /// is data (view_cells.hpp): a view cell holding its rules, and a handle per
  /// source naming the source and the view in two Int64 boxes. A step on a
  /// stream that may hold handles is a guarded fold over those cells in the
  /// graph, so the view need not be known while compiling: nothing about it is
  /// written to the store, and the source is never given a link.
  static bool isDynamicViewShape(const ShapeLiteral &shape);
  std::vector<zigzag::CellRef>
  lowerDynamicView(const ShapeLiteral &shape,
                   std::span<const zigzag::CellRef> owners);
  /// What a table-lookup site is looking at. Every site that asks the table
  /// "what is this cell?" asks this instead, so a view handle (which the
  /// table does not hold) is never taken for a plain cell.
  enum class HandleKind : std::uint8_t {
    None,
    Shape,
    /// Carries the shape marker but is not in the table: cleared, or made by
    /// another session or engine. Its shape is unknown, so it refuses.
    UnrecordedShape,
    Lens,
    ViewHandle
  };
  [[nodiscard]] HandleKind handleKindOf(zigzag::CellRef cell) const;
  /// Refuses, by name, a handle used where the graph would compute with a
  /// value; @p op is the operator.
  void refuseHandleOperand(HandleKind kind, std::string_view op) const;

  /// A marked shape handle may be read only where a shape is consumed (a shape
  /// rule or target, a predicate, any/all/none, project/apply_shape, a call
  /// argument or let that passes it on, a top-level return). Every other read
  /// of a variable that holds one refuses through refuseShapeCells with this
  /// one message family. A consumer says so for the very next read only:
  /// compileArgument and compilePathExpression take the flag on entry, so
  /// nothing nested inherits it.
  struct ShapeUse {
    bool consumes{false};
    std::string_view use{"a value here"};
  };
  ShapeUse shapeUse_{};
  ShapeUse takeShapeUse() {
    const ShapeUse taken = shapeUse_;
    shapeUse_            = {};
    return taken;
  }
  struct ShapeUseScope {
    ShapeUseScope(VQLCompiler &compiler, const bool consumes,
                  const std::string_view use)
        : compiler_(compiler), saved_(compiler.shapeUse_) {
      compiler.shapeUse_ = {consumes, use};
    }
    ~ShapeUseScope() { compiler_.shapeUse_ = saved_; }
    ShapeUseScope(const ShapeUseScope &)            = delete;
    ShapeUseScope &operator=(const ShapeUseScope &) = delete;

  private:
    VQLCompiler &compiler_;
    ShapeUse saved_;
  };
  void refuseShapeCells(std::span<const zigzag::CellRef> cells,
                        std::string_view use) const;
  /// True for `$name` alone, as a value or as a path with nothing after it.
  static bool namesBareVariable(const ValueExpr &expression);
  static bool namesBareVariable(const PathExpression &path);
  /// Refuses a bare shape variable among the factors of @p condition, where no
  /// candidate exists for it to be tested against.
  void refuseShapeConditions(const BooleanExpr &condition,
                             std::string_view use) const;
  /// The view dimensions when the core names them; absent while no view was
  /// ever made, which means no handle exists and no step needs a guard.
  [[nodiscard]] std::optional<ViewDimensions> viewDimensions() const;
  /// Refuses a computed rule whose expression is not a literal or a plain
  /// path from the source: anything else would be compiled at the use site's
  /// bindings, not the binding scope's.
  static void requireStaticComputedRule(const VirtualDimensionDefRule &rule);
  /// Lowers one traversal of a computed dimension: the expression evaluated
  /// against each source cell, copied to a fresh cell as the oracle did.
  std::vector<zigzag::CellRef>
  compileComputedDimension(const VirtualDimensionDefRule &rule,
                           std::span<const zigzag::CellRef> sources);
  /// Why @p rest may not follow a computed result, or nothing when it may: its
  /// result is a fresh cell, so only value() or count() may follow.
  static std::optional<std::string>
  computedResultEndsRefusal(const VirtualDimensionDefRule &rule,
                            std::span<const PathStep> rest);
  /// Why a step cannot be taken through a handle, or nothing when it can:
  /// only dimension steps, value() and count() follow a view.
  [[nodiscard]] std::optional<std::string>
  viewStepRefusal(const PathStep &step) const;
  /// Whether a step is one the guarded fold takes (a dimension step, or
  /// value()/count() without arguments); any other step refuses handles.
  [[nodiscard]] bool isViewFollowableStep(const PathStep &step) const;
  /// Refuses, for a handle known while compiling, and otherwise at run time,
  /// a view handle used where its provenance would be lost.
  void refuseViewHandles(std::span<const zigzag::CellRef> cells,
                         const std::string &where) const;
  void refuseViewHandleArgument(const ValueExpr &argument) const;
  [[nodiscard]] static std::string viewHandleRefusal(const std::string &where);
  /// Run-time half of refuseViewHandles for cells the compiler cannot see.
  void rejectViewHandlesAtRunTime(std::span<const zigzag::CellRef> cells,
                                  const std::string &message);
  /// One step of @p stream through the view guard: a plain cell is stepped as
  /// ever, a handle by the fold of its view's rules over its source. Every
  /// cell a handle reaches is wrapped in a handle of the same view.
  std::vector<zigzag::CellRef>
  emitViewStep(const PathStep &step, std::span<const zigzag::CellRef> stream,
               bool hasContext, std::span<const PathStep> rest,
               const ViewDimensions &dims);
  /// A fresh handle over the cell @p source holds, of the view whose number is
  /// the raw Int64 @p viewNumber; returns the slot that holds the handle.
  zigzag::CellRef emitViewHandle(zigzag::CellRef source,
                                 zigzag::CellRef viewNumber,
                                 const ViewDimensions &dims);
  zigzag::CellRef emitLinkRead(zigzag::CellRef from, zigzag::DimRef dimension);
  zigzag::CellRef emitBinary(zigzag::vortex::OpcodeKind kind,
                             zigzag::CellRef left, zigzag::CellRef right,
                             std::string_view label);
  void emitRequire(zigzag::CellRef condition, const std::string &message);
  void emitJump(zigzag::CellRef target);
  /// What emitted steps do to a dimension: a number the program knows or a
  /// slot the graph computes (a view's pivot target, read from its rule).
  struct DimOperand {
    std::optional<zigzag::DimRef> fixed;
    zigzag::CellRef slot{};
    static DimOperand constant(zigzag::DimRef dimension) {
      return DimOperand{.fixed = dimension, .slot = {}};
    }
    static DimOperand fromSlot(zigzag::CellRef cell) {
      return DimOperand{.fixed = std::nullopt, .slot = cell};
    }
  };
  void bindDimension(zigzag::CellRef op, const DimOperand &dimension);
  zigzag::CellRef emitExtreme(zigzag::CellRef anchor,
                              const DimOperand &dimension,
                              zigzag::DimVector direction);
  void emitRefuseCycle(zigzag::CellRef extreme, const DimOperand &dimension,
                       zigzag::DimVector direction, std::string_view what);
  /// The creation chain of a `+d.x%...` step: one fresh cell per value at the
  /// end of the rank, each in a mutation group of its own.
  std::vector<zigzag::CellRef>
  compileDimensionCreation(zigzag::CellRef current,
                           const SignedDimensionStep &step,
                           const DimOperand &dimension);
  /// Thrown while compiling when a view is made after a step was compiled
  /// without its guard (the view dimensions did not exist yet); the compile
  /// then restarts with them named.
  struct ViewDimensionsNeededEarlier {};
  CompilationResult compileOnce(const QueryExpression &query,
                                const CompilationOptions &options,
                                bool nameViewDimensions);
  std::unordered_set<zigzag::CellRef> sessionCells_;
  /// Whether steps of the path being compiled may meet a handle: its anchor is
  /// a variable or a context, or a projection made it.
  bool guardSteps_{false};
  bool viewGuardsSkipped_{false};
  mutable std::optional<ViewDimensions> viewDimensions_;
  /// The three subroutines a view step calls, and the slots it passes its
  /// arguments and reads its answers in. Built once per compile.
  struct ViewRoutines {
    zigzag::CellRef resolveEntry{}, resolveHandle{}, resolveSource{},
        resolveView{}, resolveNumber{};
    zigzag::CellRef wrapEntry{}, wrapSource{}, wrapNumber{}, wrapHandle{};
    zigzag::CellRef foldEntry{}, foldView{}, foldName{}, foldDirection{},
        foldMint{}, foldAllowed{}, foldPhysical{}, foldComputed{};
  };
  std::optional<ViewRoutines> viewRoutines_;
  const ViewRoutines &viewRoutines(const ViewDimensions &dims);
  void emitCallRoutine(zigzag::CellRef entry);
  void emitWrite(zigzag::CellRef slot, const zigzag::vortex::CellValue &value);
  zigzag::CellRef emitRawCopy(zigzag::CellRef value);
  zigzag::CellRef emitReferenceCopy(zigzag::CellRef reference);
  std::vector<zigzag::CellRef> compileLensStep(const PathStep &step,
                                               zigzag::CellRef current,
                                               bool hasContext);
  std::vector<zigzag::CellRef>
  compileLensCall(const FunctionInvocation &fn,
                  std::optional<zigzag::CellRef> context);
  std::optional<zigzag::CellRef> compileLensValue(const FunctionInvocation &fn,
                                                  zigzag::CellRef ctxCell);
  /// Q-LENS. A lens is a pair of callback lambdas the compiler keeps under a
  /// handle cell (the shape table); applying `$source/$lens` compiles get, and
  /// `$source/$lens%value` compiles put, at that step.
  zigzag::CellRef registerLens(LensPair pair);
  /// The lens variable @p name is bound to, if it is bound to one.
  std::optional<LensPair> lensBoundTo(const std::string &name) const;
  std::vector<zigzag::CellRef>
  compileLensApplication(const SignedDimensionStep &step, const LensPair &lens,
                         zigzag::CellRef source, bool hasContext);
  std::optional<zigzag::CellRef> compileShapeValue(const ShapeLiteral &shape,
                                                   zigzag::CellRef ctxCell);
  /// Q-SHAPEVAR, compile_shape_value.cpp. A shape is syntax: it is expanded
  /// (variables, shape-returning calls and nested literals concatenated, right
  /// most rule last) while compiling and handed to the static projection
  /// paths. `registerShape` gives an expanded shape a handle cell.
  /// A shape value is a handle that heads its rules as cells (shape_cells.hpp).
  /// A Binding is the handle a shape function's parameter is bound to while the
  /// body expands: it is made from a shape the caller already holds, so no
  /// target is evaluated for it.
  enum class ShapeMint : std::uint8_t { Value, Binding };
  zigzag::CellRef registerShape(ShapeLiteral expanded,
                                ShapeMint mint = ShapeMint::Value);
  /// compile_shape_cells.cpp. Writes @p expanded's rules under @p shape as
  /// cells; @p rulesLeft is the rule budget the whole tree shares.
  void fillShapeCells(zigzag::CellRef shape, const ShapeLiteral &expanded,
                      const ShapeDimensions &dims, ShapeMint mint,
                      std::size_t depth, std::size_t &rulesLeft);
  /// Captures one rule's target onto @p rule as the data its kind names; see
  /// shape_cells.hpp for the four forms.
  /// True when the target is a nested shape.
  bool captureShapeTarget(zigzag::CellRef rule, const DimensionShapeRule &spec,
                          const ShapeDimensions &dims, ShapeMint mint,
                          std::size_t depth, std::size_t &rulesLeft);
  /// Emits the graph that evaluates a computed target once and writes a fresh
  /// scalar copy (or the void marker) onto @p rule.
  void emitShapeTargetCapture(zigzag::CellRef rule,
                              const DimensionShapeRule &spec,
                              const ShapeDimensions &dims);
  void appendShapeRules(ShapeLiteral &result, const ShapeLiteral &from);
  ShapeLiteral shapeOfVariable(const std::string &name);
  /// The rules a marked handle stands for; refuses a handle with no record.
  const ShapeLiteral &shapeOfHandle(zigzag::CellRef cell,
                                    const std::string &name);
  ShapeLiteral expandShape(const ShapeLiteral &shape);
  ShapeLiteral staticShapeOfCall(const FunctionInvocation &call);
  /// A literal, a variable or a call of a shape-returning definition as a
  /// shape; nullopt when @p expression is none of those.
  std::optional<ShapeLiteral> resolveShapeValue(const ValueExpr &expression);
  /// The handle of a call to a definition whose body is a shape literal;
  /// nullopt when @p call names no such definition.
  std::optional<zigzag::CellRef>
  compileShapeCall(const FunctionInvocation &call);
  /// Q9, compile_field_weave.cpp.
  void compileFieldWeave(const FieldWeave &weave, zigzag::CellRef resultStream);
  /// S1, compile_semantic_shape.cpp: project, apply_shape.
  std::vector<zigzag::CellRef> compileSemanticShapeStep(const PathStep &step,
                                                        zigzag::CellRef current,
                                                        bool hasContext);
  std::vector<zigzag::CellRef>
  compileSemanticShapeCall(const FunctionInvocation &fn,
                           std::optional<zigzag::CellRef> context);
  std::vector<zigzag::CellRef>
  compileProject(const FunctionInvocation &fn, const ShapeLiteral &shape,
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
  /// How many iterator or quantifier bodies the code being emitted is inside:
  /// a cell the compiler mints once is written once per turn there.
  std::size_t loopDepth_{0};
  /// Shape handles this compile made with a rule marked d.vql.late, and
  /// whether the one being filled has one. A late handle is read by the
  /// consumers of this query only (shapeOfHandle).
  std::unordered_set<zigzag::CellRef> lateHandles_;
  bool lateTargetMade_{false};
  struct LoopScope {
    explicit LoopScope(VQLCompiler &compiler) : compiler_(compiler) {
      ++compiler_.loopDepth_;
    }
    ~LoopScope() { --compiler_.loopDepth_; }
    LoopScope(const LoopScope &)            = delete;
    LoopScope &operator=(const LoopScope &) = delete;

  private:
    VQLCompiler &compiler_;
  };
  std::string compilingFrameDefinitionName_;
  std::vector<std::string> activeCalls_;
  std::size_t maxNamedFrameVariants_{};
  std::size_t remainingClosedFunctorTemplateWork_{};
  std::size_t remainingClosureCaptureWork_{};
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
  /// @p allowViews is true only where a path projects its own result: a
  /// projection with pivot or computed rules answers view handles there, and
  /// is refused everywhere a projection is a mutation (project, apply_shape).
  std::vector<zigzag::CellRef>
  applyShapeProjection(std::span<const zigzag::CellRef> owners,
                       const ShapeLiteral &shape, bool allowViews = false);
  std::optional<zigzag::CellRef> compileAnchor(const AnchorNode &anchor);
  std::optional<zigzag::CellRef> compilePathStep(const PathStep &step,
                                                 zigzag::CellRef inStreamCell);
  zigzag::CellRef emitFirstCell(zigzag::CellRef input);
  zigzag::CellRef emitCellPresence(zigzag::CellRef candidate);
  /// Fails the program, naming @p what, when @p extreme still has a neighbour
  /// on @p dimension in @p direction. A rank walk stops at the first repeated
  /// cell, so the "last" cell of a cycle still links onwards; attaching there
  /// would splice into the loop and break the links already in it.
  void emitRefuseCycle(zigzag::CellRef extreme, zigzag::DimRef dimension,
                       zigzag::DimVector direction, std::string_view what);
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
  zigzag::CellRef
  compileCartesianComparison(zigzag::vortex::VqlComparisonKind comparison,
                             std::span<const zigzag::CellRef> left,
                             std::span<const zigzag::CellRef> right);
  zigzag::CellRef compileShapePredicate(const ShapeLiteral &shape,
                                        zigzag::CellRef candidate);
  /// Q-SHAPEPRED, compile_shape_predicate.cpp. A predicate that is a shape
  /// held by a variable, or returned by a call, lowers as the literal it
  /// stands for; nullopt when @p expression is neither (an ordinary value, a
  /// functor, a variable bound to no handle), which the caller keeps handling
  /// as before.
  std::optional<zigzag::CellRef>
  compileShapePredicateValue(const ValueExpr &expression,
                             zigzag::CellRef candidate);
  /// @p shape with every variable, call and nested-literal rule (also inside
  /// targets) replaced by the rules it names. Returns @p shape itself when it
  /// names nothing, so a plain literal compiles exactly as before.
  /// The predicate of a compiled cell that turns out to be a shape handle;
  /// nullopt for any other cell.
  std::optional<zigzag::CellRef>
  shapePredicateOfCell(zigzag::CellRef value, zigzag::CellRef candidate);
  const ShapeLiteral &resolvedPredicateShape(const ShapeLiteral &shape);
  ShapeLiteral expandPredicateShape(const ShapeLiteral &shape);
  std::optional<ShapeLiteral> shapeNamedByTarget(const ValueExpr &target);
  bool variableHoldsHandle(const std::string &name) const;
  /// True when @p argument is known while compiling to be a shape: a literal,
  /// a variable bound to a shape handle (recorded or not), or a call of a
  /// definition whose whole body is a shape literal.
  bool argumentNamesShape(const ValueExpr &argument) const;
  /// True when calling @p call returns a shape: the definition's whole body is
  /// a shape literal, names a parameter that was given a shape, or calls a
  /// definition that does, so a definition that returns its argument is
  /// followed instead of its result being read as a value.
  bool callReturnsShape(const FunctionInvocation &call) const;
  bool returnsShapeGiven(const DefDeclaration &definition,
                         const std::vector<bool> &argumentIsShape,
                         std::size_t depth) const;

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
  ShapeLimits shapeLimits_{};
  std::size_t remainingShapeProjectionRules_{defaultShapeMaxRules};
  std::size_t remainingShapePredicateRules_{defaultShapeMaxRules};
  std::size_t remainingShapePredicateValidationNodes_{defaultShapeMaxRules};
  std::size_t remainingContractEmissions_{};
  std::size_t nextContractValidatorId_{};
  bool compilingGraphContract_{false};
  std::unordered_map<const ValueExpr *, zigzag::CellRef> shapeTargetConstants_;
  /// Handle cell to expanded shape (decision D4: shapes are syntax, so the
  /// compiler resolves them and the VM never holds one). Seeded from
  /// CompilationOptions::initialShapes and grown only by registerShape, so it
  /// is bounded by the distinct shape values one query writes.
  CompiledHandleTable shapeHandles_;
  std::unordered_set<const ShapeLiteral *> shapesExpanding_;
  /// Shapes a predicate expanded; the predicate compiler keys caches by
  /// address, so these must outlive the compile and never move.
  std::deque<ShapeLiteral> expandedPredicateShapes_;
  std::vector<std::string> shapeCallsActive_;
  zigzag::CellRef compileShapePredicateImpl(const ShapeLiteral &shape,
                                            zigzag::CellRef candidate);
};

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_COMPILER_HPP
