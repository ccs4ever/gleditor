# Vagent checker inheritance safety review — R1

> **Superseded by R2:** the event multiple-inheritance counterexample below is not valid vagent
> source. The parser permits one event base. R2 withdraws the source-level claim and scopes the
> remaining issue to hand-built public ASTs.

**Verdict: MAJOR finding.** Reviewed the inheritance-specific checker snapshot and regression tests;
no build was run. SHA-256 values:

```text
apps/common/xanadu/vagent/checker.hpp
aedad92af2acebd56b0e7ebfe3729438177bcd4f60ba26d9afb270fc0338d693

apps/common/xanadu/vagent/checker.cpp
a2c5a610df4e5a9946a6d8490721b03505255572e60b649272c916a50f804919

tests/xuzz/vagent_checker_test.cpp
5e11c4003f04e63b7abe291bf645b71c543ac2b1040763765be78a207d2a2a20
```

## MAJOR — conflicting fields from unrelated event bases silently resolve by base order

The topological cache copies inherited event fields using unconditional assignment
(`apps/common/xanadu/vagent/checker.cpp:259-269`), so when two bases contribute the same field with
different types, the later base silently replaces the earlier type. The event declaration validation
only rejects a child event's own field when it duplicates a field from a direct base
(`checker.cpp:1596-1604`); it does not compare the inherited maps of sibling bases. Later lookup
reads the already-overwritten cache (`checker.cpp:486-494`).

Counterexample:

```vagent
event HasCount { value: Int }
event HasName { value: Str }
event Combined extends HasCount, HasName
event Tick
agent A {
  on Combined as e { let text: Str := e.value }
}
```

`Combined` is accepted and `e.value` is typed as `Str` solely because `HasName` appears last.
Reversing the base order types it as `Int`. The checker should reject an inherited
same-name/different-type conflict (while continuing to accept same-typed diamond inheritance) rather
than make event field type depend silently on declaration order. Add direct conflict and same-type
diamond tests.

## Other reviewed behavior

- The Kahn prepass is iterative and visits each inheritance edge/node with a work debit; it builds
  caches only from valid global declarations, reports remaining cycles, and aborts through a typed
  diagnostic when the finite work budget is exhausted (`checker.cpp:204-291`).
- Ancestry walks use an explicit work vector and visited set, memoize completed `(source,target)`
  answers, and debit nodes and edges while evaluating uncached queries (`checker.cpp:293-324`).
- Flattened capacity methods and event fields charge cache-copy work, and subsequent consumers
  charge bounded cache reads/copies (`checker.cpp:242-270,444-455,486-494`). Capacity signature
  conflicts are checked when declarations are validated (`checker.cpp:1607-1628`); shared-ancestor
  diamonds retain one method and pass same-signature checks.
- The new tests cover a repeated diamond graph, deep flat capacity/event chains, a deep cycle,
  graph-work refusal, and flattened-interface-work refusal
  (`tests/xuzz/vagent_checker_test.cpp:548-555,642-712`).

I did not identify a budget bypass or recursion remaining in the reviewed inheritance ancestry/cache
paths. The accepted event field ambiguity is the actionable defect.
