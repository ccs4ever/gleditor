# Vagent checker inheritance safety re-review — R2

**Verdict: MINOR finding.** Reconsideration confirms that the R1 source-level event conflict example
was invalid: vagent event syntax admits only one declared base. The initial claim that a CLI program
could be accepted with two event bases is withdrawn. The remaining issue is limited to direct
callers that construct the public AST themselves.

Reviewed snapshot hashes:

```text
apps/common/xanadu/vagent/parser.cpp
4d2e1d0f6469908a0f838c5df4e1576a8d4ddbd7dbfb7bc0eee09a269b9363e0

apps/common/xanadu/vagent/checker.hpp
aedad92af2acebd56b0e7ebfe3729438177bcd4f60ba26d9afb270fc0338d693

apps/common/xanadu/vagent/checker.cpp
a2c5a610df4e5a9946a6d8490721b03505255572e60b649272c916a50f804919

tests/xuzz/vagent_checker_test.cpp
5e11c4003f04e63b7abe291bf645b71c543ac2b1040763765be78a207d2a2a20
```

## MINOR — public checker accepts an event AST with more than one base

The parser consumes exactly one identifier after `event E extends`
(`apps/common/xanadu/vagent/parser.cpp:548-559`); a comma after that base is left where a top-level
declaration/opening brace is expected, so CLI source with multiple event bases is rejected. The
language/parser contract must remain single-base; do not extend event syntax to make the R1 example
valid.

The checker API accepts a public `Program` AST (`checker.hpp:18-19`), but does not enforce that
parser invariant. `buildInheritance` will process every name in an event's `Decl::names` vector and
silently overwrite duplicate inherited field names with the last base's type
(`checker.cpp:259-270`). Event declaration validation checks an event's own fields against its bases
but has no base-count guard (`checker.cpp:1586-1604`). A hand-built `Program` whose `Combined` event
has `names = {"HasCount", "HasName"}`, where those events declare `value: Int` and `value: Str`,
respectively, passes declaration validation; its cached `value` type depends on base order.

Correction: at the checker boundary, reject a manually constructed event declaration with more than
one base (typed `InvalidDeclaration` is suitable). This is defensive AST validation, not support for
event multiple inheritance. No CLI/parser change is needed. The checker should keep accepting
same-base event chains; capacity diamonds remain supported and covered by tests.

## Other reviewed behavior

The Kahn inheritance prepass, iterative/memoized ancestry, cache construction/consumption work
debits, cycle refusal and finite `CheckLimits::maxInheritanceWork` remain as assessed in R1. The
event conflict finding is now scoped to malformed/manually-built AST input only; I found no
resource-budget bypass in the reviewed paths. No build or tests were rerun.
