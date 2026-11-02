# Round 1 critique: PL Design & Ergonomics Expert (vagent-belief-vlog-unification.md)

Paths below are relative to the repo root. Tally: 2 BLOCKING, 5 MAJOR, 3 MINOR.

## BLOCKING

### 1. (§3.3, line 525) Scanning API contract scrambles signatures and breaks `tab(find())`

Full title: String scanning built-in specification scrambles return types, inverts cursor mutation
contracts, and destroys `tab(find("foo"))` generator composition

**Problem:** In §3.3, the specifications for string scanning built-ins contradict both Griswold's
canonical Icon contract and the live implementation in
`apps/common/xanadu/vagent/compiler.cpp:2413–2700`:

1. §3.3.5 specifies that `find(s: Str): Generator<Int>` "updates `&pos` to the found location and
   yields the position integer". This fatal side-effect destroys the most common generator idiom in
   goal-directed scanning: `tab(find("foo"))`. `tab(i)` is specified (§3.3.2) as computing traversed
   substring length as $\text{length} = |\text{oldPos} - i|$. If `find` has already updated `&pos`
   to $i$ prior to yielding, then when `tab(i)` executes, $\text{oldPos} = i$. Consequently, `tab`
   computes $\text{length} = |i - i| = 0$ and produces the empty string `""` instead of the text
   preceding `"foo"`! Furthermore, downstream failure in an alternation like
   `tab(find("a")) | tab(find("b"))` would have already moved `&pos` during the argument evaluation
   of `find`, corrupting the cursor before `tab` even executes.
1. In §3.3.6, `upto(cs: Str): Generator<Int>` is specified as yielding positions *without*
   automatically advancing `&pos` ("conventionally paired with `tab(upto(cs))`"). `find` and `upto`
   are sibling search generators over positions; having `find` advance `&pos` while `upto` does not
   is an irreconcilable semantic inconsistency.
1. In §3.3.4, §3.3.7, and §3.3.8, `match(s)`, `any(cs)`, and `many(cs)` are specified as advancing
   `&pos` and producing `Str`. In Icon and in `compiler.cpp:2501, 2605, 2657`, `match`, `any`, and
   `many` all return `Ty::K::Int` (the position after the matched prefix/character/run) and *do not*
   advance `&pos`. Moving `&pos` is the sole responsibility of `tab(i)` and `move(i)`. If `match`,
   `any`, and `many` advanced `&pos` and returned `Str`, then standard expressions like
   `tab(match("prefix"))` and `tab(many(" \t"))` would become type errors (`tab` requires `Int`),
   and non-destructive lookahead (`if match("header") then ...`) would become impossible.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:571–575`:
  "`find(s: Str): Generator<Int>` ... On each produce: updates `&pos` to the found location and
  yields the position integer."
- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:583–584`:
  "`upto(cs: Str): Generator<Int>` ... yields the position without automatically advancing `&pos`
  past the character (conventionally paired with `tab(upto(cs))`)"
- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:553–554`: "`tab(i: Int): Str`
  ... Produces the substring between the old `&pos` and new `&pos`:
  $\text{offset} = \min(\text{old}, i) - 1$, $\text{length} = |\text{old} - i|$."
- `apps/common/xanadu/vagent/compiler.cpp:2500–2501`: `match` produces `endPos` (`Ty::K::Int`)
  without updating `scanPosG_`.
- `apps/common/xanadu/vagent/compiler.cpp:2570, 2578`: `find` yields candidate index `i`
  (`Ty::K::Int`) without updating `scanPosG_`.
- `apps/common/xanadu/vagent/compiler.cpp:2604–2605`: `any` produces `any+` (`Ty::K::Int`) without
  updating `scanPosG_`.
- `apps/common/xanadu/vagent/compiler.cpp:2656–2657`: `many` produces end index `i` (`Ty::K::Int`)
  without updating `scanPosG_`.

**Fix:** Realign §3.3 with the orthogonal Icon / vicon architecture:

1. Positional analysis functions: `find(s: Str): Generator<Int>`, `upto(cs: Str): Generator<Int>`,
   `bal(cs: Str, open: Str = "(", close: Str = ")"): Generator<Int>`, `match(s: Str): Int`,
   `any(cs: Str): Int`, and `many(cs: Str): Int`. ALL of these return or generate integer positions
   $\ge 1$ and leave `&pos` strictly unmodified.
1. Motion and extraction functions: `tab(i: Int): Str` and `move(i: Int): Str` are the ONLY
   functions that update `&pos` and produce ephemeral quoting substring cells
   (`value(s, off, len)`).
1. This restores uniform composition: `tab(find("foo"))` advances `&pos` to the start of `"foo"` and
   returns the preceding text; `move(length(s))` or `tab(match(s))` consumes the matched token; and
   lookahead guards (`if find("error") then ...`) evaluate without side-effects on `&pos`.

______________________________________________________________________

### 2. (§2.6, line 398) Grammar gap: `for believes` and `findall` prohibit `from S` filtering

Full title: EBNF grammar in `vagent §8.9` omits source provenance clauses from `for believes` and
`findall`, blocking iteration and collection over peer/sensor beliefs

**Problem:** The core deliverable of Proposals P18.1 and P18.3 is multi-source belief epistemics
(facts from `self`, peers, and sensors coexisting on `d.clause` with distinct provenance on
`+d.source`). Section 2.6 specifies source filtering for standalone `believes P(...) from S`.

However, the official `vagent` EBNF grammar (`design/vagent-agent-language.md:1295–1297`) specifies:

```ebnf
Primary ::= ...
          | 'for' ( Ident 'in' Expr | 'believes' Term ) 'do' Expr
          | 'believes' Term [ 'from' Postfix ]
          | 'findall' '(' TermArg ',' Term ')'
```

1. In a `for believes` loop, the grammar permits only `'for' 'believes' Term 'do' Expr`. The syntax
   `for believes at(who, ?x, ?y) from peer do body` is a SYNTAX ERROR because `from Postfix` is not
   permitted in `for believes`.
1. In `findall`, the syntax is strictly `'findall' '(' TermArg ',' Term ')'`. `Term` cannot take a
   `from` clause (`Term ::= Ident '(' [ TermArg { ',' TermArg } ] ')'`). The syntax
   `findall(?x, at(who, ?x, _) from peer)` is a SYNTAX ERROR.
1. Consequently, developers cannot iterate over beliefs asserted by a specific peer or sensor, nor
   can they collect all solutions from a specific source into a list. An agent is forced to either
   iterate over all sources combined or manually write procedural filtering checks over `$sender`.

**Evidence:**

- `design/vagent-agent-language.md:1295–1297`:
  `'for' ( Ident 'in' Expr | 'believes' Term ) 'do' Expr` `'believes' Term [ 'from' Postfix ]`
  `'findall' '(' TermArg ',' Term ')'`
- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:345`: "When evaluating a
  belief query `believes P(t_1, \dots, t_n)` or `findall(?X, P(...))`..."
- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:401–409`: Table §2.6 defines
  `from self`, `from peer`, `from sensor`, and `from deduced`, but provides no corresponding grammar
  production for `for believes` or `findall`.

**Fix:** Amend the grammar in `vagent §8.9` and incorporate the grammar extension into Proposal
P18.1:

```ebnf
Primary ::= ...
          | 'for' ( Ident 'in' Expr | 'believes' Term [ 'from' Postfix ] ) 'do' Expr
          | 'believes' Term [ 'from' Postfix ]
          | 'findall' '(' TermArg ',' Term [ 'from' Postfix ] ')'
```

Update `Gen::forExpr` and `Gen::findallExpr` in `apps/common/xanadu/vagent/compiler.cpp` to parse
the optional source filter operand and emit the source-constrained clause scan.

______________________________________________________________________

## MAJOR

### 3. (§2.4, line 283) Conflation of inter-agent `tell`/`untell` with local belief mutation

Full title: Section 2.4 conflates external speech acts (`tell`/`untell`) with local belief mutation
primitives, violating provenance integrity

**Problem:** Section 2.4 is titled "Belief Mutation Mechanics (`believe`, `forget`, `revise`,
`tell`, `untell`)" and specifies in line 312: "`believe P(args)` (implicit source: `self`) /
`tell S P(args)` (source: `S`)" and in line 327: "`forget P(args)` (source: `self`) /
`untell S P(args)` (source: `S`)".

This conflates an external speech act (`Action`) with internal belief base maintenance:

1. In `vagent §8.9:1300` and `compiler.cpp:4809–4854`, `tell S P(args)` is an `Action` where `S` is
   the *recipient* peer of an outbox envelope (`kKindTell`). It stages a message to be transmitted
   across the wire to peer `S`. The executing agent does NOT mutate its own belief base with source
   `S`.
1. Presenting `tell S P(args)` as a local mutation primitive implies that an agent could execute
   `tell "rover/sensor#1" at(...)` to unilaterally forge beliefs attributed to a remote sensor in
   its own local store.
1. Furthermore, §2.4 defines `revise P(args)` strictly as `(source: self)`. It leaves completely
   unspecified how incoming `$tell` messages for keyed predicates trigger atomic functional
   dependency replacement on the receiver.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:312, 327`: "`believe P(args)`
  (implicit source: `self`) / `tell S P(args)` (source: `S`)"
- `design/vagent-agent-language.md:1300`: `Action ::= ... | ( 'tell' | 'untell' ) Postfix Term`
- `apps/common/xanadu/vagent/compiler.cpp:4809–4813, 4854`: "`tell`/`untell` stage one outbox entry
  carrying the fact and the recipients... mints the received envelope as an event whose type is
  `$tell <pred>` / `$untell <pred>`"

**Fix:** Restructure §2.4 to clearly separate:

1. **Local Agent Mutation Statements**: `believe Term`, `forget Term`, and `revise Term`. These
   operate strictly and exclusively under source `self`.
1. **Inter-Agent Communication Actions**: `tell Recipient Term` and `untell Recipient Term`. These
   stage network outbox envelopes and perform zero local belief mutations.
1. **Reactive Perception Dispatch**: When an agent's dispatcher receives an incoming `$tell` or
   `$untell` event (or a percept from a host sensor driver), the runtime executes the internal
   clause update with `source = envelope.source`.

______________________________________________________________________

### 4. (§2.6, line 408) Unspecified lexical and grammatical status of `from deduced` pseudo-source

Full title: `from deduced` pseudo-source filter lacks formal grammar specification and risks symbol
collision with agent identifiers

**Problem:** In §2.6, line 408 introduces: `believes P(...) from deduced` | Exclusively solutions
derived via Horn clauses (no bare ground facts).

1. In `vagent §8.9:1296`, the grammar rule is `'believes' Term [ 'from' Postfix ]`. `Postfix`
   evaluates to an expression yielding an `Agent` or `Str` identifier (e.g. `self`, `peer_name`,
   `"satellite"`).
1. `deduced` is not declared as a reserved keyword in `vagent §8.1` or §8.9. If it is an unreserved
   identifier, the compiler will attempt to resolve a variable named `deduced` in scope. If no such
   variable exists, it fails with `CompileError::UnknownIdentifier`.
1. If `deduced` is made a reserved keyword, it creates a breaking change in the surface language,
   preventing variables, record fields, or goals from being named `deduced`.
1. If `from "deduced"` is treated as a magic string, an agent or sensor genuinely named `"deduced"`
   cannot be addressed.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:408`:
  `believes P(...) from deduced`
- `design/vagent-agent-language.md:1296`: `Primary ::= ... | 'believes' Term [ 'from' Postfix ]`

**Fix:** Disambiguate the syntax in §2.6 and §8.9:

- Avoid polluting the identifier space by avoiding a bare unquoted keyword `deduced`.
- Prefer either an explicit query form `deduces P(...)` (a sibling to `believes P(...)`), or a
  structured keyword-atom syntax: `believes P(...) from :deduced` (or `believes P(...) from rule`).
- Formally document the grammar production in Proposal P18.1.

______________________________________________________________________

### 5. (§4.4, line 760) Unchecked fire-and-forget: non-void effect method calls in event handlers

Full title: Missing static validation allows non-void effect methods to be invoked in event
handlers, leaving return values undefined

**Problem:** Section 4.4 specifies fire-and-forget lowering when an effect method is called inside
an event handler, because Rule V6 forbids handlers from suspending turns: "When an effect method is
called inside an event handler... The handler does not suspend; execution continues immediately to
the next statement in the handler block. If the effect subsequently succeeds in the host, the result
is discarded silently."

However, capacity effect methods may declare return types (`vagent §8.9:1245`):

```vagent
capacity Spectrometry {
  effect def sample_composition(): Spectrum within 10s
}
```

If a developer writes inside an event handler: `let s := sample_composition()`

1. What does `sample_composition()` evaluate to in the handler?
1. Because the handler cannot suspend, the host effect has not executed, and no `Spectrum` result
   exists. Does `s` receive `()`, `false`, an uninitialized cell, or does the VM fault?
1. The specification fails to define a compile-time check prohibiting non-void effect method calls
   in non-suspending contexts.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:767–771`: "The handler does
  not suspend... If the effect subsequently succeeds in the host, the result is discarded silently."
- `design/vagent-agent-language.md:850`: "`x := m(args)`, `let x := m(args)`, `m` an effect: waits
  for the host's result"
- `design/vagent-agent-language.md:1245`:
  `MethodSig ::= [ 'effect' [ 'preempts' ] ] 'def' Ident '(' [ ParamList ] ')'`
  `[ ':' Type ] [ 'within' Duration ]`

**Fix:** Add an explicit static semantic rule to the `vagent` checker in §4.4 and Proposal P18.6:

- Any `effect` method whose return type is not `Unit` / `()` cannot be invoked from inside an event
  handler, lifecycle block (`on install`/`on uninstall`), or regular `def` method.
- The compiler must reject such calls at compile time with `CompileError::CannotSuspend` ("calling
  non-void effect method '%s' requires intention suspension; only permitted in plan bodies").
- In event handlers, only void effect methods (`effect def m(...) within ...` with no return type or
  `: ()`) are legal under fire-and-forget semantics.

______________________________________________________________________

### 6. (§2.7, line 414) Range-restriction and dynamic floundering contract in negation as failure

Full title: Stratified negation specification lacks formal rule range-restriction and conflates
unsoundness floundering with goal search failure

**Problem:** Section 2.7 introduces stratified negation under Clark's Completion:
`not believes P(...)`.

1. **Incomplete Static Check Definition**: Line 425 states: "The vagent checker verifies that every
   variable occurring inside a `not believes` literal is bound by an earlier positive literal in the
   same conjunction, guard, or rule body." In Horn rules ($Head \leftarrow Body$), this definition
   is inadequate. Logic programming requires strict **range-restriction** (safety): every variable
   occurring in the rule head $Head$ and every variable occurring in a negative body literal
   $\text{not } Q$ must occur in at least one *positive* body literal. If a rule like
   `safe(X) :- not dangerous(X)` is accepted because $X$ appears in the head, any goal query
   `believes safe(?who)` with an unbound argument will immediately flounder at runtime.
1. **Ambiguous Floundering Failure Semantics**: Line 428 states: "The runtime triggers a contract
   violation (`floundered_negation`), failing the statement with cause `'floundered'`." In `vagent`,
   "failing a statement" in a plan triggers intention failure (`PlanFailed`), whereas in an `if`
   condition, guard, or `while` loop, expression failure normally evaluates to false (or triggers
   backtracking). Floundering is a logical unsoundness trap, not ordinary search failure: evaluating
   a floundered `not believes P(?X)` to false would unsoundly assert that $\forall X. P(X)$ holds!
   The document fails to distinguish between search failure (falsehood) and execution faults.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:425–430`: "Static Check: The
  vagent checker verifies that every variable occurring inside a `not believes` literal is bound by
  an earlier positive literal in the same conjunction, guard, or rule body. Dynamic Guard: If an
  unbound logic variable reaches `not believes P(?X)`, the runtime triggers a contract violation
  (`floundered_negation`), failing the statement with cause `'floundered'`."
- `design/vagent-agent-language.md:594`: "`faulted` (conditions whose last evaluation faulted)"
- `design/vagent-agent-language.md:1468`: "`ConditionFault{intention, cause}`"

**Fix:**

1. Formally define the static safety rule as strict **range-restriction**: for any rule
   $H \leftarrow B_1 \land \dots \land B_m$, all variables in the head and in negative literals must
   appear in at least one positive body literal:

   ```math
   \text{vars}(H) \cup \bigcup_{B \in \text{neg}} \text{vars}(B) \subseteq \bigcup_{B \in \text{pos}} \text{vars}(B)
   ```

   A rule violating range-restriction must be rejected at compile time with
   `CompileError::UnsafeRuleVariable`.

1. Specify that dynamic floundering is not normal goal failure (which yields no solutions or false),
   but an evaluation fault that raises `ConditionFault{intention, cause: "floundered"}` and halts
   deliberation on that intention.

______________________________________________________________________

### 7. (§2.3, line 246) Key ordering in `PredicateDecl`: unenforced grammar and silent compiler bug

Full title: `PredicateDecl` allows arbitrary key placement in grammar while compiler silently drops
non-leading keys

**Problem:** Section 2.3 defines keyed predicates with leading keys:
$P(K_1, \dots, K_k, V_1, \dots, V_m)$. In `vagent §6.1`, it states: "Every predicate is declared,
with its key arguments first and marked `key` when it has any."

However:

1. In `vagent §8.9:1238–1239`, the grammar places no syntactic constraints on `key` placement:
   `PredParam ::= [ 'key' ] Ident ':' Type ;`
1. In `apps/common/xanadu/vagent/compiler.cpp:4170–4180`, the compiler iterates through parameters:
   when it encounters the first parameter without `key`, it sets `leadingKeys = false`. If
   subsequent parameters carry `key`, they are SILENTLY IGNORED! The compiler does not increment
   `pi.nkeys` and emits no error or warning. A user declaring
   `predicate item(id: Str, key sku: Str)` believes `sku` is an enforced functional dependency, but
   at runtime it is treated as an unkeyed value!
1. Proposal P18.3 (§5.3) states "Add compiler validation for `key` modifiers in `PredicateDecl`",
   but omits the specific validation rule and diagnostic code.

**Evidence:**

- `apps/common/xanadu/vagent/compiler.cpp:4170–4180`:

  ```cpp
  bool leadingKeys = true;
  for (const PredParam &p : pd->params) {
    ...
    if (p.key && leadingKeys) {
      ++pi.nkeys;
    } else {
      leadingKeys = false;
    }
  }
  ```

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:852–855`: "Proposal P18.3 ...
  Add compiler validation for `key` modifiers in `PredicateDecl`."

**Fix:** Specify in §2.3 and Proposal P18.3:

- If any parameter marked `key` follows a parameter without `key`, the checker must reject the
  declaration with `CompileError::InvalidKeyOrder` ("key parameter '%s' must appear before all
  non-key parameters in predicate '%s'").
- Formally clarify that all-key predicates ($m = 0$, set semantics per source) and zero-key
  predicates ($k = 0$, bag semantics per source) are valid.

______________________________________________________________________

## MINOR

### 8. (§3.2, line 516) Ergonomics of non-positive indexing in string scanning

Full title: Imprecise classification of position 0 as negative and unnormalized return in `pos(0)`

**Problem:** Line 517 states: "Negative positions count backward from the end: $0$ is after the last
character ($\text{len} + 1$), $-1$ is before the last character, and $-\text{len}$ is before the
first character ($1$)." Zero is non-positive, not negative. In Icon and vicon §6, the convention is
non-positive indexing ($i \le 0$), where index conversion is
$\text{normalized}(i) = \text{length}(s) + 1 + i$. Furthermore, §3.3.1 states `pos(i)` produces $i$.
If `pos(0)` succeeds when `&pos` is at the end of the string, returning $0$ is unergonomic because
passing that result to `tab` would require re-normalization.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:516–518, 545–548`.

**Fix:** Update §3.2 to refer to "Non-positive positions ($i \le 0$)" and state the canonical
conversion formula:

```math
\text{pos} = \begin{cases} i & \text{if } i > 0 \\ \text{length}(s) + 1 + i & \text{if } i \le 0 \end{cases}
```

Clarify that `pos(i)` returns the normalized 1-based positive position upon success.

______________________________________________________________________

### 9. (§2.1, line 186) Ground term requirement on `believe` and `revise` unstated

Full title: Missing static validation rule requiring `believe` and `revise` operands to be ground
terms

**Problem:** Section 2 establishes that facts on `d.clause` with `!link(C, +d.spin)` are "ground
assertional facts" and that `+P`/`-P` events reflect extensional ground transitions only. However,
the document never explicitly specifies the static checking rule for `believe Term` and
`revise Term`. If a programmer writes `believe at(self, ?x, 10.0)`, can a pattern variable `?x` be
asserted into the extensional database? If so, the clause is no longer ground, breaking ground
unification and event generation.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:186–202, 312–342`.

**Fix:** Explicitly specify in §2.4 and Proposal P18.3 that `believe` and `revise` statements
require strictly ground terms. The compiler must reject any occurrence of a pattern variable
(`?var`) in a `believe` or `revise` statement with `CompileError::UngroundFact`. Pattern variables
are permitted only in queries (`believes`, `findall`), retractions (`forget`), event triggers (`+P`,
`-P`), and Horn rules.

______________________________________________________________________

### 10. (§5.2, line 824) Proposal Register P18.2 omits C++ interface signatures in `vlog.hpp`

Full title: Proposal P18.2 lists `vlog.hpp` in affected files without specifying C++ API signatures
or proving zero-sentinel compliance

**Problem:** Proposal P18.2 lists `apps/common/xanadu/zigzag/vlog.hpp` and `vlog.cpp` under "Files
Affected", stating that the SLD resolution engine is implemented there. However, no C++ class,
struct, or method declarations are provided. Under repository house rules (`AGENTS.md`), all runtime
APIs must use `std::expected` / `std::optional` with zero sentinel values (`noCell`). Without
explicit signatures, implementers cannot verify interface ergonomics or house-rule compliance.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:824–845`.
- `AGENTS.md` (Standing rules: zero sentinels outside wire/disk formats).

**Fix:** Add the concrete C++ interface declaration to Proposal P18.2 in `vlog.hpp`:

```cpp
struct SldOptions {
  std::uint32_t maxFuel{1000};
  std::optional<std::string_view> sourceFilter;
};

using SldSolutionCallback = std::function<bool(const SldSolution &)>;

std::expected<std::size_t, SldError> solve(CellRef goal, SldOptions options,
                                           SldSolutionCallback onSolution);
```

______________________________________________________________________

VERDICT: NOT CONVERGED
