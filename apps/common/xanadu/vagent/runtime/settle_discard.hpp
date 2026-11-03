/**
 * @file settle_discard.hpp
 * @brief Dropping a belief event whose settle turn failed (P19 I3-2b).
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_SETTLE_DISCARD_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_SETTLE_DISCARD_HPP

#include <cstdint>
#include <optional>

#include "common/xanadu/vagent/runtime/epistemic_ingress.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vagent::runtime {

enum class DiscardResult : std::uint8_t {
  /// The head event is unlinked; the rest of the queue is intact.
  Head,
  /// The unlink was refused, so the whole queue was dropped instead.
  WholeQueue,
  /// Even dropping the whole queue was refused; the queue is unchanged.
  Stuck,
};

/// Unlink @p first, the head of the queue linked from @p agent on @p pending,
/// and record it in @p report. @p next is the head's successor as the caller
/// read it. This is plain link editing: it mints no cell and spends no VM
/// budget, so it works after a turn ran out of either. The two refusals are
/// unreachable for refs read from the same arena, which is why @p next is a
/// parameter: a test can hand in one the arena does not hold.
DiscardResult discardQueueHead(zigzag::ArenaManifold &arena,
                               zigzag::CellRef agent, zigzag::DimRef pending,
                               zigzag::CellRef first,
                               std::optional<zigzag::CellRef> next,
                               SettleReport &report);

} // namespace xanadu::vagent::runtime

#endif
