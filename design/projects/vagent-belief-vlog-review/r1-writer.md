# Round 1 critique: Technical Writer (vagent-extended-capabilities-and-belief-vlog-unification.md)

Paths below are relative to the repo root. Tally: 1 BLOCKING, 4 MAJOR, 3 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

None (Round 1).

## BLOCKING

### 1. (§3.3, line 526) Contradiction in String Scanning Signatures & Composability

Full title: Contradictory Semantics, Return Types, and Composability Failure Across the Nine String
Scanning Built-ins

**Problem:** Section 3.3 defines the nine string scanning built-ins (`pos`, `tab`, `move`, `match`,
`find`, `upto`, `any`, `many`, `bal`). However, the specification suffers from severe internal
contradictions regarding return types, cursor movement, and composability, directly breaking both
Icon/`vicon` idioms and the document's own acceptance test battery:

1. **Cursor movement contradiction in `find` vs. acceptance tests**: In §3.3.5 (lines 574–575),
   `find(s: Str): Generator<Int>` is specified as: "On each produce: updates `&pos` to the found
   location and yields the position integer." However, in §6 (line 946), the acceptance test battery
   specifies: "Asserts that a failing alternation `tab(find("a")) | tab(find("b"))` restores `&pos`
   to its initial value upon backtrack." If `find` updates `&pos` to the found position, then
   composing it with `tab(find(...))` fails completely: `tab(i)` sets `&pos` to `i` and produces the
   substring between old `&pos` and new `i` (line 553). If `find` has already updated `&pos` to $i$,
   then inside `tab(i)`, `old == i`, and `tab` produces an empty string (`length = |i - i| = 0`)!

1. **Analysis vs. Movement category confusion in `match`, `any`, and `many`**: In standard Icon and
   `vicon §6.2`
   ([`vicon-goal-directed-language.md:345–349`](../../vicon-goal-directed-language.md#L345-L349)),
   scanning functions are split into two orthogonal categories:

   - *Positional movement functions* (`tab(i)`, `move(i)`): explicitly set/move `&pos` and return
     the traversed substring (`Str`).
   - *Analysis functions* (`find(s)`, `upto(cs)`, `match(s)`, `any(cs)`, `many(cs)`, `bal(...)`):
     test or locate positions in the subject and return/generate integer target positions (`Int` /
     `Generator<Int>`), leaving `&pos` unchanged. They are designed to compose with `tab(...)`
     (e.g., `tab(match("prefix"))`, `tab(many(" "))`, `tab(any("aeiou"))`, `tab(upto(","))`).

   In §3.3, the document arbitrarily fractures this architecture:

   - `upto` and `bal` return `Generator<Int>` without moving `&pos` (noting in line 584:
     "conventionally paired with `tab(upto(cs))`").
   - `find` moves `&pos` AND returns `Int`.
   - `match`, `any`, and `many` move `&pos` AND return `Str` (subsections 4, 7, 8). Because `match`,
     `any`, and `many` return `Str`, an agent cannot write `tab(many(" \t"))` or
     `tab(match("prefix"))`—it would trigger a static type error since `tab` requires an `Int`
     argument. Furthermore, in the existing codebase
     ([`compiler.cpp:2500–2501`](../../../apps/common/xanadu/vagent/compiler.cpp#L2500-L2501)),
     `match` returned `Ty::K::Int` (the matched end position), not `Str`.

1. **Class diagram type mismatch and sentinel ban violation**: In §3.3 (lines 528–542), the Mermaid
   `classDiagram` specifies methods returning bare `CellRef`, whereas the subsection headings
   specify `Str` or `Int`. The diagram also uses `Cset cs`, whereas the text uses `cs: Str`. Most
   critically, under the repository's standing house rules (`AGENTS.md` and `brief-common.md`),
   sentinels are strictly banned. If `tab`, `move`, `match`, `any`, or `many` fail (which they do on
   out-of-bounds or non-matching characters, lines 555, 567, 591), returning bare `CellRef` in a C++
   API forces either a sentinel (`noCell`) or an exception. The signatures must return
   `std::optional<CellRef>` (or `std::expected`).

**Evidence:**

Doc §3.3.4 (line 566): "`match(s: Str): Str` ... advances `&pos` by $\text{length}(s)$ and produces
$s$."

Doc §3.3.5 (lines 574–575): "`find(s: Str): Generator<Int>` ... updates `&pos` to the found location
and yields the position integer."

Doc §3.3.6 (lines 583–584): "`upto(cs: Str): Generator<Int>` ... yields the position without
automatically advancing `&pos` past the character (conventionally paired with `tab(upto(cs))`)"

Doc §6 (line 946): "Asserts that a failing alternation `tab(find("a")) | tab(find("b"))` restores
`&pos` to its initial value upon backtrack."

Repo `apps/common/xanadu/vagent/compiler.cpp:2500–2501`:

```cpp
mov(S(out), S(endPos));
return tyOf(Ty::K::Int);
```

Spec `design/vicon-goal-directed-language.md:345–349`:

```markdown
tab(i) sets &pos to i and produces the substring between the old and new positions...
find(sub) is a generator: it produces the first position where sub occurs...
many(cs), upto(cs), match(sub) are the same shape.
```

**Fix:** Harmonize the scanning functions with the canonical Icon and `vicon §6` two-tier
architecture:

1. Positional movement built-ins:

   - `tab(i: Int): Str` (advances `&pos` to absolute index $i$, returns traversed `Str`)
   - `move(i: Int): Str` (advances `&pos` by relative offset $i$, returns traversed `Str`)

1. Analysis built-ins (all compute positions, leave `&pos` unchanged, return `Int` or
   `Generator<Int>`, and compose with `tab(...)`):

   - `find(s: Str): Generator<Int>` (generates match positions without advancing `&pos`)
   - `upto(cs: Str): Generator<Int>` (generates character positions without advancing `&pos`)
   - `match(s: Str): Int` (returns position `&pos + length(s)` if prefix matches, else fails; does
     not advance `&pos`)
   - `any(cs: Str): Int` (returns `&pos + 1` if current char is in `cs`, else fails; does not
     advance `&pos`)
   - `many(cs: Str): Int` (returns position after longest run of `cs`, else fails; does not advance
     `&pos`)
   - `bal(cs: Str, open: Str = "(", close: Str = ")"): Generator<Int>` (generates balanced positions
     without advancing `&pos`)

1. Update the `ScanningContext` class diagram to reflect these signatures and replace fallible
   `CellRef` return types with `std::optional<CellRef>`.

______________________________________________________________________

## MAJOR

### 2. (§1, line 1; §5) Missing Refused Alternatives & Open Questions Table

Full title: Absence of Dedicated Refused Alternatives Section and Numbered Open Questions Table

**Problem:** The repository's review protocol (`SKILL.md §1 & §2.1`) and `brief-common.md` mandate
that all major design documents follow house style:

1. **Refused Alternatives**: "Designs justify every mechanism by citing exact repository files and
   line numbers (`file:line`), pricing the cost (bytes, cycles, links, messages, segments), and
   recording refused alternatives with the technical reason they were rejected."
1. **Open Questions Table**: "Deferred (`U-X<k>`): Recorded in the document's Open Questions table
   with a concrete default or scope boundary."

The document contains neither section. While isolated justifications are scattered in prose (e.g.,
why reactive TMS was rejected in §2.9, why inline 0-based loops are deprecated in §1.2/§3.5, and why
unisolated choice points fail in §2.8), they are not collected into a formal Refused Alternatives
section. Furthermore, several critical operational questions lack a documented resolution or
default.

**Evidence:**

Running a search for refused alternatives in the target document yields zero dedicated sections:

```sh
grep -inE "refused|alternative" design/vagent-extended-capabilities-and-belief-vlog-unification.md
```

`SKILL.md §2.1`: "Does the document follow house style (priced rulings, cited `file:line` evidence,
refused alternatives section, open questions table)?"

**Fix:** Add two dedicated sections:

1. **Refused Alternatives (§7)**: Formally record and price why alternatives were rejected:

   - *C++ side table or std::unordered_map for beliefs*: Refused (violates ZigZag topological
     persistence and Nelsonian single-primitive invariant).
   - *Full reactive Truth Maintenance System (TMS) for Horn rules*: Refused (§2.9; cascading graph
     walks exhaust turn fuel; turn-boundary settle loop provides bounded reconsideration).
   - *0-based scanning ABI in compiler*: Refused (breaks compatibility with Icon and `vicon §6`).
   - *Synchronous blocking effect calls*: Refused (violates Rule V6; stalls single-threaded actor
     engine).
   - *Direct query execution on ambient actor choice stack*: Refused (§2.8; speculative backtracking
     reverts mutable actor variables).

1. **Open Questions Table (§8)**: Provide a table of deferred decisions with concrete defaults:

   - `U-B1`: Handling circular Horn clause recursion exceeding `vagent.condition_fuel` (default:
     fail branch, raise `ConditionFault` if unbounded).
   - `U-B2`: Multi-head Horn rules and constraint logic extensions (default: deferred to post-1.0).
   - `U-B3`: UTF-8 character indexing vs. byte slicing for non-ASCII csets (default: codepoint
     collation conforming to `FontManager`/FriBidi text primitives).
   - `U-B4`: Duplicate source assertions on unkeyed predicates (default: set deduplication per
     source).

______________________________________________________________________

### 3. (§2.9, §4.5, §6) Ambiguous Cross-References Omitting vagent Doc Name

Full title: Dangling and Colliding Section References Due to Omission of `vagent` Companion Document
Name

**Problem:** The specification contains several cross-references that omit the companion document
name, causing them to collide with local sections of the current document or point to non-existent
local sections:

1. In §2.9 (line 484): "`vagent` already solves this through the settle loop (§4.2 step 6)." Local
   §4.2 is `Host Binding Contract and Contention Dispatch`, which contains no step 6. The intended
   target is `[vagent §4.2 step 6](file:///data/git/gleditor/design/vagent-agent-language.md#L450)`.
1. In §4.5 (line 785): "The adapter derives its timer heap from `waiting.deadline` cells across all
   intentions (§5.6)." Local §5.6 is
   `Proposal P18.6: Native Skill Effect Lowering and Preemption Dispatch`. The intended target is
   `[vagent §5.6](file:///data/git/gleditor/design/vagent-agent-language.md#L587)`
   (`Intentions and reserved fields`).
1. In §6 (line 926, Table 6): "None: Implements unbuilt specification of §5.3 and §6.6" Local §5.3
   is `Proposal P18.3`, and the document has no §6.6. The intended target is
   `[vagent §5.3, §6.6](file:///data/git/gleditor/design/vagent-agent-language.md#L516-L542)`.

**Evidence:**

Doc §2.9 (line 484): `the settle loop (§4.2 step 6)`

Doc §4.5 (line 785): `all intentions (§5.6)`

Doc §6 (line 926): `specification of §5.3 and §6.6`

Sibling document `design/vagent-agent-language.md`:

- §4.2 (line 450): Turn structure step 6 is the settle loop.
- §5.6 (line 587): "Intentions and reserved fields" defining `waiting.deadline`.
- §5.3 & §6.6 (lines 516, 740): Capacities, skills, and effect methods.

**Fix:** Qualify all cross-references to the companion document with explicit markdown links, e.g.,
`[vagent §4.2 step 6](file:///data/git/gleditor/design/vagent-agent-language.md#L450)`,
`[vagent §5.6](file:///data/git/gleditor/design/vagent-agent-language.md#L587)`, and
`[vagent §5.3, §6.6](file:///data/git/gleditor/design/vagent-agent-language.md#L516-L542)`.

______________________________________________________________________

### 4. (§4.3, §5.6, §6) Inaccurate Native Skill Status, Lowering & Value Return

Full title: Mischaracterization of Native Skill Execution Status, Turn Yield Bytecode, and Omission
of Value Return Unpacking

**Problem:** Section 4 and Proposal P18.6 contain multiple factual inaccuracies regarding the actual
codebase:

1. **Inaccurate status in Table 6**: In §6 (line 926, Table 6), the document states: "Declarations
   checked; effect execution stubbed | Complete lowering, plan suspension, and preemption dispatch".
   This is factually incorrect. Native skill lowering in plans and handlers, outbox staging, turn
   suspension on `kIntentionWaiting`, resume on host completion, preemption cancellation, and
   deadline timeouts are already implemented in `compiler.cpp:3950–4052`,
   `agent_executor.cpp:2525–2731`, and `tests/xuzz/vagent_effects_test.cpp:1–1094` (over 1,000 lines
   of tests covering mock devices, preemption, deadlines, and resume). Proposal P18.6 fails to
   acknowledge the existing implementation and does not specify what concrete diff or extension is
   actually being proposed.

1. **Inaccurate VM bytecode citation for turn yielding**: In §4.3 (lines 728 and 757), the text
   states: "The VM executes `#Return` back to `agent_executor`, completing the turn." In the actual
   compiler implementation
   ([`compiler.cpp:4011–4014`](../../../apps/common/xanadu/vagent/compiler.cpp#L4011-L4014)): the
   plan intention cuts to $d_0$, pops its activation frame, calls the settle loop (`rtSettle_`), and
   jumps directly to `haltOp_`. It does not execute `#Return`.

1. **Omission of the return value unpacking path**: In §4.3 (lines 735–736 and 740–760), the
   step-by-step lowering details only the failure check:
   `Verify match.ok == true (if false, fail statement)`. It completely omits how non-unit return
   values are unpacked from `waiting.match` (`kEffectOutcomeValue`, `compiler.cpp:4036–4046`) and
   bound into the caller's target variable or expression register.

**Evidence:**

Repo `apps/common/xanadu/vagent/compiler.cpp:3950–4052`: Complete implementation of effect staging,
`rtSettle_` call, `haltOp_` jump, and `kEffectOutcomeValue` unpacking:

```cpp
emit(OpcodeKind::Cut, "cut", {S(dRegAt(0))});
emitFramePop();
callLabel(rtSettle_);
jump(haltOp_);
```

Repo `apps/common/xanadu/vagent/agent_executor.cpp:2525–2731`: Full `startEffect`, `cancelEffect`,
`expireDeadlines`, and `finishEffect` routines.

Repo `tests/xuzz/vagent_effects_test.cpp`: Exhaustive test suite for native skills.

**Fix:**

1. Revise §6 Table 6 and §5.6 (P18.6) to accurately reflect that core lowering and execution are
   already implemented in `compiler.cpp` and `agent_executor.cpp`. Frame P18.6 as formalizing the
   ABI invariants, documenting the bytecode lowering, and hardening edge cases (e.g., composite
   value copying and error propagation).
1. Correct the turn-yielding description in §4.3 (line 757) to state that the plan cuts choice
   depth, pops the frame, invokes `rtSettle_`, and branches to `haltOp_`.
1. Add the return value unpacking step to §4.3: when `match.ok == true`, read `match.value`
   (`kEffectOutcomeValue`), copy composite values via `callCopy`, and bind into the destination
   register.

______________________________________________________________________

### 5. (§4.3, line 741; §4.5, line 780) Conflating effectId with Host token

Full title: Terminology and Architectural Conflation of Agent `effectId` with Host Registry `token`

**Problem:** Section 4 conflates two distinct identifier lifecycles:

1. `effectId`: The agent-internal integer allocated from `$next_id`, written to `kOutboxEffectId`
   and `waiting.key` during turn execution.
1. `token`: The `std::uint64_t` minted asynchronously by `HostBindings::start(...)` in the C++ host
   registry when the adapter drains the outbox between turns.

In §4.3 lines 741–754, the document writes:

- "1. Token Allocation: Read `$next_id`, increment it, and bind the token to $T_{\text{effect}}$."
- "3. Intention State Update: Write `waiting.kind = "effect"`. Write `waiting.key = token`."

And in §4.5 line 780:

- "the adapter immediately invokes `NativeSkill::cancel(in_flight_token)`."

This is architecturally inaccurate:

- The agent does not allocate a "token" during the turn; it allocates an `effectId`. The host
  `token` does not exist when the intention is suspended.
- The host `token` is returned by `HostBindings::start` when the adapter drains `d.outbox`
  ([`agent_executor.cpp:2553–2557`](../../../apps/common/xanadu/vagent/agent_executor.cpp#L2553-L2557))
  and is held in the C++ runtime's `state.flights` table.
- When preemption occurs, the adapter calls `HostBindings::cancel(token)`
  ([`agent_executor.cpp:2568`](../../../apps/common/xanadu/vagent/agent_executor.cpp#L2568)), which
  in turn delegates to `NativeSkill::cancel(token)`. The adapter does not invoke `NativeSkill`
  directly.

**Evidence:**

Repo `apps/common/xanadu/vagent/runtime_abi.hpp:358–360`:

```cpp
/// The effect's id (Int, from `$next_id`): the intention's `waiting.key`, and
/// the only field of a cancel entry.
inline constexpr std::string_view kOutboxEffectId = "id";
```

Repo `apps/common/xanadu/vagent/host_bindings.hpp:88–89, 137`:

```cpp
struct EffectRequest {
  std::uint64_t token{};
};
std::uint64_t start(...);
void cancel(std::uint64_t token);
```

Repo `apps/common/xanadu/vagent/agent_executor.cpp:2531–2558`:

```cpp
state.flights.emplace(item.effectId, Flight{.token = 0, ...});
const auto token = bindings_->start(...);
flight->second.token = token;
```

**Fix:** Distinguish `effectId` and `token` cleanly:

1. Rename step 1 in §4.3 to "Effect ID Allocation: Read `$next_id`, increment it, and bind
   $ID_{\text{effect}}$."
1. In step 2, stage `kOutboxEffectId = ID_{\text{effect}}`.
1. In step 3, write `waiting.key = ID_{\text{effect}}`.
1. In §4.5, clarify that the adapter maps `effectId` to the host `token` via its internal flights
   table, and dispatches cancellation through `HostBindings::cancel(flight.token)`.

______________________________________________________________________

## MINOR

### 6. (§TOC, lines 57–87; §6, line 931) TOC Title Mismatches & Missing Battery

Full title: Mismatched Section Titles in Table of Contents and Omission of Acceptance Battery

**Problem:** Numerous entries in the Table of Contents (lines 55–88) do not match the actual
headings in the body:

- Line 57: `[1.2 Why Re-Implementing Logic in Front End Is Defective]` vs. heading line 132:
  `### 1.2 Why Re-Implementing Logic in the Agent Front End Is Defective` (missing "the Agent").
- Line 63: `[2.4 Belief Mutation Mechanics (believe, forget, etc.)]` vs. heading line 283:
  `### 2.4 Belief Mutation Mechanics (believe, forget, revise, tell, untell)`.
- Line 64: `[2.5 Deductive vs. Assertional Reasoning]` vs. heading line 343:
  `### 2.5 Deductive vs. Assertional Reasoning in believes and findall`.
- Line 71: `[3.2 The Scanning Contract: 1-Based Indices, Csets]` vs. heading line 512:
  `### 3.2 The Scanning Contract: 1-Based Indices, Csets, and Substrings`.
- Line 72: `[3.3 Exhaustive Specification of Scanning Built-ins]` vs. heading line 525:
  `### 3.3 Exhaustive Specification of the Scanning Built-ins`.
- Line 74: `[3.5 Harmonization: std:goal vs. Compiler Lowering]` vs. heading line 629:
  `### 3.5 Harmonization: std:goal Library vs. Compiler Lowering`.
- Line 78: `[4.3 Effect Lowering in Plans: Suspension & Turn Yielding]` vs. heading line 703:
  `### 4.3 Effect Lowering in Plans: Suspension and Turn Yielding`.
- Line 79: `[4.4 Effect Staging in Handlers: Fire-and-Forget]` vs. heading line 760:
  `### 4.4 Effect Staging in Handlers: Fire-and-Forget Semantics`.
- Lines 82–87: Proposals P18.1–P18.6 in the TOC use abbreviated titles that differ from the full
  section titles in §5.1–§5.6.
- Subsection `### Acceptance Verification Battery` (line 931) under §6 is missing entirely from the
  TOC.

**Fix:** Align all Table of Contents entry titles exactly with their target headings, and add
`[6.1 Acceptance Verification Battery](#acceptance-verification-battery)` under Section 6.

______________________________________________________________________

### 7. (§2.4, line 312) Ambiguous Set Semantics for Unkeyed Predicate Sources

Full title: Missing Clarification on Source Deduplication for Unkeyed Predicate Assertions

**Problem:** In §2.4 (line 321), the insertion branch states:

> "Search for a clause $C_{target}$ on `+d.clause` matching `args` identically: If found: link a new
> peer cell for $S$ to the posward tail of $C_{target}$'s `+d.source` rank."

For unkeyed predicates, if the same source $S$ calls `believe P(1)` twice, it is ambiguous whether a
duplicate peer cell for $S$ is appended to `+d.source` (multiset semantics) or whether insertion is
idempotent (set semantics). If duplicate cells are appended, a subsequent `forget P(1)` unlinks only
one peer cell, leaving the fact believed despite the forget.

**Fix:** Explicitly state that `+d.source` maintains set semantics: when linking source $S$ to an
existing matching clause $C_{target}$, if a peer cell for $S$ is already present on $C_{target}$'s
`+d.source` rank, the operation is an idempotent no-op.

______________________________________________________________________

### 8. (§2.8, line 464) Query Trail Safety Formula Imprecision on Caller Vars

Full title: Query Trail Safety Formula Inaccurately Forbids Caller Variable Speculative Bindings

**Problem:** In §2.8 (line 464), the trail safety condition is stated as:

```math
\text{Trail safety condition: } \quad \forall t \in \text{QueryTrail}, \quad t.\mathit{dense} \ge m_0.\mathit{cellCount}
```

This formula states that every trail entry recorded during query evaluation must belong to a cell
minted at or after $m_0$. However, if a query `believes P(?x)` is passed an unbound logic variable
`?x` allocated in the caller prior to the query ($dense(?x) < m_0.cellCount$), a speculative branch
unifying `?x` will record a `TrailEntry` with $t.dense < m_0.cellCount$. When that speculative
branch fails, `release(m_i)` restores `?x`. The true invariant is that **mutable actor variables**
on `$agent`'s `d.vars` rank are never trailed or modified by logic search, and all query-internal
choice points are pruned back to $d_0$.

**Fix:** Clarify the prose and formula to state that the isolation condition applies to actor state:
$\forall t \in \text{QueryTrail}, \quad t.\mathit{dense} \notin \text{ActorVarSlots}$, ensuring that
speculative unification trail entries on caller logic variables are safely undone at choice-point
boundaries without touching actor variables.

______________________________________________________________________

VERDICT: NOT CONVERGED
