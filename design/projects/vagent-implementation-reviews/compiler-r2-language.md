# Compiler R2 language semantics review

**Scope:** read-only audit of `apps/common/xanadu/vagent/compiler.hpp/.cpp` and
`tests/xuzz/vagent_compiler_test.cpp` against `design/vagent-agent-language.md` §§8.2–8.3, 6.7 and
5.8. This covers the scalar compiler iteration only; it does not approve the full agent compiler or
execution path.

## Result: CONVERGED

I found no concrete semantic violation in the implemented block/`let`/`if`, scalar assignment, and
nested rollback subset. The documented subset boundaries are explicit in `compiler.hpp` (including
unsupported syntax, one expression statement, and matching conditional-arm types); this review does
not treat unimplemented language features as defects.

- **Typing and success/failure:** scalar types are checked before graph emission; `Bottom`
  represents non-producing `fail`; comparisons yield the right-hand value on success; boolean values
  are rejected in search positions unless the outer operator is a comparison. `if` selects on
  expression success, not Bool truth. Matching and non-producing conditional arms are handled
  consistently with the declared subset.
- **Statement bounds and alternatives:** each block child goes through `bounded()`, which records
  choice depth, creates a statement choice point, and cuts to the recorded depth after the first
  result. Tests cover first-result behavior and confirm the choice stack is empty after completion.
- **Lexical locals:** `let` initializers are preflighted before the new binding enters scope, so a
  shadow initializer resolves to the outer name. Nested scopes are popped after their block;
  duplicate declarations in one scope and reads beyond scope are rejected. The test with nested
  shadowing confirms the outer binding is unchanged.
- **Condition purity:** `if` conditions are recursively preflighted in pure mode; both assignment
  forms are refused there. The subset has no calls/actions to evade this check. Tests cover direct
  and block-nested assignment in a condition.
- **Rollback:** writes use Vortex field-value writes under the enclosing arena marks. A nested
  failed block fails its enclosing bounded statement, and the outer failure path restores prior `:=`
  writes. A test also verifies that a successful bounded `:=` remains subject to an already-open
  outer choice point, matching §8.3's clarification that irreversibility is only within bounded
  search. `<-` alternatives restore writes when backtracked.

The review is limited to static inspection of the cited code and its tests; no build or test command
was run.
