/**
 * @file fuzz_epistemic_ingress.cpp
 * @brief Fuzz checked ingestion end to end (P19 I3-1b, I3-1c).
 *
 * The bytes decode into a script over a fixed program with one continuous
 * and one discrete arbitrated predicate and four principals: bind, unbind,
 * retire, rebind at another incarnation, set the wall clock synchronised or
 * not, observe, resend and pump. Observation fields are drawn adversarially.
 * After every observation the harness aborts unless:
 *
 * - its outcome is typed, and never left pending after a pump;
 * - a refusal or duplicate left every belief, support and record as it was;
 * - each (principal, key) has one support and at most one current record,
 *   of the profile's incarnation (`EpistemicView::violation`);
 * - the epistemic cells stay within the ceiling;
 * - resending committed bytes under an unchanged binding is `Duplicate`;
 * - a refused relayed copy changed neither health counters nor contact.
 *
 * One host and one compiled program serve the whole process, so an input
 * costs its own steps rather than a compilation; each input runs in a fresh
 * actor, which terminates at the end and takes its ingress state with it.
 * The limits are fixed and small, so eviction and budget paths are reached.
 *
 * Set FUZZ_EPISTEMIC_REPORT to print each outcome code, for checking that a
 * corpus reaches every refusal.
 */
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fuzzer/FuzzedDataProvider.h>

#include "../xuzz/epistemic_invariants.hpp"
#include "common/xanadu/vagent/runtime/program.hpp"

namespace {

using namespace xanadu::vagent::runtime;
using namespace xanadu::vagent::runtime::epistemic_test;

// Bounds one iteration's work; not a source-language limit.
constexpr std::size_t maxHarnessInputBytes = 4 * 1024;
constexpr int maxHarnessSteps              = 48;

constexpr auto kProgram = R"vagent(
predicate at(key who: Str, x: Float)
  arbitrated by truest(model: "beta_reputation", min_confidence: 0.5,
    prior_alpha: 1, prior_beta: 1, clock: "wall", max_age: 10s,
    cluster_tolerance: 0.5, frame: "enu", units: "m",
    min_variance: 0.0001, max_variance: 100.0)
predicate level(v: Float)
  arbitrated by truest(model: "beta_reputation", min_confidence: 0.5,
    prior_alpha: 1, prior_beta: 1, clock: "wall", max_age: 10s,
    cluster_tolerance: 0.5, frame: "enu", units: "m",
    min_variance: 0.0001, max_variance: 100.0)
predicate door(key id: Str, open: Bool)
  arbitrated by ranked(model: "beta_reputation", min_confidence: 0.5,
    prior_alpha: 2, prior_beta: 2, clock: "wall", max_age: 10s)
event Quit { }
agent A {
  on Quit { terminate }
  var adds: Int := 0
  on +at(?w, ?x) { adds := adds + 1 }
  on -door(?d, ?o) { adds := adds - 1 }
}
)vagent";

constexpr std::string_view kPredicates[] = {"at", "door", "level"};
constexpr std::string_view kKeyless[]    = {"level"};
const std::array<std::string, 4> kPrincipals{
    std::string(64, '1'), std::string(64, '2'), std::string(64, '3'),
    std::string(64, '4')};
const std::array<std::string, 5> kAliases{"gps", "odo", "door", "relay",
                                          "spare"};

void require(bool ok, const char *what) {
  if (ok) return;
  std::fprintf(stderr, "epistemic invariant broken: %s\n", what);
  std::abort();
}

double wallSeconds() {
  return std::chrono::duration<double>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

double pickDouble(FuzzedDataProvider &in) {
  constexpr std::array table{
      0.0,
      -0.0,
      0.25,
      1.0,
      100.0,
      100.00000001,
      0.0001,
      0.00009999,
      -1.0,
      1e-300,
      1e300,
      std::numeric_limits<double>::denorm_min(),
      std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::quiet_NaN(),
  };
  if (in.ConsumeBool())
    return table[in.ConsumeIntegralInRange<std::size_t>(0, table.size() - 1)];
  return in.ConsumeFloatingPoint<double>();
}

WireValue pickValue(FuzzedDataProvider &in) {
  switch (in.ConsumeIntegralInRange(0, 3)) {
  case 0:
    return WireValue::text(in.ConsumeBool() ? "self" : "other");
  case 1:
    return WireValue::real(pickDouble(in));
  case 2:
    return WireValue::integer(in.ConsumeIntegral<std::int64_t>());
  default:
    return WireValue::boolean(in.ConsumeBool());
  }
}

/// What the harness last told the executor about one principal, so most
/// observations are plausible and reach the ingestion turn's later steps.
struct SourceState {
  std::uint64_t incarnation{1};
  std::uint64_t sequence{0};
  std::uint64_t events{0};
};

CheckedObservation pickObservation(FuzzedDataProvider &in, double now,
                                   std::array<SourceState, 4> &sources) {
  CheckedObservation o;
  const auto which = in.ConsumeIntegralInRange<std::size_t>(0, 3);
  auto &source     = sources[which];
  o.principal      = in.ConsumeIntegralInRange(0, 15) == 0
                         ? std::string("not-a-token")
                         : kPrincipals[which];
  o.incarnation    = in.ConsumeIntegralInRange(0, 7) == 0
                         ? in.ConsumeIntegralInRange<std::uint64_t>(0, 3)
                         : source.incarnation;
  if (in.ConsumeIntegralInRange(0, 3) == 0)
    o.relayedBy = kPrincipals[in.ConsumeIntegralInRange<std::size_t>(0, 3)];
  // Mostly fresh, sometimes a reuse that is a retry or a conflict.
  o.eventId =
      in.ConsumeIntegralInRange(0, 15) == 0
          ? std::string{}
          : "e" + std::to_string(in.ConsumeIntegralInRange(0, 3) == 0
                                     ? in.ConsumeIntegralInRange<std::uint64_t>(
                                           0, source.events)
                                     : ++source.events);
  switch (in.ConsumeIntegralInRange(0, 7)) {
  case 0:
    o.sequence = in.ConsumeIntegral<std::uint64_t>();
    break;
  case 1:
    o.sequence = in.ConsumeIntegralInRange<std::uint64_t>(0, source.sequence);
    break;
  default:
    o.sequence = ++source.sequence;
  }
  const int predicate = in.ConsumeIntegralInRange(0, 5);
  o.predicate         = predicate < 2   ? "at"
                        : predicate < 4 ? "door"
                        : predicate < 5 ? "level"
                                        : "elsewhere";
  // Mostly the right shape, so ingestion's later steps are reached.
  if (o.predicate == "level" && in.ConsumeIntegralInRange(0, 3) != 0) {
    o.tuple = {WireValue::real(pickDouble(in))};
  } else if (in.ConsumeIntegralInRange(0, 3) != 0) {
    // Three keys, past the per-source key limit of two.
    constexpr std::array keys{"self", "other", "third"};
    o.tuple = {WireValue::text(
        keys[in.ConsumeIntegralInRange<std::size_t>(0, keys.size() - 1)])};
    if (o.predicate == "door")
      o.tuple.push_back(WireValue::boolean(in.ConsumeBool()));
    else
      o.tuple.push_back(WireValue::real(pickDouble(in)));
  } else {
    const auto arity = in.ConsumeIntegralInRange(0, 3);
    for (int i = 0; i < arity; ++i) o.tuple.push_back(pickValue(in));
  }
  constexpr std::array offsets{-5.0, -1.0,  -0.5, -0.01, 0.0,
                               -9.9, -11.0, 0.5,  30.0,  -1000.0};
  o.acquiredAt          = in.ConsumeIntegralInRange(0, 7) == 0
                              ? pickDouble(in)
                              : now + offsets[in.ConsumeIntegralInRange<std::size_t>(
                                 0, offsets.size() - 1)];
  o.acquiredUncertainty = in.ConsumeIntegralInRange(0, 7) == 0
                              ? pickDouble(in)
                              : 0.01 * in.ConsumeIntegralInRange(0, 3);
  o.clockBasis = in.ConsumeIntegralInRange(0, 7) == 0 ? "actor_mapped" : "wall";
  o.clockEpoch = in.ConsumeIntegralInRange<std::uint64_t>(0, 1);
  o.clockMappingVersion = in.ConsumeIntegralInRange<std::uint64_t>(0, 1);
  o.frame               = in.ConsumeIntegralInRange(0, 7) == 0 ? "ecef" : "enu";
  o.units               = in.ConsumeIntegralInRange(0, 7) == 0 ? "ft" : "m";
  if (o.predicate != "door" ? in.ConsumeIntegralInRange(0, 7) != 0
                            : in.ConsumeIntegralInRange(0, 7) == 0)
    o.variance = in.ConsumeBool() ? 0.25 : pickDouble(in);
  return o;
}

LocalHost::Options harnessOptions() {
  LocalHost::Options options;
  auto &limits                 = options.settings.epistemic;
  limits.maxSources            = 2;
  limits.dedup                 = 2;
  limits.history               = 2;
  limits.keysPerSource         = 2;
  limits.ingressQueuePerSource = 2;
  limits.cells                 = 4096;
  return options;
}

LocalHost &sharedHost() {
  static LocalHost host = [] {
    auto program = CompiledProgram::fromSource(kProgram);
    require(program.has_value(), "the fixed program compiles");
    return LocalHost(std::move(*program), harnessOptions());
  }();
  return host;
}

/// Refusals the executor answers by return value only, without logging.
bool unlogged(EpistemicRefusal refusal) {
  return refusal == EpistemicRefusal::UnknownSource ||
         refusal == EpistemicRefusal::InvalidObservation;
}

std::string codeOf(const IngestOutcome &o) {
  switch (o.kind) {
  case IngestOutcome::Kind::Pending:
    return "Pending";
  case IngestOutcome::Kind::Committed:
    return "Committed";
  case IngestOutcome::Kind::Duplicate:
    return "Duplicate";
  case IngestOutcome::Kind::Refused:
    return o.code.value_or("Refused");
  }
  return "?";
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data,
                                      std::size_t size) {
  if (size > maxHarnessInputBytes) return 0;
  FuzzedDataProvider in(data, size);
  static const bool report = std::getenv("FUZZ_EPISTEMIC_REPORT") != nullptr;

  auto &host                  = sharedHost();
  static std::uint64_t inputs = 0;
  const auto limits           = harnessOptions().settings.epistemic;
  const auto pid = host.launch("A", "a" + std::to_string(++inputs));
  require(pid.has_value(), "the agent launches");
  host.pump();
  auto &executor   = host.executor();
  const double now = wallSeconds();

  // Changes to authorization since a commit excuse a resend that is not
  // `Duplicate`.
  std::uint64_t generation = 0;
  struct Committed {
    CheckedObservation observation;
    std::uint64_t generation;
  };
  std::optional<Committed> last;
  std::array<SourceState, 4> sources{};
  // Executor-wide, so it must not carry over from the previous input.
  executor.setWallClockSynchronised(true);
  for (std::size_t i = 0; i < kPrincipals.size(); ++i)
    for (const auto *predicate : {"at", "door", "level"})
      require(executor
                  .bindSource(*pid, {std::string(predicate) + std::to_string(i),
                                     kPrincipals[i], 1, predicate,
                                     std::chrono::hours(1)})
                  .has_value(),
              "the harness's own bindings are accepted");
  host.pump();

  const auto deliver = [&](const CheckedObservation &o) {
    const EpistemicView before(executor, *pid, kPredicates, kKeyless);
    const auto snapshot = before.snapshot();
    const auto duplicateStat =
        std::string(kStatRefusedPrefix) + std::string(kTurnResultDuplicate);
    const auto duplicates = executor.stat(*pid, duplicateStat);
    // Like a duplicate, a conflicting resend leaves the committed entry, so
    // report mode reads it from its stat.
    const auto conflictStat = std::string(kStatRefusedPrefix) +
                              std::string(kTurnResultConflictingEvent);
    const auto conflicts = executor.stat(*pid, conflictStat);
    const auto receipt   = executor.observe(*pid, o);
    host.pump();
    if (report && !receipt)
      std::fprintf(stderr, "receipt %s\n",
                   epistemicRefusalName(receipt.error()).data());
    if (report && executor.stat(*pid, duplicateStat) > duplicates)
      std::fprintf(stderr, "outcome Duplicate\n");
    if (report && executor.stat(*pid, conflictStat) > conflicts)
      std::fprintf(stderr, "outcome ConflictingEvent\n");
    const EpistemicView after(executor, *pid, kPredicates, kKeyless);
    const auto broken = after.violation();
    if (broken) std::fprintf(stderr, "%s\n", broken->c_str());
    require(!broken, "ingestion invariants");
    require(*executor.epistemicCells(*pid) <= limits.cells, "cell ceiling");
    // Unbound and malformed input is answered by the return value alone, and
    // writes nothing anywhere.
    if (!receipt && unlogged(receipt.error())) {
      if (report)
        std::fprintf(stderr, "outcome %s\n",
                     epistemicRefusalName(receipt.error()).data());
      // Beliefs only: InvalidObservation is also the logged refusal of a
      // bound source's bad tuple, which is charged and may earn a profile.
      require(after.snapshot().beliefs == snapshot.beliefs,
              "a refusal before queueing changes no belief");
      return std::optional<IngestOutcome>{};
    }
    const auto outcome =
        executor.ingestOutcome(*pid, o.principal, o.incarnation, o.eventId);
    if (!outcome)
      std::fprintf(stderr, "no outcome: %.8s inc %llu event '%s' %s%s\n",
                   o.principal.c_str(),
                   static_cast<unsigned long long>(o.incarnation),
                   o.eventId.c_str(), receipt ? "queued" : "refused ",
                   receipt ? "" : epistemicRefusalName(receipt.error()).data());
    require(outcome.has_value(), "every observation has an outcome");
    require(outcome->kind != IngestOutcome::Kind::Pending,
            "no outcome is left pending after a pump");
    // A refused resend of an event the store holds keeps its entry.
    require(receipt || outcome->kind != IngestOutcome::Kind::Pending,
            "an ingress refusal leaves no pending entry");
    if (report) std::fprintf(stderr, "outcome %s\n", codeOf(*outcome).c_str());
    const auto later = after.snapshot();
    if (outcome->kind != IngestOutcome::Kind::Committed)
      require(later.beliefs == snapshot.beliefs, "a refusal writes nothing");
    if (o.relayedBy && outcome->kind == IngestOutcome::Kind::Refused) {
      const auto was = snapshot.health.find(o.principal);
      const auto is  = later.health.find(o.principal);
      if (was != snapshot.health.end() && is != later.health.end())
        require(was->second == is->second,
                "a refused relayed copy leaves health and contact alone");
    }
    // A refusal before queueing committed nothing, whatever an earlier
    // event under the same ID left in the log.
    return receipt ? outcome : std::optional<IngestOutcome>{};
  };

  for (int step = 0; step < maxHarnessSteps && in.remaining_bytes() > 0;
       ++step) {
    const auto which     = in.ConsumeIntegralInRange<std::size_t>(0, 3);
    const auto principal = kPrincipals[which];
    switch (in.ConsumeIntegralInRange(0, 13)) {
    case 12:
    case 13: {
      // Scripted steps into states random bytes rarely reach: a principal
      // past its key limit, and an event ID reused with other bytes.
      auto good = [&](std::string_view key, double x) {
        auto &state = sources[which];
        CheckedObservation o;
        o.principal   = principal;
        o.incarnation = state.incarnation;
        o.eventId     = "e" + std::to_string(++state.events);
        o.sequence    = ++state.sequence;
        o.predicate   = "at";
        o.tuple       = {WireValue::text(std::string(key)), WireValue::real(x)};
        o.acquiredAt  = now - 1.0;
        o.acquiredUncertainty = 0.01;
        o.clockBasis          = "wall";
        o.frame               = "enu";
        o.units               = "m";
        o.variance            = 0.25;
        return o;
      };
      if (in.ConsumeBool()) {
        for (const auto *key : {"self", "other", "third"})
          (void)deliver(good(key, 1.0));
      } else {
        const auto first = good("self", 1.0);
        (void)deliver(first);
        auto conflicting  = first;
        conflicting.tuple = {WireValue::text("self"), WireValue::real(2.0)};
        (void)deliver(conflicting);
      }
      last.reset();
      break;
    }
    case 0: {
      const bool at = in.ConsumeBool();
      // Usually the principal's own alias, so a rebind is a supersession.
      const auto alias =
          in.ConsumeIntegralInRange(0, 3) == 0
              ? kAliases[in.ConsumeIntegralInRange<std::size_t>(0, 4)]
              : std::string(at ? "at" : "door") + std::to_string(which);
      EpistemicSourceGrant grant{
          alias, principal, in.ConsumeIntegralInRange<std::uint64_t>(0, 4),
          at ? "at" : "door",
          in.ConsumeIntegralInRange(0, 7) == 0 ? -std::chrono::seconds(1)
                                               : std::chrono::hours(1)};
      if (executor.bindSource(*pid, grant) &&
          grant.incarnation > sources[which].incarnation)
        sources[which] = {grant.incarnation, 0, sources[which].events};
      host.pump();
      ++generation;
      break;
    }
    case 1:
      executor.unbindSource(*pid, principal, in.ConsumeBool() ? "at" : "door");
      ++generation;
      break;
    case 2:
      (void)executor.retireSource(*pid, principal);
      host.pump();
      ++generation;
      break;
    case 3:
      executor.setWallClockSynchronised(in.ConsumeIntegralInRange(0, 3) != 0);
      ++generation;
      break;
    case 4:
      if (last) {
        const auto statName =
            std::string(kStatRefusedPrefix) + std::string(kTurnResultDuplicate);
        const auto duplicates = executor.stat(*pid, statName);
        const auto beliefs =
            EpistemicView(executor, *pid, kPredicates, kKeyless).snapshot();
        const auto again = deliver(last->observation);
        // A resend never changes what is believed, and the committed entry
        // stays committed.
        require(EpistemicView(executor, *pid, kPredicates, kKeyless)
                        .snapshot()
                        .beliefs == beliefs.beliefs,
                "resending committed bytes writes nothing");
        // Bytes acquired near max_age may age out between the two sends.
        if (last->generation == generation &&
            last->observation.acquiredAt > now - 9.0) {
          require(again && again->kind == IngestOutcome::Kind::Committed,
                  "a committed outcome stays committed");
          require(executor.stat(*pid, statName) == duplicates + 1,
                  "resending committed bytes is a duplicate");
        }
      }
      break;
    case 5: {
      // Several observations before one pump: the per-source queue share
      // of two pushes back, and the queue drains in one go.
      const EpistemicView before(executor, *pid, kPredicates, kKeyless);
      const auto snapshot = before.snapshot();
      std::vector<CheckedObservation> batch;
      for (int i = in.ConsumeIntegralInRange(2, 5); i > 0; --i) {
        auto o = pickObservation(in, now, sources);
        if (const auto receipt = executor.observe(*pid, o);
            receipt || !unlogged(receipt.error()))
          batch.push_back(std::move(o));
      }
      host.pump();
      const EpistemicView after(executor, *pid, kPredicates, kKeyless);
      require(!after.violation(), "ingestion invariants after a burst");
      bool changed = false;
      for (const auto &o : batch) {
        const auto outcome =
            executor.ingestOutcome(*pid, o.principal, o.incarnation, o.eventId);
        require(outcome && outcome->kind != IngestOutcome::Kind::Pending,
                "every observation of a burst has an outcome");
        if (report)
          std::fprintf(stderr, "outcome %s\n", codeOf(*outcome).c_str());
        changed |= outcome->kind != IngestOutcome::Kind::Refused;
      }
      if (!changed)
        require(after.snapshot().beliefs == snapshot.beliefs,
                "a refused burst writes nothing");
      last.reset();
      break;
    }
    default: {
      const auto o = pickObservation(in, now, sources);
      if (const auto done = deliver(o);
          done && done->kind == IngestOutcome::Kind::Committed)
        last = Committed{o, generation};
      break;
    }
    }
  }
  host.percept(*pid, "Quit");
  host.pump();
  require(executor.context(*pid) == nullptr, "the input's actor stops");
  return 0;
}
