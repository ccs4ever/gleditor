# I2B-2 round 2: codebase / adversarial re-review

Static re-review of `/tmp/vagent-p19-i2b2/i2b2-r2.diff` and the revised tests. I did not rebuild or
run tests. I relied on the implementer's statement that 70 Scoped\* tests pass.

## Round 1 findings

- **R1 M1 (uncharged name copy): closed.** The name walk now sums the charged span lengths first. It
  compares the sum to the selector length, debits those bytes, and only then calls `textOf`. Names
  of a different length are never copied.
- **R1 M2 (first-writer-wins reach): closed.** `reach` is now an `unordered_multimap` holding every
  root, and `reached` de-duplicates (cell, root) pairs.
  - `rootsOf` merges pin matches and reach entries, and the `target` selector replaces `roots` with
    the pin's roots.
  - Nested entry iterates all entries and charges each.
  - `ACellPinnedTwiceIsAttributedToBothRoots` fails under first-writer-wins: the cell is pinned as
    both "a" and "b", walked from "a", and published under "b".
- **R1 M3 (backtracking test passed for the wrong reason): closed.** The cursor is now parked on a
  real Float via `.w`. The no-Fail twin must return Value 1.5. The Fail twin must refuse after
  backtracking. With the trail removed, the Fail twin would succeed and the test would fail.
- **R1 m4 (selectors shadowing fields): closed.** Fields are now `.name`, and any other word refuses
  `TypeMismatch`. `UnknownSelectorWordRefusesInsteadOfMissing` covers the unknown-word refusal.
- **R1 m6 (arity): closed.** The recheck sits at the top of `stepScopedStructure`.
- **R1 m8 (Str `Deref` debit): closed.** The written copy is debited.
- **R1 m9 (`target` returns false silently): accepted as documented.**
- **R1 m5 (record-tag marker) and m10 (proxies): still open, both MINOR.** See below.

## New findings

1. MINOR - `vortex_scoped_evaluation.cpp`, the name-walk length sum
   (`length += static_cast<std::size_t>(span.length)`) is unchecked. On 64-bit this cannot
   realistically wrap, because spans are bounded by real scrolls. The sum is also compared before
   the debit, so a wrapped value could in theory match a short selector and then run `textOf` on a
   large name. Add a saturating add, or refuse when `length > limits.bytes`.

1. MINOR - the record-tag marker is still reachable as `.Observation`, because the walk starts at
   the first `d.vars` cell. This is now deliberate and obscure, not a collision. Skip the marker or
   add a one-line test and doc note.

1. MINOR - `reach` is uncapped in memory. It is charged as traversal work (one unit per root per
   `reached` call), so growth is bounded by the instruction and work grants. This is acceptable, but
   the doc should say the bound is the work grant and not `limits.fields`.

1. MINOR - `(void)toArg` and the dense `positional(...) && (...)` condition in `verifyScope` remain.
   This is style only.

1. MINOR - the following paths are not tested:

   - the length-mismatch skip and the byte debit for names;
   - cyclic or oversized `vars` chains (`Cyclic or oversized record`);
   - a `Cursor` arg binding (the check is only hit through a result binding);
   - the destination-alias refusal (`from != f.destination`);
   - the arity recheck. It is defence in depth and unreachable from a verified graph, so this is
     acceptable.

## Regressions looked for, none found

- The multimap does not duplicate entries, because `none_of` guards each emplace. Equal-range
  iteration is charged.
- `.name` selectors cannot reach `first`, `next` or `target` by accident. A field named `first` is
  read as `.first`.
- Point's destination check (`from != f.destination`) cannot reject a legitimate caller cell,
  because caller cells are never the scope's own destination record.
- Verification order: the Cursor-on-arg/result check now precedes the schema check, so the earlier
  unreachable-line problem is fixed.
- Same-register in/out still behaves, because inputs are read before outputs are written. Every
  `ScopeType` switch handles `Cursor`.
- The new tests are meaningful. The `target` test needs the owned list element to resolve to the pin
  to reach `deref=true`, and its `plain=false` case needs a non-reference. The non-finite and Ln(0)
  test is only meaningful if the Choice alternative would otherwise yield Empty, and it asserts
  `Refused` with an exact refusal code.

Tally: BLOCKING 0, MAJOR 0, MINOR 5.

The approval-scope question is an owner decision and is excluded from this verdict.

VERDICT: PASS
