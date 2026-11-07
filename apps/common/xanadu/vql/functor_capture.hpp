#ifndef COMMON_XANADU_VQL_FUNCTOR_CAPTURE_HPP
#define COMMON_XANADU_VQL_FUNCTOR_CAPTURE_HPP

#include <optional>
#include <span>
#include <vector>

#include <gleditor/cpp26.hpp>

#include "common/xanadu/vql/ast.hpp"

namespace xanadu::vql {

using FunctorLookup =
    gleditor::cpp26::function_ref<std::optional<std::vector<PathStep>>(
        std::string_view)>;

/// Refuse ordered projections, cycles and over-budget syntax in a capture body.
void validateFunctorCaptureSteps(std::span<const PathStep> steps);

/// Freeze references to existing functors while preserving application scope.
std::vector<PathStep>
captureFunctorApplications(std::span<const PathStep> steps,
                           FunctorLookup lookup);

} // namespace xanadu::vql
#endif
