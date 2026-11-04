#ifndef COMMON_XANADU_VAGENT_P16_HPP
#define COMMON_XANADU_VAGENT_P16_HPP

// Phase 6's P16 ceremony seam: the three-state checkpoint flag, the
// re-kind/restore windows, and the no-interleave discipline
// (design/vagent-metaprogramming-implementation.md lines 526-575,
// design/vagent-metaprogramming.md spec :2163-2179).
//
// CONTRACT NOTE: agreed seam between two parallel work items. The mesh
// side (worker_context/mesh_actor/or_parallel) calls these hooks from
// the adapter Executor callback inside pumpOne; the ceremony side
// (this TU + fragment.cpp/meta.cpp/genesis.cpp) implements them.
// Signatures may refine; call-site semantics below are fixed.

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

namespace xanadu::vagent {

// The checkpoint flag's three states (spec :2163-2172). The mesh side
// samples pending(pid) INSIDE the Executor callback — the message pops
// before the callback runs (mesh_actor.cpp:468/:473-477), so
// pending==1 at flag time means the flagged message is sole-queued;
// pending==0 sampled at callback end is the empty-mailbox test a
// self-send trips.
enum class P16Flag : std::uint8_t { Clear, Raised, Checkpoint };

class P16Ceremony {
public:
  // Flag-time window: called from the Executor callback when a
  // checkpoint message is processed with pending(pid)==1 (sole-queued).
  // Re-kinds the marked ref slots (Int64 → OpHandle over the marker
  // inventory phase-3 emission + reify mint). Returns false to veto.
  [[nodiscard]] virtual bool onFlagged() = 0;

  // End-of-flagged-turn window: pending(pid)==0 re-check passed —
  // restore every re-kinded slot (OpHandle → Int64(*handleTarget))
  // kind-guarded, minus the +d.name-bearing records the dim arm owns
  // (spec :2394-2396). Returns the publish command — issued
  // synchronously from the same callback (no-interleave, spec
  // :2171-2179): no turn of either kind may run between this point and
  // publishValue.
  [[nodiscard]] virtual std::expected<void, std::string>
  onFlaggedTurnEnd() = 0;

  // Kind-guarded restore on every exit path — abort/fault/short-turn.
  // Idempotent: safe to call when nothing was re-kinded.
  virtual void restoreAll() = 0;

  virtual ~P16Ceremony() = default;
};

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_P16_HPP
