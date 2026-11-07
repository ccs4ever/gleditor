/**
 * @file epistemic_abi_liveness.hpp
 * @brief Names the liveness slice (P19 I3-3c1, `i3-3-design.md` 5.2) shares
 *        between the executor, the emitted routines and the translator
 *        registry.
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_ABI_LIVENESS_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_ABI_LIVENESS_HPP

#include <string_view>

namespace xanadu::vagent::runtime {

// Profile fields, all additive: absent reads as "never heard" (the stamps),
// "not silent" (`silent`) and "no instance" (`liveness_instance`). A
// profile that holds a stamp and no `liveness_instance` is shape-1 state whose
// stamps cannot be placed on this executor's clock.
/// Bool, true while the host's heartbeat or rebase deadline has passed.
inline constexpr std::string_view kProfileSilent = "silent";
/// Float, the steady second the profile went silent.
inline constexpr std::string_view kProfileSilentSince = "silent_since";
/// Text: why the profile is silent, one of the causes below.
inline constexpr std::string_view kProfileSilentCause = "silent_cause";
/// Text, 16 hex: the executor steady clock `last_receive`, `last_contact`,
/// `silent_since` and every stream's `received` are stamps of.
inline constexpr std::string_view kProfileLivenessInstance =
    "liveness_instance";

// A stream record's own fields, beside its `clock_instance`.
/// Float, the steady second of the stream's newest commit.
inline constexpr std::string_view kStreamReceived = "received";
/// Bool, true from `data` passing with no commit until the next commit.
inline constexpr std::string_view kStreamStale = "stale";

// `silent_cause`. `heartbeat`: no fresh message by the deadline. `rebase`: a
// resume could not place the old stamps (U-18). `clock`: a jump dropped the
// source's paired mapping and the pairing missed the deadline (4.5).
inline constexpr std::string_view kSilentCauseHeartbeat = "heartbeat";
inline constexpr std::string_view kSilentCauseRebase    = "rebase";
inline constexpr std::string_view kSilentCauseClock     = "clock";

// `$turn.liveness`, run by `clockEntry`: `now` (steady seconds) and `items`, a
// record keyed by position whose entries are the records below. The host
// builds one per pid per wake (S-13c); a heartbeat that ends a silence sends
// a `clear`.
inline constexpr std::string_view kTurnLiveness      = "liveness";
inline constexpr std::string_view kLivenessNow       = "now";
inline constexpr std::string_view kLivenessItems     = "items";
inline constexpr std::string_view kLivenessKind      = "kind";
inline constexpr std::string_view kLivenessSource    = "source";
inline constexpr std::string_view kLivenessPredicate = "predicate";
/// The arena stamp the deadline was armed from; absent when it was armed
/// from nothing. The flag is set only if no newer stamp is in the arena.
inline constexpr std::string_view kLivenessStamp = "stamp";
inline constexpr std::string_view kLivenessCause = "cause";
/// A silence of a source never heard makes its profile at this incarnation,
/// within `max_sources` (the record's), as an isolate does.
inline constexpr std::string_view kLivenessIncarnation = "incarnation";
inline constexpr std::string_view kLivenessMaxSources  = "max_sources";
inline constexpr std::string_view kLivenessKindSilent  = "silent";
inline constexpr std::string_view kLivenessKindStale   = "stale";
inline constexpr std::string_view kLivenessKindClear   = "clear";

} // namespace xanadu::vagent::runtime

#endif // COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_ABI_LIVENESS_HPP
