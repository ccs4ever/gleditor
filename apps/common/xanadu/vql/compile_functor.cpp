#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/functor_capture.hpp"

#include <stdexcept>

namespace xanadu::vql {

std::optional<std::vector<PathStep>>
VQLCompiler::functorArgument(const ValueExpr &argument) const {
  if (const auto *path =
          std::get_if<std::shared_ptr<PathExpression>>(&argument.kind);
      path && *path)
    return functorArgument(**path);
  if (const auto *name = std::get_if<std::string>(&argument.kind)) {
    const auto found = functorBindings_.find(*name);
    if (found != functorBindings_.end()) return found->second;
  }
  return std::nullopt;
}

std::optional<std::vector<PathStep>>
VQLCompiler::functorArgument(const PathExpression &path) const {
  if (!path.anchorPredicates.empty() || path.shapeProjection || path.cloneTail)
    return std::nullopt;
  std::vector<PathStep> steps;
  if (!path.isFunctor) {
    if (path.anchor.kind != AnchorKind::Variable) return std::nullopt;
    const auto found = functorBindings_.find(path.anchor.name);
    if (found == functorBindings_.end()) return std::nullopt;
    if (path.anchor.versionPin || path.anchor.derefMaster)
      throw std::invalid_argument(
          "qualified functor anchors require runtime functor handles");
    steps = found->second;
  }
  auto captured = captureFunctorApplications(
      path.steps,
      [&](std::string_view name) -> std::optional<std::vector<PathStep>> {
        const auto found = functorBindings_.find(std::string(name));
        if (found == functorBindings_.end())
          throw std::invalid_argument("unbound point-free functor '$" +
                                      std::string(name) + "'");
        return found->second;
      });
  steps.insert(steps.end(), captured.begin(), captured.end());
  return steps;
}

} // namespace xanadu::vql
