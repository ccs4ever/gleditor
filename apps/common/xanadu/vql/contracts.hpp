/**
 * @file contracts.hpp
 * @brief Shared VQL structural contract matching for direct and Vortex paths.
 */
#ifndef COMMON_XANADU_VQL_CONTRACTS_HPP
#define COMMON_XANADU_VQL_CONTRACTS_HPP

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <gleditor/cpp26.hpp>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vql/ast.hpp"

namespace xanadu::vql {

using ContractBindings =
    std::unordered_map<std::string, std::vector<zigzag::CellRef>>;

/// A def result violated its signature, or a type name is not supported.
class TypeContractError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/// Evaluates structural type and destructuring contracts over one VQL arena.
/// The default evaluator is borrowed for each call and is never retained.
class ContractMatcher {
public:
  using DefaultEvaluator =
      gleditor::cpp26::function_ref<std::vector<zigzag::CellRef>(
          const ValueExpr &, std::optional<zigzag::CellRef>)>;
  enum class MissingField : std::uint8_t { Refuse, BindEmpty };

  ContractMatcher(zigzag::vortex::VortexCore &core,
                  MultiStoreCoordinator &coordinator);

  [[nodiscard]] bool matchDestructure(const DestructurePattern &pattern,
                                      std::optional<zigzag::CellRef> context,
                                      ContractBindings &bindings,
                                      MissingField missing,
                                      DefaultEvaluator evaluateDefault);
  [[nodiscard]] bool match(const TypeExpr &type,
                           std::span<const zigzag::CellRef> cells,
                           ContractBindings *bindings,
                           DefaultEvaluator evaluateDefault);
  [[nodiscard]] bool satisfiesType(const TypeExpr &type,
                                   std::span<const zigzag::CellRef> cells,
                                   ContractBindings *bindings,
                                   DefaultEvaluator evaluateDefault);

  /// Reject unsupported names independent of whether a value reaches a branch.
  static void validateType(const TypeExpr &type);
  [[nodiscard]] static std::vector<std::string>
  contractCaptureNames(const TypeExpr &type);

private:
  [[nodiscard]] bool satisfiesPrimary(const PrimaryType &type,
                                      std::span<const zigzag::CellRef> cells,
                                      ContractBindings *bindings,
                                      DefaultEvaluator evaluateDefault);
  [[nodiscard]] bool satisfiesAtom(const PrimaryType &type,
                                   zigzag::CellRef cell,
                                   ContractBindings *bindings,
                                   DefaultEvaluator evaluateDefault);
  [[nodiscard]] bool hasBasicType(const BasicType &type,
                                  zigzag::CellRef cell) const;

  zigzag::vortex::VortexCore &core_;
  MultiStoreCoordinator &coordinator_;
};

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_CONTRACTS_HPP
