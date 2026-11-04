/**
 * @file frame_eligibility.hpp
 * @brief Bounded, conservative eligibility analysis for reusable VQL frames.
 */
#ifndef COMMON_XANADU_VQL_FRAME_ELIGIBILITY_HPP
#define COMMON_XANADU_VQL_FRAME_ELIGIBILITY_HPP

#include <cstddef>
#include <expected>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "common/xanadu/vql/ast.hpp"

namespace xanadu::vql {

struct FrameEligibilityLimits {
  std::size_t maxNodes{100'000};
  std::size_t maxDepth{256};
};

struct FrameEligibility {
  std::unordered_set<std::string> reusableDefinitions;
  std::unordered_map<std::string, std::string> refusalReasons;
};

struct FrameEligibilityError {
  std::string message;
};

/// Classify definitions safe to lower to reusable activation-frame graphs.
/// Unsupported or open definitions have a refusal reason. Malformed/cyclic
/// ASTs and traversal-budget exhaustion are hard errors, so they cannot fall
/// through to recursive inline expansion.
[[nodiscard]] std::expected<FrameEligibility, FrameEligibilityError>
analyzeFrameEligibility(const QueryExpression &query,
                        FrameEligibilityLimits limits = {});

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_FRAME_ELIGIBILITY_HPP
