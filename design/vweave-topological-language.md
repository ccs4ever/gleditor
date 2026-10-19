# Vweave: A Topological, Vortex-Native Programming Language Over Zigzag Manifolds

**Document Version:** 1.0 — The Orthotopal Specification\
**Compilation Target:** Vortex Hyperstructural Runtime Core — `link` and `value`, and nothing else
(§6)\
**Primary Substrate:** 2-Rank Multidimensional Manifold (`zigzag::Manifold`, `ArenaManifold`,
`CompactOpNode`)\
**Status:** Specification only; non-breaking design document. Implements no new C++ core primitives;
rests entirely on existing mechanisms built in `apps/common/xanadu/vortex/` and
`apps/common/xanadu/zigzag/`.

______________________________________________________________________

## Contents

- [1. The Premise: Beyond Immigrant Paradigms](#1-the-premise-beyond-immigrant-paradigms)
- [2. The Quadripartite Consultation](#2-the-quadripartite-consultation)
  - [2.1 PL Design: The 1D Stream vs. Graph Topology][toc-01]
  - [2.2 Vortex Engine: Mechanical Invariants of the Spool][toc-02]
  - [2.3 Zigzag Power User: Sensory Navigation and Live Projection][toc-03]
  - [2.4 Technical Writer: The Loom Metaphor and Lexical Precision][toc-04]
- [3. Architectural Foundations & Invariants](#3-architectural-foundations--invariants)
  - [3.1 Invariant 1: The Nelsonian 2-Rank Manifold](#31-invariant-1-the-nelsonian-2-rank-manifold)
  - [3.2 Invariant 2: The Single-Primitive Invariant][toc-05]
  - [3.3 Invariant 3: Dual-Wing Spatial Invocation][toc-06]
  - [3.4 Invariant 4: Topological Scope & Zero Unanchored Cells][toc-07]
- [4. Formal EBNF Grammar](#4-formal-ebnf-grammar)
- [5. Type System: Topological & Structural Types](#5-type-system-topological--structural-types)
  - [5.1 Primitive Values & Payloads](#51-primitive-values--payloads)
  - [5.2 Spatial Types: Cells, Ranks, Rings, and Shapes][toc-08]
  - [5.3 Dimensional Contracts along `d.contract`][toc-09]
- [6. Language Mechanics & Operational Semantics](#6-language-mechanics--operational-semantics)
  - [6.1 Warps (Execution Ranks) and Wefts (Data Ranks)][toc-10]
  - [6.2 Pivots, Cross-Stitching, and Dimensional Swivels][toc-11]
  - [6.3 Control Flow: Topological Rings and Deflection][toc-12]
  - [6.4 Subroutines: Dual-Wing Spindles and Spatial Closures][toc-13]
  - [6.5 Clone Superposition and Multi-Presence][toc-14]
- [7. Lowering to the Vortex Runtime](#7-lowering-to-the-vortex-runtime)
  - [7.1 Production-to-Lowering Matrix](#71-production-to-lowering-matrix)
  - [7.2 Memory Model & Compressed Sparse Row (CSR) Layout][toc-15]
  - [7.3 Ephemeral Execution & Promotion](#73-ephemeral-execution--promotion)
- [8. Standard Library: `std:weave`](#8-standard-library-stdweave)
- [9. Worked Canonical Programs](#9-worked-canonical-programs)
  - [9.1 Topological In-Place Quicksort](#91-topological-in-place-quicksort)
  - [9.2 Concurrent Tokenizer & Frequency Lattice][toc-16]
  - [9.3 Xanadu Transclusion & Citation Weave][toc-17]
- [10. The `xuzz` Interactive Experience & Live Projection][toc-18]

______________________________________________________________________

## 1. The Premise: Beyond Immigrant Paradigms

Over the course of the Vortex runtime development, five front ends and extensions were designed:

- **VQL**: XPath/XQuery projected onto dimensional paths (`/+d.1/-d.2`).
- **VPL**: APL/J adapted so array axes are named dimensions.
- **vlace**: Landin/Milner functional programming with closures, ADTs, and thunks along `d.spin`.
- **vicon**: Griswold's Icon goal-directed evaluation using choice-point trails along `d.scan`.
- **vprolog / Vlog**: Colmerauer/Kowalski logic programming with Horn clauses along `d.clause`.

Every one of these languages is an **immigrant** from conventional computer science. They were born
on von Neumann architectures, linear address spaces, hardware-bound call stacks, trees (ASTs), or
flat memory arrays, and were translated into Ted Nelson's ZigZag space.

None of them were **born from the manifold itself**.

In Ted Nelson's ZigZag, there are no files, no directories, no linear memory buffers, and no nested
hierarchies. There is only an infinite, orthogonal hyper-matrix of informational atoms (**cells**)
connected along named, non-intersecting dimensions subject to the **2-Rank Manifold Invariant**:
each cell has at most one positive neighbor and at most one negative neighbor per dimension.

`Vweave` is a textual, Vortex-native programming language designed from the substrate up. It does
not map conventional language idioms onto ZigZag; it turns the topological physics of ZigZag into
first-class programming constructs.

______________________________________________________________________

## 2. The Quadripartite Consultation

### 2.1 PL Design: The 1D Stream vs. Graph Topology

Conventional programming languages suffer from the tyranny of the 1D character stream. A lexer reads
a linear sequence of tokens; a parser arranges them into an Abstract Syntax Tree (AST). Trees impose
an artificial hierarchy on computation: nested scopes, nested expressions, and parent-child
relations.

In ZigZag, structure is strictly non-hierarchical. A cell can maintain relationships along $N$
orthogonal dimensions simultaneously. Previous front ends either flattened this space into linear
pipelines or forced tree blocks onto the manifold.

`Vweave` resolves this tension through **Orthogonal Rank Declarations**. Code is not structured as
nested blocks, but as independent linear trajectories (**Warps**) cross-cut by perpendicular data
trajectories (**Wefts**). Scope is not a stack frame; it is an orthogonal coordinate projection. By
expressing computation as spatial intersections (**Pivots**), `Vweave` provides a readable 1D/2D
textual grammar that compiles directly into a multigraph without an intermediate hierarchical AST.

### 2.2 Vortex Engine: Mechanical Invariants of the Spool

Under the hood in `apps/common/xanadu/vortex/`, the runtime engine is minimal and uncompromising:

1. `link(cell, dim, dir, [target])`
1. `value(cell, [data])`

The entire runtime—the Merkle-piece append-only ops spool (`CompactOpNode`), the zero-allocation
`ArenaManifold`, the dual-wing parameter calling conventions, and instruction streams along
`+d.spin`—is built strictly from those two calls.

For `Vweave` to be native, it must introduce **zero new runtime primitives**. Every weave, swivel,
ring-loop, and clone must compile to a deterministic sequence of `link` and `value`. Ephemeral
evaluations execute in `ArenaManifold` with `mark()` and `release()`; persistent results commit
through `promote()`.

### 2.3 Zigzag Power User: Sensory Navigation and Live Projection

In `xuzz` and the ZigZag projection architecture, navigation is physical. An operator maps two
dimensions to the screen ($X$ on `d.1`, $Y$ on `d.2`). To inspect metadata, $Y$ swivels to `d.time`.
To view category assignments, $X$ swivels to `d.clone`.

In `Vweave`, programming mirrors this spatial manipulation. Writing code is weaving fabric: one lays
down a Warp of execution and shoots Wefts of data across it. Where they cross, a Pivot forms.
Debugging inside `xuzz` is not reading a text trace; it is watching the execution cursor step
through the manifold, swiveling dimensions in real time and weaving live data cells into the space.

### 2.4 Technical Writer: The Loom Metaphor and Lexical Precision

To make a multidimensional topological language intuitive, `Vweave` adopts the unified mental model
of **The Loom**:

- **Cell**: The discrete informational atom carrying a 64-bit scalar or primedia span.
- **Rank**: A continuous sequence of cells linked along a single dimension.
- **Warp**: A rank dedicated to sequential control flow or operational progression.
- **Weft**: A rank dedicated to data pipelines, collections, or environments.
- **Pivot (`*`)**: A cell belonging to two or more orthogonal ranks simultaneously.
- **Swivel**: Changing the cursor's active dimensional trajectory at a pivot.
- **Spindle**: A reusable spatial component with Inputs and Outputs wings.
- **Ring**: A closed rank where $+d$ eventually links back to the origin, forming a loop.
- **Loom**: The active manifold enclosing the program's ranks.

______________________________________________________________________

## 3. Architectural Foundations & Invariants

```
                               ▲ +d.2 (Inputs / Weft)
                               │
                               │
      -d.1 (Prev Step) ──────[Pivot]────── +d.1 (Next Step / Warp)
                               │
                               │
                               ▼ -d.2 (Outputs / Weft)
                              ╱
                             ╱ +d.spin (Spindle Execution)
                            ▼
```

### 3.1 Invariant 1: The Nelsonian 2-Rank Manifold

Along any dimension $d$, every cell $C$ has at most one positive neighbor and at most one negative
neighbor:

$$
\text{deg}^+(C, d) \le 1, \quad \text{deg}^-(C, d) \le 1
$$

Branching is never an inline fork along the same dimension; branching is a **dimensional
deflection** into an orthogonal dimension.

### 3.2 Invariant 2: The Single-Primitive Invariant

The language runtime compiles strictly to Vortex's:

$$
\text{link}(c, d, \text{dir}, [t]) \quad \text{and} \quad \text{value}(c, [v])
$$

No external call stacks, no linear byte heaps, and no hidden CPU register mappings. State is
topology.

### 3.3 Invariant 3: Dual-Wing Spatial Invocation

Subroutines do not push activation records onto a stack. A subroutine in `Vweave` is an anchored
**Pivot Cell** ($S$):

- Inputs arrive along the **Inputs Wing** (`-d.2`).
- Outputs emit along the **Outputs Wing** (`+d.2`).
- Instructions execute along the **Spin Dimension** (`+d.spin`).
- Lexical variables index along the **Environment Rank** (`+d.vars`).

### 3.4 Invariant 4: Topological Scope & Zero Unanchored Cells

Every cell minted by `Vweave` belongs to at least one rank upon creation. There are no orphan
pointers or free-floating heap blocks. Memory reclamation is topological: resetting the
`ArenaManifold` head reclaims unreachable subgraphs in $O(1)$.

______________________________________________________________________

## 4. Formal EBNF Grammar

```ebnf
Program         ::= { DimensionDecl | LoomDecl | SpindleDecl } ;

DimensionDecl   ::= "dim" DimensionName { "," DimensionName } ";" ;
DimensionName   ::= "d." Ident ;

LoomDecl        ::= "loom" Ident [ "along" DimensionName ] "{" { LoomItem } "}" ;
LoomItem        ::= WarpDecl | WeftDecl | SpliceStmt | PivotDecl ;

WarpDecl        ::= "warp" Ident "along" DimensionName "{" { StepExpr } "}" ;
WeftDecl        ::= "weft" Ident "along" DimensionName "{" { CellDef } "}" ;

StepExpr        ::= [ CellLabel ] ( Action | PivotRef | RingRef ) [ StepArrow ] ;
StepArrow       ::= "~" DimensionName ">" | "<" DimensionName "~" | "->" ;

CellDef         ::= [ CellLabel ] Literal [ PivotAnnotation ] [ StepArrow ] ;
CellLabel       ::= "@" Ident ;
PivotAnnotation ::= "*" DimensionName [ ":" CellRef ] ;

Action          ::= Invocation | Assignment | Deflection | Swivel | SpliceStmt | SeverStmt ;

Invocation      ::= Ident "(" [ ArgumentList ] ")" ;
ArgumentList    ::= ValueExpr { "," ValueExpr } ;

Deflection      ::= "deflect" Condition "to" StepArrow ;
Swivel          ::= "swivel" DimensionName ;

RingRef         ::= "ring" DimensionName "(" Ident ")" ;
SpliceStmt      ::= "splice" StepArrow "{" { CellDef } "}" ;
SeverStmt       ::= "sever" StepArrow ;

SpindleDecl     ::= "spindle" Ident "(" [ ParamList ] ")" [ "->" "(" [ OutputList ] ")" ]
                    [ "contract" "{" ContractExpr "}" ]
                    "{" { StepExpr } "}" ;

ParamList       ::= Ident { "," Ident } ;
OutputList      ::= Ident { "," Ident } ;

Literal         ::= Integer | Float | String | SymLiteral | SpanLiteral ;
SymLiteral      ::= "#" Ident ;
SpanLiteral     ::= "span(" Ident "," Integer "," Integer ")" ;
```

______________________________________________________________________

## 5. Type System: Topological & Structural Types

```
                       ┌─────────────────────────┐
                       │       Value Types       │
                       │ (Scalar, Span, Symbol)  │
                       └────────────┬────────────┘
                                    │ wraps
                       ┌────────────▼────────────┐
                       │         Cell[T]         │
                       └────────────┬────────────┘
                                    │ links along d
                       ┌────────────▼────────────┐
                       │       Rank[d, T]        │
                       └────────────┬────────────┘
                                    │ intersects
                       ┌────────────▼────────────┐
                       │     Pivot[d1, d2]       │
                       └────────────┬────────────┘
                                    │ spans
                       ┌────────────▼────────────┐
                       │     Shape { ... }       │
                       └─────────────────────────┘
```

### 5.1 Primitive Values & Payloads

A cell carries either:

- `Int`: 64-bit two's complement integer.
- `Float`: 64-bit IEEE 754 floating-point scalar.
- `Sym`: 64-bit hashed atom identifier (`#name`).
- `Span`: Nelsonian primedia coordinate `(author_id, offset, length)`.

### 5.2 Spatial Types: Cells, Ranks, Rings, and Shapes

- `Cell[T]`: An individual node holding payload `T`.
- `Rank[d, T]`: An open sequence of cells linked strictly along dimension `d`, each holding `T`.
- `Ring[d, T]`: A closed rank along `d` where traversing posward cycles to the origin cell.
- `Pivot[d1, d2]`: A cell guaranteeing $\text{deg}(C, d1) > 0$ and $\text{deg}(C, d2) > 0$.
- `Shape { +d.1: T1, -d.2: T2, ... }`: An orthotopal neighborhood contract asserting that the target
  cell has positive/negative neighbors along specific dimensions.

### 5.3 Dimensional Contracts along `d.contract`

`Vweave` supports topological contracts. A Spindle asserts its input shape:

```vweave
spindle PopNode(node: Cell) -> (val: Cell, next: Cell)
contract {
    node matches Shape { +d.data: Any, +d.next: Cell }
} {
    // Verified safe from missing links along +d.next
}
```

______________________________________________________________________

## 6. Language Mechanics & Operational Semantics

### 6.1 Warps (Execution Ranks) and Wefts (Data Ranks)

Computation in `Vweave` is a collision between two perpendicular ranks:

- The **Warp** is the timeline of control. By default, it moves along `d.step`.
- The **Weft** is a data stream. It moves along a data dimension, such as `d.data` or `d.tokens`.

```vweave
loom WordStream along d.step {
    weft Inputs along d.tokens {
        @w1: "Hypertext" -> @w2: "Intertwingled" -> @w3: "Transclusion"
    }

    warp Processor along d.step {
        @s1: FetchToken() *d.tokens: @w1 ~d.step>
        @s2: PrintUpper()                ~d.step>
        @s3: AdvanceToken()              ~d.step>
        @s4: ring d.step(@s1)
    }
}
```

When `@s1` executes, it pivots onto `@w1`. The operation `PrintUpper()` accesses its input by
reaching negward along `d.tokens`.

### 6.2 Pivots, Cross-Stitching, and Dimensional Swivels

The fundamental operator in `Vweave` is the **Pivot** (`*`). A pivot declares that a single cell
exists simultaneously on two or more dimensions:

```vweave
@pivot: 1042 *d.category: #Metrics ~d.time>
```

A running cursor does not branch; it **swivels**. A swivel command reorients the cursor's momentum
vector:

```vweave
warp Dispatcher along d.step {
    @start -> CheckCondition() -> deflect true to ~d.branch>
           -> @default_path    ~d.step> ...
}
```

If `CheckCondition()` produces `true`, the cursor swivels $90^\circ$ into `+d.branch`, entering an
orthogonal operational rank without stack unwinding.

### 6.3 Control Flow: Topological Rings and Deflection

Traditional languages implement loops using jumps (`JMP`). `Vweave` implements loops as
**Topological Rings**:

```
        @loop_start ──~d.step─► [Op 1] ──~d.step─► [Op 2]
             ▲                                        │
             │                                        │ ~d.step
             └─────────────── ring d.step ────────────┘
                                     │
                             (deflect ~d.exit>)
                                     │
                                     ▼
                                  @exit_op
```

```vweave
warp Counter along d.step {
    @init: 0 *d.val
    @loop: ReadInput() ~d.step>
           Increment(@init) ~d.step>
           deflect (@init *d.val >= 10) to ~d.exit>
           ring d.step(@loop)

    @exit_op: Finalize()
}
```

- To iterate: The cursor follows `~d.step>` around the ring.
- To break: `deflect` steers the cursor along `~d.exit>`, exiting the ring.
- Loops execute in bounded memory because no stack frames are created.

### 6.4 Subroutines: Dual-Wing Spindles and Spatial Closures

A function in `Vweave` is called a **Spindle**. It specifies how parameters align along the **Inputs
Wing** (`-d.2`) and how return values emerge along the **Outputs Wing** (`+d.2`):

```
                    -d.2 (Inputs Wing)
              [y] ◄─── [x] ◄───┐
                               │
                        [ @pivot: Add ]
                               │
                               ▼
                          [result]
                     +d.2 (Outputs Wing)
```

```vweave
spindle Add(x: Int, y: Int) -> (sum: Int) {
    warp along d.spin {
        fetch -d.2(x)      ~d.spin>
        fetch -d.2(y)      ~d.spin>
        sum = x + y        ~d.spin>
        emit +d.2(sum)
    }
}

warp Main along d.step {
    @p: Add(40, 2) ~d.step>
    Print(@p.+d.2)
}
```

### 6.5 Clone Superposition and Multi-Presence

Data in ZigZag is never copied. If an entity exists in multiple contexts, it is cloned along
`d.clone`. Walking negward along `d.clone` resolves to the canonical **Master Headcell**.

```vweave
loom ParallelAnalysis along d.step {
    weft Records along d.data {
        @master: Document("Hypertext")
    }

    warp MetricsWarp along d.step {
        @c1 = clone @master *d.view: #WordCount ~d.step>
        ComputeWordCount(@c1)
    }

    warp SentimentWarp along d.step {
        @c2 = clone @master *d.view: #Sentiment ~d.step>
        ComputeSentiment(@c2)
    }
}
```

`@c1` and `@c2` are distinct cells for control flow, but both share identical identity and
underlying text via their `d.clone` link.

______________________________________________________________________

## 7. Lowering to the Vortex Runtime

`Vweave` introduces zero new primitives to the C++ core. Every construct lowers directly to Vortex's
`link`, `value`, and `ArenaManifold`.

### 7.1 Production-to-Lowering Matrix

| `Vweave` Syntax       | Vortex Runtime Lowering                                                     | Operational Effect                           |
| :-------------------- | :-------------------------------------------------------------------------- | :------------------------------------------- |
| `dim d.foo;`          | `Store::makeDimension("d.foo")`                                             | Mints dimension identity in Merkle catalog   |
| `@c: 42`              | `c = makeScalarCell(42)`                                                    | Emits `OpKind::Structure (MakeCell)`         |
| `@a ~d.1> @b`         | `link(a, d.1, POS, b)`                                                      | Links `a` to `b` posward; `b` to `a` negward |
| `@p *d.2: @q`         | `link(p, d.2, POS, q)`                                                      | Establishes orthogonal pivot                 |
| `clone @master`       | `c = makeCell(); link(master, d.clone, POS, c)`                             | Extends the `d.clone` rank                   |
| `splice ~d.1> { @x }` | `link(curr, d.1, POS, x); link(x, d.1, POS, next)`                          | Inserts `@x` between `curr` and `next`       |
| `sever ~d.1>`         | `link(curr, d.1, POS, nullopt)`                                             | Breaks dimensional connection                |
| `ring d.step(@start)` | `link(curr, d.step, POS, start)`                                            | Closes 2-rank loop                           |
| `Spindle(...)`        | Allocate pivot; assemble wing links along `d.2`; emit bytecode on `+d.spin` | Prepares dual-wing execution frame           |

### 7.2 Memory Model & Compressed Sparse Row (CSR) Layout

In `apps/common/xanadu/zigzag/manifold.hpp`, links are stored as a compact Compressed Sparse Row
(CSR) index:

```cpp
struct LinkTriple {
  DimId dim;
  CellRef pos;
  CellRef neg;
};
```

When `Vweave` compiles:

1. Each cell occupies one entry in the `CellSlot` array.
1. Its dimensional connections occupy contiguous records in the CSR run.
1. Ephemeral evaluations execute entirely within `ArenaManifold`.

### 7.3 Ephemeral Execution & Promotion

Algorithms execute inside `ArenaManifold`. Ephemeral computations allocate cells in the arena spool.
When a computation finishes, calling `promote()` persists the resulting subgraph into the author's
spool, while unpromoted intermediate cells are discarded in $O(1)$ by resetting the arena head.

______________________________________________________________________

## 8. Standard Library: `std:weave`

The standard library module `std:weave` provides native operations for manipulating manifolds:

```vweave
module std:weave;

// Walks rank along dim from origin, applying fn at each cell
spindle Walk(origin: Cell, dim: DimId, fn: Spindle) { ... }

// Folds values along a dimension into an accumulator
spindle Fold(origin: Cell, dim: DimId, acc: Cell, fn: Spindle) -> (res: Cell) { ... }

// Splices an entire rank between two adjacent cells
spindle WeaveRank(target: Cell, dim: DimId, rankHead: Cell) { ... }

// Projects a 2D orthotope into an EDL for Xanadu rendering
spindle ProjectToXanadoc(root: Cell, dX: DimId, dY: DimId) -> (doc: Cell) { ... }
```

______________________________________________________________________

## 9. Worked Canonical Programs

### 9.1 Topological In-Place Quicksort

Traditional Quicksort shifts array indices in linear memory. In `Vweave`, Quicksort **re-links cells
along dimension `d.order` without copying or array allocations**:

```vweave
loom QuicksortLoom along d.step {
    dim d.order, d.less, d.greater;

    spindle Partition(head: Cell) -> (pivot: Cell, lessHead: Cell, greaterHead: Cell) {
        warp along d.step {
            pivot = head ~d.step>
            curr = pivot.+d.order ~d.step>

            @partition_loop:
                deflect (curr == nil) to ~d.done>
                next = curr.+d.order

                // Sever curr from its current link
                sever curr.~d.order>

                // Deflect into less or greater rank based on value
                if (curr.val < pivot.val) {
                    splice curr into ~d.less>
                } else {
                    splice curr into ~d.greater>
                }

                curr = next ~d.step>
                ring d.step(@partition_loop)

            @done:
                emit +d.2(pivot, lessHead, greaterHead)
        }
    }

    spindle Sort(head: Cell) -> (sorted: Cell) {
        (piv, less, greater) = Partition(head)
        sLess = Sort(less)
        sGreater = Sort(greater)

        WeaveChain(sLess, piv, sGreater, d.order)
        emit +d.2(sLess)
    }
}
```

### 9.2 Concurrent Tokenizer & Frequency Lattice

This program reads text, extracts words along `d.tokens`, and builds a 2D frequency lattice along
`d.vocab` and `d.count`:

```vweave
loom WordLattice along d.step {
    dim d.step, d.tokens, d.vocab, d.count;

    warp Ingest along d.step {
        @doc = OpenDocument("system://permascroll/speech.txt") ~d.step>
        @tok = TokenizeToWeft(@doc, d.tokens)                 ~d.step>

        cursor = @tok
        @scan:
            deflect (cursor == nil) to ~d.finish>
            word = cursor.val

            vocabCell = LookupOrSplice(d.vocab, word)
            vocabCell *d.count <~ IncCell()

            cursor = cursor.+d.tokens ~d.step>
            ring d.step(@scan)

        @finish:
            ExportLattice(d.vocab, d.count)
    }
}
```

```
               d.count (Tallies)
                  ▲
                  │  [3]
                  │   │
                  │  [2]
                  │   │
  d.vocab ───────[ "hypertext" ]───────[ "zigzag" ]───────►
```

### 9.3 Xanadu Transclusion & Citation Weave

Demonstrating seamless integration with Xanadu primedia spans (`std:xanadu`):

```vweave
loom TransclusionForge along d.step {
    dim d.step, d.transclude, d.annot;

    warp AssembleArticle along d.step {
        @quote = transclude span("author:ted", 1040, 240) *d.step>

        @comment = "Essential definition of Nelsonian intertwingularity."
                   *d.annot: @quote ~d.step>

        PublishSpan(@quote, @comment)
    }
}
```

______________________________________________________________________

## 10. The `xuzz` Interactive Experience & Live Projection

`Vweave` is designed to be experienced visually inside `xuzz` (or its compatibility links `xudu` and
`zigzag`).

```
┌──────────────────────────────────────┬──────────────────────────────────────┐
│ VWEAVE TEXT BUFFER                   │ XUZZ 2D MANIFOLD VISUALIZER (d.1/d.2)│
├──────────────────────────────────────┼──────────────────────────────────────┤
│ 01: warp Pipeline along d.step {     │                                      │
│ 02:   @fetch -> Read()    ~d.step>   │          [Read] (Step 1)             │
│ 03:   @eval  -> Calc()    ~d.step>   │            │                        │
│ 04:             *d.val: 42           │            ▼ +d.step                 │
│ 05:   @emit  -> Print()   ~d.step>   │  [42] ◄──[Calc] (Step 2, Active)     │
│ 06: }                                │   -d.2     │                        │
│                                      │            ▼ +d.step                 │
│ [CURSOR AT LINE 03]                  │          [Print] (Step 3)            │
│                                      │                                      │
└──────────────────────────────────────┴──────────────────────────────────────┘
```

1. **Bilateral Caret Synchronization**: Moving the text cursor in the code editor animates the
   visualizer camera in `xuzz` to focus on the corresponding cell in the manifold.
1. **Interactive Swiveling**: While paused on cell `@eval`, pressing `Ctrl+X` / `Ctrl+Y` in `xuzz`
   swivels the screen axes from `(d.step, d.val)` to `(d.time, d.clone)`, exposing the microversion
   history and clone topology of the executing instruction.
1. **No AST Disconnect**: Because `Vweave` is homoiconic, the program edited in the buffer is the
   exact slice executing in the engine. Saving the text file appends `OpKind::Structure` records
   directly into the store's ops spool.

[toc-01]: #21-pl-design-the-1d-stream-vs-graph-topology
[toc-02]: #22-vortex-engine-mechanical-invariants-of-the-spool
[toc-03]: #23-zigzag-power-user-sensory-navigation-and-live-projection
[toc-04]: #24-technical-writer-the-loom-metaphor-and-lexical-precision
[toc-05]: #32-invariant-2-the-single-primitive-invariant
[toc-06]: #33-invariant-3-dual-wing-spatial-invocation
[toc-07]: #34-invariant-4-topological-scope--zero-unanchored-cells
[toc-08]: #52-spatial-types-cells-ranks-rings-and-shapes
[toc-09]: #53-dimensional-contracts-along-dcontract
[toc-10]: #61-warps-execution-ranks-and-wefts-data-ranks
[toc-11]: #62-pivots-cross-stitching-and-dimensional-swivels
[toc-12]: #63-control-flow-topological-rings-and-deflection
[toc-13]: #64-subroutines-dual-wing-spindles-and-spatial-closures
[toc-14]: #65-clone-superposition-and-multi-presence
[toc-15]: #72-memory-model--compressed-sparse-row-csr-layout
[toc-16]: #92-concurrent-tokenizer--frequency-lattice
[toc-17]: #93-xanadu-transclusion--citation-weave
[toc-18]: #10-the-xuzz-interactive-experience--live-projection
