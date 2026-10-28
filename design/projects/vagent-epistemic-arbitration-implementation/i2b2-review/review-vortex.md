# I2B-2 review: Vortex/VM persona

Line numbers are in the working tree. Files: `vortex_scoped_evaluation.cpp` (VSE),
`scoped_evaluation.cpp` (SE). Method: read only; nothing was built or run, so every failure scenario
is derived from the code.

## Soundness conclusions (no counterexample found)

- Frame-private, scratch or post-mark cells cannot be published as PinnedRef. A Point to a PinnedRef
  result needs `rootOf(from)` (a pin or a reach entry). Owned Record/List copies have no root.
- Reach entries are only pins or cells read through `readLink` from them. The scope never relinks
  caller cells, so the reach set needs no trailing.
- Backtracking is safe. `setValueBits` is trailed (arena_manifold.cpp:1347), so cursor bits rewind.
  Assignment flags rewind through `restoreScopedAssignments`. Reach is monotone and holds only
  caller cells.
- Cursors cannot escape into writable state.
  - Sources are verified to be arg-bound or a temp Cursor (VSE ~228).
  - Generic ops reject a Cursor input as "nonscalar" (VSE ~520-535).
  - Cursor cannot be a schema field (SE:332, 646).
- Nested scopes are sound. A child's reach starts empty. Parent reach becomes a child pin only
  through the explicit `pinned` set (VSE ~358).
- Numeric Deref reads value bits only (`asDouble/asInt64/asBool`), so there is no uncharged parse.

## Findings

1. MAJOR, uncharged and unbounded text read in field lookup.

   - Evidence: `a.textOf(*name) == selector` in the `else` branch of `stepScopedStructure` (VSE
     ~934). The loop charges 1 per name cell, but `textOf` copies the whole name text.
   - The generic and Deref paths use `codec.boundedText`, which sums span lengths, refuses over
     `limits.bytes`, and calls `debitBytes`. This path does none of that.
   - Scenario: a caller record has up to `limits.fields` field-name cells with multi-MB or
     persistent spans. Each `field` Link then allocates and copies all of them for 1 work unit per
     name. The cost is bounded by neither `traversalWork` nor `bytesRemaining`.
   - Fix: compare lengths first via `a.contentOf(name)` (summing `span.length` with `charge()`), and
     skip any name whose length is not `selector.size()`. Only then call `textOf`, and `debitBytes`
     for the bytes read.

1. MINOR, `position()` never checks `assigned()` on the dereferenced target.

   - Evidence: VSE ~912. `assigned(cell)` tests the register's own cell. For a PinnedRef or Cursor
     the data read is `handleTarget(cell)`, which is never checked.
   - `enterScope` builds `pinned` from the parent's `bindingTargets`, so a nested PinnedRef can name
     a parent temp or result leaf.
   - `argsProtection.protectedCells` holds only the handle cells (`reference()` does
     `seen_.insert(handle)`), not the pin targets. The "Nested arguments read unassigned scoped
     data" rule (SE/VSE ~423) therefore does not cover them.
   - Before this iteration a pin was opaque. Now Link/Deref read through it, so the child can read a
     parent's unassigned placeholder `()` via Deref Str.
   - I could not build an end-to-end case, because the parent needs a handle to its own temp.
   - Fix: after resolving the target, look it up in `assignmentIndex` and require `assigned`, or
     reject nested pins that are `bindingTargets` of unassigned parent cells.

1. MINOR, a pin or reach cell can alias the destination.

   - Evidence: `replaceRecord(saved.destination)` (SE:559) relinks `vars` on the caller's
     destination root at publication. Aliasing checks cover only schema-field cells.
   - Scenario: pin = destination root D. The scope reaches D's old `payload` value, or D itself, and
     Points the result `best` at it. After publication the handle names an orphaned old cell, or D
     contains a handle to D (a cycle).
   - This predates reach for the D-as-pin case, but reach widens it.
   - Fix: in `reached()` and the pin loop, refuse D and cells on D's `vars` chain.

1. MINOR, reach is keep-first, so refusals depend on walk order.

   - Evidence: `f.reach.emplace(target, root)` (VSE ~893), and `rootOf` returns the first pin by
     name.
   - Scenario: pins A and B share a list tail. If the scope reaches tail cell X from A first, then
     Point X to a result declared with root B fails with TypeMismatch, even though X is legitimately
     B-reachable.
   - Fix: store a set of roots per cell, or document it as a ruling.

1. MINOR, reserved selectors shadow field names.

   - Evidence: `first`, `next` and `target` are matched before the field walk (VSE ~915-930).
   - A caller record with a field named `first` or `target` cannot be read. Hash-ordered name lookup
     gives no escape.
   - `target` only follows to cells that are already pins or reach (`rootOf(*target)`), so it cannot
     follow a `d.values` OpHandle in caller data to an unpinned cell. It silently returns
     `found=false`.
   - Fix: prefix the structural selectors (e.g. `@first`) or add a field-forcing form, and document
     the `target` limit in the ABI "Structural reads" section.

1. MINOR, the structural path skips two generic checks.

   - The generic path rejects an output whose `cloneMaster(target) != target` (VSE ~600).
     `bound(write)` has no equivalent.
   - Frame cells are fresh, so this is defence in depth only. Add the check to `bound`.
   - Also, `codec.reference` raises InvalidOperands for a dead reference, while `position()` raises
     TypeMismatch for "Structural reference is dead". Pick one code.

1. MINOR, dead verifier line.

   - `require(b.target.starts_with("temp.") || b.type != Cursor)` (VSE ~147) is unreachable.
     - arg/result bindings must match a schema type, and Cursor is refused in schemas.
     - outcome/error must be Str.
   - Test case "cursor-result" expects InvalidSchema but fails on "Binding type does not match
     schema", not on this line. It does not exercise what its label suggests. Remove the line, or
     move it before the schema branch.

1. MINOR (tests), several tests do not prove their names.

   - `AReferenceCannotBeLaunderedAcrossPinRoots`: it asserts only `TypeMismatch`. If the `first`
     Link were a no-op, the cursor would stay at the `theirs` root, which is also not root `mine`,
     and the test would still pass. It never proves that reach attributes the element to `theirs`.
     Add a positive twin: walking from `mine` and publishing under `mine` succeeds. Also assert the
     message or the refusing site.
   - `BacktrackingUnassignsACursorMovedInTheFailedBranch`: Deref Float on a cursor at a list root is
     TypeMismatch with or without the unassigned flag restored. It also does not assert which Choice
     branch ran first. It would pass if backtracking did nothing, so it proves nothing about the
     intended cause. Use a cursor assigned before the Choice, and observe the restored position or
     flag distinctly.
   - Coverage gaps:
     - `target` selector (no test).
     - positive Deref Int/Bool/Str.
     - List-root cursor.
     - reach through a nested scope.
     - the reach cap (`limits.fields`).
     - cyclic or oversized record in `field`.
     - the large-name case from finding 1.

## Tally

BLOCKING: 0. MAJOR: 1 (finding 1). MINOR: 7.

VERDICT: FAIL
