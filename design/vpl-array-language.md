# Vortex Parallel Language (VPL) Specification

**Document Version:** 1.0 — Iota Over Dimensions **Compilation Target:** Vortex Hyperstructural
Runtime Core — `link` and `value`, and nothing else (§3.1)

VPL is a third front end onto the same manifold that [Vortex](vortex-hyperstructural-runtime.md)
runs and [VQL](vql-query-language.md) queries: an APL, with APL's glyphs, whose arrays are regions
of a zzstructure rather than rectangles of memory. **Like VQL, it is a surface syntax and nothing
more — every glyph compiles to Vortex's `link` and `value` and introduces no primitive of its own.**
If an expression below cannot be written as a finite sequence of those two calls, it is a defect in
this document rather than a feature of the language; §3.1 gives the lowerings. Nothing here is wired
into the gleditor build, and unlike its two siblings this one began as an amusement — *what would
APL be if its axes were real dimensions you could walk?* It is written down because the answer
turned out to be less silly than expected, and because it puts pressure on parts of the convergence
that VQL never reaches.

The hook is a pun that happens to be a design. In APL, an array's **rank** is its number of axes. In
Zigzag, a **rank** is a sequence of cells along one dimension. The two words mean almost opposite
things — a count of axes against a walk along one — and reconciling them is most of this document.

______________________________________________________________________

## Contents

- [1. The Collision, and the Terminology That Survives It](#1-the-collision-and-the-terminology-that-survives-it)
- [2. What a VPL Value Is](#2-what-a-vpl-value-is)
- [3. The Glyph Set](#3-the-glyph-set)
  - [3.1 Every Glyph Is `link` and `value`](#31-every-glyph-is-link-and-value)
- [4. Worked Examples](#4-worked-examples)
  - [4.1 Iota, Shape, and the First Surprise](#41-iota-shape-and-the-first-surprise)
  - [4.2 Transpose Is a Viewport](#42-transpose-is-a-viewport)
  - [4.3 Reduction Along a Walk](#43-reduction-along-a-walk)
  - [4.4 Compress, Grade, and Ragged Axes](#44-compress-grade-and-ragged-axes)
  - [4.5 Outer Product Over Two Dimensions](#45-outer-product-over-two-dimensions)
  - [4.6 Disclose Is the Clone Master](#46-disclose-is-the-clone-master)
  - [4.7 Assignment Is a Weave, So Time Is an Axis](#47-assignment-is-a-weave-so-time-is-an-axis)
  - [4.8 A Document, Counted](#48-a-document-counted)
- [5. Where the Analogy Breaks](#5-where-the-analogy-breaks)
- [Appendix: Versioning and Change History](#appendix-versioning-and-change-history)

______________________________________________________________________

## 1. The Collision, and the Terminology That Survives It

Both words stay, because both communities would reject the substitute, and they are disambiguated by
never appearing in the same role:

- **rank** (unqualified) means the Zigzag one: the sequence of cells reached by walking one
  dimension posward and negward from a cell. This is the meaning the rest of `design/` already uses.
- **valence** means the APL one: how many dimensions a value is indexed along. `⍴⍴A` in APL gives
  the rank of `A`; in VPL it gives the **valence** of `A`.

The substitution is not arbitrary. APL's rank is the *arity of an indexing operation* — how many
subscripts you must supply — and "valence" is already the word for how many arguments something
takes. The glyph does not change: `⍴⍴` still answers it.

______________________________________________________________________

## 2. What a VPL Value Is

> A VPL value is a **view**: a set of cells, plus an ordered list of dimension cells to index them
> by. Its valence is the length of that list. Its shape is the extent along each.

```
view   ::= ( origin : CellRef , axes : [ DimRef ] )
```

Three consequences, each of which is a design decision rather than a consequence of notation:

- **A scalar is a cell of valence 0.** Not a special case: R6 gives a cell canonical bits alongside
  its rendering, so `3.14` as a VPL scalar *is* a cell carrying `0x40091EB851EB851F` and two
  permascroll bytes. Arithmetic reads the bits; printing reads the span.
- **Indexing is a walk, not an offset.** `A[3]` steps three cells posward along `A`'s first axis.
  This is the whole of VPL's cost model and it is discussed in §5.
- **A view is not a copy.** `B←A` binds a second name to the same cells; APL's value semantics do
  not survive, and §4.7 argues they are replaced by something better rather than merely lost.

______________________________________________________________________

## 3. The Glyph Set

Structural glyphs first, because they are the ones whose meaning moved:

| glyph | APL                | VPL                                                                       |
| ----- | ------------------ | ------------------------------------------------------------------------- |
| `⍳`   | index generator    | mint a rank of `n` cells along the current first axis                     |
| `⍴`   | shape / reshape    | extents along each axis; dyadic, re-lays cells onto a new axis list       |
| `⍉`   | transpose          | **rebind which dimensions are which axes** — a viewport change (§4.2)     |
| `⌽`   | reverse            | walk the first axis negward instead of posward                            |
| `⊖`   | reverse first axis | walk the *last* axis negward; `⌽` and `⊖` differ by which axis, as in APL |
| `↑`   | take               | the first `n` cells of the walk                                           |
| `↓`   | drop               | skip the first `n`; negative counts from the end, as everywhere else      |
| `⊂`   | enclose            | a cell whose content is a view — VQL's "complex data containment"         |
| `⊃`   | disclose / first   | resolve a clone to its **master** (§4.6)                                  |
| `∊`   | membership         | is this cell on that rank                                                 |
| `⍸`   | where              | the cells of a rank satisfying a predicate                                |
| `≡`   | depth / match      | valence when monadic; address identity when dyadic                        |

Operators (higher-order) keep their APL meanings exactly, which is the point of borrowing them:

| glyph | operator      | over a manifold                                                                                 |
| ----- | ------------- | ----------------------------------------------------------------------------------------------- |
| `/`   | reduce        | fold along a rank, in walk order                                                                |
| `\`   | scan          | the same, emitting every intermediate — so a scan mints a rank                                  |
| `¨`   | each          | apply per cell, fanning out exactly as a VQL step does                                          |
| `⌿`   | compress      | filter a rank by a boolean rank of the same extent                                              |
| `∘.`  | outer product | the cartesian of two ranks, woven as a 2-valence view (§4.5)                                    |
| `⍤`   | rank          | restrict a function to the first `k` axes — APL's rank operator, over dimensions that are cells |
| `⌸`   | key           | group a rank by a key function; each group is a clone rank                                      |

And the Xanadu-specific additions, kept to three because a borrowed notation earns nothing by
growing:

| glyph | VPL                                                                                  |
| ----- | ------------------------------------------------------------------------------------ |
| `⍟`   | the microversion a view was read at — its point in hypertime                         |
| `⍫`   | scrub: the same view as of another microversion (`⍫` is not APL's; nothing is spare) |
| `⌺`   | the transclusions of a view: every other cell addressing any of its spans            |

### 3.1 Every Glyph Is `link` and `value`

VQL earns its claim to add no primitives by showing the compilation for each construct (its §1), and
VPL is held to the same standard. Every glyph above lowers to `link(cell, ±dim, [target])` and
`value(cell, [offset], [length], [replacement])` — Vortex §1 — and to nothing else:

| glyph     | lowering                                                                            |
| --------- | ----------------------------------------------------------------------------------- |
| `n⍳d`     | `n` × `link(cursor, +d, -1)`, each allocation becoming the next cursor              |
| `A[k]`    | `k` × `link(cursor, +axis)` in read form, following the returned cell each time     |
| `⍴A`      | `link(cursor, +axis)` in read form until it answers nothing, counting the steps     |
| `⍉A`      | **no calls at all** — the axis list is a property of the view, not of the cells     |
| `⌽A` `⊖A` | likewise none: the sign on the axis flips, and `link`'s read form is already signed |
| `↑` `↓`   | bounded and skipped read-form walks; nothing is written                             |
| `⊂A`      | `value(cell, 0, -1, A)` — a cell whose content is a view                            |
| `⊃C`      | `link(cursor, -d.clone)` in read form until it answers nothing: the master          |
| `f/A`     | one read-form walk, `value(c)` per cell for the operand's bits, no writes           |
| `f\A`     | the same walk, plus one `link(...,-1)` and one `value(...,repl)` per intermediate   |
| `A∘.f B`  | `⍴A × ⍴B` applications of `f`, each lowering as `f` does                            |
| `A⌸f`     | one walk, then `link(master, +d.clone, member)` per group member                    |
| `⍟A`      | none — the microversion is what the read already happened at                        |
| `A⍫T`     | none of its own: the same lowering, folded from `T` instead of from the head        |
| `⌺A`      | one read-form walk per span, over the `d.transclude` rank                           |

Three things are worth reading off that table, because they are the parts a glyph hides.

**`⍉`, `⌽` and `⊖` are free.** They change the view's axis list or an axis's sign, and a view is
`(origin, axes)` — not cells. That is why §4.2 can call the visualizer's viewport a `⍉`: rebinding
which dimension is "across" costs nothing because nothing about the manifold changed.

**`\` and `⌸` write.** A scan mints a cell per intermediate and `⌸` links every group member onto a
clone rank, so both appear in hypertime. APL trains the instinct that these are cheap traversals;
here they are weaves, and §4.3 and §4.7 are about that.

**`⍫` compiles to nothing new**, which is the reconciliation that makes hypertime free rather than a
feature: scrubbing is the same lowering folded from a different starting microversion, exactly as
`Store::rebuildManifold()` is the same fold from a different index.

______________________________________________________________________

## 4. Worked Examples

Every example assumes `H` is the home cell and that `d.1`, `d.2` and `d.doc` exist. `⎕` prints.

### 4.1 Iota, Shape, and the First Surprise

```apl
      A ← 5⍳d.1              ⍝ mint five cells on a d.1 rank from H
      ⍴A
5
      ⍴⍴A                    ⍝ valence: one axis
1
      A[3]                   ⍝ walk three posward
      ⌽A                     ⍝ the same five cells, walked negward
      ⍴⌽A
5
```

`5⍳d.1` is `weave H/d.1%%%%%` in VQL, and the surprise is that it is *not* idempotent: running it
twice mints ten cells, because minting is an operation and operations accumulate. APL's `⍳5` is a
function of its argument; VPL's is a function of its argument *and* of when you ran it. The glyph is
the same; the algebra is not, and §4.7 is where that stops being a wart.

### 4.2 Transpose Is a Viewport

APL's `⍉` permutes axes. VPL's does exactly that, and because an axis is a dimension *cell*, the
permutation is a structural fact rather than a reinterpretation of strides:

```apl
      V ← H ⍴ (d.1 d.2 d.3)  ⍝ a 3-valence view over three dimensions
      ⍴V
4 7 2
      ⍴⍉V                    ⍝ reverse the axis list
2 7 4
      ⍴(2 0 1)⍉V             ⍝ arbitrary permutation, as in APL
7 4 2
```

This is `apps/zigzag`'s `swapDimensions`/`cycleDimensions` with a better notation. The viewport the
visualizer draws *is* a `⍉` of the underlying view — which means the 3D display has been a VPL
expression evaluator all along, and nobody wrote the language.

### 4.3 Reduction Along a Walk

```apl
      +/ 10⍳d.1              ⍝ sum ten freshly minted (empty) cells
0
      C ← 1 2 3 4 5 ⍴ d.1    ⍝ five scalar cells on a rank
      +/C
15
      ×/C
120
      +\C                    ⍝ scan mints a rank of partial sums
1 3 6 10 15
      ⍴+\C
5
```

`+/C` reads each cell's canonical bits (R6) and never parses a character, which is the whole reason
scalars carry both halves. `+\C` **mints five cells**, because a scan produces a value and every VPL
value is cells — so a scan is a weave, and appears in hypertime. An APL programmer's instinct that
`+\` is free is wrong here by exactly the cost of five operations.

### 4.4 Compress, Grade, and Ragged Axes

```apl
      W ← ⎕READ 'chapter.xanadoc' ⍝ a document's d.doc rank
      (5<≢¨W)⌿W              ⍝ the cells with more than five characters
      ⍋≢¨W                   ⍝ grade up by length
      W[⍋≢¨W]                ⍝ sorted, as in APL
      ⍸'the'∊¨W              ⍝ where 'the' occurs
```

`≢¨W` is "length of each", and it is the first place VPL has to admit something APL never does: a
cell's content is a *run* of spans (U3), so `≢` is the sum of the run's lengths, and two cells with
equal `≢` may share no address at all. The comparison is on extent, not identity; `≡` dyadic is
identity.

Ragged axes are the deeper admission. APL arrays are rectangular. A manifold region is not: walking
`d.2` from each cell of a `d.1` rank can give ranks of different lengths. `⍴` therefore answers the
extent of the *first* walk along each axis and marks the view ragged; `⍴⍴` is still exact, because
the number of axes is a property of the view rather than of the cells.

```apl
      ⍴R                     ⍝ ragged: 4 7 with a warning
4 7
      ≢¨R                    ⍝ the honest shape: per-row extents
7 3 7 5
```

APL would pad. VPL refuses to, for the reason the whole tree refuses to: a padded cell is a cell
that is not there, and inventing one would make `⌺` report a transclusion of nothing.

### 4.5 Outer Product Over Two Dimensions

```apl
      A ∘.+ B                ⍝ the classic: every pair, summed
      A ∘.{⍺ link +d.link ⍵} B   ⍝ every pair, *linked*
```

The second is where the notation pays for itself. An outer product whose function is `link` weaves a
complete bipartite graph between two ranks on a third dimension, in one expression, and the result
is a 2-valence view over it. In VQL that is a nested `for`; in Vortex it is a double loop over
`link`. Here it is the shape of the thing you wanted.

It also costs `⍴A × ⍴B` operations in the spool, which the notation entirely fails to suggest. That
is the same complaint APL has always attracted and the same answer: the expression is short because
the *idea* is small, not because the work is.

### 4.6 Disclose Is the Clone Master

```apl
      M ← ⊃C                 ⍝ the master of C's clone rank
      M ← 'override'         ⍝ every clone of C now reads 'override'
      ⊃¨G                    ⍝ each group's master, after ⌸
```

`⊃` is APL's "first item", and a clone rank's master is the negward end — the *first* cell if you
walk `d.clone` backwards. The correspondence is close enough to be slightly unsettling: APL's
disclose picks the representative of a nested value, and a master is exactly the representative of a
clone group.

`⌸` (key) groups a rank and, in VPL, **each group is a clone rank whose master holds the key**. So
`W⌸≢¨W` groups a document's cells by length, and reading any group member gives the length, because
the member resolves through the master that *is* the length. Grouping and identity-sharing turn out
to be one operation viewed twice.

### 4.7 Assignment Is a Weave, So Time Is an Axis

This is the section that justifies the document.

```apl
      A ← 1 2 3 ⍴ d.1
      T ← ⍟A                 ⍝ remember where we are in hypertime
      A[2] ← 99
      +/A
103
      +/ A ⍫ T               ⍝ the same expression, as of T
6
```

APL lost value semantics at `B←A` (§2) and gets something else back: **every assignment is an
operation, so every intermediate value a program ever held is still addressable.** `⍫` is not a
debugger feature and not a journal; it is the same read against an earlier microversion, and it
costs a fold rather than a copy.

The consequence an APL programmer will feel is that *`⍳` is not referentially transparent* and
cannot be — and that this is true of `+\`, `∘.`, `⌸` and every other glyph that produces a value,
because producing a value means minting cells. VPL is therefore not a pure array language with
effects bolted on; it is an array language whose values are all in one append-only store.
Idempotence is available, but you ask for it with `⍫` rather than getting it by default.

```apl
      ⌺A                     ⍝ who else quotes A's spans
      ⌺A ⍫ T                 ⍝ who quoted them then
```

### 4.8 A Document, Counted

The APL one-liner, over a xanadoc:

```apl
      ⍝ word frequency of a document, descending
      W ← ⎕SPLIT ⎕READ 'chapter.xanadoc'
      F ← W⌸≢W                    ⍝ group by word, count each group
      F[⍒⊃¨F]                     ⍝ order by count, descending
      10↑F[⍒⊃¨F]                  ⍝ the top ten
```

```apl
      ⍝ every passage this document shares with any other
      ⌺ ⎕READ 'chapter.xanadoc'

      ⍝ ...and how much of the document that is
      (+/≢¨⌺D) ÷ +/≢¨D
0.31
```

That last expression is `Store::diffVersions()`'s Identity Gold ratio, in nine glyphs. It is also
$O(\text{cells} \times \text{quotations})$ and looks $O(1)$, which §5 is about.

______________________________________________________________________

## 5. Where the Analogy Breaks

Four places, none fatal, all worth knowing before writing an interpreter.

**Indexing is a walk, and this is the convergence's own open question.** `A[5000]` walks five
thousand cells. U1 in [`store-slice-convergence.md`](store-slice-convergence.md) records exactly
this: a Green enfilade answers "the *n*-th thing" in $O(\log n)$ while a zzstructure rank answers
only successor and predecessor, so random access is the one place a structure map regresses against
what it replaced. Every measurement in that note is of *adjacency* walks — and **APL is a language
built on random access.** VPL is therefore the workload U1 asks for: if anything is going to demand
an order-statistic index per rank, it is `A[⍋A]`.

**Arrays are rectangular; manifolds are ragged.** §4.4 makes `⍴` answer the first walk and mark the
view, rather than padding. Every APL identity that assumes rectangularity — `⍴⍴`, `⍉`, `∘.` — holds
only on the rectangular subset, and an interpreter must say so rather than produce a plausible
answer over a ragged one.

**Values are not copies, and functions are not pure.** §4.7 treats that as a feature, and it is, but
it means the enormous body of APL identities that rewrite expressions for speed — `+/⍳n` to a closed
form, say — are *not* valid transformations here, because the discarded expression had side effects
in the spool. An optimiser may only rewrite what it can prove mints nothing.

**The spool is append-only, so `⍳` in a loop is a leak.** APL's idioms allocate freely and rely on a
garbage collector; R8 is explicit that the persistent side has none, because DELETE is REARRANGE TO
LIMBO. An interpreter should therefore run in an `ArenaManifold` by default and promote deliberately
— which is R8's two regimes arriving as a language-level default rather than as a library rule, and
is the strongest argument that VPL wants writing after `promote()` exists rather than before.

______________________________________________________________________

## Appendix: Versioning and Change History

The rule is the one its siblings use:

- **Major** — a change a conforming implementation could not ignore: a glyph gains, loses or changes
  meaning; a default changes what an existing expression does.
- **Minor** — an addition that breaks nothing, a refinement, a clarification, a correction to
  normative prose, punctuation.

Editorial changes that alter no normative text bump neither.

| version | commit | date       | change                                                                                                           |
| ------- | ------ | ---------- | ---------------------------------------------------------------------------------------------------------------- |
| 1.0     | *this* | 2026-09-11 | Initial specification: views, the rank/valence collision, the glyph set, and the four places the analogy breaks. |
