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
