# vagent standard imports and adopting-agent template checking

Status: preimplementation plan, 2026-10-24. The check-only frontend and its current tests are the
baseline. This plan does not implement a loader, change source semantics, or assert that checked
source can execute. No checker module/template changes were made while writing it. A separately
requested inheritance-safety correction is described at the end.

## Evidence from the current frontend

`Parser::parseProgram` records a module path, optional import alias and selected symbol list;
`Program` owns one unit only (`ast.hpp`, `Program` and `Import`). `Parser::type` currently accepts
an identifier, list or generic type, but no dotted or colon-qualified type reference.

`Checker::run` recognizes only the two paths `std:agent` and `std:goal`. It does not validate
selected exports or alias collisions. `Checker::function` has independent hardcoded resolution
branches: `std:agent` aliases are ignored, while `std:goal` aliases also expose selected names
unqualified. Standard signatures are not obtained from a single manifest. These are frontend binding
gaps, not evidence that cross-file linking exists.

The following were reproduced with `build/vagent check -`, using the built check-only CLI and
headless/XDG isolation. Exit 0 printed `-: checked`; the rejected cases returned located
diagnostics.

| Source shape                                                                | Current result                         | What it demonstrates                                                                                           |
| --------------------------------------------------------------------------- | -------------------------------------- | -------------------------------------------------------------------------------------------------------------- |
| `import std:goal (missing)` followed by an empty agent                      | Exit 0                                 | Unknown selected exports are accepted even when unused.                                                        |
| `import std:agent as base (text)` followed by `base.text(1)` in a handler   | Exit 1, unknown method/function `text` | The parsed alias is not resolved for std:agent.                                                                |
| `import std:goal as scan (tab)` followed by bare `tab(1)` in a method       | Exit 0                                 | Alias and unqualified-import effects are not distinguished.                                                    |
| `std:goal as shared (tab)` plus `std:agent as shared (text)`                | Exit 0                                 | One alias can denote different modules without a diagnostic.                                                   |
| Global plan updates `count`; an agent with `count: Int` adopts its goal     | Exit 1, unknown `count`                | The plan is checked once with no adopting-agent context.                                                       |
| Unused global achievement goal tests `count > 0`; an agent declares `count` | Exit 1, unknown `count`                | The fallback checks an unbound goal template as though it had an empty owner scope.                            |
| Agent A adopts that global goal and has `count`; agent B is empty           | Exit 0                                 | Only the explicitly bound context is validated; this alone says nothing about a later delegated adoption at B. |

Relevant design contracts are vagent §5.7 (plans have stable qualified identities and imported plans
contribute to a library), §6.2 (goal clauses resolve in the adopting agent's scope), §7.1 (Agent
values carry no question interface, and delegation is admitted through `accepts`), §8.9
(import/alias grammar), and §8.11 (logical module names are distinct from loader paths).

## First extension: a standard-source export catalog

Add a small language-level catalog for the source interfaces vagent actually exposes. Each export
needs canonical module path, export name, declaration category, source signature/type scheme,
purity/effect classification, and implementation status. Signature information belongs in one place
used by import validation, callee resolution and purity analysis. This is a source catalog; it must
not pretend every low-level Vortex module entry has a vagent-callable signature.

For example, vagent §10 defines source-callable std:agent functions `text`, `intentions`,
`is_maintain`, `restorers`, `members` and `children`; its capacities Clock, Schedules, Accepts and
Logging have their own methods. Map is a generic type, and the built-in events are event types. A
selector naming one of these categories is not automatically a call to a function of that name.

The scanner calls used in the guide have evidence-backed signatures: `tab(Int)` and `move(Int)`
produce Str; `match(Str)`, `find(Str)`, `many(Str)` and `upto(Str)` produce positions. The current
checker also assumes signatures for `any`, `bal`, `subject` and `pos`. Verify those separately
against the language-level contract before calling the source catalog complete. In particular, the
cited vicon §6 explains tab/move/find/many/upto/match, while its export list names bal without
defining a complete vagent argument contract. Do not infer a zero-argument signature solely from its
presence in a runtime export list.

`VortexStdLib::buildGoalModule` has lower-level exports such as scan and control routines whose
Inputs Wing operands are opcode/cell references. Those cannot simply be surfaced as ordinary source
calls: vagent has no general CellRef/function-value type. Mark such entries as internal lowering
interfaces or explicitly unsupported source exports until their source contract is specified. Keep
the distinction between specified scanning prerequisites and currently registered runtime symbols; a
catalog entry does not implement its body.

### Binding decisions to record before coding

The import grammar specifies aliases and selectors but does not fully state their binding effects.
The following is the proposed conventional interpretation, not an inference from passing tests:

- An import without `as` exposes its selected source exports unqualified. Without a selector it
  exposes that module's supported public source exports.
- An import with `as alias` exposes the selected exports through that alias. It does not
  additionally introduce bare names. An independent unaliased import may introduce them.
- `std:agent` remains implicitly imported as specified. An explicit selector does not remove that
  prelude. An explicit alias adds a qualified view of the same origins, not a second module.
- Validate every selector even when unused. Repeated imports of the same canonical origin can
  coalesce; an alias bound to different module origins is ambiguous and must fail. Do not generate
  duplicate distinct declarations for two aliases of one origin.
- Preserve the specified precedence of an agent/behavior's own methods, then its used capacities,
  then implicit std:agent methods. Do not globally replace that order with catalog-first lookup.
- Resolve a qualified call through its lexical head binding. A local value that shadows an alias is
  a value, not a reason to silently fall back to the hidden module. If it is not callable, report
  that type error. Define and test collision handling for explicit import aliases and top-level
  declarations rather than relying on iteration order.

These policies need one short addition to the module section of the design and guide. They should be
reviewed as source-language clarifications before changing current alias acceptance.

### Minimal code changes

1. Introduce `standard_catalog.hpp/.cpp` with immutable export descriptors and reusable signature
   schemes. Keep polymorphic `text` and list/map types explicit; do not use an undiagnosed Unknown
   result to stand in for a generic signature.
1. Build an `ImportScope` once per Program in checker setup. Its entries bind lexical names to
   canonical export IDs; retain category and selector filtering. Reject unavailable custom module
   paths exactly as today until a real resolver is supplied.
1. Replace repeated ad hoc name checks in `Checker::function` with lookup through that scope and the
   specified owner-method/capacity precedence. Share resolution with `derivePurity`.
1. Flatten a Field callee's identifier path for qualified namespace lookup. A module alias can
   contain a capacity export, so `base.Clock.now()` needs more than the existing one-Field check. A
   namespace path is not an arbitrary runtime value traversal.
1. Keep syntax boundaries honest: qualified type references such as `geo.Point` are not in the
   current Type grammar. Supporting them requires a separate grammar/type-resolution change; a
   function alias patch must not claim imported nominal type support.

### Positive and negative checks

Positive, after the binding policy above is adopted:

```vagent
import std:agent as base (text)
event Tick
agent A { on Tick { let value := base.text(1) } }
```

```vagent
import std:goal as scan (tab, match, upto)
event Parse { value: Str }
agent A {
  on Parse as p {
    let value := p.value ? (scan.tab(scan.match("ID:")) & scan.tab(scan.upto(";")))
  }
}
```

Required negatives: an unknown selector; selecting a function and calling it with the wrong type;
calling an unselected export through an alias; two different origins under one alias; an alias
shadowed by an Int local used as a namespace; treating a capacity/type export as a function; and a
custom module path without a resolver. The aliased std:goal example must not also make bare tab
available unless another import does. Aliasing std:agent must not remove its implicit bare text.

## Second extension: concrete template contexts

Keep module declaration identity separate from the adopting owner. A top-level goal or plan is not
an empty agent. Conversely, checking it once in the first agent that mentions it is insufficient: a
second adopting agent can have different fields or used capacities. Cache by both template origin
and concrete context, never by the goal's spelling alone.

A minimal checker refactor can split existing `declaration` handling into:

- template shape validation: name, parameter types, goal kind/clauses, target existence, binding
  names, and declaration identity;
- concrete checking: goal clauses, plan where/bodies, field/method access, purity and suspension
  rules in a particular adopting context.

Introduce a worklist keyed by `(template declaration ID, adopting context ID)`. Seed it from local
adopt/achieve sites and local plans referring to a global goal; when checking a goal, enqueue its
visible module-level plans in their specified library order. Checking a plan may discover further
goals; insert the pair before walking its conditions/body to terminate recursive goal graphs.
Explicit plan parameter subsets retain their existing meaning. Module imports and predicate/type
references stay lexical to the template's origin; agent field lookup and applicable capacity/method
lookup use the adopting context. A template does not acquire the lexical imports of an unrelated
caller merely because that caller adopts its goal.

Do not conflate every owner with an Agent declaration. A skill body can adopt a global goal, and a
behavior can be mounted by several agent types. The concrete context must retain the containing
agent plus any behavior/skill field frame used by the declaration. Reachability of initial/dynamic
install and mount sites can be conservatively indexed from named source constructors. Unsupported
context combinations must remain an explicit diagnostic or unresolved analysis result, not a silent
parameter-only check. Exact field precedence for a global template reached through a behavior/skill
needs verification against the design before implementation; the current single `Context::owner`
cannot represent both lexical frame and adopting agent.

### A required boundary for delegated adoption

The worklist above covers concrete source-level uses. It is not a proof about incoming delegated
goals. §7.1 admits goals by a runtime `accepts(sender, goal_name)` call, and Agent has no static
interface listing every goal it may receive. Therefore, checking only explicit adopt expressions
must not become a claim that every globally visible goal is safe for every Agent.

Before lowering/actor execution, choose and document the availability contract: either validate all
goals that an agent exposes for remote adoption, or derive an explicit per-agent checked goal
manifest and define rejection of goals outside it. The latter needs a specified admission/fault
rule; this frontend plan does not invent it. An unused global template can retain owner-dependent
constraints instead of being falsely checked in an empty owner, but compilation cannot use that as
permission to execute an unvalidated instantiation. Until this contract is settled, the check-only
analysis must expose unresolved template/context requirements and not present them as completed
agent linking.

Suggested API direction: `analyze(Program)` returns an owned analysis with resolved symbols,
concrete template-check results and explicit unresolved requirements; `check` can remain the
convenience diagnostic wrapper with a documented policy for those requirements. A future compiler
must consume the same analysis or call the same specialization service, rather than repeat a
different owner-binding algorithm. IDs in analysis should be stable declaration IDs; raw pointers
may be internal accelerators only while their Program lifetime is pinned.

### Template regression cases

Positive after specialization is implemented:

```vagent
goal Work() perform
plan work for Work { count := count + 1 }
agent Counter {
  var count: Int := 0
  on Initialize { adopt Work() }
}
```

A separate negative must adopt this same template in an agent whose count is Str. Two Int-owning
agents should each get a checked instantiation without duplicating source declaration identity. A
local goal with the same unqualified name must keep local resolution precedence. A missing predicate
must still fail independent template-shape/reference validation; deferring owner fields is not
permission to accept arbitrary unknown names.

Also test a recursive G-to-H-to-G plan graph, a where-bound query variable used in the body,
parameter-subset bindings, and a pure helper resolved differently in two adopting owners. Cover
skill and behavior contexts before claiming those routes are supported. Preserve the one-bound,
one-unbound-agent example in the evidence table as an admission-boundary test when the remote
manifest rule is specified.

## What a real cross-file resolver must add

Standard catalogs and same-unit specialization do not load another user module. A genuine resolver
needs a supplied source/artifact provider, dependency-cycle handling, canonical logical module
paths, verified artifact/source identities, export visibility and a closure of pinned versions.
Filesystem paths locate sources; they are not module or type identity.

The current Type stores only a name and generic arguments. Before merging user units, resolve a
nominal event/record to an origin-qualified declaration ID: defining module identity/version,
lexical owner path, declaration name and category. Two modules' distinct Point declarations must not
become equal because both source spellings are Point. Two aliases of the same declaration must
remain equal. Apply that distinction in predicates, questions, goals, records, inheritance and
native capacity signatures. Preserve canonical origin in program symbols and eventual wire type
names; alias spellings never become provenance or code identity. The Type source grammar also needs
a deliberate way to name qualified imported types.

## Separate inheritance-safety evidence and correction

Flat source inheritance does not consume parser syntactic nesting. A 30,001-capacity reversed chain
(997,799 source bytes) exceeded an 8-second CPU probe; that run was killed by its CPU bound, not
proven to be a default-stack crash. A separate 8,001-capacity chain (253,798 bytes), admitted by
existing parser limits, exited with SIGSEGV in 0.182 seconds under a recorded 512 KiB stack budget.
Both probes were headless, had core dumps disabled and subprocess CPU/time limits. The small-stack
result demonstrates unbounded native recursion independently of its default-machine failure
threshold.

The authorized safety fix replaces recursive inheritance/capability walks with one iterative
prepass, cached interfaces/fields and memoized iterative ancestry queries. An explicit CheckLimits
inheritance-work budget counts graph visits and copied interface entries, so pathological flattening
gets a named refusal rather than unbounded work or a silent semantics restriction. Deep-chain,
deep-cycle and budget regression tests accompany it. Parent integration verification must establish
the rebuilt results; the pre-fix measurements above are not post-fix pass claims.
