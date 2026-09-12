/**
 * @file vql_engine.hpp
 * @brief Direct query evaluation engine for VQL v13.0 operating over
 * ArenaManifold and Store.
 */
#ifndef COMMON_XANADU_VQL_VQL_ENGINE_HPP
#define COMMON_XANADU_VQL_VQL_ENGINE_HPP

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vql/ast.hpp"
#include "common/xanadu/vql/multi_store.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {

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

  // Predicate & Boolean Evaluation
  bool evaluatePredicate(const BooleanExpr &expr, zigzag::CellRef context);
  bool evaluateTerm(const BooleanTerm &term, zigzag::CellRef context);
  bool evaluateFactor(const BooleanFactor &factor, zigzag::CellRef context);

  zigzag::vortex::CellValue evaluateValueExpr(const ValueExpr &expr,
                                              zigzag::CellRef context);

  bool compareValues(const zigzag::vortex::CellValue &left, CompOp op,
                     const zigzag::vortex::CellValue &right);

  // Execution Block & FLWOR
  std::vector<zigzag::CellRef> executeBlock(const ExecutionBlock &block);

  void evaluateBindings(const ExecutionBlock &block, std::size_t bindingIndex,
                        std::vector<zigzag::CellRef> &accumulatedResults);

  void executeActionClause(const ActionClause &action,
                           std::vector<zigzag::CellRef> &accumulatedResults);

  void executeEffectClause(const EffectClause &eff);

  zigzag::CellRef cellFromValue(const zigzag::vortex::CellValue &val);

  std::unique_ptr<zigzag::vortex::VortexCore> ownedCore_{nullptr};
  std::unique_ptr<MultiStoreCoordinator> ownedCoordinator_{nullptr};
  MultiStoreCoordinator &coordinator_;
  zigzag::vortex::VortexCore *core_{nullptr};

  std::unordered_map<std::string, std::vector<zigzag::CellRef>> env_{};
};

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_VQL_ENGINE_HPP
