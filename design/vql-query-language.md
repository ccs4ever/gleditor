# Vortex Query Language (VQL) Specification

**Document Version:** 5.0.0 — Xanalogical Revision **Compilation Target:** Vortex Hyperstructural
Runtime Core (Single-Primitive Matrix Manifold)

VQL is the declarative, XQuery-like companion language for
[Vortex](vortex-hyperstructural-runtime.md): every VQL query compiles down to the same
`link`/`get`/`set` primitives that make up Vortex's single-primitive core. As with the Vortex spec,
this document specifies a language design consuming the Zigzag `zzstructure` manifold model (see
[zigzag-multidimensional-space-and-projection.md](zigzag-multidimensional-space-and-projection.md));
nothing here is wired into the gleditor build.

The language rests on three design choices: a single directional traversal operator (`/`, with
direction carried by a sign on the dimension name rather than a second token), `%` creation sugar so
a path can extend the manifold instead of only reading it, and a Xanalogical mutation model —
`weave`, ordinary path expressions evaluated for effect — in place of an XQuery-style `update { }`
block, since a query and an edit are the same kind of walk over the same primitives.

> **§7 reconciles this specification with
> [`store-slice-convergence.md`](store-slice-convergence.md)**, which makes a zigzag cell an
> operation in the xudu ops spool. The grammar is unaffected; what moves is which cells a mutation
> is allowed to persist into, what `##` resolves to, and what `><` is built on. Read it alongside
> §1's invariants.

______________________________________________________________________

## 1. Architectural Foundations & Operational Invariants

Vortex Query Language (VQL) is a declarative, zero-allocation path navigation and graph mutation
language tailored specifically for **zzstructures** (multidimensional bidirectional graphs). VQL
queries do not evaluate into ephemeral tabular heaps, relational tuples, or serialized JSON arrays.
Instead, every query expression compiles down to spatial traversals, generator pipelines, and link
mutations driven by the atomic engine:

```math
\mathcal{M} = \langle \text{link}, \, \text{get}, \, \text{set} \rangle
```

### Fundamental Invariants

- **The Single-Primitive Core**: Path traversal and every mutation, update, insertion, and
  allocation compile strictly to `link(cell, dim, dir, [target])` — reading if `target` is omitted,
  and otherwise writing: `target == -1` allocates, `target == -2` deallocates/isolates, and any
  other value (including a literal `0`) links straight to that cell, since `0` is Cell 0, the
  origin/home cell — an ordinary addressable target, not a sentinel. `link` returns the
  linked/allocated/broken cell, or nothing if there wasn't one; VQL treats an empty result the same
  way any other step that "returns nothing" is treated (§3). `get(cell, [offset], [length])`/
  `set(cell, value, [offset], [length])` are the matching content accessors — see Vortex §2's
  `get_cell_value`/`set_cell_value`, renamed here to match how they're spelled in every VQL surface
  form. Vortex §2 also defines `new(cell, dim, dir, [value])`/`break(cell, dim, dir)` as named entry
  points fixing `link`'s `target` to `-1`/`-2` respectively (`new`'s optional `value` is a `set()`
  applied to the freshly allocated cell); VQL reaches all three the same way — as ordinary
  `FunctionInvocation`s (§3) — so `$cell/break(d.foo, +1)` and `$cell/link(d.foo, +1, -2)` compile
  to the same call.
- **Dual-Wing Invocation Topology**: Built-in functions, custom routines, and opcodes adhere to the
  dual-wing interface. Out-parameters project negward along `-d.grab` (chained posward along
  `+d.step` for multiple returns). In-parameters project posward along `+d.grab` and chain along
  `+d.step`.
- **Snapshot-Before-Write Execution**: When updating nodes in-place or writing query projections,
  all input operand payloads are fully dereferenced and snapshotted prior to invoking `set()` or
  mutating pointers, preventing self-aliasing hazards.
- **Associative Cursor Scopes**: Query variables are not stored in stack frames or hash lookups.
  They resolve directly against the active Spin-Head cursor along `+d.vars`, with bound payloads or
  complex subgraphs projecting perpendicularly along `+d.values`.
- **Quantum Identity Synchronization (`d.entangle`)**: `$a><$b` entangles two cells along
  `d.entangle` — sugar for `link($a, d_entangle, +1, $b)` directly (§4.6). Entangled cells share an
  underlying pointer to a single `CellValue` variant, so mutating one immediately propagates to
  every entangled cell, across all viewports and observer ranks.
- **Zero-Allocation Lazy Rank-Streaming**: A path step (`/dim`) does not materialize an intermediate
  collection. It instantiates an $O(1)$ spatial cursor that lazily yields matching `cell_id`
  coordinates along `dim`'s entire rank until `link`'s read form returns nothing or a loopback
  boundary is reached — a single `/` step already walks the whole rank, not one hop of it. Absence
  of a link, not a `cell_id` of `0`, is what ends the stream, so a rank can freely pass through Cell
  0 without truncating early.
- **Xanalogical Mutation**: VQL does not have a separate "update" sublanguage. `%` creation sugar
  and `link`/`set` calls are ordinary path steps with side effects; a query that reads and a query
  that writes are the same grammar, evaluated under `return` or `weave` respectively. This mirrors
  the rest of the system: an edit is a walk that happens to allocate and link, not a different kind
  of operation layered on top of the read path.
- **Persistent Star-Pivot Caching**: Expensive generative transformations (e.g., regex compilation
  or macro expansions) memoize their topological results off the root origin (`##` / Cell 0) along
  `+d.cache`, anchoring invocation nodes to input and output manifolds.

______________________________________________________________________

## 2. Syntactic Token & Structural Shorthand Matrix

| Token / Operator    | Structural Equivalent                   | Functional & Spatial Semantic Mapping                                                                       |
| ------------------- | --------------------------------------- | ----------------------------------------------------------------------------------------------------------- |
| `##`                | Origin Anchor (`cell_id = 0`)           | Grounds query context to the absolute system environment origin.                                            |
| `#`                 | Root Metacells                          | Lazily streams all disjoint root manifold entry points across the matrix.                                   |
| `^`                 | Process Manifold (`d.cursors`)          | Streams all active Spin-Head execution cursor threads.                                                      |
| `^NAME`             | `^[. = "NAME"]`                         | Short-circuits the cursor scan, locking directly onto the named thread node.                                |
| `.`                 | Context Identity                        | The current step's context cell — a valid anchor on its own, or `get(context)` when dereferenced.           |
| `/dim`              | `LazyRankStream(dim, posward)`          | Traverses `dim`'s entire rank posward from the context cell.                                                |
| `/-dim`             | `LazyRankStream(dim, negward)`          | Traverses `dim`'s entire rank negward — the `-` binds to the dimension name, not the operator.              |
| `/dim%`             | Create (`link(@, dim, dir, -1)`)        | Allocates a new cell along `dim` (at the tail of any existing rank); see §4.5.                              |
| `/dim%VALUE`        | Create + init                           | Allocates and initializes a new cell's content to `VALUE` — bare, quoted, or `$variable`.                   |
| `/dim%%...`         | Batch create                            | Each additional `%` allocates one more cell along `dim`; all of them join the step's result set — see §4.5. |
| `A><B`              | Entangle (`link(A, d_entangle, +1, B)`) | Binds `A` and `B` to one shared payload; chainable (`A><B><C`) and combinable with `%` — see §4.6.          |
| `.[offset, length]` | `get(ctx, off, len)`                    | Zero-copy virtual string slice dereference.                                                                 |
| `@`                 | Context Identity Address                | Evaluates to the numerical `cell_id` coordinate of the context node itself.                                 |
| `[...]`             | Predicate / Index Window                | Applies inline boolean filters or relative index clamps to a stream.                                        |
| `(...)`             | Macro Dimension Group                   | Groups dimensional sequences into a compound traversal segment.                                             |
| `*`                 | Kleene Repetition                       | Repeats the preceding macro group zero or more times until termination.                                     |
| `{...}`             | Weave Block                             | Groups a comma-separated list of effect items under `weave`.                                                |

There is a single traversal operator: `/` already walks a whole rank, so there is no "one hop" form
to distinguish it from, and direction is a property of the dimension name (an optional leading `-`),
not a second operator. Recursive or compound repetition is `(...)*`'s job (§3).

______________________________________________________________________

## 3. Formal EBNF Grammar

```
QueryExpression       ::= ExecutionBlock | PathExpression

ExecutionBlock         ::= ( ForClause | LetClause )+ WhereClause? ActionClause

ForClause              ::= "for" VariableRef "in" PathExpression
LetClause              ::= "let" VariableRef ":=" ( PathExpression | ValueExpr )
WhereClause            ::= "where" BooleanExpr ( ( "and" | "or" ) BooleanExpr )*
ActionClause           ::= ReturnClause | EffectClause | ConditionalClause

ReturnClause           ::= "return" ReturnItem ( "," ReturnItem )*
ReturnItem             ::= PathExpression | FieldWeave
FieldWeave             ::= PathStep+

EffectClause           ::= "weave" ( EffectItem | "{" EffectItem ( "," EffectItem )* "}" )
EffectItem             ::= LetClause | PathExpression | ForClause EffectClause

ConditionalClause      ::= "if" BooleanExpr ActionClause ( "else" ActionClause )?

PathExpression         ::= AnchorNode PathStep* EntangleTail?
AnchorNode             ::= "##" | "#" | NamedCursor | VariableRef | LiteralCellId | "."
NamedCursor            ::= "^" [a-zA-Z_][a-zA-Z0-9_]*
VariableRef            ::= "$" [a-zA-Z_][a-zA-Z0-9_]*
LiteralCellId          ::= [0-9]+

PathStep               ::= "/" StepSelector PredicateClause* RangeClamp?
StepSelector           ::= SignedDimension CreateSuffix? | MacroDimensionGroup | FunctionInvocation
SignedDimension        ::= "-"? DimensionIdentifier
CreateSuffix           ::= ( "%" CreateValue? )+
CreateValue            ::= ValueExpr | BareLiteral
BareLiteral            ::= (run of characters excluding whitespace, "%", ",", "(", ")", "[", "]", "{", "}")

EntangleTail            ::= ( "><" EntangleOperand )+
EntangleOperand         ::= PathExpression | BareCreate
BareCreate              ::= "%" CreateValue?

DimensionIdentifier    ::= [a-zA-Z_][a-zA-Z0-9_]* ( "." [a-zA-Z_][a-zA-Z0-9_]* )*
MacroDimensionGroup    ::= "(" PathStep+ ")" RepetitionModifier?
RepetitionModifier      ::= "*" | "+" | "?"

PredicateClause        ::= "[" BooleanExpr "]"
RangeClamp             ::= "[" IndexParam ( "," IndexParam )? "]"
IndexParam             ::= "-"? [0-9]+

BooleanExpr            ::= BooleanTerm ( "or" BooleanTerm )*
BooleanTerm            ::= BooleanFactor ( "and" BooleanFactor )*
BooleanFactor           ::= "not"? ( ComparisonExpr | PredicateTest )
ComparisonExpr         ::= ValueExpr CompOp ValueExpr
CompOp                 ::= "=" | "!=" | "<" | ">" | "<=" | ">="
PredicateTest          ::= PathExpression | "." | ExtendedTruthinessCheck | FunctionInvocation

ExtendedTruthinessCheck ::= "?" ( PathExpression | ValueExpr )

ValueExpr              ::= PathExpression | ScalarLiteral | VariableRef | FunctionInvocation | "@"
ScalarLiteral           ::= StringLiteral | NumericLiteral | BooleanLiteral
StringLiteral           ::= '"' [^"\\]* '"'
NumericLiteral          ::= ("-" | "+")? [0-9]+ ( "." [0-9]+ )?
BooleanLiteral          ::= "true" | "false"

FunctionInvocation      ::= Identifier "(" ArgumentList? ")"
ArgumentList            ::= ValueExpr ( "," ValueExpr )*
```

A few grammar points worth calling out explicitly:

- **A bare `PathExpression` is a complete query.** `##/d.people[. = "Alice"]` is valid on its own —
  no `for`/`return` ceremony required for a one-off traversal. Chaining a function onto a path is
  just another step (`$path/func()`, reachable via `StepSelector`'s `FunctionInvocation`
  alternative), applied per cell in `$path`'s stream and terminating to an empty set wherever `func`
  returns nothing — the same map-and-terminate behavior every other `PathStep` already has, so there
  is no separate pipe-forward operator layered on top of it.
- **Direction lives on `SignedDimension`'s optional leading `-`.** `/-d.parent` traverses `d.parent`
  negward; there is no second traversal operator for it.
- **`.` is a valid `AnchorNode`.** This is what lets a predicate step *into* a dimension from its
  own candidate cell — `[./d.inputs[. = $pattern]]` reads as "this candidate's `d.inputs` rank has a
  member equal to `$pattern`."
- **A `FunctionInvocation` reached via `StepSelector` takes its context implicitly.** If its
  `ArgumentList` doesn't already open with an explicit `@` or `.`, the context cell the step is
  chained from is prepended as the first argument — as `@` (a cell reference) if the function's
  first parameter expects a cell, or as `.` (the dereferenced value) if it expects a value; which
  one a given function takes is fixed by its declared signature, not inferred per call. This is why
  `$cell/link(d.foo, +1, $target)` means `link(@, d.foo, +1, $target)` with `@` bound to `$cell` —
  `link` returns the cell it just linked, so the chain is just as continuable — and why
  `$path/substring(1, 8)` and `$path/set("foo")` need no explicit context argument at all:
  `substring` and `set` both expect a value first, so `.` is prepended instead of `@`. Mutation is
  not a separate "statement" grammar competing with the "expression" grammar for the same syntax;
  it's the same PathExpression machinery, evaluated for effect under `weave` instead of for value
  under `return`.
- **`NumericLiteral` accepts a leading `+`.** Every example writes directions as `+1`/`-1` for
  visual symmetry, so the grammar accepts the `+` explicitly rather than relying on it being
  optional-and-ignored.
- **`><` is a suffix on `PathExpression`, not a `PathStep`.** `EntangleTail` binds after all
  `PathStep`s have run, so `$a><$b` entangles the *results* of `$a` and `$b`'s full paths, not an
  intermediate cell partway through either one. See §4.6 for the creation-combining forms
  (`%VALUE><$a`, `$a><%VALUE`).

______________________________________________________________________

## 4. Evaluation Semantics & Compilation Rules

### 4.1 Lazy Rank-Streaming Engine

`/dim` and `/-dim` compile to the same loop; the sign only picks `direction`:

```cpp
cell_id current = start_node;
int direction = signed_dimension.negated ? -1 : +1;
cell_id target_dim = signed_dimension.dim;
while (true) {
    std::optional<cell_id> next = link(current, target_dim, direction);
    if (!next || *next == start_node) break; // Terminate on absence or loopback
    if (eval_predicates(*next)) {
        yield *next;
    }
    current = *next;
}
```

`start_node` itself may legitimately be Cell 0 (`##`, §2) — the loop terminates purely on `link`'s
read form returning nothing, never on a `cell_id` value, so nothing here special-cases 0.

- **Index Clamping (`[n]`)**: Evaluated lazily with 1-based indexing.
  - `[1]`: Short-circuits traversal on the first matching cell, avoiding unnecessary traversals over
    dense ranks.
  - `[-1]`: Scans to the rank tail, returning only the final matching coordinate.
  - `[start, end]`: Emits an index-windowed slice over the matching elements.

### 4.2 Slicing Semantics

VQL supports zero-copy virtual slicing on text payloads:

- **Direct Path Step**: `$node/.[offset, length]` calls `get($node, offset, length)`.
- **Mutation Patching**: `$node/set(replacement, offset, length)` calls
  `set($node, replacement, offset, length)`. Numeric and boolean variants ignore offsets and lengths
  entirely.

### 4.3 Extended Truthiness Rules

Predicates evaluate conditions according to canonical truthiness semantics:

- **Truthy**: `true`, non-zero double, and case-insensitive strings: `"true"`, `"1"`, `"yes"`,
  `"on"`. Unrecognized non-empty strings default to true. A bare `PathExpression` used as a
  condition (`PredicateTest`) is truthy if it selects at least one cell, independent of that cell's
  own content.
- **Falsy**: `false`, `0.0`, empty string `""`, and case-insensitive strings: `"false"`, `"0"`,
  `"no"`, `"off"`. A `PathExpression` used as a condition is falsy if it selects nothing.

A direct scalar comparison like `where $inv/d.inputs = $pattern` is a type error in intent even
where it happens to parse: `$inv/d.inputs` is a rank, not a value, and may stream zero, one, or many
cells. The correct form filters the rank with a predicate and lets its non-emptiness stand for the
condition — `where $inv/d.inputs[. = $pattern]` — or binds a single dereferenced value first with
`let` before comparing it.

### 4.4 Result Materialization: Topological Return Weaving

`return` avoids heap allocations or string serializations, and it does not introduce a JSON-shaped
literal that has nothing to do with the cell model it's returning from:

1. The engine instantiates an Invocation Result Cell appended posward along the active cursor's
   `+d.results` rank.

1. A `PathExpression` return item (e.g. `return $status_code`) links its resolved value posward off
   the invocation cell along `+d.values` — the scalar-return case.

1. A `FieldWeave` return item — a bare chain of `PathStep`s with no anchor, always starting with `/`
   — is implicitly rooted at the Invocation Result Cell instead of the current query context. This
   is what lets a multi-field result be written as ordinary `%`-creation (§4.5) rather than a
   bracketed record literal:

   ```
   return
       /d.vars%"worker_id"/d.values%$worker/@,
       /d.vars%"status"/d.values%$status_code
   ```

   Each item creates a labeled `d.vars` cell off the result cell and pairs it with a `d.values` cell
   holding the projected value — exactly the same `d.vars`/`d.values` shape Vortex §4 uses for
   cursor scopes, so a query result and a variable scope are the same structure, walkable the same
   way.

### 4.5 Cell Creation Sugar (`%`)

`%`, suffixed onto a `SignedDimension` step, is sugar over allocation:

- **Bare (`/dim%`)**: `link(@, dim, dir, -1)`. Applied once per cell in the current context stream
  (like any other step).
- **Initialized (`/dim%VALUE`)**: the same allocation, immediately followed by `set(@, VALUE)` on
  the newly allocated cell. `VALUE` may be a bare token (`/d.status%OK`), a quoted string when it
  contains spaces or punctuation (`/d.status%"needs review"`), or a variable (`/d.status%$value`).
- **Tail-seeking**: if the context cell already has a link along `dim` in the given direction, `%`
  walks to the tail of that rank first (the same traversal §4.1 already performs) and allocates
  there, rather than clobbering the context cell's own link slot. Without this, a loop that calls
  `/d.step%$ch` once per character would overwrite the same cell every iteration instead of building
  a chain; with it, `for $ch in EXPLODE($pattern, "") weave $nfa_start/d.step%$ch` builds the chain
  directly (§6.2), with no separate "insert at end" primitive needed.
- **Chainable**: whatever `%` lands on (freshly allocated or, on repeat, freshly appended) becomes
  the new context for the rest of the path, same as any other step — `/d.name%/d.results%VALUE`
  creates an empty cell along `d.name`, then from there creates and initializes a cell along
  `d.results`.
- **Batching (`/dim%%`, `/dim%VALUE1%VALUE2`, ...)**: each `%` after the first allocates *another*
  new cell along the same dimension — tail-seeking again, so it lands after the one just created —
  rather than re-describing the same cell. `/d.child%%` creates two empty cells on `d.child`;
  `/d.child%"Alice"%"Bob"` creates two, each initialized in order. The step's result is the set of
  every cell it just created, not only the last one, so a later step in the same path fans out over
  all of them: `/d.child%"Alice"%"Bob"/d.status%"active"` gives *both* new cells their own
  `d.status` child, the same way a later step already fans out over any other multi-cell result
  (like a plain `/dim` rank). A `for` loop calling `/dim%$value` once per iteration (§6.2) reaches
  the same "many new siblings" outcome for a runtime-determined count; `%%` is for a
  compile-time-known one.
- **Existing targets don't go through `%`.** Pointing a dimension at a cell that already exists — as
  opposed to allocating a new one — is `link(dim, dir, target)` directly, reachable as an ordinary
  `FunctionInvocation` step (§3). `%` is specifically the "make a new cell" case.

### 4.6 Entanglement Sugar (`><`)

`><` is sugar over `link(A, d_entangle, +1, B)`: two cells wired along `d.entangle` share a single
underlying `CellValue` pointer (§1), so setting either one's content afterward sets both.

- **Base form (`A><B`)**: `link(A, d_entangle, +1, B)`. `A` and `B` are each a full
  `PathExpression`'s result — `EntangleTail` (§3) binds after all of a path's own `PathStep`s, not
  mid-traversal.
- **Chainable (`A><B><C`)**: each additional operand nests the accumulated expression as the new
  `target` argument of a fresh `link` call rooted at the new operand —
  `link(C, d_entangle, +1, link(A, d_entangle, +1, B))`. Since a literal, already-existing `target`
  always echoes back out as `link`'s return value, the inner call returns `B`, so the outer call
  reduces to `link(C, d_entangle, +1, B)`: `A` and `B` entangle directly, and `C` joins the same
  group through `B`. The group ends up sharing one payload regardless of which pair is directly
  linked.
- **Payload authority follows argument order, not chain length.** In `link(cell, dim, dir, target)`,
  a fresh entanglement's shared payload is seeded from `cell`'s (the first argument's) existing
  content — `target`'s prior content is discarded. Since the leftmost operand in an `EntangleTail`
  always ends up as `cell` in the first `link` call that establishes the group (see the chaining
  rule above), the leftmost operand's value is what the whole group ends up sharing.
- **Combines with `%` (`A><%`, `A><%VALUE`, `%VALUE><A`)**: a bare `%` — with no dimension prefix,
  distinct from `/dim%`'s dimension-attached form — allocates a free cell with no incoming
  structural link at all; `%VALUE` allocates and immediately `set()`s it to `VALUE`, same as
  `/dim%VALUE` does for a dimension-attached create. Because payload authority follows argument
  order (previous bullet), where the `%` sits in the chain determines whether its `VALUE` survives:
  - **`%VALUE><$a`**: the fresh cell is the leftmost (`cell`) operand, so its `VALUE` is
    authoritative — after entangling, `$a` holds `VALUE` too.
  - **`$a><%VALUE`**: `$a` is leftmost, so `$a`'s existing value is authoritative; the fresh cell's
    `VALUE` is overwritten by the join and never observed. Write `$a><%` instead if the intent is
    just "give me a new cell entangled with `$a`" — the value is discarded either way, so spelling
    out a `VALUE` there is misleading.
  - Any `VALUE` past the first operand in a longer chain is likewise ignored, for the same reason.
- **Maps over a rank like any other step.** `$path/d.name><%` takes each cell in `d.name`'s posward
  rank (§4.1) and entangles it with its own freshly created partner — one new cell per rank member,
  not one new cell shared by the whole rank. §4.7 covers the mirror case: an *existing* single cell
  reused as the target for many context cells.

### 4.7 Existing-Target Fan-Out

A dimension link is a single pos/neg pair (§1) — one cell's `dim`/`dir` slot holds exactly one
partner. `%` (§4.5) never collides with this, because it allocates a fresh cell per context-stream
member; but `link(dim, dir, target)` naming an *existing* cell as `target` (§4.5's "existing
targets" case) has no such escape hatch on its own. Evaluated once per context-stream cell like
every other step, the second and later calls would each overwrite `target`'s own back-link, silently
discarding the previous context cell's connection — `$context/link(dim, dir, $a)` over a multi-cell
`$context` is not "link every context cell to `$a`," it's "link the last context cell to `$a`, and
quietly drop the rest."

VQL resolves this the same way §4.6 resolves "one cell, many partners": an existing-cell `target` is
always drawn from `entangle_generator(target)` (Vortex §2) rather than being reused directly — one
pull per context-stream cell, unconditionally, with no branch on how many cells are in the context.
The generator's first pull is `target` itself, so a single-cell context gets exactly what writing
`target` directly would have given it — no synthetic entanglement, no special case. Every pull after
the first allocates a fresh cell entangled with `target` — sharing its value, mutated together
(§4.6) — and *that* fresh cell becomes the structural link partner for one more context cell.
`target`'s own dimension slot still only ever holds the most recently generated partner directly,
but every generated cell (and `target`) shares one underlying payload, so the group reads as a
single logical value no matter which member is dereferenced.

This is transparent at the call site: `$context/link(dim, dir, $a)` parses and means the same thing,
and compiles the same way, whether `$context` has one cell or many — a 1-node and an N-node context
both draw from the same generator, they just happen to draw a different number of times.

______________________________________________________________________

## 5. Memory Management & Topological Garbage Collection

VQL relies on geometric reachability rather than linear allocation trackers:

```mermaid
graph TD
    Root["Root Set: Origin (0), d.cursors, Dimension Anchors"]
    Reach["Reachable Active Manifolds"]
    Unreach["Unreachable Clusters"]
    GC["Reclaimed by GC"]

    Root -->|"link(..., read)" traversal| Reach
    Reach -->|"link(..., -2) severs a link"| Unreach
    Unreach --> GC
```

- **Allocation**: `%` (§4.5) or a direct `link(cell, dim, dir, -1)` materializes a new cell directly
  into the coordinate system.
- **Disconnection & Re-linking**: `link(cell, dim, dir, -2)` — or `break(cell, dim, dir)` (§1) —
  severs a pointer. If a cell's aggregate link count reaches zero across all dimensions, the cell is
  eagerly evicted from the matrix storage map.
- **Sub-graph Isolation**: If a pipeline or temporary manifold is severed from the root set, it is
  marked as unreachable during the next sweep phase and reclaimed without manual deallocation.

______________________________________________________________________

## 6. Canonical Production Query Examples

### 6.1 Deep Structural Navigation with Zero-Copy Slice

Locates an active worker thread, pulls its buffer variable, slices a token out of the string, and
weaves the result as a `d.vars`/`d.values` pair off the invocation cell instead of a JSON-shaped
literal:

```
for $worker in ^
where $worker/d.name[. = "HTTP_WORKER"]
let $raw_header := $worker/d.vars[. = "buffer"]/d.values/.
let $status_code := $raw_header.[9, 3]
where $status_code = "200"
return
    /d.vars%"worker_id"/d.values%$worker/@,
    /d.vars%"status"/d.values%$status_code
```

`^` already streams every cursor cell (§2), so no further dimension step is needed to reach them.
`where $worker/d.name[. = "HTTP_WORKER"]` filters the rank instead of comparing it directly;
`where $status_code = "200"` stays a direct comparison because `$status_code` is already a
dereferenced scalar from `let`, not a rank.

### 6.2 Topological Regex Compilation with Star-Pivot Caching

Checks whether a regular expression has already been woven into an NFA manifold. On a cache hit, it
returns the cached entry root directly. On a cache miss, it weaves a new Thompson NFA graph and
caches the result off the origin cell, using `if`/`else` (§3) to choose between the two:

```
for $compiler in ^COMPILER_THREAD
let $pattern := $compiler/d.vars[. = "regex"]/d.values/.
let $cache_root := ##/d.cache[. = "regex_compile"]
let $hit := $cache_root/d.invocations[./d.inputs[. = $pattern]][1]
if $hit
  return $hit/d.outputs/@
else
  weave {
    let $new_inv := $cache_root/d.invocations%,
    $new_inv/d.inputs%$pattern,
    let $nfa_start := $new_inv/d.outputs%"#START",
    for $ch in EXPLODE($pattern, "")
      weave $nfa_start/d.step%$ch,
    $compiler/link(d.results, +1, $nfa_start/@)
  }
```

The character loop leans on §4.5's tail-seeking: each `$nfa_start/d.step%$ch` walks to wherever the
chain currently ends and appends there, so the loop builds one linked rank instead of repeatedly
overwriting `$nfa_start`'s own `d.step` link. The final line points `d.results` at the NFA's actual
entry cell — an existing target, so it's spelled with `link`, not `%`.

### 6.3 Shared Identity via Entanglement

Seeds a new user's `d.theme` cell from the system default by entangling it with the default cell
instead of copying the value, so a later change to the default propagates to every user who hasn't
set their own theme:

```
for $new_user in ##/d.users%$username
let $default_theme := ##/d.settings[. = "default_theme"]/d.values
weave $default_theme><$new_user/d.theme%
```

`$default_theme` is written first so its value is the one the group ends up sharing (§4.6): the
right-hand operand is an ordinary `/dim%` create with no `VALUE` of its own, so there's nothing for
the join to discard. A user who later gets their own theme breaks out of the group with
`$new_user/d.theme/break(d_entangle, +1)` — `link(..., -2)` under the hood (§1), not a different
"unentangle" primitive.

### 6.4 In-Place Graph Rewriting & Edge Re-Targeting

Updates a deprecated microservice node, rewires active event routes to its replacement, and
disconnects the retired cell for automatic garbage collection:

```
for $old_service in ##/d.services[. = "auth_v1"]
let $new_service := ##/d.services[. = "auth_v2"]/@
weave {
    for $caller in $old_service/-d.route
      weave $caller/link(d.route, +1, $new_service),

    // Clear all incoming and outgoing connections of old service
    $old_service/break(d.services, +1),
    $old_service/break(d.services, -1)
    // Topological GC automatically reclaims $old_service once isolated
}
```

`/-d.route` walks `d.route` negward from `$old_service` — the same traversal operator as every other
step in the query, with the direction living on the dimension name rather than the operator.

______________________________________________________________________

## 7. Reconciliation with the Unified Store/Slice

[`store-slice-convergence.md`](store-slice-convergence.md) makes a zigzag cell an operation in the
xudu ops spool, which changes what the primitives underneath this language are operating on. Its §13
works the compatibility through from the storage side, and
[`vortex-hyperstructural-runtime.md`](vortex-hyperstructural-runtime.md) §5 records the engine
consequences. **The grammar in §3 is unaffected.** What follows is the surface-level fallout, for
whoever writes the compiler.

### 7.1 Two regimes, and a query can be in either

The convergence splits the manifold in two (its R8). A `Manifold` is a replay of an operations
spool: durable, addressable, and the thing a document *is*. An `ArenaManifold` is scratch: cells
that exist because a query made them, backed by no operation and collected when unreachable.

The rule for which one a VQL mutation lands in is **not** "reads are ephemeral, writes are
persistent". It is:

> **Only a user-generated update persists. Navigation never does.**

So `weave` on behalf of a person editing a document writes operations. A cursor moving, a rank being
streamed, an intermediate manifold a pipeline built and will discard — none of these earn a name in
hypertime, however much structure they create along the way. §5's reachability GC below is
`ArenaManifold`'s garbage collector specifically; the persistent side has no garbage, because an
append-only spool never drops anything.

This is what `promote()` is for: an arena result a person decides to keep is folded into the
persistent manifold as operations, at the moment they decide, and not before.

### 7.2 `##` is the origin cell, but the origin is not cell zero

Convergence R5 makes `0` mean *no cell*, because a cell's address is its index in the operations
spool and index 0 is the state-zero slot — nothing is recorded there. The origin is a genesis cell
named `home`.

`##` therefore keeps working exactly as §2 describes, because it was always a token rather than a
number. Two smaller things change: `LiteralCellId` may not be written as `0`, and §4.1's remark that
"a rank can freely pass through Cell 0 without truncating early" needs no defending any more —
absence and zero are the same value again, and the loop still terminates on `link`'s read form
returning nothing rather than on an id.

`link`'s `-1`/`-2` sentinels go the same way (Vortex §5.1): `CellRef` is unsigned, so allocate and
isolate become verbs. `new(...)`/`break(...)` (§1) were already the spellings this language
preferred; they stop being sugar over a magic target and become the primary forms, with
`link(dim, dir, target)` reserved for the literal-target case.

### 7.3 `><` entangles along `d.clone`, and the head is the leftmost operand

§4.6 says payload authority follows argument order — the leftmost operand's value is what the group
shares. That is a rank with a distinguished head, which is precisely what zigzag's `d.clone` already
is, and the convergence uses it: `d.entangle` is a `d.clone`-shaped rank, not a shared pointer.

Everything in §4.6 and §4.7 survives, including `entangle_generator`, which is still needed for
exactly the reason it was introduced — a dimension slot holds one partner per direction, and that is
as true of a 12-byte `{dim, pos, neg}` triple as it was of a `LinkSlot`.

One thing improves: entangled cells no longer share a mutable box, so a `set()` on the group records
one operation against the head and **every prior value remains addressable**. §6.3's worked example
— a user's theme entangled with the system default — now leaves a history of what the default was,
which under a shared payload it could not.

### 7.4 `set(replacement, offset, length)` is two operations on a persistent cell

§4.2's "Mutation Patching" compiles to a single `set` only in the arena regime. A persistent cell's
content is a span into an append-only permascroll: splicing inside it is an insert and a delete
against that cell's own text.

The syntax is unchanged and the semantics are unchanged. What changes is that the cost is visible,
which is the correct signal — it is the difference between editing a document and editing a scratch
value.

### 7.5 The one open question: `d.cache` is a leak with good manners

§1's star-pivot caching memoises off the origin along `+d.cache`, and the origin is in the Root Set,
so §5's sweep can never reach a conclusion about it. A memoisation table that is unreachable by
design from the collector grows without bound.

It is also, by §7.1's rule, plainly derived state. The likely answer is that `d.cache` hangs off the
**cursor** rather than the origin, so a compiled-NFA cache dies with the query that filled it and a
longer-lived cache has to be asked for explicitly. §6.2's example would change shape. This is left
open rather than decided here, because it is a question about VQL's caching model and not about the
storage underneath it.
