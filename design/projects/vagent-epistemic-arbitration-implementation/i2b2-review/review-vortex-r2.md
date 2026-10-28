# I2B-2 round 2: Vortex/VM persona

I read the revised code and tests only; nothing was built or run. File abbreviations: VSE =
`vortex_scoped_evaluation.cpp`, SE = `scoped_evaluation.cpp`.

## Closure of round-1 findings

1. Field-name text read (was MAJOR): CLOSED.
   - `stepScopedStructure` now sums `contentOf` span lengths with a charge per span.
   - It calls `textOf` only when the length equals the wanted length, and `debitBytes` runs first.
   - The pairing is the same one `ScopeCodec::string` uses.
   - Residual, trivial: `contentOf` is walked before the `valueKindOf == None` test. That is
     harmless, because it is charged.
1. `position()` does not check `assigned()` on the dereferenced target: still open, still MINOR.
   - The implementer's "no constructible case" is plausible and I could not build one either.
   - A one-line `assignmentIndex` lookup on the target would remove the question.
1. Destination alias: PARTIALLY closed.
   - The new `from != f.destination` check on Point to a PinnedRef result removes the
     destination-root cycle.
   - Cells on the destination's old `vars` chain (name, marker and value cells that `replaceRecord`
     detaches) can still be published.
     - Scenario: pin = destination, `.payload` reaches the old value cell, Point to the result.
     - The result is an orphaned but valid cell, with no cycle, so it is not unsafe.
   - No test exercises the new refusal.
1. Order-dependent attribution: CLOSED.
   - `reach` is a multimap and `reached()` records every root, deduplicated.
   - `snapshot` accepts if any entry matches the declared root.
   - `ACellPinnedTwiceIsAttributedToBothRoots` covers it.
   - The `target` branch correctly replaces `roots` with the target pin's roots.
1. Selector shadowing and the silent `found=false` on an unknown word: CLOSED.
   - Field names must be `.name`, and unknown words refuse with TypeMismatch.
   - A field name that is empty after the dot cannot be selected. This is acceptable.
   - The `target` selector still reaches only already-pinned cells, which is now documented.
1. Skipped `cloneMaster` check and an inconsistent dead-reference code: unchanged, MINOR.
1. Dead verifier line: CLOSED. The Cursor-must-be-temp check now runs before the schema branch.
1. Tests: mostly CLOSED.
   - The cross-root test has a positive twin.
   - The backtracking test parks the cursor on a real Float, with a Fail-free twin that succeeds.
     The failing branch can only refuse through the restored unassigned state.
   - `target` and unknown-selector tests exist.
   - Still no tests for: the destination check, reach through a nested scope, the work-charged
     reach, or Deref Int/Bool/Str positives.
     - The best-of-N test may cover some of the positives. I did not verify.

## New regression review (the fixes themselves)

- Multimap reach is sound.
  - Entries are still only caller cells.
  - `reached()` takes `equal_range` and emplaces afterwards, so no iterator is invalidated.
  - The same root can appear twice in `rootsOf`, once from the pin and once from reach. This is
    harmless and deduplicated on insert.
- Reach is no longer capped by `limits.fields`, so memory is bounded only by the shared work grant.
  - `reached()` charges per root, `rootsOf` per pin and entry, and nested entry charges per entry.
  - Each Link adds at most |roots| entries, so growth is bounded by work.
  - The multimap's native allocation is not byte-debited. This is acceptable but should be a stated
    ruling.
- The `.name` loop is still bounded.
  - Its visited-set and `fields` cap are unchanged, and charging is correct.
  - The selector itself is debited on every Link through `boundedText`. A scan over N candidates
    pays selector bytes N times. This matches the generic path and is not a bug.
- Str Deref now debits the written copy, which matches the generic path.
- The arity re-check in `stepScopedStructure` is redundant but safe. It runs before any operand
  indexing.
- Generic checks the structural path still skips: `cloneMaster` on outputs, and the pipeline check
  on the destination register. Frame cells are fresh, so these are defence in depth only.

## Findings

1. MINOR: destination chain cells remain publishable (above). Fix: refuse cells on the destination's
   `vars` chain or in `destProtection`, and add a test.
1. MINOR: `position()` should require `assigned` on the dereferenced target. It is cheap and removes
   the argument.
1. MINOR: the structural outputs lack `cloneMaster` parity, and a dead reference gives TypeMismatch
   where `codec.reference` gives InvalidOperands.
1. MINOR: missing tests for the destination check, nested reach and the reach work charge.

Tally: BLOCKING 0, MAJOR 0, MINOR 4.

VERDICT: PASS
