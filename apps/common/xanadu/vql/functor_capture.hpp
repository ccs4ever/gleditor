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
using FunctorSpanLookup =
    gleditor::cpp26::function_ref<std::optional<std::span<const PathStep>>(
        std::string_view)>;
using FunctorCopyCharge = gleditor::cpp26::function_ref<void(std::size_t)>;

/// Refuse ordered projections, cycles and over-budget syntax in a capture body.
void validateFunctorCaptureSteps(std::span<const PathStep> steps);
/// Estimate owned AST copy work before a vector of steps is copied.
std::size_t functorCaptureCopyWork(std::span<const PathStep> steps);
/// Charge AST copy work incrementally before traversing or copying owned data.
void chargeFunctorCaptureCopyWork(std::span<const PathStep> steps,
                                  FunctorCopyCharge charge);

/// Freeze references to existing functors while preserving application scope.
std::vector<PathStep>
captureFunctorApplications(std::span<const PathStep> steps,
                           FunctorLookup lookup);

/// Freeze applications from borrowed bindings, charging copy work before each
/// owned AST copy or splice.
std::vector<PathStep>
captureFunctorApplications(std::span<const PathStep> steps,
                           FunctorSpanLookup lookup, FunctorCopyCharge charge);

} // namespace xanadu::vql
#endif
