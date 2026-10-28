# I2B-2 codebase / adversarial review

Static review of the diff and surrounding code. I did not rebuild or run tests.

1. MAJOR - `apps/common/xanadu/vortex/vortex_scoped_evaluation.cpp` ~L955-960 (field-selector walk
   in `stepScopedStructure`). `a.textOf(*name) == selector` copies each caller field-name cell in
   full.

   - That copy is not bounded, charged per byte, or debited from `bytesRemaining`.
   - `ScopeCodec::string` bounds spans and debits bytes first. `Deref` Str uses `boundedText`, and
     the other string paths charge per byte.
   - Scenario: a caller record has up to `limits.fields` names of MB-sized text. Each `Link` then
     copies all of them for one `charge()` per node, repeated for every instruction in the step
     limit.
   - Fix: compare spans without copying (compare `contentOf` length to `selector.size()` first, and
     only then read the text), or run each name through `codec.boundedText`. At minimum, skip names
     whose length differs from the selector and charge per byte compared.

1. MAJOR - `vortex_scoped_evaluation.cpp` ~L894-900 (`reached`, `f.reach`), with
   `scoped_evaluation.cpp` ~L470-476. `reach` is a `CellRef -> root` map and the first writer wins.

   - A cell reachable from two pins (a shared child, or the same cell reached again after a
     backtracked branch) is permanently attributed to whichever root walked there first.
   - A correct program whose result schema says `pinned_root: B` then refuses at publication with
     `TypeMismatch`. Whether it refuses depends on which root was walked first, including in
     branches that were later undone.
   - The design doc's "no trail needed" claim holds for soundness, but the outcome is
     order-dependent.
   - Fix: record `(cell, root)` pairs, for example `unordered_map<CellRef, small set of names>`. The
     `limits.fields` cap then counts pairs.

1. MAJOR - the test `ScopedStructureTest.BacktrackingUnassignsACursorMovedInTheFailedBranch` passes
   for the wrong reason.

   - `Point arg.obs -> cur` runs, then `Fail`, then the Choice alternative runs
     `Deref cur -> Float n`.
   - If the cursor were not restored (the trail removed), `cur` would still point at the pinned list
     head. `asDouble` of that cell is `nullopt`, so `Deref` refuses with the same `TypeMismatch`.
   - The test cannot tell "restored to unassigned" from "left pointing at a non-Float".
   - Fix: have the failed branch move the cursor onto a real Float cell (`Link first`, then
     `Link w`). Assert that the post-backtrack read refuses; this is only meaningful if reading the
     stale cursor would have succeeded.

1. MINOR - `vortex_scoped_evaluation.cpp` ~L930-950. The selectors `first`, `next` and `target` take
   precedence over record fields of the same name.

   - A caller record with a field called `first`, `next` or `target` can never be read.
   - Nothing in the doc table or in a test records this.
   - The doc says "move along first/next/target/field", so the shadowing should be stated
     explicitly. Better, add a distinct `field:` prefix or separate selector namespace, so a data
     field cannot lose to navigation names.
   - Add a test with a record field named `next`.

1. MINOR - the field walk starts at the first `d.vars` cell and does not skip the record-tag marker.

   - `scoped_evaluation.cpp` ~L562-565 and L235-252 show the codec treats the first name as a
     marker.
   - A selector equal to the record's tag (for example "Observation") matches the marker. The result
     is a values lookup on a cell that is not a field.
   - It most likely yields `found=false`, but it is untested.
   - Skip the marker in the walk, or test it.

1. MINOR - `stepScopedStructure` indexes `in[0]`, `in[1]`, `out[0]` and `out[1]` with no arity
   check.

   - It relies only on `verifyScope`. If anything changes the wings after verification, this is
     out-of-bounds access.
   - `stepScope`'s generic path checks `i < in.size()` in places.
   - Fix: add one `require(in.size() == n && out.size() == m, InvalidGraph, ...)` at the top of the
     function. It is cheap defence in depth.

1. MINOR - `verifyScope` builds `kinds` with `emplace`, so a duplicate register keeps the first
   entry.

   - Duplicates are already refused by `registers.insert(b.reg).second` at L123, so this is safe
     today.
   - The invariant lives far from its use, and `(void)toArg;` at ~L233 is a smell.
   - Use `const auto &[to, ignored]` pattern removal, or have `kindOf` return a struct with named
     fields.
   - The condition `(from != PinnedRef || fromArg) && (from == Cursor || fromArg)` is hard to read;
     spell out the allowed combinations.

1. MINOR - `Deref` Str does not go through the same debit path as other string outputs.

   - `boundedText` debits the read, but the write into `dest` interns a second copy that is not
     debited.
   - The generic path debits at ~L721.
   - The aggregate byte grant is therefore under-charged by up to 2x per Str `Deref`.
   - Fix: `codec.debitBytes(text.size())` before `writeValue`.

1. MINOR - `Link target` silently returns `found=false` when the target is not a pin or reached
   cell.

   - That is a read oracle for whether an arbitrary `OpHandle` in caller data points at a pinned or
     reached cell.
   - A refusal (`TypeMismatch`) may be more consistent than a quiet false, or the doc should say so.

1. MINOR - proxy cells. `valueKindOf` and `textOf` are used on `from` and on names without proxy
   resolution consistency.

   - `asDouble`, `asInt64` and `asBool` resolve through proxies into foreign manifolds.
   - The `None`-kind check for Str and for names looks at the proxy slot itself.
   - A foreign-store proxy can therefore be Deref'd as a number but not as a Str, and may be walked
     structurally.
   - Add one explicit `isProxy` refusal or one documented behaviour, and a test.

1. MINOR - test gaps, so several guarded behaviours would not fail if removed.

   - `MissingFieldReportsNotFoundWithoutMovingTheCursor` writes into a different cursor `other`, and
     never inspects `cur`. The "cursor unmoved" claim is untested.
   - Untested: `Deref` for Str, Bool and non-finite Float; `Link target` (including the positive
     case); the reach cap (`ResourceLimit`); nested scopes taking a parent's reached cells; a
     hostile cyclic `vars` record (`Cyclic or oversized record`); `Cursor` as an `arg.` binding (the
     `Cursor bindings must be temps` check is only hit via
     result.`); same-register in/out for `Point\`.
   - `AReferenceCannotBeLaunderedAcrossPinRoots` still refuses (for the wrong reason) if the reach
     feature is removed, because without reach any non-pin reference mismatches. Only
     `SelectsAReachedObservation...` guards reach.

1. MINOR - style.

   - Comment wording is mostly "why". `bound(reg, bool write)` uses a boolean flag; two named
     lambdas or an enum would be clearer.
   - `scoped_evaluation.cpp` `schema()`: `case Cursor: invalid(...)` falls into `default:`. It is
     correct because `invalid` is `[[noreturn]]`, but clang-tidy may flag it, so add
     `[[fallthrough]]` or reorder.
   - Same pattern in `materialize` (Cursor into List).
   - In `snapshot`, `mismatch(); break;` leaves a dead `break`.

Checked and fine:

- Every `switch` on `ScopeType` in the tree is in `scoped_evaluation.cpp` (schema, snapshot,
  default-value, materialize), `vortex_scoped_evaluation.cpp` (the Deref switch has a `default`) and
  the new code. Cursor is handled in each: schema, snapshot and materialize refuse; the zero-value
  switch is covered by `default`/schema refusal because Cursor cannot occur in a schema.
- No `map::at` on a missing key is reachable: `bindingTargets` and `bindingTypes` are keyed by every
  binding register, and `readable`/`writable` are subsets of them.
- Same-register in/out for `Link cur,sel -> cur,found` works. `Point` is also safe because the
  source is read before the destination write.
- `rootOf` is linear in pins but each step is charged, and pins are bounded by schema fields.
- The reach cap reuses `limits.fields`.

Tally: BLOCKING 0, MAJOR 3, MINOR 9.

VERDICT: FAIL
