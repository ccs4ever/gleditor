/**
 * @file static_functor_program.hpp
 * @brief Bounded, symbolic native-frame programs for named VQL functors.
 */
#ifndef COMMON_XANADU_VQL_STATIC_FUNCTOR_PROGRAM_HPP
#define COMMON_XANADU_VQL_STATIC_FUNCTOR_PROGRAM_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/xanadu/vql/ast.hpp"

namespace xanadu::vql {

struct StaticFunctorProgramLimits {
  std::size_t maxNodes{100'000};
  std::size_t maxDepth{256};
  std::size_t maxWork{1'000'000};
};

struct StaticFunctorProgramError {
  std::string message;
};

struct StaticFunctorProgramKey {
  std::size_t formalIndex{};
  std::string structuralIdentity;

  bool operator==(const StaticFunctorProgramKey &) const = default;
};

struct StaticFunctorAliasUse {
  enum class Role : std::uint8_t { Value, Functor };

  std::string name;
  Role role{Role::Value};

  bool operator==(const StaticFunctorAliasUse &) const = default;
};

/// Owned compile-time AST payload. It is lowered to ordinary Vortex graph
/// instructions and is never stored in a runtime frame or closure descriptor.
struct StaticFunctorProgram {
  StaticFunctorProgramKey key;
  std::shared_ptr<const std::vector<PathStep>> steps;
  std::vector<StaticFunctorAliasUse> aliases;
  std::size_t workUnits{0};
};

/// An unsupported but well-formed form returns `nullopt`; malformed/cyclic
/// input and exhausted node/depth/work budgets return an error.
[[nodiscard]] std::expected<std::optional<StaticFunctorProgram>,
                            StaticFunctorProgramError>
normalizeStaticFunctorProgram(std::span<const PathStep> steps,
                              std::size_t formalIndex,
                              StaticFunctorProgramLimits limits = {});

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_STATIC_FUNCTOR_PROGRAM_HPP
