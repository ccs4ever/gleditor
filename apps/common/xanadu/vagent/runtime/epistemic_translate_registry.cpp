#include "common/xanadu/vagent/runtime/epistemic_translate_registry.hpp"

#include <algorithm>
#include <array>

#include "common/xanadu/vagent/runtime/epistemic_abi_health.hpp"
#include "common/xanadu/vagent/runtime/epistemic_shape.hpp"
#include "common/xanadu/vagent/runtime/runtime_abi.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu::vagent::runtime {

namespace {

using zigzag::CellRef;
using zigzag::DimVector;

// -- Samples ---------------------------------------------------------------

void put(const PopulateContext &c, std::string_view name,
         const WireValue &value) {
  setField(c.arena, c.vd, c.profile, name, writeValue(c.arena, c.vd, value));
}

void appendToList(const PopulateContext &c, std::string_view name,
                  const WireValue &element) {
  const auto list = fieldValue(c.arena, c.vd, c.profile, name);
  if (!list) {
    put(c, name, WireValue::list({element}));
    return;
  }
  const auto made = writeValue(c.arena, c.vd, element);
  if (const auto first = c.arena.linked(*list, c.vd.grab)) {
    zigzag::expectWritten(
        c.arena.link(zigzag::rankTail(c.arena, *first, c.vd.step), c.vd.step,
                     DimVector::POS, made));
  } else {
    zigzag::expectWritten(c.arena.link(*list, c.vd.grab, DimVector::POS, made));
  }
}

WireRecord sampleObservation(const std::string &token, std::string_view status,
                             std::optional<std::string_view> retiredBy,
                             std::int64_t sequence, bool optionalFields) {
  const auto text = [](std::string_view v) {
    return WireValue::text(std::string(v));
  };
  WireRecord r;
  r.set(std::string(kObsStatus), text(status));
  r.set(std::string(kObsVerdict), text(kObsVerdictUntested));
  r.set(std::string(kObsVerdictEpoch), WireValue::integer(0));
  r.set(std::string(kObsVerdictRevisions), WireValue::list({}));
  r.set(std::string(kObsPinned), WireValue::boolean(false));
  r.set(std::string(kObsProfile), WireValue::text(token));
  r.set(std::string(kObsHealth), text(kObsHealthDegraded));
  r.set(std::string(kObsHealthFraction), WireValue::real(0.25));
  r.set(std::string(kObsPredicate), text("sample"));
  r.set(std::string(kObsSource), WireValue::text(token));
  WireRecord tuple;
  tuple.set("0", text("key"));
  tuple.set("1", WireValue::real(1.5));
  r.set(std::string(kObsTuple), WireValue::record(std::move(tuple)));
  r.set(std::string(kObsIncarnation), WireValue::integer(1));
  r.set(std::string(kObsEventId),
        WireValue::text("sample-" + std::to_string(sequence)));
  r.set(std::string(kObsDigest), text("d1"));
  r.set(std::string(kObsSequence), WireValue::integer(sequence));
  r.set(std::string(kObsAcquiredAt), WireValue::real(10.0 + sequence));
  r.set(std::string(kObsAcquiredUncertainty), WireValue::real(0.5));
  r.set(std::string(kObsClockBasis), text("wall"));
  r.set(std::string(kObsClockEpoch), WireValue::integer(1));
  r.set(std::string(kObsClockMappingVersion), WireValue::integer(1));
  r.set(std::string(kObsFrame), text("enu"));
  r.set(std::string(kObsUnits), text("m"));
  r.set(std::string(kObsReceivedSteady), WireValue::real(11.0));
  r.set(std::string(kObsReceivedWall), WireValue::real(12.0));
  if (optionalFields) {
    r.set(std::string(kObsVariance), WireValue::real(0.125));
    r.set(std::string(kObsRelayedBy), text("relay"));
    r.set(std::string(kObsVersion), WireValue::integer(kObservationVersion));
    r.set(std::string(kObsDomain), text("sample-domain"));
    r.set(std::string(kObsModel), text("beta_reputation"));
    r.set(std::string(kObsModelEpoch), WireValue::integer(0));
    r.set(std::string(kObsCalibrationEpoch), WireValue::integer(0));
    r.set(std::string(kObsFeatureSchema), WireValue::integer(0));
    r.set(std::string(kObsFeatures), WireValue::list({}));
    r.set(std::string(kObsClockInstance), text("instance"));
  }
  if (retiredBy) r.set(std::string(kObsRetiredBy), text(*retiredBy));
  return r;
}

/// A record and its clone occurrence: the value cell of the `profile` field
/// is spliced in after the profile's master, as ingestion does.
void addRecord(const PopulateContext &c, CellRef anchor,
               std::string_view status,
               std::optional<std::string_view> retiredBy, std::int64_t seq,
               bool optionalFields) {
  const auto token = c.arena.textOf(c.profile);
  const auto record =
      writeValue(c.arena, c.vd,
                 WireValue::record(sampleObservation(token, status, retiredBy,
                                                     seq, optionalFields)));
  const auto view = fieldValue(c.arena, c.vd, record, kObsProfile);
  if (!view) return;
  const auto old = c.arena.linked(c.profile, c.clone);
  zigzag::expectWritten(
      c.arena.link(c.profile, c.clone, DimVector::POS, *view));
  if (old)
    zigzag::expectWritten(c.arena.link(*view, c.clone, DimVector::POS, old));
  appendToRank(c.arena, anchor, c.epistemic, record);
}

// -- I3-1: profiles, histories and the policy master -----------------------

void populateNothing(const PopulateContext &) {}

void populateScalars(const PopulateContext &c) {
  put(c, kProfileRejectionStreak, WireValue::integer(3));
  put(c, kProfileRejectCount, WireValue::integer(7));
  put(c, kProfileContentRefusals, WireValue::integer(2));
  put(c, kProfileRefusedCount, WireValue::integer(5));
  put(c, kProfileLastRefusal, WireValue::text("StaleSequence"));
  put(c, kProfileFaulted, WireValue::boolean(true));
}

void populateLiveness(const PopulateContext &c) {
  put(c, kProfileLastReceive, WireValue::real(123.5));
  put(c, kProfileLastContact, WireValue::real(124.5));
}

void populateStreams(const PopulateContext &c) {
  auto streams = fieldValue(c.arena, c.vd, c.profile, kProfileStreams);
  if (!streams) {
    put(c, kProfileStreams, WireValue::record({}));
    streams = fieldValue(c.arena, c.vd, c.profile, kProfileStreams);
  }
  if (streams)
    setField(c.arena, c.vd, *streams, "sample_stream",
             c.arena.makeScalarCell(std::int64_t{9}));
}

void populateDedup(const PopulateContext &c) {
  WireRecord seen;
  seen.type = "Seen";
  seen.set(std::string(kObsIncarnation), WireValue::integer(1));
  seen.set(std::string(kObsEventId), WireValue::text("sample-dedup"));
  seen.set(std::string(kObsDigest), WireValue::text("d2"));
  appendToList(c, kProfileDedup, WireValue::record(std::move(seen)));
}

void populateViews(const PopulateContext &c) {
  if (c.support)
    addRecord(c, *c.support, kObsStatusCurrent, std::nullopt, 101, false);
}

void populateHealthScalars(const PopulateContext &c) {
  put(c, kProfileResetEpoch, WireValue::integer(2));
  put(c, kProfileActionSeq, WireValue::integer(3));
  put(c, kProfileFaultSeq, WireValue::integer(1));
  // What the estimator writes for the one reason and one action the samples
  // below add; the plan refuses any other figure.
  put(c, kProfileHealthCells, WireValue::integer(healthBound(1, 1)));
  put(c, kProfileHealthRefusedWeight, WireValue::real(0.75));
  put(c, kProfileHealthWeight, WireValue::real(1.5));
}

void populateReasons(const PopulateContext &c) {
  WireRecord reason;
  reason.set(std::string(kReasonKind),
             WireValue::text(std::string(kReasonKindDegraded)));
  reason.set(std::string(kReasonCode),
             WireValue::text(std::string(kHealthCodeFraction)));
  reason.set(std::string(kReasonOrigin),
             WireValue::text(std::string(kReasonOriginOperator)));
  reason.set(std::string(kReasonGrant), WireValue::text("grant-1"));
  reason.set(std::string(kReasonIssuer), WireValue::text(std::string(64, 'c')));
  reason.set(std::string(kReasonIncarnation), WireValue::integer(1));
  reason.set(std::string(kReasonWall), WireValue::real(99.5));
  appendToList(c, kProfileReasons, WireValue::record(std::move(reason)));
}

void populateActions(const PopulateContext &c) {
  WireRecord action;
  action.set(std::string(kActionSeq), WireValue::integer(4));
  action.set(std::string(kReasonKind),
             WireValue::text(std::string(kHealthKindClear)));
  action.set(std::string(kReasonCode),
             WireValue::text(std::string(kHealthCodeFraction)));
  action.set(std::string(kActionCount), WireValue::integer(1));
  action.set(std::string(kActionRemoved), WireValue::integer(1));
  appendToList(c, kProfileActions, WireValue::record(std::move(action)));
}

void populateHistory(const PopulateContext &c) {
  if (c.support)
    addRecord(c, *c.support, kObsStatusRetired, kRetiredByAdvance, 102, false);
}

void populateRecordFields(const PopulateContext &c) {
  if (c.support)
    addRecord(c, *c.support, kObsStatusCurrent, std::nullopt, 103, true);
}

void populateRetiredChain(const PopulateContext &c) {
  addRecord(c, c.profile, kObsStatusRetired, kRetiredByRetire, 104, true);
}

// -- Constants each entry covers, by C++ name --------------------------------

constexpr std::array<std::string_view, 0> kNoFields{};
constexpr std::array kProfileScalarFields{
    kProfileIncarnation, kProfileFaulted,         kProfileRejectionStreak,
    kProfileRejectCount, kProfileContentRefusals, kProfileRefusedCount,
    kProfileLastRefusal, kProfileRetired};
constexpr std::array kLivenessFields{kProfileLastReceive, kProfileLastContact};
constexpr std::array kStreamFields{kProfileStreams};
constexpr std::array kDedupFields{kProfileDedup};
constexpr std::array kHealthScalarFields{
    kProfileResetEpoch,  kProfileActionSeq,           kProfileFaultSeq,
    kProfileHealthCells, kProfileHealthRefusedWeight, kProfileHealthWeight};
constexpr std::array kReasonFields{
    kProfileReasons, kReasonKind,   kReasonCode,        kReasonOrigin,
    kReasonGrant,    kReasonIssuer, kReasonIncarnation, kReasonWall};
constexpr std::array kActionFields{kProfileActions, kActionSeq, kActionCount,
                                   kActionRemoved};
constexpr std::array kRecordFields{kObsStatus,
                                   kObsVerdict,
                                   kObsVerdictEpoch,
                                   kObsVerdictRevisions,
                                   kObsPinned,
                                   kObsProfile,
                                   kObsHealth,
                                   kObsHealthFraction,
                                   kObsPredicate,
                                   kObsSource,
                                   kObsTuple,
                                   kObsIncarnation,
                                   kObsEventId,
                                   kObsDigest,
                                   kObsSequence,
                                   kObsAcquiredAt,
                                   kObsAcquiredUncertainty,
                                   kObsClockBasis,
                                   kObsClockEpoch,
                                   kObsClockMappingVersion,
                                   kObsFrame,
                                   kObsUnits,
                                   kObsReceivedSteady,
                                   kObsReceivedWall,
                                   kObsVariance,
                                   kObsRelayedBy,
                                   kObsVersion,
                                   kObsDomain,
                                   kObsModel,
                                   kObsModelEpoch,
                                   kObsCalibrationEpoch,
                                   kObsFeatureSchema,
                                   kObsFeatures,
                                   kObsClockInstance,
                                   kObsRetiredBy};

// The shipped names above are the only ones pinned at compile time; the
// header and the master are the program's own cells, not profile fields.
constexpr std::array kShapeFields{kShapeHeaderName, kShapeVersionField};
// The audience record (I3-3a2): its name and its one field.
constexpr std::array kAudienceFields{kAudienceRecordName, kAudienceTokenField};

constexpr std::array<OwnedStructure, 15> kOwned{{
    {"agent.shape_header",
     "d.epistemic",
     "agent header",
     kNoFields,
     Fate::Derived,
     Fate::Derived,
     {},
     1,
     populateNothing},
    // Written by the first bind's source-change turn, which every populated
    // fixture runs, so it needs no sample of its own.
    {"agent.audience",
     "d.epistemic",
     "agent rank",
     kAudienceFields,
     Fate::Carried,
     Fate::Carried,
     {},
     1,
     populateNothing},
    {"agent.profiles",
     "d.trust",
     "agent rank",
     kNoFields,
     Fate::Carried,
     Fate::Carried,
     {},
     1,
     populateNothing},
    {"profile.scalars",
     "d.vars",
     "profile field",
     kProfileScalarFields,
     Fate::Carried,
     Fate::Carried,
     {},
     1,
     populateScalars},
    {"profile.liveness", "d.vars", "profile field", kLivenessFields,
     Fate::Dropped, Fate::Carried,
     "stamps of the old executor's steady clock mean nothing to the new one", 1,
     populateLiveness},
    {"profile.streams",
     "d.vars",
     "profile field",
     kStreamFields,
     Fate::Carried,
     Fate::Carried,
     {},
     1,
     populateStreams},
    {"profile.dedup",
     "d.vars",
     "profile list",
     kDedupFields,
     Fate::Carried,
     Fate::Carried,
     {},
     1,
     populateDedup},
    {"profile.views",
     "d.clone",
     "profile rank",
     kNoFields,
     Fate::Carried,
     Fate::Carried,
     {},
     1,
     populateViews},
    {"profile.health_scalars",
     "d.vars",
     "profile field",
     kHealthScalarFields,
     Fate::Carried,
     Fate::Carried,
     {},
     1,
     populateHealthScalars},
    {"profile.reasons",
     "d.vars",
     "profile list",
     kReasonFields,
     Fate::Carried,
     Fate::Carried,
     {},
     1,
     populateReasons},
    {"profile.actions",
     "d.vars",
     "profile list",
     kActionFields,
     Fate::Carried,
     Fate::Carried,
     {},
     1,
     populateActions},
    {"support.history",
     "d.epistemic",
     "support rank",
     kNoFields,
     Fate::Carried,
     Fate::Carried,
     {},
     1,
     populateHistory},
    {"record.fields",
     "d.vars",
     "record field",
     kRecordFields,
     Fate::Carried,
     Fate::Carried,
     {},
     1,
     populateRecordFields},
    {"profile.retired_chain",
     "d.epistemic",
     "profile rank",
     kNoFields,
     Fate::Carried,
     Fate::Carried,
     {},
     1,
     populateRetiredChain},
    {"predicate.policy_master",
     "d.epistemic",
     "predicate master",
     kShapeFields,
     Fate::Derived,
     Fate::Derived,
     {},
     1,
     populateNothing},
}};

constexpr std::array kNotOwned{
    NotOwnedDimension{"d.behavior",
                      "a mounted behavior; the schema road reconciles it"},
    NotOwnedDimension{"d.skill",
                      "a mounted skill; the schema road reconciles it"},
    NotOwnedDimension{"d.capacity", "static program structure"},
    NotOwnedDimension{"d.space",
                      "hosted spaces are carried by the generic copiers"},
    NotOwnedDimension{"d.member",
                      "hosted spaces are carried by the generic copiers"},
    NotOwnedDimension{"d.holon", "children are carried by the generic copiers"},
    NotOwnedDimension{"d.intention", "refused for an arbitrating agent (A12)"},
    NotOwnedDimension{"d.subgoal", "refused with its intention"},
    NotOwnedDimension{"d.outbox", "staged effects refuse the schema road"},
    NotOwnedDimension{"d.belief", "predicates are the new program's cells"},
    NotOwnedDimension{"d.pending", "a settle in flight refuses a checkpoint"},
    NotOwnedDimension{"d.source", "supports are carried with their clauses"},
    NotOwnedDimension{"d.extends", "static program structure"},
    NotOwnedDimension{"d.scan", "derived by the runtime each turn"},
};

constexpr std::array kHost{
    HostStateRow{"bindings, aliases",
                 "kept; D3 refuses what would make them stale",
                 "lost; the host rebinds and the arena's incarnation floor "
                 "refuses a lower one"},
    HostStateRow{"floors", "kept",
                 "lost; relearned from the first stale advance turn"},
    HostStateRow{
        "outcomes, changes", "kept",
        "lost; a resend meets the arena's dedup window and answers Duplicate"},
    HostStateRow{"queued, queuedBy, queuedVia, inFlight",
                 "zero at quiescence, asserted", "lost"},
    HostStateRow{"charges, uncounted, refusedWhileFaulted",
                 "kept; counts of refusals decided before the swap", "lost"},
    HostStateRow{"marks (estimator replay marks)", "kept",
                 "lost; one replay can count again"},
    HostStateRow{"healthGrants", "kept; they name the pid and principal",
                 "lost; operators re-grant"},
    HostStateRow{"resetMarks", "kept; they order readings across the swap",
                 "lost; the arena's reset_epoch is authoritative"},
    HostStateRow{"pendingHealth_", "zero for the pid at quiescence",
                 "refused earlier by checkpoint"},
    HostStateRow{"healthOutcomeRing_",
                 "kept; healthOutcome(receipt) still answers",
                 "lost with the executor"},
    HostStateRow{"healthActionSeq_", "kept; it numbers audit records only",
                 "restarts at 1; the profile's action_seq carries"},
    HostStateRow{"latched (latch cache)", "cleared after the commit",
                 "the new executor's, empty"},
    HostStateRow{"last_receive, last_contact (arena)", "kept",
                 "cleared; liveness starts from the first contact"},
    HostStateRow{"clockInstance, wallSynchronised", "the executor's own",
                 "the new executor's own"},
    HostStateRow{"audit sink, nextHealthCounter_", "the executor's, kept",
                 "the new executor's"},
    HostStateRow{"queued health actions and readings", "refuse the upgrade",
                 "refused earlier by checkpoint"},
    HostStateRow{"admission record", "kept until resumeAdmission or its ttl",
                 "not applicable"},
    HostStateRow{"source table (signed adapter)", "kept",
                 "lost; the host reloads its configuration and rebinds"},
    HostStateRow{"audience mirror (executor and adapter)", "kept",
                 "cleared; re-read from the arena at the first committed "
                 "source change"},
    HostStateRow{"delegation cache, wall high-water mark", "kept",
                 "lost; the next message re-validates and the mark restarts"},
    HostStateRow{"revocation registry", "kept",
                 "reloaded from the ledger; fail-closed until ready()"},
    HostStateRow{"heartbeat and report floors, observation rings", "kept",
                 "lost; replays fall through to the arena's floors and dedup"},
    HostStateRow{"per-peer budgets, pending settles", "kept", "lost"},
    HostStateRow{
        "flights, requests, schedule",
        "flights and requests refuse the schema road; schedule recomputed",
        "rebuilt; pending requests fail as migrated"},
};

bool contains(std::span<const std::string_view> names, std::string_view name) {
  return std::ranges::find(names, name) != names.end();
}

} // namespace

std::span<const OwnedStructure> allOwnedStructures() noexcept { return kOwned; }
std::span<const NotOwnedDimension> notOwnedDimensions() noexcept {
  return kNotOwned;
}
std::span<const HostStateRow> hostState() noexcept { return kHost; }

bool ownsProfileField(const std::string_view name) noexcept {
  return std::ranges::any_of(kOwned, [&](const OwnedStructure &entry) {
    return (entry.where == "profile field" || entry.where == "profile list") &&
           contains(entry.fields, name);
  });
}

bool ownsRecordField(const std::string_view name) noexcept {
  return std::ranges::any_of(kOwned, [&](const OwnedStructure &entry) {
    return entry.where == "record field" && contains(entry.fields, name);
  });
}

bool ownsDimension(const std::string_view name) noexcept {
  static constexpr std::array valueDims{
      std::string_view{"d.vars"}, std::string_view{"d.values"},
      std::string_view{"d.grab"}, std::string_view{"d.step"}};
  return contains(valueDims, name) ||
         std::ranges::any_of(kOwned, [&](const OwnedStructure &entry) {
           return entry.dimension == name;
         });
}

} // namespace xanadu::vagent::runtime
