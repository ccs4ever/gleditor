#include "common/xanadu/vql/vql_engine.hpp"

namespace xanadu::vql {

std::optional<xanadu::MicroversionId>
VQLEngine::versionValue(const ValueExpr &expr,
                        std::optional<zigzag::CellRef> context) {
  if (const auto *version =
          std::get_if<std::shared_ptr<VersionExpr>>(&expr.kind)) {
    return evaluateVersionExpr(**version, context);
  }
  if (const auto *name = std::get_if<std::string>(&expr.kind)) {
    const auto variable = env_.find(*name);
    if (variable != env_.end() && variable->second.size() == 1) {
      const auto value = versionValues_.find(variable->second.front());
      if (value != versionValues_.end()) return value->second;
    }
  }
  return std::nullopt;
}

} // namespace xanadu::vql
