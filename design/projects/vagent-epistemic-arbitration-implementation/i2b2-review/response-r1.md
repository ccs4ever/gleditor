# I2B-2 round 1 disposition (implementer)

Revised diff: /tmp/vagent-p19-i2b2/i2b2-r2.diff. vagent_runtime_test: all pass (70 Scoped\* tests).

Fixed:

- Uncharged field-name read (vortex M1, codebase M1): lengths compared from charged span list first;
  bytes debited before textOf. Field selectors are now `.name`; unknown words refuse TypeMismatch
  (codebase/vortex MINOR shadowing, architect MINOR typo).
- Order-dependent attribution (codebase M2, vortex m4, ML m7 tail): reach is a multimap recording
  every root; test ACellPinnedTwiceIsAttributedToBothRoots (fails under first-writer-wins).
- Reach budget (ML M2, architect MINOR): no longer capped by limits.fields; each record is charged
  as traversal work against the shared work grant.
- Destination alias (vortex m3): Point to a PinnedRef result refuses the destination record.
- Str Deref now debits the written copy; stepScopedStructure re-checks arity.
- Unreachable verifier line (vortex m7): Cursor-on-arg/result check now precedes the schema check.
- Tests: Backtracking test now parks the cursor on a real Float with a no-Fail twin returning Value
  (codebase M3, vortex m8); cross-root test has a positive twin; new tests for `target` (owned
  reference to pin, non-reference not found), unknown selector, non-finite stored Float and Ln(0)
  under an open Choice refusing (not Empty), and an end-to-end best-of-N with profile lookup by
  source name, Int alpha promotion, Float accumulator, max selection and PinnedRef export (ML M3,
  architect M3).
- ABI doc: selectors, provenance, numeric idioms, refused alternatives and why C++ (architect M2,
  MINOR), ranked/ties pattern.

Disputed / deferred:

- ML M1 (ties/ranked cannot be returned): spec §6.2 prescribes owner-held generator state with one
  scoped evaluation per resume yielding one binding, and `truest` fails on a tie; documented in the
  ABI doc. No list append is needed for the initial queries.
- Architect M1 (approval scope): giving existing opcodes scope-only meanings is recorded as an owner
  decision; the implementer will ask the owner before committing. Reviewers should judge everything
  else on its merits.
- Not changed: `target` to a non-pin handle yields found=false (documented); position() does not
  re-check assignment of nested pins' targets (no constructible case found by reviewers).
