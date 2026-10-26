# Final vagent checker safety review — R3

**Verdict: MAJOR findings remain.** The R2 repeated local-method scan finding is closed by the
owner/kind/name index and explicit resolution-work accounting. The new lookup budget also covers the
reported uses/import/record-field/environment work. Three other cross-product scans remain uncharged
and can exceed the configured work budgets while the input remains within AST and inheritance
limits.

Reviewed snapshot SHA-256:

```text
apps/common/xanadu/vagent/checker.hpp
e13e024ed1ceae028e78921241084edd96ddc263fcc4b97abc642e9c81bdfc7a

apps/common/xanadu/vagent/checker.cpp
6c4eaefcbbebe427068b17434d539fa32404858e9e00cdce1cddd51d5b0e823c

tests/xuzz/vagent_checker_test.cpp
b09dbce6a30630619f5922b0a7e4a6d6cb481a1e0bec03b9b21eccdc04309499
```

## MAJOR — capacity signature comparison work is not charged

`Checker::declaration()` merges inherited capacity methods and calls `sameSignature()` when the same
method name arrives from another base (`apps/common/xanadu/vagent/checker.cpp:1769-1787`).
`sameSignature()` loops over every parameter (`checker.cpp:63-75`). The inheritance budget charges
graph visits and copied interface entries in `buildInheritance()` / `capacityMethods()`, but this
declaration-time merge loop neither charges inheritance work nor resolution work. Thus the counters
bound the number of compared method entries, but not the potentially large signature-vector
comparison for each entry.

Counterexample: declare two capacities with the same method and P `Int` parameters; then declare N
capacities, each extending the previous capacity and the second original capacity. Capacity syntax
accepts an identifier list of bases (`parser.cpp:575-580`). Each derived declaration merges the
inherited method from both bases and compares all P parameters. The input has O(N+P)
declarations/parameters and stays below `maxAstNodes` and `maxInheritanceWork`, while signature
comparisons take Θ(NP). For example, N=20,000 and P=20,000 has well below one million AST nodes and
inheritance entries but performs hundreds of millions of parameter comparisons.

Correction: debit signature parameters before each `sameSignature()` loop, using a documented shared
checker work budget, or memoize signature identity/equivalence so inherited identical signatures do
not recompare their parameter vectors at each descendant. Include the declaration-time merge path,
not only cached interface construction.

## MAJOR — plan binding lookup performs an uncharged Cartesian scan

For each explicitly bound name, plan validation linearly searches the target goal's parameter vector
with `std::ranges::find_if` (`apps/common/xanadu/vagent/checker.cpp:1887-1898`). No
`resolutionWork()` debit is made for the examined parameters. The syntax is parser-admitted:
`Parser::plan()` parses an optional identifier list in parentheses (`parser.cpp:472-480`), and goal
declarations carry a parameter list.

Counterexample: one goal has P parameters and one plan lists all P names in reverse order in
`for Goal(...)`. The checker performs Θ(P²) parameter comparisons from O(P) AST input while
`maxResolutionWork` charges neither the candidate comparisons nor a bound proportional to them. At
P=20,000 this is roughly 200 million examined parameters before any refusal from the resolution
budget.

Correction: index goal parameters by name once (preserving duplicate-name behavior) or debit each
examined parameter against `maxResolutionWork` before comparison. Add a large valid goal/plan
regression and a low-budget refusal/retry assertion.

## MAJOR — final global-goal bound check rescans the full checked-goal set

At the end of `run()`, each top-level goal runs `std::ranges::any_of(checkedGoals_)` to determine
whether it was already checked in an owner scope
(`apps/common/xanadu/vagent/checker.cpp:2022-2026`). The set can contain one `(goal, owner)` pair
for every referenced global goal/owner combination, but this scan is not charged to
`maxResolutionWork`.

Counterexample: define G top-level goals and one agent with G plans, each targeting a distinct
global goal. Each goal is entered in `checkedGoals_` for that agent. The final pass then scans the
set once per global goal, Θ(G²) pair comparisons, although the program AST is Θ(G) and the normal
lookup/environment work is linear. With G=20,000 this is about 200 million comparisons under the
default AST and resolution budgets.

Correction: maintain a set of goal declarations known to be checked in any owner scope, and query it
directly in the final loop; or debit every examined entry. The semantic criterion is only whether
`entry.first == &decl`, so the indexed set preserves behavior.

## Closure and remaining scope

- R2's lookup regression is closed: `member()` now uses the owner/kind/name index
  (`checker.cpp:455-466`); insertion uses `emplace`, preserving the former first-declaration
  selection (`checker.cpp:228-250`). The flat 8,000-method test exercises lookup of the final
  method, and the low-budget test refuses and then accepts on retry
  (`tests/xuzz/vagent_checker_test.cpp:839-870`).
- The new `maxResolutionWork` counter charges index construction, owner `uses` collection, copied
  environment entries, each member lookup, capacity-use candidates, implemented-capacity names,
  imported selectors, and record-field scans
  (`checker.cpp:219-250, 455-466, 527-558, 639-640, 675-713, 754-760, 1377-1387`).
  `copyEnvironment()` charges the copied entries before making the copy.
- The prior inheritance and purity guards remain in place. The event single-base source grammar
  check and manual-AST shape refusal remain consistent. I found no claim of a wall-clock guarantee;
  these findings concern concrete, deterministic work omitted from the newly stated work budgets.
- I did not build or run tests; the parent owns integration verification.
