# Vweave: A Topological, Vortex-Native Programming Language Over Zigzag Manifolds

**Document Version:** 1.2 — The Structured Orthotopal Specification\
**Compilation Target:** Vortex Hyperstructural Runtime Core — `link` and `value`, and nothing else
(§7)\
**Primary Substrate:** 2-Rank Multidimensional Manifold (`zigzag::Manifold`, `ArenaManifold`,
`CompactOpNode`)\
**Status:** Specification only; non-breaking design document. Implements no new C++ core primitives;
rests entirely on existing mechanisms built in `apps/common/xanadu/vortex/` and
`apps/common/xanadu/zigzag/`.\
**Reconciliation History:** v1.1 rectified the calling model and removed `loom`/`nil`. v1.2
eliminates GOTO-style deflection and manual ring jumps in favor of structured `walk`, `loop`, and
`if/else`, while eliminating ambiguous `-dim>` arrow syntax.

______________________________________________________________________

## Contents

- [1. The Premise: Beyond Immigrant Paradigms](#1-the-premise-beyond-immigrant-paradigms)
- [2. The Multi-Agent Architectural Audit & Dialectic][toc-01]
  - [2.1 The Indictment Across Five Axes][toc-02]
  - [2.2 Rectifications: From v1.0 to v1.2][toc-03]
- [3. Architectural Foundations & Invariants][toc-04]
  - [3.1 Invariant 1: The Nelsonian 2-Rank Manifold & Collision Semantics][toc-05]
  - [3.2 Invariant 2: The Single-Primitive & Host Dispatch Model][toc-06]
  - [3.3 Invariant 3: Re-entrant Invocation via Activation Headcells][toc-07]
  - [3.4 Invariant 4: Continuous Open Space & Topological GC][toc-08]
- [4. Formal EBNF Grammar (v1.2)](#4-formal-ebnf-grammar-v12)
- [5. Type System: Topological & Structural Types][toc-09]
  - [5.1 Primitive Values & Payloads](#51-primitive-values--payloads)
  - [5.2 Spatial Types: Cells, Ranks, Rings, and Shapes][toc-10]
  - [5.3 Topological Boundary Guards & Contracts][toc-11]
- [6. Language Mechanics & Operational Semantics][toc-12]
  - [6.1 Warps (Execution) & Wefts (Data) with Dimension Inheritance][toc-13]
  - [6.2 Pivots, Cross-Stitching, and Directional Stepping][toc-14]
  - [6.3 Structured Control Flow: Implied Rings (`walk`, `loop`) & Diamonds][toc-15]
  - [6.4 Subroutines: Activation Headcells and Spatial Wings][toc-16]
  - [6.5 Clone Superposition and Ring Splicing][toc-17]
- [7. Lowering to the Vortex Runtime](#7-lowering-to-the-vortex-runtime)
  - [7.1 Production-to-Lowering Matrix](#71-production-to-lowering-matrix)
  - [7.2 Memory Model & Compressed Sparse Row (CSR) Layout][toc-18]
  - [7.3 Ephemeral Execution & Store Promotion][toc-19]
- [8. Standard Library: `std:weave`](#8-standard-library-stdweave)
- [9. Worked Canonical Programs](#9-worked-canonical-programs)
  - [9.1 Semantic Tagging & Dimensional Video Sorter][toc-20]
  - [9.2 Topological Insertion & Priority Queue][toc-21]
  - [9.3 Xanadu Transclusion & Citation Weave][toc-22]
- [10. The `xuzz` Interactive Experience & Live Projection][toc-23]
- [Appendix: Versioning and Change History][toc-24]

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
each cell has at most one positive neighbor and at most one negative neighbor per dimension:

$$
\text{deg}^+(C, d) \le 1, \quad \text{deg}^-(C, d) \le 1
$$

`Vweave` is a textual, Vortex-native programming language designed from the substrate up. It does
not map conventional language idioms onto ZigZag; it turns the topological physics of ZigZag into
first-class programming constructs.

______________________________________________________________________

## 2. The Multi-Agent Architectural Audit & Dialectic

### 2.1 The Indictment Across Five Axes

An adversarial multi-agent critique identified four systemic problems in the initial design:

1. **The Recursion Paradox:** Static singleton pivot cells with hardcoded `-d.2` inputs prohibited
   recursion and re-entrancy.
1. **The "Nil" and Container Heresy:** Enclosing blocks (`loom`) and sentinel pointers
   (`curr == nil`) betrayed continuous, non-hierarchical zzspace.
1. **GOTO-Style Deflection & Manual Rings:** Using `deflect cond to ~d.label>` and manual
   `ring d.step(@label)` reproduced 1950s assembly spaghetti inside a spatial language.
1. **Arrow Syntax Confusion:** Overloading `-` in step arrows (`-d.dim>`) created severe visual
   confusion with negward direction.

### 2.2 Rectifications: From v1.0 to v1.2

- **Implied Topological Rings (`walk`, `loop`):** Programmers no longer manually name `@label` and
  close with `ring d.step(@label)`. A `walk` or `loop` block automatically synthesizes its closed
  topological ring and boundary deflection in the manifold.
- **Structured Topological Diamonds (`if / else`):** Deflection is no longer an unstructured GOTO.
  Conditionals construct a topological fork-join diamond (swiveling to `d.branch` and reuniting at a
  convergence pivot).
- **Unambiguous Stepping:** Dropped `-` from arrow verbs. The step verb is `->`. Stepping across
  dimensions explicitly declares target sign: `-> +d.dim` (posward) or `-> -d.dim` (negward). Bare
  `->` steps along the active warp dimension.
- **Activation Headcells:** Subroutines allocate ephemeral frames on `d.grab`/`d.stack`, unlocking
  unbounded recursion and concurrency.
- **Topological Boundary Guards:** `!has_link(+d.dim)` replaces `nil`.

| Concept        | v1.0 (Initial Proposal)                 | v1.2 (Structured Orthotopal)                      |
| :------------- | :-------------------------------------- | :------------------------------------------------ |
| **Loops**      | Manual `@label` + `ring d.step(@label)` | Implied ring via `walk` and `loop`                |
| **Branching**  | Unstructured `deflect to ~d.label>`     | Structured `if / else` topological diamond        |
| **Step Arrow** | Confusing `-d.dim>` / `~d.dim>`         | Clear `->` (active) and `-> +d.dim` / `-> -d.dim` |
| **Scope**      | Enclosing `loom { ... }` block          | Open top-level ranks (`warp` / `weft`)            |
| **Boundary**   | Sentinel pointer check (`nil`)          | Topological guard (`!has_link(+d.dim)`)           |
| **Splicing**   | `splice item into ~curr.+d.order>`      | `splice item after curr along d.order`            |

______________________________________________________________________

## 3. Architectural Foundations & Invariants

```
                        +d.grab (Inputs Wing)
              [y] ◄──+d.step── [x] ◄───┐
                                       │
                              [ Activation Headcell ] ──+d.vars──► { locals }
                                       │
                                       ▼
                                   [result]
                        -d.grab (Outputs Wing)
                                       │
                                       ▼ +d.spin
                            [ Opcode Execution Stream ]
```

### 3.1 Invariant 1: The Nelsonian 2-Rank Manifold & Collision Semantics

Along any dimension $d$, every cell $C$ has at most one positive neighbor and at most one negative
neighbor:

$$
\text{deg}^+(C, d) \le 1, \quad \text{deg}^-(C, d) \le 1
$$

Branching is never an inline fork along the same dimension; branching is an orthogonal deflection.

#### Runtime Collision Semantics

When an operation attempts to link cell $A$ posward along dimension $d$:

1. **Default Mode (Topological Fault):** If $A$ already has a neighbor in $+d$, the engine raises
   `DimensionalCollisionFault`. Re-linking requires explicit `sever +d` first.
1. **Structured Splicing:** Inserting between cells uses atomic `splice`, updating both reciprocal
   links without leaving dangling half-links.

### 3.2 Invariant 2: The Single-Primitive & Host Dispatch Model

The persistent manifold compiles strictly to Vortex's two primitives:

$$
\text{link}(c, d, \text{dir}, [t]) \quad \text{and} \quad \text{value}(c, [v])
$$

Instruction streams dispatch along `+d.spin`. ALU operations are evaluated via host C++ `OpcodeKind`
dispatch during `VortexVM::step()`, preserving hardware speed while keeping state in
`ArenaManifold`.

### 3.3 Invariant 3: Re-entrant Invocation via Activation Headcells

Subroutines do not push activation records to an out-of-band C++ call stack. Instead:

- Every invocation mints an ephemeral **Activation Headcell** in `ArenaManifold`.
- Operands arrive on the **Inputs Wing** (`+d.grab`) chained along `+d.step`.
- Return values emit on the **Outputs Wing** (`-d.grab`).
- Instructions execute along the **Spin Dimension** (`+d.spin`).
- The caller return continuation links along `-d.spin` (or `d.stack`).

This enables unbounded recursion, re-entrancy, and concurrency while strictly respecting
$\text{deg}^\pm \le 1$.

### 3.4 Invariant 4: Continuous Open Space & Topological GC

`Vweave` recognizes no file, module, or loom container boundaries. All ranks extend through a
continuous coordinate manifold. Memory reclamation in `ArenaManifold` is topological: resetting the
arena head reclaims unpromoted activation frames and intermediate cells in $O(1)$.

______________________________________________________________________

## 4. Formal EBNF Grammar (v1.2)

```ebnf
Program         ::= { TopLevelItem } ;
TopLevelItem    ::= DimensionDecl | RankDecl | SpindleDecl | Statement ;

DimensionDecl   ::= "dim" Ident { "," Ident } ";" ;

RankDecl        ::= ( "warp" | "weft" ) Ident [ "along" DimensionName ] "{" { Statement } "}" ;
DimensionName   ::= "d." Ident ;

Statement       ::= StepStmt | WalkStmt | LoopStmt | IfStmt | BreakStmt | EmitStmt ;

StepStmt        ::= [ CellAnchor ":" ] StepElement [ StepArrow ] ;
StepElement     ::= Literal | Invocation | SpliceStmt | SeverStmt | Assignment ;
StepArrow       ::= "->" [ DirectionalTarget ] ;
DirectionalTarget::= ( "+" | "-" ) DimensionName ;

WalkStmt        ::= "walk" Ident "along" DimensionName "{" { Statement } "}" ;
LoopStmt        ::= "loop" "{" { Statement } "}" ;

IfStmt          ::= "if" Condition "{" { Statement } "}" [ "else" ( IfStmt | "{" { Statement } "}" ) ] ;
BreakStmt       ::= "break" ;
EmitStmt        ::= "emit" ValueExpr ;

SpliceStmt      ::= "splice" ValueExpr ( "before" | "after" ) CellRef "along" DimensionName ;
SeverStmt       ::= "sever" ( "+" | "-" ) DimensionName [ "from" CellRef ] ;
Assignment      ::= ( CellRef | Ident ) "=" ValueExpr ;

CellAnchor      ::= "@" Ident [ PivotClause ] ;
PivotClause     ::= "*" DimensionName ":" ( CellAnchor | ValueExpr ) ;
CellRef         ::= ( "@" Ident | Ident | "$focus" | "$mark" ) [ "." DimensionDir ] ;
DimensionDir    ::= ( "+" | "-" ) DimensionName ;

SpindleDecl     ::= "spindle" Ident "(" [ ParamList ] ")" [ "->" "(" [ OutputList ] ")" ]
                    "{" { Statement } "}" ;

ParamList       ::= Ident { "," Ident } ;
OutputList      ::= Ident { "," Ident } ;

Condition       ::= TopologicalGuard | ValuePredicate ;
TopologicalGuard::= [ "!" ] "has_link" "(" DimensionDir [ "," CellRef ] ")" ;
ValuePredicate  ::= ValueExpr ( "==" | "!=" | "<" | "<=" | ">" | ">=" ) ValueExpr ;

ValueExpr       ::= Literal | CellRef [ ".val" ] | Invocation | SpanExpr ;
SpanExpr        ::= "span" "(" ( Ident | SymLiteral ) "," Integer "," Integer ")" ;
Literal         ::= Integer | Float | String | SymLiteral ;
SymLiteral      ::= "#" Ident ;
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
                                    │ guards
                       ┌────────────▼────────────┐
                       │    TopologicalGuard     │
                       └─────────────────────────┘
```

### 5.1 Primitive Values & Payloads

A cell carries:

- `Int`: 64-bit two's complement integer.
- `Float`: 64-bit IEEE 754 floating-point scalar.
- `Sym`: 64-bit hashed atom identifier (`#name`).
- `Span`: Nelsonian primedia coordinate `(author_id, offset, length)` addressing the Permascroll.

### 5.2 Spatial Types: Cells, Ranks, Rings, and Shapes

- `Cell[T]`: An individual node holding payload `T`.
- `Rank[d, T]`: An open sequence of cells linked strictly along dimension `d`.
- `Ring[d, T]`: A closed rank along `d` where traversing posward cycles back to the origin cell.
- `Pivot[d1, d2]`: A cell guaranteeing $\text{deg}(C, d1) > 0$ and $\text{deg}(C, d2) > 0$.

### 5.3 Topological Boundary Guards & Contracts

Topological assertions are evaluated without sentinel pointers:

- `has_link(+d.order, c)`: True if cell `c` has an outgoing link posward on `d.order`.
- `!has_link(+d.order, c)`: True if the rank terminates posward at `c`.

______________________________________________________________________

## 6. Language Mechanics & Operational Semantics

### 6.1 Warps (Execution) & Wefts (Data) with Dimension Inheritance

Computation in `Vweave` is an orthogonal collision between two perpendicular ranks:

- **Warp**: Timeline of operational control (defaults to `d.step`).
- **Weft**: Sequence of data cells (e.g., `d.tokens`, `d.data`).

#### Dimension Inheritance

Within `warp Name along d.step`, the arrow `->` automatically advances along `+d.step`. Directional
targets are needed only when changing dimensions:

```vweave
warp ProcessTokens along d.step {
    FetchNext() ->
    PrintUpper() ->
    -> +d.log
}
```

### 6.2 Pivots, Cross-Stitching, and Directional Stepping

The **Pivot** (`*`) attaches an orthogonal dimension to an existing cell without moving the cursor:

```vweave
// Attach MIME type along d.tag without stepping the cursor:
vid *d.tag: #MP4
```

To move the cursor across dimensions, specify the target direction:

```vweave
// Step posward along d.tag:
-> +d.tag

// Step negward along d.tag:
-> -d.tag
```

### 6.3 Structured Control Flow: Implied Rings (`walk`, `loop`) & Diamonds

Control flow uses structured blocks instead of manual ring jumps and GOTO deflections:

#### The `walk` Construct (Implied Boundary-Guarded Ring)

`walk item along d.dim` steps posward through a rank. It automatically checks `has_link(+d.dim)`
before each step and loops back via an implied topological ring:

```vweave
walk item along d.videos {
    Process(item)
}
```

#### The `if / else` Construct (Topological Diamond)

Conditionals compile into a topological fork-join diamond:

1. True condition deflects into `+d.branch`.
1. False condition continues on the main rank (or alternate branch).
1. Both branches join at a shared convergence pivot cell.

```vweave
if item.val < pivot.val {
    splice item before pivot along d.order
} else {
    splice item after pivot along d.order
}
```

### 6.4 Subroutines: Activation Headcells and Spatial Wings

A `spindle` defines a reusable topology template:

```vweave
spindle Add(x: Int, y: Int) -> (sum: Int) {
    sum = x + y
    emit sum
}

warp Main along d.step {
    res = Add(40, 2)
    Print(res)
}
```

The compiler lowers `Add(40, 2)` to:

1. Allocate an ephemeral Activation Headcell $A$ in `ArenaManifold`.
1. Link argument `40` to $A.+d.\text{grab}$, and chain `2` along `+d.step`.
1. Link the return continuation on $A.-d.\text{spin}$.
1. Advance the VM cursor to `Add`'s bytecode stream on `+d.spin`.
1. `emit sum` links `sum` to $A.-d.\text{grab}$ and resumes the caller continuation.

### 6.5 Clone Superposition and Ring Splicing

Data in ZigZag is never copied. If an entity exists in multiple views, it is cloned along `d.clone`.
To preserve the 2-rank manifold invariant ($\text{deg}^\pm \le 1$), clones are spliced into a
**`d.clone` ring**:

```vweave
@c1 = clone @master *d.view: #WordCount
```

```
                  ┌───────────────┐
                  │ @master (Doc) │◄───┐
                  └───────┬───────┘    │
                  +d.clone│            │+d.clone
                          ▼            │
                   ┌─────────────┐     │
                   │  @c1 (View) ├─────┘
                   └─────────────┘
```

Both cells share immutable primedia text through the headcell, but participate in separate warps.

______________________________________________________________________

## 7. Lowering to the Vortex Runtime

### 7.1 Production-to-Lowering Matrix

| `Vweave` Syntax         | Vortex Runtime Lowering                           | Operational Effect                          |
| :---------------------- | :------------------------------------------------ | :------------------------------------------ |
| `dim d.foo;`            | `Store::makeDimension("d.foo")`                   | Mints dimension identity in Merkle catalog  |
| `@c: 42`                | `c = makeScalarCell(42)`                          | Emits `OpKind::Structure (MakeCell)`        |
| `->`                    | `setCursorOpcode(c, nextOp)`                      | Advances cursor along active warp dimension |
| `-> +d.tag`             | `setCursorOpcode(c, linked(c, d.tag, POS))`       | Steps cursor posward along dimension        |
| `@p *d.tag: @q`         | `link(p, d.tag, POS, q)`                          | Establishes orthogonal pivot                |
| `walk x along d`        | Implied ring loop with `has_link(+d)` guard       | Lowered to ring loop and exit deflection    |
| `if cond { ... }`       | `emitOp(OpcodeKind::Branch, {cond, ...})`         | Lowers to topological fork-join diamond     |
| `break`                 | `emitOp(OpcodeKind::Jump, {exitPivot})`           | Deflects cursor to enclosing loop exit      |
| `splice x after y`      | `link(x, d, POS, next); link(y, d, POS, x)`       | Atomically splices `x` posward of `y`       |
| `sever +d from x`       | `link(x, d, POS, noCell)`                         | Breaks dimensional link posward             |
| `emit val`              | `link(actHead, d.grab, NEG, val); emitOp(Return)` | Links result cell to `-d.grab` of frame     |
| `Spindle(...)` (Call)   | `f = makeCell(); link(f, d.grab, POS, args); ...` | Mints activation headcell on `d.stack`      |
| `span(#auth, off, len)` | `makeSpanCell(Span{auth, off, len})`              | Mints cell carrying Permascroll span        |

### 7.2 Memory Model & Compressed Sparse Row (CSR) Layout

In `apps/common/xanadu/zigzag/manifold.hpp`, cells and links use compact cache-aligned storage:

```cpp
struct DimLink {
  DimRef dim{noCell};  // 4 bytes
  CellRef pos{noCell}; // 4 bytes
  CellRef neg{noCell}; // 4 bytes
};
static_assert(sizeof(DimLink) == 12);

struct CellSlot {
  std::uint32_t spanOffset;
  std::uint16_t spanCount;
  std::uint16_t formatFlags;
  std::uint32_t birthOp;
  std::uint32_t lastOp;
  std::uint32_t linkOffset;
  std::uint16_t linkCount;
  std::uint8_t valueKind;
  std::uint8_t flags;
  std::uint64_t valueBits;
};
static_assert(sizeof(CellSlot) == 32);
```

### 7.3 Ephemeral Execution & Store Promotion

Subroutine activation headcells and scratch calculations reside in `ArenaManifold`. When execution
completes, calling `arena.promote()` transfers only the targeted result subgraph into the author's
persistent `Store`, while ephemeral frames and dead link runs are discarded in $O(1)$ by resetting
the arena head.

______________________________________________________________________

## 8. Standard Library: `std:weave`

The `std:weave` standard library module provides foundational manifold algorithms:

```vweave
// Walks rank along dim from origin, executing fn at each cell
spindle Walk(origin: Cell, dim: DimName, fn: Spindle) { ... }

// Folds values along a dimension into an accumulator
spindle Fold(origin: Cell, dim: DimName, acc: Cell, fn: Spindle) -> (res: Cell) { ... }

// Atomically splices an entire rank between two adjacent cells
spindle WeaveRank(target: Cell, dim: DimName, rankHead: Cell) { ... }

// Projects a 2D orthotope into an EDL for Xanadu rendering
spindle ProjectToXanadoc(root: Cell, dX: DimName, dY: DimName) -> (doc: Cell) { ... }
```

______________________________________________________________________

## 9. Worked Canonical Programs

### 9.1 Semantic Tagging & Dimensional Video Sorter

This program walks a rank of video cells along `d.videos` from `$focus`, attaches an orthogonal
pivot along `d.tag` holding each video's MIME type, and weaves an ordered sequence along `d.order`:

```vweave
dim d.step, d.videos, d.tag, d.order;

// Spindle: Inserts a video cell into the d.order rank sorted by MIME tag value
spindle InsertSorted(head: Cell, item: Cell) -> (newHead: Cell) {
    if !has_link(+d.order, head) {
        emit item
    } else if item.+d.tag.val < head.+d.tag.val {
        splice item before head along d.order
        emit item
    } else {
        walk curr along d.order {
            if !has_link(+d.order, curr) {
                link(curr, d.order, POS, item)
                break
            }
            if item.+d.tag.val < curr.+d.order.+d.tag.val {
                splice item after curr along d.order
                break
            }
        }
        emit head
    }
}

// Main Warp: Walks d.videos from $focus, tags each cell, and weaves d.order
warp TagAndSort along d.step {
    orderHead = $focus

    walk vid along d.videos {
        // 1. Pivot: Attach MIME type tag cell without altering d.videos
        vid *d.tag: DetectMime(vid)

        // 2. Weave into the sorted d.order rank
        orderHead = InsertSorted(orderHead, vid)
    }

    emit orderHead
}
```

```
                     ▲ +d.tag (MIME Type)
                     │
               [ "video/webm" ]           [ "video/mp4" ]
                     ▲                          ▲
                     │ *d.tag                   │ *d.tag
d.videos ──► [ Vid 1: "clip.webm" ] ──► [ Vid 2: "intro.mp4" ] ──►
                     │                          ▲
                     │                          │
d.order  ────────────┼──────────────────────────┴──────────~d.order──────►
                     └─────────────────────────────────────► [ Vid 1 ]
```

### 9.2 Topological Insertion & Priority Queue

A priority queue in `Vweave` is a 2-rank manifold where tasks are inserted along `d.priority` based
on an integer value:

```vweave
dim d.step, d.priority;

spindle EnqueueTask(queueHead: Cell, task: Cell, priority: Int) -> (newHead: Cell) {
    task.val = priority

    if !has_link(+d.priority, queueHead) {
        emit task
    } else if priority > queueHead.val {
        splice task before queueHead along d.priority
        emit task
    } else {
        walk curr along d.priority {
            if !has_link(+d.priority, curr) {
                link(curr, d.priority, POS, task)
                break
            }
            if priority > curr.+d.priority.val {
                splice task after curr along d.priority
                break
            }
        }
        emit queueHead
    }
}
```

### 9.3 Xanadu Transclusion & Citation Weave

Demonstrating zero-copy transclusion from immutable Permascrolls into a citation mesh:

```vweave
dim d.step, d.doc, d.tag, d.cite;

spindle TagSpan(targetSpan: Span, tagName: Sym) -> (taggedCell: Cell) {
    @node = targetSpan *d.tag: tagName
    emit @node
}

warp AnnotateArticle along d.step {
    // 1. Transclude span directly from permascroll without POSIX file paths
    @quote = span(#author_ted, 1042, 128) *d.doc: $focus

    // 2. Cross-stitch tag category
    @tagged = TagSpan(@quote, #Intertwingularity)

    // 3. Weave bidirectional citation link into secondary view
    splice @tagged after $mark along d.cite
    emit @tagged
}
```

______________________________________________________________________

## 10. The `xuzz` Interactive Experience & Live Projection

`Vweave` is designed to be experienced visually inside `xuzz` (or its compatibility links `xudu` and
`zigzag`).

```
┌──────────────────────────────────────┬──────────────────────────────────────┐
│ VWEAVE TEXT BUFFER / OMNIBAR         │ XUZZ 2D MANIFOLD VISUALIZER (d.1/d.2)│
├──────────────────────────────────────┼──────────────────────────────────────┤
│ 01: warp Pipeline along d.step {     │                                      │
│ 02:   @fetch = Read() ->             │          [Read] (Step 1)             │
│ 03:   @eval  = Calc() ->             │            │                        │
│ 04:            *d.val: 42            │            ▼ +d.step                 │
│ 05:   @emit  = Print()               │  [42] ◄──[Calc] (Step 2, Active)     │
│ 06: }                                │   -d.grab  │                        │
│                                      │            ▼ +d.step                 │
│ [CURSOR AT LINE 03 ($focus)]         │          [Print] (Step 3)            │
│                                      │                                      │
└──────────────────────────────────────┴──────────────────────────────────────┘
```

1. **Contextual Flow Mode (`$focus`, `$mark`):** In the `xuzz` omnibar (`:`), commands operate
   directly on the highlighted cell (`$focus`), extending ranks along active viewport axes
   (`dX`/`dY`).
1. **Bilateral Caret Synchronization:** Moving the text cursor in the code buffer automatically
   animates the `xuzz` camera to center on the corresponding cell in the 2D/3D visualizer.
1. **Interactive Swiveling:** While paused on cell `@eval`, pressing `Ctrl+X` / `Ctrl+Y` swivels the
   visualizer axes from `(d.step, d.val)` to `(d.time, d.clone)`, exposing the microversion history
   and clone topology of the executing instruction.
1. **Homoiconic Reality:** Code is not an ephemeral AST; saving a Vweave buffer writes
   `OpKind::Structure` nodes into the ops spool. The code is the slice; the slice is the code.

______________________________________________________________________

## Appendix: Versioning and Change History

**1.0 — Initial Specification (October 19, 2026).** Established the Nelsonian 2-rank manifold as a
programming substrate, the Loom/Warp/Weft metaphor, dual-wing parameter calling, and basic lowering
to Vortex primitives.

**1.1 — The Rectified Orthotopal Specification (October 20, 2026).** Overhauled the calling model to
use Activation Headcells on `d.grab`/`d.stack`, eliminated `loom` enclosures and `nil` sentinels,
and aligned with `VortexCore` system dimensions.

**1.2 — The Structured Orthotopal Specification (October 20, 2026).** Eliminated GOTO-style
`deflect` and manual `ring` jumps in favor of structured `walk`, `loop`, and `if / else` blocks.
Dropped ambiguous `-dim>` arrow syntax in favor of clean directional stepping (`-> +d.dim`,
`-> -d.dim`), and introduced readable `splice before/after` syntax.

[toc-01]: #2-the-multi-agent-architectural-audit--dialectic
[toc-02]: #21-the-indictment-across-five-axes
[toc-03]: #22-rectifications-from-v10-to-v12
[toc-04]: #3-architectural-foundations--invariants
[toc-05]: #31-invariant-1-the-nelsonian-2-rank-manifold--collision-semantics
[toc-06]: #32-invariant-2-the-single-primitive--host-dispatch-model
[toc-07]: #33-invariant-3-re-entrant-invocation-via-activation-headcells
[toc-08]: #34-invariant-4-continuous-open-space--topological-gc
[toc-09]: #5-type-system-topological--structural-types
[toc-10]: #52-spatial-types-cells-ranks-rings-and-shapes
[toc-11]: #53-topological-boundary-guards--contracts
[toc-12]: #6-language-mechanics--operational-semantics
[toc-13]: #61-warps-execution--wefts-data-with-dimension-inheritance
[toc-14]: #62-pivots-cross-stitching-and-directional-stepping
[toc-15]: #63-structured-control-flow-implied-rings-walk-loop--diamonds
[toc-16]: #64-subroutines-activation-headcells-and-spatial-wings
[toc-17]: #65-clone-superposition-and-ring-splicing
[toc-18]: #72-memory-model--compressed-sparse-row-csr-layout
[toc-19]: #73-ephemeral-execution--store-promotion
[toc-20]: #91-semantic-tagging--dimensional-video-sorter
[toc-21]: #92-topological-insertion--priority-queue
[toc-22]: #93-xanadu-transclusion--citation-weave
[toc-23]: #10-the-xuzz-interactive-experience--live-projection
[toc-24]: #appendix-versioning-and-change-history
