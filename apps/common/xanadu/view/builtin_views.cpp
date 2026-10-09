#include "common/xanadu/view/builtin_views.hpp"

#include "common/xanadu/view/slice/stretch_vanishing_view.hpp"

namespace xanadu::view {

std::expected<ViewRegistry *, ViewError>
registerBuiltinViews(ViewRegistry &registry) {
  return registry.add(stretchVanishingDescriptor());
}

} // namespace xanadu::view
