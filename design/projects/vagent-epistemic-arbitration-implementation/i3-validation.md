# I3 validation

I3-1 lands in the slices of its [accepted design](i3-design.md). Each slice records its review and
evidence here.

## I3-1a: arbitration policy syntax and checker

Source baseline: `3e77f522` (accepted I3-1 design).

- **Syntax.** `arbitrated by truest|ranked|consensus(key: literal, ...)` on predicate declarations.
  `arbitrated` is contextual, so it and the strategy words stay usable as names. A leading minus is
  part of a numeric or duration constant.
- **Typed policy.** `epistemic_policy.{hpp,cpp}` holds the closed key table, the typed
  `ArbitrationPolicy`, every domain of design section 2 and a versioned canonical encoding with its
  SHA-256 hash (`vagent-policy-1`).
  - `score_tolerance` resolves to 1e-9 and `allow_untested` to false, so the hash records the value
    in force.
  - The encoding normalises -0.0.
- **Checker.** `InvalidEpistemicPolicy`, `MissingClusterTolerance` and `EpistemicIngressOnly`.
  `believe`, `forget`, `revise` and `believes ... from` are refused on arbitrated predicates.
- **Compiler.** It mints a policy master record on the predicate's `d.epistemic` rank, after the
  duplicate-name check. It emits no `tell`/`untell` test for an arbitrated predicate and guards both
  `rtBSet_` call sites.
- **Upgrade schema and CLI.** The upgrade schema carries the full policy and its hash. A changed,
  added or dropped policy is the refused `EpistemicPolicy` change, which
  `vagent check-runtime --upgrade` reports.

Review (sonnet personas; reports in [i3-1a-review/](i3-1a-review/)):

| Persona           | Round 1        | Round 2        |
| ----------------- | -------------- | -------------- |
| Compiler/codebase | PASS (7 MINOR) | —              |
| ML gatekeeper     | FAIL (3 MAJOR) | PASS (4 MINOR) |

Round 1 ML findings and their fixes:

- **Unresolved `score_tolerance` default:** now resolved and hashed; `allow_untested` added.
- **Hash test that varied one value:** a table that mutates every field and every optional key's
  presence.
- **Untested domains:** a per-key boundary table.
- **Minors:** zero alignment allowed, exact-only integer widening, -0.0, duration and prior
  ceilings, and documented units.

Codebase minors fixed:

- **Negatives:** they now reach the domain rules instead of failing to parse.
- **Mint order:** the master is minted only after the duplicate-name check.
- **Missing include:** `<bit>` added.
- **Design text:** section 2 lists the full key table.
- **Tests:** resume, same-policy upgrade and changed-policy refusal are now tested.

Evidence, headless with isolated XDG state:

- `make -j$(nproc) vagent vagent_runtime_test vagent_actor_test build/xuzz_test`: runtime 186, actor
  623 (13 `VagentEpistemicPolicy`), xuzz 2680 passed.
- Mutation checks: emitting `tell` tests for arbitrated predicates fails
  `PeerTellCannotWriteAnArbitratedPredicate`.
- `fuzz_vagent_runtime_policy` (new, in `make fuzz`): about 1.15 million runs in three minutes, no
  crash and no accepted policy outside its domains or with a nondeterministic hash.
- Changed C++ formatted with clang-format 19.1.7.

Building the fuzzer exposed three defects, all fixed:

- Fuzz objects missed `layoutfilade.o` and `focus_manager.o`, so no fuzz target linked.
- Fuzz objects had no header dependencies: a stale sanitizer object produced a spurious crash after
  a layout change.
- libstdc++ 16's `ranges::stable_sort` trips ASan's alloc-dealloc check inside the library; only
  that check is relaxed, in this harness.

Carried MINORs from ML round 2:

- Float duration literals round to whole milliseconds in the lexer.
- `alignment` and `decay` are still accepted on discrete predicates; I4 decides whether discrete
  policies use them.
- The policy master test checks a subset of fields; the hash is the full guard.

I3-1a is accepted.

## I3-1b, I3-1c and I3-1d: ingestion, incarnations, checkpoint and resume

Source baseline: `00db472b` (accepted I3-1a). Slices: `57331549` (typed turn result spike),
`f60e14c3` (sentinel sweep), `70d1b278` to `bdf20382` (I3-1b in five increments), `5a908ea8`
(I3-1c), `4ad3f53a` (fuzzing and simulation), `e91e8fe8`, `0124b0d2` and `08425c60` (review fixes,
`max_uncertainty` and I3-1d), and the closing round-3 commit that adds this record.

- **Ingestion** is the compiled Vortex routine `observeEntry`, run by the VM in its own turn. It
  finds or creates the source's profile, then checks in order: incarnation, dedup, the stream's
  sequence, clock epoch and mapping, and per-key acquisition order. It adopts the executor's record
  as the history entry, keeps one current record and one support per (principal, key), bounds keys
  and history, and is admitted against the cell ceiling. Every exit writes a typed `$turn_result`.
  The design is sections 3–6 of [i3-design.md](i3-design.md); its section 14 records every change
  the reviews made.
- **Host ingress** binds sources with lifetimes, aliases and a synchronous incarnation floor. It
  refuses unbound and malformed input without logging it. It checks every observation-time rule,
  `max_uncertainty` among them, gives each source and relay a fair share of the queue, keeps one
  bounded outcome log per source in which a committed entry is never overwritten, and charges each
  refusal once to the profile in a Vortex bookkeeping turn.
- **Incarnations.** Only binding advances one. The advance turn drops the predecessor's supports,
  settling `-pred`, and keeps their records on a bounded retired history on the profile. The retire
  turn leaves a tombstone that holds the floor. The arena refuses a binding below its floor, which a
  resumed actor exercises.
- **Checkpoint and resume** carry profiles, streams, dedup windows, histories and clone ranks
  through one copy map, and drop steady liveness stamps.

Review (sonnet personas; reports in [i3-1bc-review/](i3-1bc-review/)):

| Persona           | Round 1 (`4ad3f53a`) | Round 2 (`d48e85f8`) | Round 3 (`08425c60`) |
| ----------------- | -------------------- | -------------------- | -------------------- |
| ML gatekeeper     | FAIL (4 MAJOR)       | PASS (7 MINOR)       | PASS (6 MINOR)       |
| Epistemology      | FAIL (2 MAJOR)       | FAIL (1 MAJOR)       | PASS                 |
| Hardware/fault    | FAIL (5 MAJOR)       | FAIL (1 MAJOR)       | PASS                 |
| Compiler/codebase | FAIL (2 MAJOR)       | FAIL (1 MAJOR)       | PASS                 |

The owner decided two questions along the way: a `max_uncertainty` policy key (hardware round-1
MAJOR 3), and that an advance drops supports left with no current record while keeping their history
(open question 1, which codebase round 2 made blocking). The other open questions are written up in
[i3-open-questions.md](i3-open-questions.md).

Evidence, headless with isolated XDG state:

- `make -j$(nproc) vagent vagent_runtime_test vagent_actor_test build/xuzz_test xuzz`: runtime 186,
  actor 700 (100 `VagentEpistemic*`), xuzz 2680 passed. Two capture-dependent drag tests and two
  timing benchmarks failed intermittently over the slice. Each passes when rerun alone, and none
  touches vagent code.

- **Mutation checks.** Each new guard was mutated and a test failed:

  - stale-acquisition and revocation-at-drain checks, refusal classification and contact refresh;
  - incarnation ordering, the arena floor, tombstone budget exemption and retire excision;
  - sticky log, fair share, per-wake cap and epoch regression;
  - current-only key count, synchronous floor and uncertainty bound;
  - resume copy map, call-order source changes, support drop and history trim;
  - charge-once.

  One mutation survived because a redundant guard covers it: moving the advance to the back of the
  post box.

- **Noisy-sensor simulator** (32 seeds, `GLEDITOR_NOISE_SEEDS=32`): invariants held at every step,
  with 4,047 commits and every noise class reached.

- **`fuzz_epistemic_ingress`** (in `make fuzz`): more than 60 minutes in total, about 2 executions
  per second. It found two executor defects (an unlogged non-finite reading, and the ingress state
  of a stopped actor), the lifetime-saturation overflow, and five harness corrections. Each is fixed
  and in the corpus as a regression input. The seed corpus reaches 17 outcome codes.

- **Cost.** An ingestion went from 12,096 to 4,362 VM cycles, and from about 670 to about 250 arena
  slots. A source rebooting with new keys plateaus at about 1,500 epistemic cells over 160 boots.

- Changed C++ is formatted with clang-format 19.1.7. Markdown passes mdformat and mdl. No `noCell`
  was added.

Carried, recorded in design section 14:

- a failing `-pred` handler blocks the advance and retire for its source;
- the retired history is per profile and not age-ordered within one boot;
- health counters stay lifetime-cumulative (open question 2, I3-2);
- a source refused from its first event has no profile (open question 3);
- handler frames cost about 85 slots per activation, a compiler-wide cost;
- the fuzzer launches an actor per input.

I3-1b, I3-1c and I3-1d are accepted.
