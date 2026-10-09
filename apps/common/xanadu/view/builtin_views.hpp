/**
 * @file builtin_views.hpp
 * @brief The one call that installs every built-in view (design/view-system.md
 *        §8.1; plan E15).
 *
 * Each view's descriptor, with its settings and chords, ships in that view's
 * own package and is added here by name, so a view is reachable the day it
 * lands and nothing else in the framework lists view kinds (V-R1). A plugin
 * calls ViewRegistry::add() the same way.
 */
#ifndef COMMON_XANADU_VIEW_BUILTIN_VIEWS_HPP
#define COMMON_XANADU_VIEW_BUILTIN_VIEWS_HPP

#include <expected>

#include "common/xanadu/view/view.hpp"
#include "common/xanadu/view/view_error.hpp"

namespace xanadu::view {

/// Add every built-in view to @p registry, in palette order. The first
/// refusal stops the call and is returned; the views before it stay added.
std::expected<ViewRegistry *, ViewError>
registerBuiltinViews(ViewRegistry &registry);

} // namespace xanadu::view

#endif // COMMON_XANADU_VIEW_BUILTIN_VIEWS_HPP
