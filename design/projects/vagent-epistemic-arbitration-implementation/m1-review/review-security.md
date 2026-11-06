# M1 security review (persona sec)

Range `9bd27a03..61e57eae`. Verdict: **FAIL** (2 MAJOR open, 1 more MAJOR on the translator budget
gate). Method: read the range for health grants, F14, audit sink, budgets, translator, registry,
coverage script. Ran `xuzz_test --gtest_filter='VagentEpistemicGrants*'` (18 pass; the binary
predates HEAD, so not authoritative). No source edited.

## MAJOR

**S-1. The audit sink runs with `ingress_->mutex` held on the abandon path (R3-10 clause 1).**
`agent_executor.cpp:4160-4200`. `drainPosted`'s `abandon` lambda takes
`std::lock_guard lock(ingress_->mutex)` at :4162 and, still holding it, calls
`finishHealthItem(...)` at :4186 and :4198. `finishHealthItem` invokes the sink
(`agent_executor_health.cpp:594-614`). The contract says no executor lock is held and that the sink
may call `revokeHealthGrant`, which locks `ingress_->mutex` (`agent_executor_health.cpp:~125`). The
mutex is a plain `std::mutex` (`agent_executor_internal.hpp:200`). Scenario: an actor stops or is
dropped with a queued health item; the host's sink calls `revokeHealthGrant` or `healthOutcome` to
tidy up; the pump thread self-deadlocks on a lock it holds, and every reading, bind and health call
blocks behind it. A slow sink also stalls ingress for all actors. Inside the lock `finishHealthItem`
also takes `healthMutex_`; the order is ingress then health, so it is consistent, but the sink is
the problem. Fix: collect the health items in `abandon` and call `finishHealthItem` after the guard
is released (as :1978 does). Add the missing R3-10 test: a sink that calls `revokeHealthGrant`
against an `Undelivered` item. `AuditSinkSeesEveryCommittedOrRefusedAction` and
`ThrowingAuditSinkDoesNotBreakTheTurn` are the only sink tests; none covers no-lock, at-most-once,
or the null sink.

**S-2. The translator and the health reserve trust profile field values from the image.**
`epistemic_translate.cpp:283-345` (plan) checks field names and the presence of `incarnation`,
`streams` and `dedup`. It never checks a field's kind or range. `AgentExecutor::epistemicReserve`
(`agent_executor.cpp:3352-3354`) reads `health_cells` straight from the arena:
`reserve += max(bound - held, 0)`. Scenarios, from a crafted checkpoint or an old graph:

- `health_cells = INT64_MAX` on every profile makes the reserve 0, so ordinary ingestion fills the
  whole ceiling and the health bound that epi M2 charges to live profiles is not charged. A later
  in-profile reason or action then has only the 1/16 headroom.
- `health_cells = INT64_MIN` makes `bound - held` signed overflow, which is undefined behaviour in
  the admission path.
- Non-finite or negative `health_weight` and `health_refused_weight`, a huge `incarnation`, a
  `reasons` list longer than `healthReasons` (the translator does not compare against the new
  settings), or forged `reasons` entries with `origin=operator`, `issuer` and `grant` all copy
  through. The first reader to treat an operator reason as authoritative is fed a forgery. The
  checkpoint is host-trusted, but the task and the plan treat "crafted image" as in scope. Fix: in
  `planTranslation`, validate kinds and ranges for the profile scalars (finite, >= 0 weights;
  `health_cells` in `[kHealthFixedCells, healthBound]`; reasons and actions lengths within the
  settings, with the entry fields typed). Better, recompute `health_cells` from the copied lists
  rather than trusting it. Refuse with `StateInconsistent`.

**S-3. The scratch-bytes gate under-counts, so a typed pre-write refusal becomes an untyped late
throw.** `distinctTextBytes` (`epistemic_translate.cpp:~200`) adds a text only when
`valueKindOf(cell) == ValueKind::None`. `Copier::copyCell` (:471-476) interns `textOf` for every
cell, including Int64, Double and Bool cells, whose canonical text is up to about 24 bytes. A graph
with many distinct numeric texts, such as weights, passes `PeakCopyBudget` and then hits
`ArenaManifold::intern` "byte budget exhausted" (`arena_manifold.cpp:1179`) mid-copy. That is a
`std::runtime_error`, not a `Refused`, and `applyTranslation` catches only `Refused`. On the resume
path `prepare` logs at DEBUG and returns false (`agent_executor.cpp:1004`). There is no
`lastRefusal`, so the host gets no typed cause. On the upgrade path it rolls back, but the header
promises "Read-only: refuses before anything is written". The same gate also checks only the
translator's texts against the whole 32 KiB, while `UpgradeGraph::preflight` independently checks
the generic copier's bytes against the same 32 KiB, so the two together can reach 64 KiB. The arena
hard cap still holds, so this is not a bypass. Fix: count every cell's `textOf` in
`distinctTextBytes`; pass the generic copier's already-used bytes in as `fixedBytes` (as `fixedWork`
does for instructions); catch `std::exception` in `applyTranslation` and map it to
`PeakCopyBudget`/`StateInconsistent`.

## MINOR

**S-4. Refusal subjects carry text from the image, against the "never document text" rule.**
`epistemic_translate.cpp:294,336` (`subject = token`, the raw `textOf(profile)` when it is not a
token), `:360` (`textOf(anchor)`, a support's text), and `:487,490,521,559,581`
(`inconsistent( from_.textOf(old))`, any cell: a field value, a handle's text).
`kRefusalTextLimit`'s comment (`epistemic_refusal.hpp:41`) says only predicate names, tokens,
hashes, field names and reason names go in. `cut()` bounds the length to 128 but not the content,
and `toString` embeds it unescaped into the `what()` text that `prepare` logs at WARN
(`agent_executor.cpp:994`). Control characters allow log injection; cell text can be document or key
material. Fix: in these sites put a fixed label (dimension or field name, cell role) or a hash
prefix in `subject`. Escape or strip control bytes in `TransferRefusal::make`.

**S-5. F14 and the bind check are two critical sections, against the design.** `bindSource` checks
issuers under one lock at `agent_executor.cpp:3586-3594` and releases it. It takes the lock again at
:3600 to insert. Design 5.1 says check and change are one critical section. The comment on
`grantHealth` says it runs on the pumping thread, and if `bindSource` also does, the race is
unreachable, but nothing enforces it (no assert, no doc on `bindSource`). Fix: one scope, or
document and assert the thread. Only two F14 cases are tested (`grants_test.cpp:171-181`: issuer in
`sources`, issuer bound before the grant). The design table (`i3-2-design.md:1019`) also lists bind
after grant, a profile or tombstone on another actor, after a resume, uppercase hex, and the
turn-time repeat. Add them.

**S-6. `pauseAdmission(pid, ttl)` does not clamp an explicit ttl.** `agent_executor.cpp:5829`. The
settings field is bounded to 1 s..10 min, but a caller ttl goes through `saturatingAdd` unclamped.
`admissionClosedLocked` also refuses `healthAction`, `healthActionBatch`, `bindSource` and
`retireSource`. A forgotten `pauseAdmission(pid, hours{9999})` therefore blocks an operator's
isolate until `resumeAdmission`. Fix: clamp to `[kMinAdmissionPauseMs, kMaxAdmissionPauseMs]`, or
let the safety actions (isolate, reset) through a pause.

**S-7. A Degrade on a tombstone draws on the exempt headroom (`agent_executor.cpp:1692-1694`).** A
degrade-only grant is the lowest privilege (design 6.4 keeps it from evicting isolates), yet it can
drain the 1/16 headroom through tombstoned sources (about 36 cells per reason and action entry
against 1,536 headroom cells at defaults), until an isolate or reset on a live profile of a full
arena returns `EpistemicBudget`. R3-1 as written exempts "any action on a tombstone"; a degrade is
not a repair. Fix: exempt only Clear (and Reset) on tombstones.

**S-8. Smaller items.**

- `epistemicStateDigest` clone-chain loops (`epistemic_translate.cpp:~720,736,756`) have no cycle
  guard, unlike `planTranslation` (:332-337), which refuses a re-entered view. It is test-only
  today. Guard it before any production caller.
- At live-swap time the plan is re-run with `allowance = nullopt` (`agent_executor.cpp:6087`), so
  cell, work and scratch budgets are decided only in the earlier pre-flight. The pre-flight's
  `fixedWork` (`:5767`) counts the schema comparison but not `UpgradeGraph` charges, so an upgrade
  can pass pre-flight and fail `WorkBudget` inside the callback (rolled back, but after the pause).
- `kTransferWorkPerCell` = 16 is the tight worst case I derive from the passes (record cell: 1 + 1 +
  8 + 6 = 16; name cell 8; profile 9). It has no margin. Any added charge (a new pass-3 edge, for
  example) makes plan work under-count and turns apply into a `WorkBudget` refusal. Pin it with a
  test on an adversarial shape, not only the measured 14.8.
- `tools/check-translator-coverage.py`: the field check is satisfied by any `\bkName\b` mention in
  `epistemic_translate_registry.cpp`, comments included, and by the `kNoFields`-style arrays without
  checking the entry's `Fate`. It scans only `inline constexpr std::string_view` constants, only
  `ensureDimension("d.x")` string literals, and only the top-level `runtime/*.cpp` and `.hpp` files.
  A dimension minted by variable name, or in another directory, passes silently. It runs in
  `make test` only, not `lint`/CI (`Makefile` `test/translator-coverage`). As a guard it is adequate
  for the current tree; a stronger check would grep each constant inside an `OwnedStructure` field
  array.
- The translator fences dimensions only on profile and record cells (`checkDims`). Name and value
  cells reached through `vars`/`values`/`grab`/`step` are not fenced. Unregistered edges there are
  dropped silently, except `d.clone` and `d.epistemic` edges, which pass 3 refuses. No forged owned
  state is created, but a field value can link into other agent data and have it cloned under
  `d.trust` (bounded by `plan.cells`). Worth one `checkDims` on value cells, or a test pinning the
  behaviour.

## Confirmations

- epi M2 (R3-1): PARTLY. Ordinary admission adds `epistemicReserve` (live profiles only, tombstones
  skipped) and exempt actions do not (`agent_executor.cpp:3343-3367`); isolate, clear, reset and
  settle draw on the headroom. The reserve depends on the unvalidated `health_cells` (S-2), and
  tombstone-degrade is exempt (S-7).
- sec M3 (R3-1): CONFIRMED. `admitCells` returns `EpistemicBudget` past the headroom
  (`agent_executor.cpp:3359-3369`) and I found no `2 x max_sources` cap in the range. The
  `OrdinaryAdmissionReservesTheHealthBound` and `AtTheSlotReserveEveryHealthEntryRefuses` tests
  exist. Caveat: S-2.
- sec M1 (R3-5): CONFIRMED at the ingress. A dropped reading raises the mark once, only on a
  committed stream and within `healthSequenceGap` (`agent_executor.cpp:3996-4012`); refusals do not
  raise it. Tests: `ReplayedRefusalsBelowTheMarkCountNothing`, `ACreditedResendIsNotCountedTwice`,
  `ALateReadingBelowTheMarkUpdatesNothing`, `OneDroppedReadingCannotSetTheFirstMark`. I did not
  re-derive the Vortex-side arithmetic.
- sec M2 (R3-10): FINDING S-1. Serialized install (`auditSinkMutex_`), null sink with WARN, call and
  throw counts, exception containment and no secret in `HealthAudit` (`removedText` is never set,
  and the token and digest are absent) are confirmed. "No lock held" is false on the undelivered
  path, and the other clauses have no tests.

F14 (focus item): `grantHealth` (`agent_executor_health.cpp:84-101`), `bindSource`
(`agent_executor.cpp:3586-3594`) and the turn-time recheck (`agent_executor.cpp:1523-1545`) all
check host-wide on `isPrincipalToken` canonical tokens. Revive goes through `bindSource`. Issuer
tokens are lowercase-hex-only by construction (`HealthIssuer::fromPrincipal`). Residual: S-5. Grant
tokens are 128-bit `RAND_bytes`, compared by digest with `CRYPTO_memcmp`, zeroised on destruction,
and the per-grant action budget is decremented at turn time. No finding.
