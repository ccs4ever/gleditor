# Vortex Query Language (VQL) Specification

**Document Version:** 4.0.0 — Canonical Specification **Compilation Target:** Vortex Hyperstructural
Runtime Core (Two-Primitive Matrix Manifold)

VQL is the declarative, XQuery-like companion language for
[Vortex](vortex-hyperstructural-runtime.md): every VQL query compiles down to the same
`get_link`/`set_link`/`get_cell_value`/`set_cell_value` primitives that make up Vortex's
two-primitive core. As with the Vortex spec, this document specifies a language design consuming the
Zigzag `zzstructure` manifold model (see
[zigzag-multidimensional-space-and-projection.md](zigzag-multidimensional-space-and-projection.md));
nothing here is wired into the gleditor build.

______________________________________________________________________

## 1. Architectural Foundations & Operational Invariants

Vortex Query Language (VQL) is a declarative, zero-allocation path navigation and graph mutation
language tailored specifically for **zzstructures** (multidimensional bidirectional graphs). VQL
queries do not evaluate into ephemeral tabular heaps, relational tuples, or serialized JSON arrays.
Instead, every query expression compiles down to spatial traversals, generator pipelines, and link
mutations driven by the atomic engine:

$$
\mathcal{M} = \langle \text{get\_link}, \, \text{set\_link}, \,
\text{get\_cell\_value}, \, \text{set\_cell\_value} \rangle
$$

### Fundamental Invariants

- **The Two-Primitive Core**: Path traversal compiles strictly to `get_link(cell, dim, dir)`.
  Mutations, updates, insertions, and allocations compile strictly to
  `set_link(cell, dim, dir, target)`, with `target == -1` for allocation and `target == 0` for
  deallocation/isolation.
- **Dual-Wing Invocation Topology**: Built-in functions, custom routines, and opcodes adhere to the
  dual-wing interface. Out-parameters project negward along `-d.grab` (chained posward along
  `+d.step` for multiple returns). In-parameters project posward along `+d.grab` and chain along
  `+d.step`.
- **Snapshot-Before-Write Execution**: When updating nodes in-place or writing query projections,
  all input operand payloads are fully dereferenced and snapshotted prior to invoking
  `set_cell_value()` or mutating pointers, preventing self-aliasing hazards.
- **Associative Cursor Scopes**: Query variables are not stored in stack frames or hash lookups.
  They resolve directly against the active Spin-Head cursor along `+d.vars`, with bound payloads or
  complex subgraphs projecting perpendicularly along `+d.values`.
- **Quantum Identity Synchronization (`d.fuse`)**: Pipelined assignments using the pipe operator
  (`|>`) or variable aliases utilize `d.fuse` links. Fused nodes share an underlying pointer to a
  single `CellValue` variant (`std::variant<std::string, double, bool>`). Mutations immediately
  propagate across all viewports and observer ranks.
- **Zero-Allocation Lazy Rank-Streaming**: Path steps (`/`) do not materialize intermediate
  collections. Every step instantiates an $O(1)$ spatial cursor that lazily yields matching
  `cell_id` coordinates along a dimensional track until it encounters 0 (null) or a loopback
  boundary.
- **Persistent Star-Pivot Caching**: Expensive generative transformations (e.g., regex compilation
  or macro expansions) memoize their topological results off the root origin (`##` / Cell 0) along
  `+d.cache`, anchoring invocation nodes to input and output manifolds.

______________________________________________________________________

## 2. Syntactic Token & Structural Shorthand Matrix

| Token / Operator    | Structural Equivalent               | Functional & Spatial Semantic Mapping                                            |
| ------------------- | ----------------------------------- | -------------------------------------------------------------------------------- |
| `##`                | Origin Anchor (`cell_id = 0`)       | Grounds query context to the absolute system environment origin.                 |
| `#`                 | Root Metacells                      | Lazily streams all disjoint root manifold entry points across the matrix.        |
| `^`                 | Process Manifold (`d.cursors`)      | Streams all active Spin-Head execution cursor threads.                           |
| `^NAME`             | `^/d.name[. = "NAME"]`              | Short-circuits the cursor scan, locking directly onto the named thread node.     |
| `/`                 | `LazyRankStream(posward)`           | Traverses posward (`dir = +1`) along a designated dimension rank.                |
| `\`                 | `LazyRankStream(negward)`           | Traverses negward (`dir = -1`) along a designated dimension rank.                |
| `//`                | Transitive Closure ($\text{dim}^+$) | Lazily walks a dimension rank recursively until exhaustion or cycle detection.   |
| `.`                 | `get_cell_value(context)`           | Universal cell payload dereference; extracts the typed `CellValue`.              |
| `.[offset, length]` | `get_cell_value(ctx, off, len)`     | Zero-copy virtual string slice dereference.                                      |
| `@`                 | Context Identity Address            | Evaluates to the numerical `cell_id` coordinate of the context node itself.      |
| `\|>`               | Pipe-Forward Operator               | Chains a path result into a function invocation or update block.                 |
| `[...]`             | Predicate / Index Window            | Applies inline boolean filters or relative index clamps to a stream.             |
| `(...)`             | Macro Dimension Group               | Groups dimensional sequences into a compound traversal segment.                  |
| `*`                 | Kleene Repetition                   | Repeats the preceding macro group zero or more times until termination.          |
| `{...}`             | Topological Return Weaver           | Weaves query results directly into an invocation root manifold along `d.values`. |

______________________________________________________________________

## 3. Formal EBNF Grammar

```
QueryExpression     ::= ExecutionBlock | PipelineChain

ExecutionBlock       ::= ( ForClause | LetClause )+ WhereClause? ActionClause
PipelineChain        ::= PathExpression ( "|>" FunctionInvocation )*

ForClause            ::= "for" VariableRef "in" PathExpression
LetClause            ::= "let" VariableRef ":=" ( PathExpression | ValueExpr )
WhereClause          ::= "where" BooleanExpr ( ( "and" | "or" ) BooleanExpr )*
ActionClause         ::= ReturnClause | UpdateClause

ReturnClause         ::= "return" ( PathExpression | StructuralRecord )
UpdateClause         ::= "update" "{" MutationStatement ( "," MutationStatement )* "}"

PathExpression       ::= AnchorNode PathStep*
AnchorNode           ::= "##" | "#" | NamedCursor | VariableRef | LiteralCellId
NamedCursor          ::= "^" [a-zA-Z_][a-zA-Z0-9_]*
VariableRef          ::= "$" [a-zA-Z_][a-zA-Z0-9_]*
LiteralCellId        ::= [0-9]+

PathStep             ::= TraversalOp StepSelector PredicateClause* RangeClamp?
TraversalOp          ::= "/" | "\" | "//" | "\\"
StepSelector         ::= DimensionIdentifier | MacroDimensionGroup | FunctionInvocation

DimensionIdentifier  ::= [a-zA-Z_][a-zA-Z0-9_]* ( "." [a-zA-Z_][a-zA-Z0-9_]* )*
MacroDimensionGroup  ::= "(" PathStep+ ")" RepetitionModifier?
RepetitionModifier   ::= "*" | "+" | "?"

PredicateClause      ::= "[" BooleanExpr "]"
RangeClamp           ::= "[" IndexParam ( "," IndexParam )? "]"
IndexParam           ::= "-"? [0-9]+

StructuralRecord     ::= "{" FieldAssignment ( "," FieldAssignment )* "}"
FieldAssignment      ::= StringLiteral ":" ( PathExpression | ValueExpr )

MutationStatement    ::= LinkMutation | ValueMutation | UnifyMutation
LinkMutation         ::= ( PathExpression | VariableRef ) "/" "set_link" "(" DimensionIdentifier "," DirectionParam "," TargetExpr ")"
ValueMutation        ::= ( PathExpression | VariableRef ) "/" "set_val" "(" ValueExpr ( "," OffsetParam "," LengthParam )? ")"
UnifyMutation        ::= "unify" "(" TargetExpr "," TargetExpr ")"

DirectionParam       ::= "+1" | "-1" | "+" | "-"
TargetExpr           ::= PathExpression | VariableRef | "@" | "-1" | "0"
OffsetParam          ::= [0-9]+
LengthParam          ::= [0-9]+

BooleanExpr          ::= BooleanTerm ( "or" BooleanTerm )*
BooleanTerm          ::= BooleanFactor ( "and" BooleanFactor )*
BooleanFactor         ::= "not"? ( ComparisonExpr | PredicateTest )
ComparisonExpr       ::= ValueExpr CompOp ValueExpr
CompOp               ::= "=" | "!=" | "<" | ">" | "<=" | ">="
PredicateTest        ::= PathExpression | "." | ExtendedTruthinessCheck

ExtendedTruthinessCheck ::= "?" ( PathExpression | ValueExpr )

ValueExpr            ::= PathExpression | ScalarLiteral | VariableRef | FunctionInvocation | "@"
ScalarLiteral         ::= StringLiteral | NumericLiteral | BooleanLiteral
StringLiteral         ::= '"' [^"\\]* '"'
NumericLiteral        ::= "-"? [0-9]+ ( "." [0-9]+ )?
BooleanLiteral        ::= "true" | "false"

FunctionInvocation   ::= Identifier "(" ArgumentList? ")"
ArgumentList         ::= ValueExpr ( "," ValueExpr )*
```

______________________________________________________________________

## 4. Evaluation Semantics & Compilation Rules

### 4.1 Lazy Rank-Streaming Engine

When the compiler encounters `/dim`, it does not collect vertices into a temporary list. It
generates an iterative loop along `dim`:

```cpp
cell_id current = start_node;
while (current != 0) {
    cell_id next = get_link(current, target_dim, +1);
    if (next == 0 || next == start_node) break; // Terminate on null or loopback
    if (eval_predicates(next)) {
        yield next;
    }
    current = next;
}
```

- **Index Clamping (`[n]`)**: Evaluated lazily with 1-based indexing.
  - `[1]`: Short-circuits traversal on the first matching cell, avoiding unnecessary traversals over
    dense ranks.
  - `[-1]`: Scans to the rank tail, returning only the final matching coordinate.
  - `[start, end]`: Emits an index-windowed slice over the matching elements.

### 4.2 Slicing Semantics

VQL supports zero-copy virtual slicing on text payloads:

- **Direct Path Step**: `$node/.[offset, length]` calls `get_cell_value($node, offset, length)`.
- **Mutation Patching**: `$node/set_val(replacement, offset, length)` calls
  `set_cell_value($node, replacement, offset, length)`. Numeric and boolean variants ignore offsets
  and lengths entirely.

### 4.3 Extended Truthiness Rules

Predicates evaluate conditions according to canonical truthiness semantics:

- **Truthy**: `true`, non-zero double, and case-insensitive strings: `"true"`, `"1"`, `"yes"`,
  `"on"`. Unrecognized non-empty strings default to true.
- **Falsy**: `false`, `0.0`, empty string `""`, and case-insensitive strings: `"false"`, `"0"`,
  `"no"`, `"off"`.

### 4.4 Result Materialization: Topological Return Weaving

`return` statements avoid heap allocations or string serializations.

1. The engine instantiates an Invocation Result Cell appended posward along the active cursor's
   `+d.results` rank.
1. For scalar expressions or path references, the result is linked posward directly off the
   invocation cell along `+d.values`.
1. For structured records (`{ key: value }`), key-label cells are linked along `+d.vars`, with their
   projected values extending along `+d.values`, mirroring the cursor's own scope manifold.

______________________________________________________________________

## 5. Memory Management & Topological Garbage Collection

VQL relies on geometric reachability rather than linear allocation trackers:

```
Root Set: { Origin Node (0), d.cursors, Dimension Anchors }
                    |
                    v get_link(..., dim, dir)
        [ Reachable Active Manifolds ]
                    |
      (Isolated by set_link(..., 0))
                    v
        [ Unreachable Clusters ] --> Reclaimed by GC
```

- **Allocation**: `set_link(cell, dim, dir, -1)` materializes a new cell directly into the
  coordinate system.
- **Disconnection & Re-linking**: `set_link(cell, dim, dir, 0)` severs a pointer. If a cell's
  aggregate link count reaches zero across all dimensions, the cell is eagerly evicted from the
  matrix storage map.
- **Sub-graph Isolation**: If a pipeline or temporary manifold is severed from the root set, it is
  marked as unreachable during the next sweep phase and reclaimed without manual deallocation.

______________________________________________________________________

## 6. Logic Programming & Unification Integration

VQL seamlessly expresses relational and logic unification queries:

### Syntax

```
for $ans in ##//d.parent
where unify($ans, "Bob")
return $ans
```

### Unification Compilation Semantics

The `unify(TermA, TermB)` predicate executes Robinson unification directly in the matrix:

1. **Dereferencing**: Follows `+d.values` until a non-forwarding cell is reached.
1. **Variable Aliasing**: If both terms are unbound variables (`+d.values == 0`), they are fused
   along `d.fuse` (`set_link(A, d_fuse, +1, B)`) and the binding is logged to the active choice
   point's trail rank.
1. **Variable Binding**: If one term is unbound, its `+d.values` is set to the other term's node and
   recorded on the cursor's `+d.stack`/`+d.trail` rank.
1. **Ground Comparison**: If both terms are bound, their payloads are compared using
   `get_cell_value()`.
1. **Backtracking**: Query iterations that fail backtrack by popping the choice point from
   `+d.stack`, unsetting all trail-recorded links via `set_link(target, d_values, +1, 0)`, and
   resuming the search at the alternate branch address (`+d.warp`).

______________________________________________________________________

## 7. Canonical Production Query Examples

### 7.1 Deep Structural Navigation with Zero-Copy Slice

Locates an active worker thread, pulls its buffer variable, slices a token out of the string, and
evaluates its truthiness:

```
for $worker in ^/d.cursors
where $worker/d.name = "HTTP_WORKER"
let $raw_header := $worker/d.vars[. = "buffer"]/d.values/.
let $status_code := $raw_header.[9, 3]
where $status_code = "200"
return {
    worker_id: $worker/@,
    status: $status_code
}
```

### 7.2 Topological Regex Compilation with Star-Pivot Caching

Checks whether a regular expression has already been woven into an NFA manifold. On a cache hit, it
links directly to the cached entry root. On a cache miss, it weaves a new Thompson NFA graph and
caches the result off the origin cell:

```
for $compiler in ^COMPILER_THREAD
let $pattern := $compiler/d.vars[. = "regex"]/d.values/.
let $cache_root := ##/d.cache[. = "regex_compile"]

// Check Global Memoization Cache
for $inv in $cache_root/d.invocations
where $inv/d.inputs = $pattern
return $inv/d.outputs/@

// On Cache Miss: Compile and Cache NFA
where not $inv
update {
    // 1. Allocate Invocation Record off Cache Anchor
    let $new_inv := ##/d.cache/set_link(d.invocations, +1, -1),
    $new_inv/set_link(d.inputs, +1, -1)/set_val($pattern),

    // 2. Weave NFA Start Node
    let $nfa_start := $new_inv/set_link(d.outputs, +1, -1),
    $nfa_start/set_val("#START"),

    // 3. Sequentially build matching ranks along d.step
    for $ch in EXPLODE($pattern, "")
    update {
        $nfa_start/set_link(d.step, +1, -1)/set_val($ch)
    },

    // 4. Return Compiled Graph Entry Point
    $compiler/set_link(d.results, +1, $nfa_start/@)
}
```

### 7.3 Kinship Logic Unification (Prolog-Style Query)

Finds all common ancestors of "Alice" and "Bob" using recursive closure (`//`) and logic
unification:

```
for $alice_ancestor in ##/d.people[. = "Alice"]//d.parent
for $bob_ancestor in ##/d.people[. = "Bob"]//d.parent
where unify($alice_ancestor, $bob_ancestor)
return {
    ancestor_id: $alice_ancestor/@,
    ancestor_name: $alice_ancestor/.
}
```

### 7.4 In-Place Graph Rewriting & Edge Re-Targeting

Updates a deprecated microservice node, rewires active event routes to its replacement, and
disconnects the retired cell for automatic garbage collection:

```
for $old_service in ##/d.services[. = "auth_v1"]
let $new_service := ##/d.services[. = "auth_v2"]/@
update {
    // Re-route incoming event links from old to new service
    for $caller in $old_service\d.route
    update {
        $caller/set_link(d.route, +1, $new_service)
    },

    // Clear all incoming and outgoing connections of old service
    $old_service/set_link(d.services, +1, 0),
    $old_service/set_link(d.services, -1, 0)
    // Topological GC automatically reclaims $old_service once isolated
}
```
