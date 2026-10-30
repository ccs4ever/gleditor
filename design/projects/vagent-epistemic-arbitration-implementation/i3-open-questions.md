# I3-1 open questions: options

Three questions the I3-1b/1c review left for I4 and I3-2 ([design](i3-design.md) section 14). Each
lists what happens today, the options with their costs, and a recommendation. None blocks I3-1.

## Q1. Whether a support outlives its last current record

**Today.** An advance retires every record of the predecessor's incarnation but leaves the supports
on their clauses. Until the new boot reports the same key, the clause is still believed: a plain
query of `at(?w, ?x)` and `+at`/`-at` handlers see the predecessor's value as held, and only I4's
eligibility rule (a current record whose incarnation is the profile's) keeps it out of arbitration.
Supersession never causes this; only an advance does (a retire drops the supports).

**Options.**

1. **Keep the support (today).** No `-at`/`+at` churn when a device reboots and reports the same
   value; the history stays on its support for calibration. Cost: an arbitrated fact can be held
   with no current reading behind it, so the plain belief and the arbitrated answer disagree until
   the next report or until I4's query-time `max_age` excludes it.
1. **Drop the support in the advance turn**, through `rtBDrop`, settling `-at`. The belief then
   means "a current boot asserts it". Cost: a reboot raises `-at` and the first new reading `+at`
   again; and the dropped support's history needs a home, or excision can no longer find it (the
   records stay reachable only through clone occurrences). A home is a retired-history rank on the
   profile (a few cells per key, excised by the same history bound).
1. **Age it out.** Drop a support once its newest record is older than `max_age`, from the ingestion
   or a timer turn. The belief then means "recently asserted", the same rule I4 applies at query
   time. Cost: a timer per actor and a turn when it fires; the belief changes with no event from the
   source.
1. **Make plain queries of an arbitrated predicate go through arbitration** (I4). Raw supports are
   then bookkeeping that programs never read directly, and only `+at`/`-at` remain to define. Cost:
   it is a checker and lowering decision for I4, and the event semantics still need one of 1–3.

**Decided (owner).** The recommendation. Option 2 for events, keeping the history, came first (round
2: the advance drops the supports and moves their records to a retired-history chain on the profile,
design section 14); option 4 for queries follows in I4, where a query of an arbitrated predicate is
the arbitrated answer and programs never read raw supports.

**Recommendation.** 4 for queries, with 2 for events: in I4, a query of an arbitrated predicate is
the arbitrated answer, and the advance drops supports left with no current record so that `-at`
means what it says, moving their history to a retired-history rank on the profile. Calibration keeps
the records; programs never see a predecessor's reading as held.

## Q2. Health counters: per incarnation, windowed or lifetime

**Today.** `content_refusals`, `refused_count`, `reject_count`, `rejection_streak` and the `faulted`
latch are lifetime-cumulative on the profile: an advance keeps them (so a restart cannot shed a
fault) and a revived tombstone inherits them.

**Options.**

1. **Lifetime (today).** Simple, and a replaced device cannot launder its record by rebooting. Cost:
   a device that was repaired carries its old faults forever, and rates computed from totals are
   dominated by the past.
1. **Per incarnation.** Reset the counters on advance and keep the previous incarnation's totals in
   a bounded list on the profile. Cost: a reboot clears the evidence a health rule acts on, which is
   exactly what the design forbade for latches; cells per reboot, bounded by the list length.
1. **Windowed.** Keep each counter as an exponentially decayed rate with the policy's `decay` (or a
   health half-life), updated with `Exp` (I1) at each charge from the time since the last. Recent
   faults dominate, old ones fade, and a reboot changes nothing. Cost: a timestamp per counter, and
   a half-life to choose and review at the ML gate.
1. **Latches lifetime, counters windowed.** The `faulted` latch and the degradation reasons stay
   until the reset authority I3-2 introduces clears them; the counters decay.

**Decided (owner).** The recommendation, option 4, for I3-2: latches stay until the reset authority
clears them, and counters become exponentially decayed rates whose half-life is reviewed at the ML
gate.

**Recommendation.** 4. It keeps the design's rule that a restart cannot shed a fault (the latch),
lets a repaired device recover on evidence (the decayed counters), and needs no per-incarnation
storage. The half-life is a policy or system-setting value with its own ML-gate review.

## Q3. What a source refused from its first event charges

**Today.** Refusals are charged to a profile, and the ingestion turn creates the profile only on a
commit. A bound source whose every event is refused (a misconfigured frame, a broken clock) has no
profile: the outcome log shows the refusals, but its counters start clean at its first commit, so it
looks better than a source that was right and then slipped.

**Options.**

1. **Nothing (today).** No cells for sources that never commit. Cost: health is blind to a source
   that is wrong from the start.
1. **Create the profile on the first chargeable refusal** of a bound source: the bookkeeping turn
   creates it (within `max_sources` and the cell budget) when absent. A binding is the host's
   authorization, so a bound source earning a profile by sending refused events is no different from
   earning one by sending accepted ones. Cost: a profile (about 20 cells) per bound source that has
   sent anything, counted against `max_sources`.
1. **Keep the charges at the host until the profile exists**, and apply them in the first commit's
   bookkeeping. No arena cost until a commit. Cost: host state that is not checkpointed, so a resume
   loses them, and a source that never commits is never charged.

**Decided (owner).** The recommendation, option 2, implemented after I3-1's acceptance (design
section 14).

**Recommendation.** 2. It is bounded by bindings and `max_sources`, it survives checkpoints, and it
makes the first commit's health honest; the cost is cells the budget already accounts for.
