#ifndef XANADU_VORTEX_IO_PROFILE_HPP
#define XANADU_VORTEX_IO_PROFILE_HPP

#include <cstdint>

#include "common/xanadu/vortex/io_error.hpp"

// design/vortex-io-implementation-plan.md §9: IoExecutionProfile is a
// generic immutable host contract. Core/VM never inspect the profile name;
// only IoHost (and anything staging actor-turn effects ahead of it) checks
// policy before a request reaches IoService admission.

namespace xanadu::vortex {

/// The closed runtime-profile vocabulary of §9. Immutable for the life of
/// a run; the policy it implies is looked up once, not branched on by name
/// anywhere below IoHost.
enum class IoExecutionProfile : std::uint8_t {
  Interactive,
  Batch,
  ActorTurn,
  DurableEffect,
};

/// Suspension/submission legality and delivery mode implied by a profile.
/// An actor turn is atomic: direct Submit/Await is refused before
/// admission, and so is any yielding effect handler reached from inside a
/// turn. A durable effect already carries its own pre-admission durability
/// (the journal in io_actor_journal.hpp); it may suspend but never
/// silently resends on restore.
struct IoProfilePolicy {
  bool allowDirectSubmit{true};
  bool allowAwaitSuspension{true};
  bool completionIsLaterMessage{false};
};

[[nodiscard]] constexpr IoProfilePolicy
policyFor(const IoExecutionProfile profile) noexcept {
  switch (profile) {
  case IoExecutionProfile::Interactive:
    return IoProfilePolicy{.allowDirectSubmit        = true,
                           .allowAwaitSuspension     = true,
                           .completionIsLaterMessage = false};
  case IoExecutionProfile::Batch:
    return IoProfilePolicy{.allowDirectSubmit        = true,
                           .allowAwaitSuspension     = true,
                           .completionIsLaterMessage = false};
  case IoExecutionProfile::ActorTurn:
    // §9: "direct Submit/Await and any yielding VAgent handler are
    // refused before admission; runtime repeats check." A turn only
    // stages a reversible intent (io_actor_journal.hpp); IoService is
    // reached, if at all, by the host outside the turn, after commit.
    return IoProfilePolicy{.allowDirectSubmit        = false,
                           .allowAwaitSuspension     = false,
                           .completionIsLaterMessage = true};
  case IoExecutionProfile::DurableEffect:
    return IoProfilePolicy{.allowDirectSubmit        = true,
                           .allowAwaitSuspension     = true,
                           .completionIsLaterMessage = true};
  }
  return IoProfilePolicy{.allowDirectSubmit        = false,
                         .allowAwaitSuspension     = false,
                         .completionIsLaterMessage = false};
}

/// The typed refusal §9 requires before admission when a profile forbids
/// the attempted operation. Never an exception, never a logical failure.
inline constexpr IoErrorInfo profileRefusal() noexcept {
  return IoErrorInfo{IoErrorCode::ProfileRefusal, 0};
}

} // namespace xanadu::vortex

#endif
