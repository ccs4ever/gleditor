/**
 * @file vpl_engine.hpp
 * @brief Direct AST evaluation engine for Vortex Parallel Language (VPL).
 *
 * Evaluates VPL AST expressions directly against ArenaManifold and Store,
 * supporting APL/J array semantics, dual syntax equivalence, multidimensional
 * rank indexing, hypertime scrubbing, clone masters, and transclusion
 * discovery.
 */
#ifndef COMMON_XANADU_VPL_VPL_ENGINE_HPP
#define COMMON_XANADU_VPL_VPL_ENGINE_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_stdlib.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vpl/ast.hpp"
#include "common/xanadu/vpl/parser.hpp"
#include "common/xanadu/vpl/view.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace xanadu {
class Store;
} // namespace xanadu

namespace xanadu::vpl {

class VPLEngine {
public:
  VPLEngine();
  explicit VPLEngine(zigzag::ArenaManifold &arena);
  explicit VPLEngine(zigzag::vortex::VortexCore &core);
  explicit VPLEngine(Store &store);
  ~VPLEngine();

  /// Evaluates an AST node and returns the resulting VplView.
  VplView evaluate(const AstNode &node);

  /// Lexes, parses, and evaluates a VPL source expression.
  VplView evaluate(std::string_view source);

  /// Variable environment bindings.
  void setVariable(std::string_view name, VplView value);
  [[nodiscard]] std::optional<VplView> getVariable(std::string_view name) const;
  void clearVariables() noexcept;

  /// Dimension resolution: resolves or mints named dimension cell.
  zigzag::DimRef resolveDimension(std::string_view name);

  // Accessors
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
  [[nodiscard]] zigzag::vortex::VortexStdLib &stdlib() noexcept {
    return *stdlib_;
  }
  [[nodiscard]] Store *store() const noexcept { return store_; }
  void setStore(Store *store) noexcept { store_ = store; }

  /// Default dimension for 1D arrays (defaults to "d.1").
  [[nodiscard]] zigzag::DimRef defaultDim();

private:
  // Node evaluation helpers
  VplView evalScalar(const ScalarExpr &expr);
  VplView evalVector(const VectorExpr &expr);
  VplView evalDimension(const DimensionExpr &expr);
  VplView evalIdentifier(const IdentifierExpr &expr);
  VplView evalMonadic(const MonadicExpr &expr);
  VplView evalDyadic(const DyadicExpr &expr);
  VplView evalAdverb(const AdverbExpr &expr);
  VplView evalConjunction(const ConjunctionExpr &expr);
  VplView evalAssign(const AssignExpr &expr);
  VplView evalIndexing(const IndexingExpr &expr);
  VplView evalQuad(const QuadExpr &expr);
  VplView evalProgram(const Program &expr);

  // Verb implementations
  VplView applyMonadicVerb(TokenKind verb, const VplView &arg);
  VplView applyDyadicVerb(TokenKind verb, const VplView &left,
                          const VplView &right);

  // Arithmetic & logic helper
  static double computeMonadicScalar(TokenKind verb, double val);
  static double computeDyadicScalar(TokenKind verb, double left, double right);

  // Cell extraction helper
  double cellValueDouble(zigzag::CellRef cell) const;
  std::int64_t cellValueInt(zigzag::CellRef cell) const;
  std::string cellValueString(zigzag::CellRef cell) const;

  // View cell minting
  VplView mintRank(const std::vector<double> &values, zigzag::DimRef dim);
  VplView mintRank(const std::vector<std::int64_t> &values, zigzag::DimRef dim);
  VplView mintRank(const std::vector<std::string> &values, zigzag::DimRef dim);

  std::unique_ptr<zigzag::ArenaManifold> ownedArena_{nullptr};
  std::unique_ptr<zigzag::vortex::VortexCore> ownedCore_{nullptr};
  std::unique_ptr<zigzag::vortex::VortexVM> ownedVm_{nullptr};
  std::unique_ptr<zigzag::vortex::VortexStdLib> ownedStdlib_{nullptr};

  zigzag::vortex::VortexCore *core_{nullptr};
  zigzag::vortex::VortexVM *vm_{nullptr};
  zigzag::vortex::VortexStdLib *stdlib_{nullptr};
  Store *store_{nullptr};

  zigzag::DimRef defaultDim_{zigzag::noCell};
  std::unordered_map<std::string, VplView> env_{};
};

} // namespace xanadu::vpl

#endif // COMMON_XANADU_VPL_VPL_ENGINE_HPP
