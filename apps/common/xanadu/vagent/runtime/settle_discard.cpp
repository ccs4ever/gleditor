#include "common/xanadu/vagent/runtime/settle_discard.hpp"

#include <gleditor/logging.hpp>
#include <string>
#include <vector>

namespace xanadu::vagent::runtime {

namespace {
constexpr const char *kLog = "vortex.agent";
}

DiscardResult discardQueueHead(zigzag::ArenaManifold &arena,
                               const zigzag::CellRef agent,
                               const zigzag::DimRef pending,
                               const zigzag::CellRef first,
                               const std::optional<zigzag::CellRef> next,
                               SettleReport &report) {
  const auto type = arena.textOf(first);
  if (arena.link(agent, pending, zigzag::DimVector::POS, next)) {
    report.discard(type);
    return DiscardResult::Head;
  }
  GLEDITOR_LOG_WARN(kLog, "cannot unlink a failed belief event");
  // Count what the whole-queue drop loses before it happens. The walk is
  // bounded by the arena, so a chain with a cycle in it ends.
  const auto bound   = arena.cellCount();
  std::size_t walked = 0;
  std::vector<std::string> types;
  for (std::optional<zigzag::CellRef> at = first; at && walked < bound;
       at                                = arena.linked(*at, pending), ++walked)
    types.push_back(arena.textOf(*at));
  if (!arena.link(agent, pending, zigzag::DimVector::POS, std::nullopt)) {
    GLEDITOR_LOG_ERROR(kLog, "cannot drop the belief queue; it stays linked");
    return DiscardResult::Stuck;
  }
  for (const auto &each : types) report.discard(each);
  return DiscardResult::WholeQueue;
}

} // namespace xanadu::vagent::runtime
