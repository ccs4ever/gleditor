/**
 * @file vql_engine.hpp
 * @brief Direct query evaluation engine for VQL v16.0 operating over
 * ArenaManifold and Store.
 */
#ifndef COMMON_XANADU_VQL_VQL_ENGINE_HPP
#define COMMON_XANADU_VQL_VQL_ENGINE_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vql/ast.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {

using xanadu::MultiStoreCoordinator;

/// A def's result outside its declared return type, or a type name the engine
/// does not know (§4.12.1). A parameter that misses its contract is not one:
/// that call matches nothing, as a failed destructure always has.
class TypeContractError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class VQLEngine {
public:
  explicit VQLEngine(MultiStoreCoordinator &coordinator);
  explicit VQLEngine(zigzag::vortex::VortexCore &core);
  explicit VQLEngine(zigzag::ArenaManifold &arena);

  /// Executes a parsed query expression and returns the resulting cells.
  std::vector<zigzag::CellRef> execute(const QueryExpression &query);

  /// Executes a raw query string (lexing, parsing, and evaluating).
  std::vector<zigzag::CellRef> execute(std::string_view queryString);

  /// Evaluates a path expression starting from an optional set of context
  /// cells.
  std::vector<zigzag::CellRef>
  evaluatePath(const PathExpression &path,
               const std::vector<zigzag::CellRef> &contextCells = {});

  /// Evaluates a single path step over an input set of cells.
  std::vector<zigzag::CellRef>
  evaluateStep(const PathStep &step,
               const std::vector<zigzag::CellRef> &currentCells);

  /// Variable environment management.
  void setVariable(std::string_view name, zigzag::CellRef value);
  void setVariable(std::string_view name, std::vector<zigzag::CellRef> values);
  [[nodiscard]] std::vector<zigzag::CellRef>
  getVariable(std::string_view name) const;
  void clearVariables() noexcept;

  /// Executes a single statement in the query.
  std::vector<zigzag::CellRef> executeStatement(const Statement &stmt);

  /// Executes a perspective block, scoping activePerspective_ during execution.
  std::vector<zigzag::CellRef>
  executePerspectiveBlock(const PerspectiveBlock &block);

  /// Executes a reactive watch statement over chronofilade.
  std::vector<zigzag::CellRef> executeWatch(const WatchStatement &watchStmt);

  /// Records a session mount declaration.
  void executeMount(const MountDeclaration &mount);

  /// Resolves dimension name to DimRef, minting dynamically if new.
  zigzag::DimRef resolveDimension(std::string_view name);

  [[nodiscard]] MultiStoreCoordinator &coordinator() noexcept {
    return coordinator_;
  }
  [[nodiscard]] const MultiStoreCoordinator &coordinator() const noexcept {
    return coordinator_;
  }
  [[nodiscard]] zigzag::vortex::VortexCore &core() noexcept { return *core_; }
  [[nodiscard]] const zigzag::vortex::VortexCore &core() const noexcept {
    return *core_;
  }
  [[nodiscard]] zigzag::ArenaManifold &arena() noexcept {
    return core_->arena();
  }
  [[nodiscard]] const zigzag::ArenaManifold &arena() const noexcept {
    return core_->arena();
  }

private:
  // Step & Traversal helpers
  std::vector<zigzag::CellRef>
  resolveAnchor(const AnchorNode &anchor,
                const std::vector<zigzag::CellRef> &contextCells);

  std::vector<zigzag::CellRef>
  traverseDimension(const SignedDimensionStep &dimStep,
                    const std::vector<zigzag::CellRef> &inputs);

  std::vector<zigzag::CellRef>
  performCreates(const SignedDimensionStep &dimStep,
                 const std::vector<zigzag::CellRef> &inputs);

  /// find()'s second half: the lines of @p info's current documents that
  /// hold @p needle, each a cell linked along d.source to where it was found.
  void findInDocuments(const StoreInfo &info, std::string_view needle,
                       std::vector<zigzag::CellRef> &out);

  // Predicate & Boolean Evaluation
  bool evaluatePredicate(const BooleanExpr &expr,
                         std::optional<zigzag::CellRef> context);
  bool evaluateTerm(const BooleanTerm &term,
                    std::optional<zigzag::CellRef> context);
  bool evaluateFactor(const BooleanFactor &factor,
                      std::optional<zigzag::CellRef> context);
  bool evaluateShapePredicate(const ShapeLiteral &shape,
                              std::optional<zigzag::CellRef> context);

  std::optional<zigzag::vortex::CellValue>
  evaluateValueExpr(const ValueExpr &expr,
                    std::optional<zigzag::CellRef> context);

  static bool compareValues(const zigzag::vortex::CellValue &left, CompOp op,
                            const zigzag::vortex::CellValue &right);

  // Execution Block & FLWOR
  std::vector<zigzag::CellRef> executeBlock(const ExecutionBlock &block);

  void evaluateBindings(const ExecutionBlock &block, std::size_t bindingIndex,
                        std::vector<zigzag::CellRef> &accumulatedResults);

  void executeActionClause(const ActionClause &action,
                           std::vector<zigzag::CellRef> &accumulatedResults);

  void executeEffectClause(const EffectClause &eff,
                           std::vector<zigzag::CellRef> &accumulatedResults);

  zigzag::CellRef cellFromValue(const zigzag::vortex::CellValue &val);

  std::vector<zigzag::CellRef> resolveStoreAnchor(const StoreAnchor &sa);

  std::vector<zigzag::CellRef>
  evaluatePerspectiveStep(std::string_view perspName,
                          const PathStep &targetStep,
                          const std::vector<zigzag::CellRef> &currentCells);

  std::optional<zigzag::CellRef>
  deltaTarget(const FunctionInvocation &fn,
              std::optional<zigzag::CellRef> context);
  std::optional<zigzag::CellRef> sliceHomeOf(zigzag::CellRef cell);

  std::vector<zigzag::CellRef> executeScriptBody(const ScriptBody &body);

  std::vector<zigzag::CellRef>
  executeLambda(const PathLambda &lambda,
                std::optional<zigzag::CellRef> context);

  std::vector<zigzag::CellRef>
  callUserDef(const DefDeclaration &def, const std::vector<ValueExpr> &args,
              std::optional<zigzag::CellRef> context);

  using Bindings =
      std::unordered_map<std::string, std::vector<zigzag::CellRef>>;
  /// A point-free functor's steps, composed once when it is bound (§4.12.3).
  using FunctorSteps = std::shared_ptr<const std::vector<PathStep>>;

  /// What a field with nothing on its dimension does: a lambda or def
  /// parameter refuses the cell, a `for` or `watch` binding binds nothing.
  enum class MissingField : std::uint8_t { Refuse, BindEmpty };

  bool matchDestructure(const DestructurePattern &pattern,
                        std::optional<zigzag::CellRef> context,
                        Bindings &scopedBindings,
                        MissingField missing = MissingField::Refuse);

  /// Whether @p cells, as one stream, inhabit @p type. A shape inside the
  /// type binds its fields' variables into @p scopedBindings when given.
  bool satisfiesType(const TypeExpr &type,
                     std::span<const zigzag::CellRef> cells,
                     Bindings *scopedBindings);
  bool satisfiesPrimary(const PrimaryType &type,
                        std::span<const zigzag::CellRef> cells,
                        Bindings *scopedBindings);
  bool satisfiesAtom(const PrimaryType &type, zigzag::CellRef cell,
                     Bindings *scopedBindings);
  bool hasBasicType(const BasicType &type, zigzag::CellRef cell);

  /// The cells @p expr names: a path's or variable's own, so passing a cell
  /// mints nothing, and a fresh scalar only for a literal or computed value.
  std::vector<zigzag::CellRef>
  cellsOfValueExpr(const ValueExpr &expr,
                   std::optional<zigzag::CellRef> context);

  /// @p path as a functor when it is one: a leading '/', or an anchor that
  /// names a bound functor. Functor variables among its steps are inlined.
  [[nodiscard]] std::optional<FunctorSteps>
  functorOf(const PathExpression &path) const;
  [[nodiscard]] std::optional<FunctorSteps>
  functorNamed(std::string_view name) const;
  std::vector<zigzag::CellRef> applyFunctor(const std::vector<PathStep> &steps,
                                            std::vector<zigzag::CellRef> cells);

  /// Binds a `let`, as a functor when its target is one.
  std::vector<zigzag::CellRef> bindLet(const LetClause &let);
  /// Binds a parameter name to an argument, as a functor when it is one.
  void bindName(const std::string &name, std::vector<zigzag::CellRef> cells);

  std::unique_ptr<zigzag::vortex::VortexCore> ownedCore_{nullptr};
  std::unique_ptr<MultiStoreCoordinator> ownedCoordinator_{nullptr};
  MultiStoreCoordinator &coordinator_;
  zigzag::vortex::VortexCore *core_{nullptr};

  std::unordered_map<std::string, std::vector<zigzag::CellRef>> env_;
  /// Names bound to functors rather than cells; a name is in one or the other.
  std::unordered_map<std::string, FunctorSteps> functors_;
  std::string activePerspective_{};
  std::unordered_map<std::string,
                     std::unordered_map<zigzag::CellRef, zigzag::CellRef>>
      perspectiveClones_;
  std::unordered_map<std::string, MountDeclaration> sessionMounts_;
  std::unordered_map<std::string, DefDeclaration> userDefs_;
};

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_VQL_ENGINE_HPP
