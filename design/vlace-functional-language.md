# vlace: A Purely Functional Language Over an Append-Only Manifold

**Document Version:** 1.1 — A Typed Effect Boundary **Compilation Target:** Vortex Hyperstructural
Runtime Core — `link` and `value`, and nothing else (§3) **Status:** specification only; nothing
here is wired into the gleditor build. It rests on mechanisms that *are* built — the `ArenaManifold`
and its `mark`/`release`/`promote` ([convergence](store-slice-convergence.md) step 21,
`zigzag/arena_manifold.hpp`), the dual-wing calling convention and `d.spin` instruction streams
([Vortex](vortex-hyperstructural-runtime.md)), and the pinned-island memoization of Vortex §6 — and
asks the C++ core for nothing those did not already pay for (§8).

> **What v1.1 changed, and why.** The 1.0 draft claimed purity "for free" from arena evaluation and
> then quietly extended the label to effectful operations (`read`/`save`). An expert review found
> that overclaim and several real holes. v1.1 earns the title instead of asserting it: it adds a
> **typed effect boundary** (`IO`, §6.5) so `read`/`save` are effects with a type rather than pure
> functions in disguise; a **prelude** (§7.4) and **error model** (§6.6) with `Option`/`Result`, so
> `lookup` and partial functions have a representation; an **operator grammar** (§5.1) the examples
> had been using without defining; a **production→lowering table** (§5.6) discharging §5.1's own
> promise; **records** (§5.7), **user modules** (§5.8), and a **numeric tower** (§4.6); and honest
> statements of the soundness properties — the memo key (§6.3), let-generalization (§5.5),
> promotion's non-confluence (§6.5), WHNF / blackholing / tail calls (§6.7). The appendix logs it.

vlace is a fourth front end onto the manifold that [Vortex](vortex-hyperstructural-runtime.md) runs,
[VQL](vql-query-language.md) queries, and [VPL](vpl-array-language.md) indexes, and a sibling to the
goal-directed [vicon](vicon-goal-directed-language.md). Where VQL borrows XQuery's paths and VPL
borrows APL's glyphs, vlace borrows the **ML/Haskell family's text**: lambda abstraction, `let`,
algebraic data types, pattern matching, and higher-order functions, written as ordinary identifiers
and keywords rather than as a traversal operator or a glyph alphabet. And where every one of its
siblings is explicit that *producing a value mints cells and is therefore not referentially
transparent* (VPL §4.7, VQL §7.1), vlace is the front end whose **evaluation core** is referentially
transparent — not by adding a mechanism, but by committing to one regime of a split the convergence
already drew — and whose **effects** (`read`, `save`) are confined to a typed boundary (§6.5) rather
than smuggled into expressions. Purity is a property the core earns and the type system marks, not a
label the title asserts.

**Like its three siblings, vlace is a surface syntax and nothing more.** Every construct compiles to
Vortex's `link` and `value`, and introduces no primitive of its own. If an expression below cannot
be written as a finite sequence of those two calls, it is a defect in this document rather than a
feature of the language. §3 gives the acceptance test and §5 the lowerings.

The reason vlace is worth writing down is that an append-only operations spool folded into a
manifold is *already* the central data structure of a pure functional language — an immutable value
with structural sharing — and nobody built it for that. The spool only ever grows; two operations
naming one parent are two futures that do not destroy each other
([store-slice-convergence.md](store-slice-convergence.md)); `d.clone` ranks share structure without
copying (Vortex §5.2). Those are, respectively, immutability, persistent-data-structure branching,
and hash-consing — the three things an implementation of Haskell's `Data.Map` or Clojure's vectors
works hardest to provide. vlace does not provide them; it *names* them. This document's job is to
show that an ML-shaped text reaches the primitives, that purity is a consequence of running in the
arena rather than a property bolted on, and that everything new lives in a `std:fun` package written
in Vortex.

______________________________________________________________________

## Contents

- [1. What is being claimed](#1-what-is-being-claimed)
- [2. Purity over an append-only store: the reconciliation that comes first][toc-01]
  - [2.1 The spool is an immutable value; evaluation is a pure function][toc-02]
  - [2.2 The two regimes are referential transparency and its absence][toc-03]
  - [2.3 Persistent data structures are the ops DAG and `d.clone`, unasked][toc-04]
- [3. The acceptance test: `link`, `value`, and nothing else][toc-17]
- [4. What a vlace value is](#4-what-a-vlace-value-is)
  - [4.1 A value is a cell; a constructor is a functor on the Inputs Wing][toc-05]
  - [4.2 Algebraic data types are a tag cell and an argument rank][toc-06]
  - [4.3 A function is a cell; a closure is its captured environment rank][toc-07]
  - [4.4 A thunk is a cell with an unevaluated `+d.spin` body][toc-08]
  - [4.5 Numbers, text, and the scalar cell](#45-numbers-text-and-the-scalar-cell)
  - [4.6 The numeric tower, `Bool`, characters, and literals][toc-25]
- [5. The language](#5-the-language)
  - [5.1 Lexical structure, keywords, operators, and the EBNF grammar][toc-23]
  - [5.2 `let`, `letrec`, and lexical scope as a `d.vars` rank][toc-09]
  - [5.3 Lambda, application, and the dual-wing calling convention][toc-10]
  - [5.4 Pattern matching is destructuring unification, forced to WHNF][toc-11]
  - [5.5 Data declarations, Hindley–Milner, and let-generalization][toc-24]
  - [5.6 The production→lowering table](#56-the-productionlowering-table)
  - [5.7 Records: named fields and functional update][toc-26]
  - [5.8 Modules: user-facing `import` and qualified names][toc-27]
- [6. Evaluation](#6-evaluation)
  - [6.1 Lazy by default; strictness is an annotation][toc-18]
  - [6.2 Laziness is a thunk; forcing is running a `+d.spin` stream][toc-12]
  - [6.3 Memoization is Vortex §6, and the memo key is thunk identity][toc-13]
  - [6.4 Recursion, sharing, and why the spool does not grow][toc-19]
  - [6.5 Effects: the typed `IO` boundary and promotion][toc-20]
  - [6.6 The runtime error model](#66-the-runtime-error-model)
  - [6.7 WHNF, blackholing, and proper tail calls](#67-whnf-blackholing-and-proper-tail-calls)
- [7. The `std:fun` standard-library package](#7-the-stdfun-standard-library-package)
  - [7.1 Package anchoring and exported symbols](#71-package-anchoring-and-exported-symbols)
  - [7.2 What `std:fun` adds, written in Vortex](#72-what-stdfun-adds-written-in-vortex)
  - [7.3 What is reused, unchanged, from the core modules][toc-21]
  - [7.4 The prelude: names in scope in a fresh program][toc-28]
- [8. What the C++ core actually has to gain](#8-what-the-c-core-actually-has-to-gain)
  - [8.1 Nothing in the VM or the arena](#81-nothing-in-the-vm-or-the-arena)
  - [8.2 A front end: lexer, parser, type checker, compiler, driver][toc-14]
  - [8.3 Coding standards the front end is held to](#83-coding-standards-the-front-end-is-held-to)
- [9. Worked examples](#9-worked-examples)
  - [9.1 `map` and `fold` over a list](#91-map-and-fold-over-a-list)
  - [9.2 An infinite stream, taken finitely](#92-an-infinite-stream-taken-finitely)
  - [9.3 A persistent map update that copies nothing][toc-15]
  - [9.4 A word-frequency counter: the pure core and the typed effect edge][toc-22]
  - [9.5 A record, with a functional update](#95-a-record-with-a-functional-update)
- [10. What this buys that a conventional functional language does not have][toc-16]
- [11. Breakages and costs](#11-breakages-and-costs)
- [12. Reconciliation with the convergence](#12-reconciliation-with-the-convergence)
- [Appendix: Versioning and Change History](#appendix-versioning-and-change-history)

______________________________________________________________________

## 1. What is being claimed

| Functional-language concept  | Vortex / ZigZag mechanism                                      | New machinery      |
| ---------------------------- | -------------------------------------------------------------- | ------------------ |
| An immutable value           | A cell; the spool it lives in only ever grows                  | none               |
| Variable binding             | A name cell on a `d.vars` rank, value on `+d.values`           | none — Vortex §4   |
| Lexical environment          | The `d.vars`/`d.values` scope of the enclosing cursor          | none — Vortex §4   |
| A function / lambda          | A cell whose body is an opcode stream on `+d.spin`             | none               |
| Closure (captured env)       | A link from the function cell to its defining scope            | none               |
| Function application         | Dual-wing call: arguments on `+d.grab`, result on `-d.grab`    | none — Vortex §3   |
| Algebraic data constructor   | A cell whose content is the tag, arguments on the Inputs Wing  | none               |
| Record with named fields     | A constructor cell plus a field-name rank on `d.vars`          | `std:fun/record`   |
| Pattern match                | Destructuring unification forced to WHNF per layer (§5.4)      | none — Vlog's      |
| `Option` / `Result`          | Prelude ADTs; `lookup` etc. return `Option` (§6.6, §7.4)       | prelude            |
| A thunk (lazy value)         | A cell with an unforced `+d.spin` body and a `d.cache` slot    | `std:fun/thunk`    |
| Forcing a thunk              | Run the `+d.spin` stream once; memoize by thunk identity       | `std:fun/force`    |
| `let x = e in body`          | Mint `x` on `d.vars`, bind `e` on `+d.values`, evaluate `body` | `std:fun/let`      |
| `letrec` / recursion         | The same, with `x` in scope before `e` is forced               | `std:fun/letrec`   |
| `map` / `filter` / `fold`    | `std:functional`'s combinators over a rank                     | none — core        |
| Persistent list / cons       | A rank along `+d.step`; `cons` links a head                    | none               |
| Persistent map update        | A new `d.clone` head over the unchanged tail — path copying    | none — Vortex §5.2 |
| Hash-consing / sharing       | Two names on one `d.clone` rank read one master                | none — Vortex §5.2 |
| Memoization / call-by-need   | A pinned island on `d.pinning-cursors` / `d.cache`             | `std:fun` keying   |
| Referential-transparent core | Evaluation in an `ArenaManifold`; no op minted                 | none — R8          |
| Effects (`read`/`save`)      | A typed `IO a` ADT; `promote()`/snapshot at the edge (§6.5)    | `std:fun/io`       |
| Inspectable reduction (opt)  | The persistent regime names every step — intensional (§10)     | none               |

The right-hand column is the argument. Everything a pure functional language needs — immutability,
structural sharing, call-by-need, an environment model, and an effect boundary — is already a
mechanism in the manifold, built for reasons that had nothing to do with functional programming.
What is new is a `std:fun` package of thunk/force/let combinators written in Vortex, and a front end
that parses ML-shaped text and emits the VM's existing opcodes. **No new opcode, no new dimension
outside `std:fun`'s own naming, no change to the arena.**

______________________________________________________________________

## 2. Purity over an append-only store: the reconciliation that comes first

This section comes before the grammar because it is the whole reason the language is interesting and
the one thing an implementer must get right. The operative rules, stated first:

- **Evaluation runs in an `ArenaManifold` and mints no `CompactOpNode`.** Every cons, closure, and
  forced thunk is a dense-vector slot that dies with the arena.
- **`value()`'s write branch targets only arena cells.** The sole persistent write in the language
  is a promotion performed by an `IO` action at the boundary (§6.5), never by an expression.
- **Effects are typed.** `read`/`save` have type `IO a`, not a pure type; the pure fragment is
  exactly the part with no `IO` in its type (§6.5). Referential transparency is a property of that
  fragment, proven below, not of the whole language by fiat.

Its three siblings each hit the same wall from a different side and each declared it a feature to be
lived with; vlace is the front end that walks *through* it for the pure fragment and marks the rest
with a type, and the door was already cut.

### 2.1 The spool is an immutable value; evaluation is a pure function

A `Store` is an append-only operations spool folded into a `Manifold`
([store-slice-convergence.md](store-slice-convergence.md)). "Append-only" is the exact property a
pure functional language asks of its values: **nothing that exists is ever mutated; new states are
new cells, and the old ones remain readable at their own addresses.** A `MicroversionId` names a
state of the whole store, and reading at an earlier one is a fold, not a copy. So a vlace program's
input is an immutable value *once a `MicroversionId` names it* — and that is the catch v1.1 fixes:
`read` takes a `MicroversionId`, not a filename (§6.5), so what a pure function receives is a frozen
snapshot. Acquiring the id — asking "what is the current version of `chapter.xanadoc`?" — is itself
an effect (`IO MicroversionId`), because the answer depends on when you ask; the author may hold
several parallel current versions at once (system-xanadocs §3.3). Separating the two is the whole of
why 1.0's `read "chapter.xanadoc"` was not referentially transparent and 1.1's `read chapterV` is.

Evaluation, then, is a pure function from one immutable manifold to a value. The claim this document
has to defend is that *the evaluation itself writes nothing observable* — because if forcing a thunk
or allocating a cons cell appended a `CompactOpNode` to the author's permascroll, then two
evaluations of the same expression would leave different spools, and that is precisely the
referential-transparency failure VPL §4.7 records. §2.2 is why that does not happen.

### 2.2 The two regimes are referential transparency and its absence

Convergence R8 draws two concrete types:

```cpp
class Manifold;      // ops-backed. Every mutation appends a CompactOpNode.
class ArenaManifold; // dense vectors only. No ops. Dies with its owner.
```

A vlace program evaluates in an `ArenaManifold`. Every cell it mints — every cons, every closure,
every forced thunk, every intermediate of a fold — is a dense-vector slot that **appends no
operation and has no microversion**. When the evaluation ends, the arena dies and takes all of it
with it. Evaluating a **pure** expression (one with no `IO` in its type) twice therefore leaves the
persistent store byte-identical and yields equal values, which *is* referential transparency,
delivered by the arena being an arena. The claim is scoped deliberately: it holds for the pure
fragment and for `render()` (which writes nothing); it does **not** hold across a `promote()`, which
appends operations and is not confluent (§6.5). "Pure" is the part of the language the type system
can prove has no `IO`, and that part is referentially transparent; the rest is honestly typed as
effectful.

This is the same ruling VPL §5's last paragraph reaches as a *recommendation* — "an interpreter
should run in an `ArenaManifold` by default and promote deliberately" — turned into vlace's defining
commitment rather than a library convention. VPL offers it as advice because an APL program is
imperative at heart and `A←...` genuinely assigns; vlace has no assignment, so the recommendation is
not a mitigation of a hazard the language still contains, it is the semantics. The persistent regime
(§2.1) is where the *input* lives and where a *promoted answer* goes; the arena is where evaluation
happens and where purity holds.

The symmetry with vicon and Vlog is exact: those run resolution in the arena and promote only an
answer, because a failed branch of a search does not deserve a name; vlace runs evaluation in the
arena and promotes only an answer, because an intermediate value does not deserve a name. "Does not
deserve a name" is what "pure" and "speculative" turn out to share.

### 2.3 Persistent data structures are the ops DAG and `d.clone`, unasked

A pure language cannot update a value in place, so every "modification" of a collection must produce
a new collection that shares as much as possible with the old one — persistent data structures, the
subject of Okasaki's book and the engine room of Clojure and Haskell. Two mechanisms the manifold
already has are exactly these:

- **The ops DAG is path copying.** Minting a successor to a microversion branches rather than
  overwrites (R8), and the two branches share every ancestor operation. A persistent-tree update
  that rewrites the spine from the changed leaf to the root, sharing every untouched subtree, is
  what minting a branch *is*. The structural sharing is not implemented; it is the shape of the
  store.
- **`d.clone` is hash-consing.** Several names reading one master through a clone rank (Vortex §5.2)
  is sharing by structure rather than by copy. Binding `x = y` so both read one value is one `link`,
  and it is the mechanism Vlog's variable aliasing already uses (Vlog §4.2). A vlace persistent map
  whose new version differs from the old in one key shares the other $n-1$ through clone ranks and
  **mints** $O(\log n)$ cells for the changed path.

Two costs that 1.0 conflated and this version keeps apart: *minting* the changed path is $O(\log n)$
cells, but *looking up* a key still selects a child at each node by walking a rank, which has no
ordinal index — so `lookup` is $O(n)$ until convergence U1 lands an index, exactly as Vlog's
first-argument clause indexing is gated on U1 (Vlog §6.3). The sharing is the substrate; the index
is not, and §11 records it as the fourth independent demand on U1.

§9.3 works a map update to the cell. The point here is that vlace asks for no persistent-collection
library: the collections it needs are the ones the manifold is made of.

______________________________________________________________________

## 3. The acceptance test: `link`, `value`, and nothing else

VQL, VPL, vicon, and Vlog are each held to the rule that every construct must lower to a finite
sequence of

```text
link(cell, ±dim, [target])                       ->  cell_id or nothing
value(cell, [offset], [length], [replacement])   ->  cell_id
```

and that an expression which cannot be written that way is a defect in the document rather than a
feature of the language. vlace is held to the same rule, with the sharpening that it may *not* use
`value()`'s write branch against anything but an arena cell — the write that lands on a persistent
cell is the one thing a pure language forbids itself, and §2.2 is why it never needs to. A `value()`
write in vlace always targets an `ArenaManifold` slot; the only persistent write in the whole
language is `promote()` at the boundary (§6.5), and `promote()` is not an expression.

> Every vlace construct lowers to `link`/`value` over an `ArenaManifold`, mints no `CompactOpNode`,
> and is referentially transparent as a consequence. A construct that needs a persistent write mid-
> evaluation, or a primitive beyond `link`/`value` plus the memoization island, is a defect in this
> document.

The rest of the document earns that claim.

______________________________________________________________________

## 4. What a vlace value is

### 4.1 A value is a cell; a constructor is a functor on the Inputs Wing

Every vlace value is a `CellRef`. A literal integer is a scalar cell (R6); a string is a cell whose
content is a span; a constructed value is a cell whose content is the constructor's name (its tag)
and whose fields hang off the Inputs Wing — first on `+d.grab`, the rest chained on `+d.step`. This
is the dual-wing calling convention (Vortex §3), reused as a term representation exactly as Vlog
reuses it for compound terms (Vlog §3.1): a two-field `Pair 1 2` is a cell content `"Pair"` with `1`
on `+d.grab` and `2` posward on `+d.step`. There is no separate boxed-value representation, because
a constructed value and a function call have the same shape — which is what makes `apply` and
`construct` the same walk.

### 4.2 Algebraic data types are a tag cell and an argument rank

`data Tree a = Leaf a | Node (Tree a) (Tree a)` declares two constructors. Each is a cell whose
content is the tag (`"Leaf"`, `"Node"`) and whose arity is the length of its Inputs Wing — zero for
a nullary constructor like `Nil`, which is then an atom in Vlog's exact sense (a cell with content
and no `+d.grab`). A value `Node (Leaf 1) (Leaf 2)` is a tree of cells, and it is the same tree the
ZigZag visualizer draws, because it *is* a ZigZag structure. Nothing about an ADT is a vlace
invention; it is Vlog's term, named by a `data` declaration instead of appearing in a clause head.

Pattern matching on a constructor (§5.4) reads the tag by comparing content and walks the Inputs
Wing to bind fields — which is the first half of `Vlog::unify()`, used for destructuring rather than
for logic.

### 4.3 A function is a cell; a closure is its captured environment rank

A function is a cell whose body is an instruction stream posward on `+d.spin` (Vortex's process
axis, the same axis a Vlog clause body uses). Its parameters are the Inputs Wing it reads. A
**closure** is that function cell plus a link to the `d.vars`/`d.values` scope it was defined in, so
a free variable resolves by walking the captured scope rather than the caller's — ordinary lexical
scope, built from the cursor-scope machinery Vortex §4 already defines. Partial application mints a
new function cell capturing the arguments supplied so far on its own Inputs Wing; currying is
therefore allocation of one small cell per applied argument, all in the arena.

### 4.4 A thunk is a cell with an unevaluated `+d.spin` body

Call-by-need wants a value that has not been computed yet but will be, at most once. That is a cell
whose `+d.spin` body is the suspended computation and whose `-d.grab` result slot is empty until
forced. Forcing (§6.2) runs the body, writes the result, and — crucially — the thunk is memoized by
the Vortex §6 pinned-island protocol (a *pinned island* is an ephemeral cache subgraph held alive by
a cursor on `d.pinning-cursors` and reclaimed by breaking one link — Vortex §6.1), so a second force
is a cache hit, not a recomputation. A thunk is therefore structurally a zero-argument function with
a result cache, which is exactly what call-by-need is in any implementation; here the result cache
is the memoization island the runtime already has.

### 4.5 Numbers, text, and the scalar cell

A vlace number is a scalar cell (convergence R6): canonical 64-bit bits in the node for arithmetic,
the shortest round-trip rendering spooled only when the value is promoted. Arithmetic reads the bits
(`#Add`, `#Lt`, … — the VM's existing arithmetic opcodes) and allocates no content; a program that
computes a million intermediate integers in the arena spools none of them. Text is a cell whose
content is a primedia span; a substring is `value(s, offset, length)`, an ephemeral quoting cell
carrying `ephemeralBit`, never a copied string — so vlace's strings have the same zero-copy slicing
vicon's scanning does (vicon §6.2), and the same R8 fence against weaving a scratch address into
persistent structure.

### 4.6 The numeric tower, `Bool`, characters, and literals

Hindley–Milner (§5.5) cannot be implemented without ground types and literal typings, so v1.1 fixes
them:

| type     | representation                                    | literal syntax                      |
| -------- | ------------------------------------------------- | ----------------------------------- |
| `Int`    | signed 64-bit two's complement in the scalar bits | `0`, `-7`, `0xFF`, `0o17`, `0b1010` |
| `Float`  | IEEE-754 binary64 in the scalar bits (R6)         | `3.14`, `1.0e-9`, `6.022e23`        |
| `Bool`   | prelude ADT `data Bool = False \| True`           | `True`, `False`                     |
| `Char`   | a Unicode scalar value in the scalar bits         | `'a'`, `'\n'`, `'\u00e9'`           |
| `String` | a cell whose content is a primedia span (§4.5)    | `"hello"`, `"with \"escapes\""`     |

`Int` and `Float` do **not** implicitly convert; `1 + 2.0` is a type error, and `toFloat`/`round`/
`truncate` (prelude, §7.4) convert explicitly — the ML discipline, not the Lisp one. There is one
`Int` width (64-bit, wrapping on overflow — a defined value, not UB, per §6.6); arbitrary-precision
integers are a later-version prelude module, not a core type. `Bool` is an ordinary ADT, which is
why `if` lowers to a two-alt `case` on it (§5.6) rather than being primitive. A `Char` is a scalar
cell distinct from a one-character `String`, as in Haskell. Numeric and comparison operators map to
VM opcodes per the table in §5.1.

______________________________________________________________________

## 5. The language

### 5.1 Lexical structure, keywords, operators, and the EBNF grammar

vlace is written in ASCII identifiers and keywords, layout-insensitive, with `;` separating bindings
and `--` beginning a line comment. A `let`/`letrec` is closed by its `in`; a `case` alt list and a
binding list are `;`-separated; there is no indentation rule.

**Keywords** (reserved, not usable as identifiers): `let`, `letrec`, `in`, `if`, `then`, `else`,
`case`, `of`, `fn`, `data`, `type`, `import`, `as`. **Reserved symbols**: `=`, `=>`, `|`, `_`, `!`,
`.`, `,`, `;`, `(`, `)`, `[`, `]`, `{`, `}`, `::`, `->`, `@`.

**Operators** are a lexical class of their own (sequences of the symbol characters
`+ - * / < > = ! & | : . ^ %`), parsed by precedence climbing. The built-in fixity table — a prefix
program may not redefine these levels, though §5.8 lets a module declare new operators with
`infixl`/ `infixr`/`infix`:

| level | operators                   | fixity | lowering                                              |
| ----- | --------------------------- | ------ | ----------------------------------------------------- |
| 9     | `.`                         | right  | `std:fun/compose`                                     |
| 7     | `*` `/` `%`                 | left   | `#Mul` `#Div` `#Mod`                                  |
| 6     | `+` `-`                     | left   | `#Add` `#Sub`                                         |
| 5     | `:` `++`                    | right  | `cons` (the list constructor); `std:fun/append`       |
| 4     | `==` `/=` `<` `<=` `>` `>=` | none   | `#Eq` `#Neq` `#Lt` `#Lte` `#Gt` `#Gte`                |
| 3     | `&&`                        | right  | short-circuit `case` on `Bool` (§5.6)                 |
| 2     | `\|\|`                      | right  | short-circuit `case` on `Bool` (§5.6)                 |
| 1     | `>>=` `>>`                  | left   | `std:fun/bind`, `std:fun/then` (the `IO` monad, §6.5) |
| 0     | `$`                         | right  | low-precedence application (`f $ x` = `f x`)          |

A parenthesised operator is a function (`(+)  :  Int -> Int -> Int`); a backtick turns a function
into an infix operator (`` x `div` y ``). The **list type is an ordinary ADT**,
`data List a = Nil | Cons a (List a)`, with `[]` sugar for `Nil`, `(:)` the infix spelling of
`Cons`, and `[a, b, c]` sugar for `a : b : c : []`; the cons *pattern* `(h : t)` and the
list-literal pattern `[a, b]` desugar the same way (§5.6). This is why §9.1's `(h : t)` both parses
and lowers.

```ebnf
program     = { decl , ";" } , expr ;

decl        = value_decl | data_decl | type_decl | import_decl | fixity_decl ;
value_decl  = ( identifier | "(" , operator , ")" ) , { pattern } , "=" , expr ;
data_decl   = "data" , constructor , { identifier } , "=" , ctor_def ,
              { "|" , ctor_def } ;
ctor_def    = constructor , { atype }                       (* positional *)
            | constructor , "{" , field , { "," , field } , "}" ;  (* record, §5.7 *)
field       = identifier , "::" , type ;
type_decl   = "type" , constructor , { identifier } , "=" , type ;  (* alias *)
import_decl = "import" , module_path , [ "as" , identifier ]
            , [ "(" , import_name , { "," , import_name } , ")" ] ;  (* §5.8 *)
fixity_decl = ( "infixl" | "infixr" | "infix" ) , integer , operator ;

expr        = "let" , bindings , "in" , expr
            | "letrec" , bindings , "in" , expr
            | "if" , expr , "then" , expr , "else" , expr
            | "case" , expr , "of" , alt , { ";" , alt }
            | "fn" , pattern , { pattern } , "=>" , expr
            | opexpr ;
opexpr      = opexpr , operator , opexpr                    (* resolved by the fixity table *)
            | app ;
bindings    = binding , { ";" , binding } ;
binding     = pattern , "=" , expr ;
alt         = pattern , [ "if" , expr ] , "=>" , expr ;     (* optional guard, §5.4 *)

app         = app , aexpr | aexpr ;                         (* left-assoc application *)
aexpr       = qidentifier | constructor | literal
            | "(" , operator , ")"                          (* operator as function *)
            | "(" , expr , ")"
            | "(" , expr , "," , expr , { "," , expr } , ")"   (* tuple, >= 2 fields *)
            | "[" , [ expr , { "," , expr } ] , "]"            (* list literal *)
            | aexpr , "{" , fieldset , "}" ;                   (* record update, §5.7 *)
fieldset    = identifier , "=" , expr , { "," , identifier , "=" , expr } ;

pattern     = "_" | identifier | literal
            | constructor , { pattern }
            | constructor , "{" , identifier , "=" , pattern ,
                            { "," , identifier , "=" , pattern } , "}"  (* record pat *)
            | pattern , ":" , pattern                       (* cons pattern *)
            | "(" , pattern , "," , pattern , { "," , pattern } , ")"
            | "[" , [ pattern , { "," , pattern } ] , "]" ;

qidentifier = [ identifier , "." ] , identifier ;           (* Module.name, §5.8 *)
literal     = integer | float | string | character ;
type        = atype , { "->" , type } ;
atype       = identifier | constructor | "(" , type , ")" | "[" , type , "]" ;
module_path = string ;                                      (* e.g. "std:fun", "mine:util" *)
import_name = identifier | constructor | "(" , operator , ")" ;
identifier  = (* lowercase-initial *) ;
constructor = (* uppercase-initial *) ;
operator    = (* a maximal run of + - * / < > = ! & | : . ^ %, not a reserved symbol *) ;
```

The grammar is now unambiguous on the two places 1.0 was not: a parenthesised single expression
`(e)` is grouping, and a tuple requires at least one comma (there are no 1-tuples); application
binds tighter than every operator, and operators resolve by the fixity table above. Every production
has a lowering, named in §5.2–§5.8 and collected in the §5.6 table.

### 5.2 `let`, `letrec`, and lexical scope as a `d.vars` rank

`let x = e in body` mints a name cell `x` on the current cursor's `d.vars` rank, links `e`'s value
(a thunk, by default — §6.1) on its `+d.values`, and evaluates `body` in the extended scope. This is
Vortex §4's associative scope used verbatim: a name on `d.vars`, its binding on `d.values`.
`letrec`/mutually-recursive `let` differ by one thing — the names are minted on `d.vars` *before*
their right-hand sides are built, so a right-hand side can reference them, and because the bindings
are thunks (unforced), the forward reference is not yet a cycle to be computed, only a cell to be
pointed at. A recursive `letrec fib = fn n => ...` is a thunk whose body mentions a `d.vars` cell
that already exists.

Because a binding is a thunk on `d.values` and not an assignment, there is no mutation and no order
dependence beyond data dependence; `let` is sugar over the `std:fun/let` combinator (§7.2), which is
itself a `link` onto `d.vars` and a `link` onto `d.values`.

### 5.3 Lambda, application, and the dual-wing calling convention

`fn x => e` mints a function cell (§4.3) whose body is `e` compiled onto `+d.spin`, capturing the
current scope. Application `f a` places `a` on `f`'s copy's Inputs Wing (`+d.grab`) and runs the
body, whose result lands on `-d.grab` — the dual-wing call Vortex §3 defines and the VM's `#Call`
opcode performs. Multi-argument application `f a b` chains `a`, `b` along `+d.step` on the Inputs
Wing; a partially applied `f a` mints a closure capturing `a` (§4.3). No calling convention is
invented: vlace functions are called the way every Vortex opcode is.

### 5.4 Pattern matching is destructuring unification, forced to WHNF

`case e of P => body ; ...` tries each alt in order. Matching a pattern against a scrutinee **forces
the scrutinee to weak head normal form one constructor layer at a time** (§6.7) and compares: a
constructor pattern forces the scrutinee to its outermost constructor, compares the tag (content),
and recurses into the Inputs Wing only as far as the sub-patterns demand; a literal pattern forces
and compares scalar bits or content; `_` matches anything and forces nothing; a variable pattern
binds. This is *destructuring*, not full unification — it never binds a variable *in the scrutinee*,
only in the pattern — so the direction is fixed and the two hazards full unification carries do not
arise the way a naive `Vlog::unify()` call would invite:

- **Variable patterns are linear and never mutually unified.** A pattern binds each of its variables
  to a distinct fresh name; a *repeated* binder — `case p of (x, x) => …` — is a **compile error**
  (`CompileError::NonLinearPattern`, §8.2), not Prolog's "succeed iff equal". Equality tests are
  written with a guard (below), never by rebinding. This is the ML/Haskell rule, chosen so that
  matching cannot accidentally become unification.
- **Matching is cycle-bounded.** A scrutinee may be a cyclic or infinite value (a `letrec` stream,
  §9.2); forcing to WHNF one layer at a time terminates against such a value because it never forces
  past what a sub-pattern demands, and the one traversal that could loop — comparing two constructor
  terms structurally — uses the cycle-bounded walk `Manifold::cloneMaster()` already provides (Vlog
  §4.4). vlace does **not** build rational-tree cycles the way Vlog's `=/2` does, because
  destructuring never writes into the scrutinee; the occurs-check question (Vlog §4.4) is therefore
  moot for matching.

A **guard** `P if g => body` matches `P`, then evaluates `g : Bool`; if `False`, the alt fails and
control falls through to the next alt as if `P` had not matched. The first alt whose pattern matches
*and* whose guard holds wins, left to right (ML first-fit, not committed choice).

The subtlety a pure language needs: **a failed match (or a failed guard) must leave no bindings
behind.** The compiler brackets each attempt with the arena's `mark()`/`release()` (Vlog §5.2): a
partial destructuring that then fails is truncated away, so no binding from a non-matching alt
escapes into the next. Exhaustiveness is checked statically (§5.5); what a *runtime* non-match of
the last alt does — only reachable with guards or the quarantined dynamic mode — is defined in §6.6,
not left to chance.

### 5.5 Data declarations, Hindley–Milner, and let-generalization

`data` introduces constructors (§4.2) and, at runtime, nothing beyond their tag cells. vlace is
**statically typed with Hindley–Milner inference, and the type checker is normative** — a conforming
vlace rejects an ill-typed program before evaluation. (v1.0 offered an optional "run dynamically
typed" mode; v1.1 removes it from the conformance surface, because a dynamic regime voids
parametricity and therefore the free theorems the `std:functional` fusion of §7.3 relies on. An
implementation may provide an unchecked debug mode, but a program run that way is not "vlace" and
may trap at `case` time per §6.6.) The type system is erased before lowering: the manifold sees tag
cells and argument ranks, not types.

**Types** are written with the `type` grammar of §5.1: type variables are lowercase (`a`),
constructors uppercase (`Int`, `List a`), functions `a -> b`, and inference is let-generalized. A
`data Tree a = …` introduces a type constructor `Tree` of arity 1 and its data constructors as
functions into it.

**Let-generalization under laziness.** HM generalizes a `let`-bound definition's inferred type over
the variables not free in the environment. The value restriction exists because *mutable references
and exceptions* can observe a generalized type at two instantiations unsoundly — and vlace's pure
fragment has neither: there is no `ref`, and effects are confined to the `IO` type (§6.5), which is
not generalizable past its `IO` constructor. Therefore **vlace generalizes every `let`/`letrec`
binding freely**, and this is sound as a theorem, not an assertion: the only constructs that would
break it are absent from the typed language. The one caveat is the boundary — a binding whose type
is `IO a` is generalized at `a` only where `a` is itself generalizable, exactly as Haskell treats
`IORef`-free `IO`. Laziness is orthogonal: it changes *when* a binding is forced, not *whether* its
type may be generalized, and §5.2's "the forward reference is only a cell to point at" is about
evaluation order, which this rule does not depend on.

**Exhaustiveness.** The checker verifies that every `case` covers its scrutinee's constructors; a
non-exhaustive `case` without a catch-all is rejected as `CompileError::NonExhaustiveMatch` (§8.2).
Guards (§5.4) make exhaustiveness undecidable in general, so a `case` with guards must end in an
unguarded catch-all (`_ => …`) or be rejected; this keeps the guarantee that a *well-typed* program
never reaches §6.6's partial-match trap except through the quarantined unchecked mode.

Typeclasses/traits are out of scope for 1.1; §11 records why (dictionary passing is more front end
than this version specifies — the manifold would carry a dictionary as an ordinary record cell,
§5.7).

### 5.6 The production→lowering table

§5.1 promises every production has a lowering. Here they are in one place, each citing where the
mechanism is defined. "Lowers to" means a finite sequence of `link`/`value` over the arena (§3).

| production                | lowers to                                                               | ref        |
| ------------------------- | ----------------------------------------------------------------------- | ---------- |
| `value_decl` `f p… = e`   | `f = fn p… => e`; bind `f` on `d.vars` as a thunk                       | §5.2, §5.3 |
| `data_decl`               | one tag-cell template per constructor; a field rank for records         | §4.2, §5.7 |
| `type_decl`               | nothing at runtime (an alias, erased with the types)                    | §5.5       |
| `import_decl`             | resolve a module cell on `d.stdlib`/`d.modules`; bind qualified names   | §5.8       |
| `fixity_decl`             | nothing at runtime (parser-table entry)                                 | §5.1       |
| `let` / `letrec`          | mint names on `d.vars`, bind thunks on `d.values`, evaluate body        | §5.2       |
| `if c then t else e`      | `case c of True => t ; False => e` (Bool is an ADT, §4.6)               | §5.6, §4.6 |
| `case e of …`             | marked destructuring per alt, first-fit with guards                     | §5.4       |
| `fn p… => e`              | a function cell; body on `+d.spin`; captures the scope                  | §5.3       |
| `opexpr a ⊕ b`            | application of `⊕`'s lowering (fixity table, §5.1) to `a`, `b`          | §5.1       |
| `app f a`                 | dual-wing `#Call`: `a` on `+d.grab`, result on `-d.grab`                | §5.3       |
| `(e)`                     | `e` (grouping)                                                          | §5.1       |
| `(a, b, …)` tuple         | a `Tuple`-tagged cell, fields on the Inputs Wing                        | §4.1       |
| `[a, b, …]` list          | `a : b : … : []` — `Cons`/`Nil` cells (§5.1)                            | §5.1       |
| `C { f = e, … }` record   | a `C` cell with fields placed by the field rank                         | §5.7       |
| `r { f = e }` update      | copy `r`'s constructor, replace field `f`, share the rest via `d.clone` | §5.7, §2.3 |
| cons / list / record pats | the inverse of the above, as WHNF-forcing destructuring                 | §5.4       |

### 5.7 Records: named fields and functional update

A record is a `data` declaration with named fields: `data Point = Point { x :: Int, y :: Int }`. It
is an ordinary constructor cell (§4.2) with one addition — the constructor carries a **field-name
rank** (a `d.vars` rank off the constructor template, naming `x`, `y` in Inputs-Wing order), so a
field name resolves to an Inputs-Wing position. This is `std:fun/record` (§7.2): construction
`Point { x = 1, y = 2 }` places fields by name rather than position; a field accessor `x p` is
`std:fun/field` indexed by the name rank; **functional update** `p { y = 5 }` copies `p`'s
constructor cell, replaces the `y` field, and shares every other field with `p` through a `d.clone`
link (§2.3) — one changed field costs one new cell, not a copy of the record. Record patterns
`Point { x = px, y = py }` destructure by name (§5.4). Records are erased to positional tag cells at
lowering; the field names live only in the constructor template and the type checker, so a record
and a positional `data` of the same shape have the same runtime representation.

### 5.8 Modules: user-facing `import` and qualified names

A vlace program imports other modules — the `std:*` library and user modules alike — with
`import "std:fun"`, optionally aliased (`import "std:collections" as C`) or selective
(`import "std:fun" (map, fold, (.))`). An unqualified import brings a module's exported names into
scope; an aliased import makes them available qualified (`C.insert`); a selective import brings only
the named symbols. Resolution is the `d.stdlib` walk of §7.1 for library modules and a `d.modules`
rank off `home` for user modules, which a program defines by promoting a module cell whose exports
hang on `+d.vars` — the same shape `std:fun` has, so a user module and a library module are
indistinguishable to the resolver. A qualified name `M.f` (grammar `qidentifier`, §5.1) walks the
module's export rank for `f`. This is the one piece §10's "a vlace program can call a VQL query"
needs that 1.0 left implicit: calling a sibling is `import "vql"` (a module whose exports are the
query combinators), and the call runs in vlace's arena (§3), so a sibling invoked from vlace never
writes persistently behind the type system's back.

______________________________________________________________________

## 6. Evaluation

### 6.1 Lazy by default; strictness is an annotation

vlace is **call-by-need**: a binding and a function argument are thunks, forced only when a `case`
scrutinee, an arithmetic operand, or the top-level result demands a value. This is the Haskell
default rather than the ML one, and it is chosen because the manifold makes it cheap in the one way
it is usually expensive — the thunk's memoization is the runtime's existing pinned island (§6.3),
not a per-thunk heap cell with a mutable indirection. A `!` prefix on a binding or a constructor
field (`data T = C !Int`) forces strictly, for the usual space-leak reasons; strictness is an
annotation on an otherwise lazy language, as in Haskell.

### 6.2 Laziness is a thunk; forcing is running a `+d.spin` stream

A thunk (§4.4) is a cell with a suspended `+d.spin` body and an empty result slot. `force` checks
the result slot; if empty, it runs the body — which is exactly what the VM does to any `+d.spin`
instruction stream — writes the single result on `-d.grab`, and returns it. The write is to an arena
cell, so it mints no operation (§2.2); a forced thunk is still a referentially transparent value,
because forcing changed only arena scratch that dies with the evaluation. `force` is idempotent by
construction: the second call finds the slot full. Both `thunk` and `force` are `std:fun` routines
(§7.2), each a handful of `link`/`value` calls.

### 6.3 Memoization is Vortex §6, and the memo key is thunk identity

Call-by-need's whole value over call-by-name is that a thunk is forced at most once, and that
requires a memo. vlace reuses Vortex §6's pinned-island *mechanism* — a `d.cache` rank anchored to
`d.pinning-cursors`, reclaimed by breaking one link — but **not** its argument-comparison *keying*.
Vortex §6.2 keys a cache hit by comparing an opcode's input operands on `+d.grab`; a thunk takes no
arguments, so there is nothing to compare. **The normative key is thunk cell identity**: the island
holds at most one entry per thunk cell, found by the thunk's own ref, not by an argument scan. This
is a thin specialization of the §6 island (one entry, keyed by the entry's owning cell) rather than
a new runtime facility — the `d.cache` structure and its GC are unchanged; only the lookup is "this
thunk's slot" instead of "the entry whose `+d.grab` matches." (The §1 table's "none — Vortex §6"
means no new *arena/VM machinery*; the keying is a `std:fun`-level convention, §7.2, not a C++
change — §8.1.) Identity keying is the standard call-by-need semantics: *this* thunk is forced once
and its result reused wherever *this* thunk is shared, sound because a thunk suspends a pure
computation (effects are `IO`, which forces to an *action*, not to its performance — §6.5, so a
thunk is never a suspended effect). The first force is a cache miss that runs the body and records
the result; every later force of the same thunk is a cache hit.

Identity keying is **intensional**: two syntactically equal thunks `e` and `e` are distinct cells
with distinct cache entries, so equal expressions may do equal work twice without sharing it. That
does not break referential transparency — both yield the same value — but it means the *cost model*
and the reduction graph (§10) are intensional, which §10 now states outright. Common-sub-expression
elimination, which *would* share equal expressions by keying on structure rather than identity, is a
**non-normative optimization**: an implementation may key by a structural hash of the suspended
expression and its captured-environment *values*, but only in the arena regime where the extra
sharing is unobservable, and never in a way that a program could detect. The two keyings are not
interchangeable, and v1.1 names identity as the one the semantics depend on.

Flushing a memo (reclaiming space under memory pressure) is Vortex §6.3's one `link` that severs the
cache head and lets topological GC reclaim the island. Eviction-then-recompute is safe precisely
because the thunk is pure: recomputing yields the same value. (This is why §6.5 forbids thunking an
`IO` action's *performance*: an evicted-then-reforced effect would run twice. A thunk of type `IO a`
forces to the action value, not to its effect, so eviction is still safe.)

### 6.4 Recursion, sharing, and why the spool does not grow

A recursive function is a thunk referencing its own `d.vars` cell (§5.2); it does not expand into
anything persistent, because the recursion lives in the arena. A program that builds a
million-element list builds a million arena cells and spools none of them. Sharing — two parts of a
program referring to one value — is one cell reachable by two links, the ordinary ZigZag situation;
the value is computed once (memoized, §6.3) and read from both. The append-only persistent spool is
touched by exactly one operation per promoted answer (§6.5) and by nothing else, so the headline
worry about functional programming on an append-only store — *every allocation is a permanent write*
— is answered by every allocation being an arena write instead.

### 6.5 Effects: the typed `IO` boundary and promotion

A vlace program's pure fragment computes values; its effects — reading a document, saving a result —
are **typed**, not positional. v1.0 claimed "IO at the edge as a property of where the arena ends
rather than a monad," and a review called that backwards: a monad is *what buys* equational
reasoning about effects, and dropping it keeps the benefit (a pure core) while discarding the cost
that purchases reasoning. v1.1 adopts the monad.

`IO a` is an ordinary ADT (the manifold carries it as a record cell, §5.7, exactly as §5.5 says a
dictionary would) with the usual operations, lowered to `link`/`value` like everything else:

```ocaml
pure  :: a -> IO a
(>>=) :: IO a -> (a -> IO b) -> IO b      -- std:fun/bind
(>>)  :: IO a -> IO b -> IO b             -- std:fun/then
```

`IO a` is a *description* of an effect, a value a pure expression may build and pass around freely;
performing it happens only at the program's boundary, when the driver runs the top-level `IO` action
(§8.2). The effectful primitives are typed accordingly:

```ocaml
currentVersion :: Key -> IO MicroversionId   -- "what is the head of this doc now?" — an effect
read           :: MicroversionId -> Doc       -- PURE: a frozen snapshot has one value (§2.1)
save           :: Key -> a -> IO ()           -- promote the value under the current head
getLine        :: IO String                   -- console input (§6.6, §7.4)
putStr         :: String -> IO ()             -- console output
```

The split that fixes 1.0's unsoundness is `currentVersion`/`read`: asking *which* version is current
is an effect (the answer changes as the store advances, and an author holds several heads at once —
system-xanadocs §3.3), but reading *a named microversion* is pure, because a frozen snapshot has
exactly one value. So `read v` where `v : MicroversionId` is referentially transparent, and the
non-transparent part is quarantined in `IO`. `save` performs `promote()`.

**Promotion (`promote()`, convergence V3, built).** `save` compacts the value's reachable subgraph
into the persistent `Manifold`, writing one run of operations and rewriting ephemeral scratch spans
into real permascroll addresses (vicon §9.3, Vlog §5.5). Two properties v1.0 got wrong and v1.1
states correctly:

- **Promotion preserves `d.clone` sharing.** The §2.3 / §9.3 claim that a persistent-map update
  "copies nothing" is only true at the boundary if promotion detects shared clone-rank subgraphs and
  emits them once, linking both roots, rather than re-minting each shared subtree as fresh ops. v1.1
  **requires** `promote()` to share: promoting `m1` (which shares $n-1$ subtrees with `m0`) must
  cost $O(\log n)$ persistent ops, not $O(n)$. (The built `promote()` "works by mapping refs"; the
  mapping must be many-to-one on a shared master. This is a requirement v1.1 places on the promotion
  pass, not a property it assumes.)
- **Promotion is parent-relative and not confluent.** `save` takes the current head as the parent it
  extends (the built `promote()` already carries an explicit parent microversion). Promoting `a`
  then `b` versus `b` then `a` yields different spools and different `GlobalOpRef`s. So the
  "evaluate twice → byte-identical store" guarantee (§2.2) is scoped to the **pure fragment and
  `render()`**; across a `save` the store changes by construction, and the order of two `save`s
  matters. This is not a defect — it is what "the one effect" means — but §2.2's RT claim must not
  be read as covering it.

`render()` (bits and spans to characters, `Version::materialize()`'s crossing) is the lighter
boundary and writes nothing, so a REPL printing a value performs no effect; only running an `IO`
action that reaches `save` promotes. Equational reasoning about effectful programs is now available:
`m >>= f` obeys the monad laws because `(>>=)` is an ordinary function over the `IO` ADT, and `save`
appears in a program's *type* (`… -> IO ()`), so the checker — and the reader — can tell an
effectful program from a pure one, which v1.0's untyped `save`-in-a-`let` could not.

### 6.6 The runtime error model

vlace separates three failure modes, and v1.1 states each — v1.0's "expressions never fail, only
diverge" slogan was false the moment `read`/`save`/division existed.

- **Recoverable absence and failure are values, not control flow.** The prelude (§7.4) provides
  `data Option a = None | Some a` and `data Result e a = Err e | Ok a`. Partial operations return
  them: `lookup : k -> Map k v -> Option v`, `head : List a -> Option a`,
  `toInt : String -> Option Int`. There is no exception for "not found"; the type says it can be
  absent, and the caller must `case` on it. This is the ML/Haskell discipline and it is why §9.3's
  `lookup` returns `Option v`, not a bare value.
- **Arithmetic faults are `Result`, not traps, where a type can carry them.** Integer division or
  modulo by zero is not a silently-wrong value and not undefined behaviour: `div`/`mod` return
  `Result ArithError Int` (`Err DivByZero`), and the infix `/` on `Int` is sugar that requires the
  caller to handle it, or uses the checked-at-type partial `unsafeDiv : Int -> Int -> Int` whose
  divide-by-zero is a defined divergence (§6.7's budget trip), never UB. `Int` overflow wraps (two's
  complement, §4.6) — a defined value, documented, not a fault.
- **Genuinely partial situations trap into a defined divergence.** A `case` whose last alt fails at
  runtime (reachable only through a guard or the quarantined unchecked mode, since the checker
  proves exhaustiveness otherwise, §5.5), or `unsafeDiv _ 0`, does not corrupt state or return a
  wrong value: it raises an internal `MatchFail`/`ArithFault` that the driver reports as a bounded
  divergence (`evaluate()` returns `nullopt`, §8.2), with the arena truncated by the enclosing
  `release()` so no partial write escapes. Promotion never persists such a state because the answer
  is forced before `save` (§6.7) and a trapped force reaches the driver, not the store.
- **I/O failure is `IO (Result …)`.** `read`'s snapshot is total once the `MicroversionId` is in
  hand, but acquiring one can fail (no such document):
  `currentVersion : Key -> IO (Result IOError MicroversionId)`. A missing file, a permission error,
  a swarm timeout are `Err` values threaded through `IO`, exactly as Haskell's
  `IO (Either IOError a)` would. There is no out-of-band exception channel; failure that a program
  should handle is a value in its type.

So "divergence, not failure" (§11) is now precise: a *well-typed, total* use of the language either
produces a value or diverges, and every *handleable* failure is reflected into
`Option`/`Result`/`IO` rather than being a hidden trap.

### 6.7 WHNF, blackholing, and proper tail calls

Three operational points v1.0 left implicit and an implementer cannot:

- **WHNF is defined per cell kind.** A value is in weak head normal form when its outermost shape is
  known without forcing its components: a constructor cell (tag known, Inputs-Wing fields may be
  unforced thunks); a function/closure cell; a scalar cell carrying R6 bits (WHNF does *not* force
  the deferred rendering — that is spooled only at promotion, §4.5, so forcing to WHNF still
  allocates no content); a quoting cell. `seq a b` forces `a` to WHNF (this shape, not its interior)
  and returns `b`; `!` on a binding or field forces to WHNF at bind time (§6.1). `evaluate()` (§8.2)
  forces the result to WHNF; `show`/`render` fully force (and so may diverge or loop on a cyclic
  value, which the renderer must bound with a depth limit — §9.2's stream is infinite).
- **Thunks are three-state and blackholed.** A thunk is *unevaluated*, *under evaluation*, or
  *forced*. `force` on an unevaluated thunk sets it *under evaluation* (blackholes it) before
  running the body; a re-entrant force of a thunk already *under evaluation* — `let x = x in x`, a
  self-demanding `letrec` — is a defined divergence (`<<loop>>`, a budget trip, §8.2), not an
  infinite silent recursion. v1.0's "`force` is idempotent by construction" is true only *after* the
  first force completes; blackholing is what makes the in-progress case defined. A blackholed thunk
  truncated by a `release()` (a failed match abandoned mid-force) reverts to *unevaluated* with the
  rest of the arena, leaving no dangling marker.
- **Tail calls reuse the frame.** A function application in tail position lowers to a `#Call` that
  **reuses the caller's `d.stack` frame** rather than pushing a new one, so a tail-recursive loop
  runs in constant stack space. Without this a strict
  `letrec loop = fn n acc => if n == 0 then acc else loop (n - 1) (acc + n)` would push a million
  frames and trip the budget on a constant-space problem; with it, it is a loop. The compiler (§8.2)
  identifies tail positions (the body of a `fn`, the arms of a tail `if`/`case`, the `in` of a
  `let`) and emits a frame-reusing `#Call`; a tail call retains no `mark()`, so the arena does not
  grow across the loop either. This is the single property that decides whether non-trivial
  recursion runs in bounded space, and v1.1 makes it normative.

______________________________________________________________________

## 7. The `std:fun` standard-library package

### 7.1 Package anchoring and exported symbols

`std:fun` is a module on the `d.stdlib` rank off `home`, beside the seven the Vortex spec §7.2
lists, the `std:logic` Vlog §8.3 adds, and the `std:goal` vicon adds. It is anchored and resolved
exactly as they are (Vortex §7.1): the module is a cell on `+d.stdlib` whose content is `"std:fun"`,
its exported symbols extend posward on `+d.vars`, and each symbol's entry opcode hangs on
`+d.values`. Resolving `import("std:fun/force")` walks
`home/d.stdlib[./d.name = "std:fun"]/d.vars[./d.name = "force"]/d.values` — the same walk
`VortexStdLib::resolve()` already performs for `std:math/abs` (verified against
`vortex_stdlib.hpp`). Bootstrapping adds one line to `VortexStdLib::bootstrap()`: weave `std:fun`
after `std:functional`, whose combinators it builds on.

### 7.2 What `std:fun` adds, written in Vortex

Every `std:fun` symbol is a Vortex routine — an opcode stream on `+d.spin` — assembled by the
single-primitive invariant, exactly as the core modules and `std:logic` are (Vortex §7.3). The
package is deliberately thin, because §7.3's reuse carries most of the weight. It exports:

- **Evaluation discipline**: `thunk` (suspend an expression as a `+d.spin` body, §4.4), `force` (run
  it once and memoize, §6.2), `seq` (force the first argument, return the second — Haskell's `seq`),
  `strict` (the `!`-annotation lowering).
- **Binding**: `let`, `letrec` (mint on `d.vars`, bind a thunk on `d.values`, §5.2), `fix` (the
  fixed-point combinator, for anonymous recursion).
- **Application**: `apply`, `curry`, `uncurry`, `compose` (`.`), `flip`, `const`, `id` — the
  combinator basis, each a few `link`s on the Inputs Wing.
- **Pattern/ADT support**: `match` (the `case`-alt driver that marks, unifies for destructuring, and
  releases on no-match, §5.4), `tag` (read a cell's constructor tag), `field` (a field by name or
  Inputs-Wing position), `record` (construct and functionally update by field name, §5.7).
- **Effects**: `io` (the `IO` monad — `pure`, `(>>=)`, `(>>)`, and the primitives `read`/`save`/
  `currentVersion`/`getLine`/`putStr`, §6.5), `show` (render any value to `String`, §6.7).
- **Lazy lists / streams**: `cons`, `head`, `tail`, `take`, `drop`, `repeat`, `iterate`, `unfold` —
  built on `std:collections`' ranks but with lazy tails (a tail is a thunk), which is what makes
  §9.2's infinite stream finite to take from.

### 7.3 What is reused, unchanged, from the core modules

- **`std:functional`** (Vortex §7.2) supplies `map`, `filter`, `fold`, `zip` directly — these *are*
  vlace's higher-order list functions; `std:fun` adds only their lazy-tail variants.
- **`std:collections`** supplies the persistent sequence, map, and set whose structural sharing is
  §2.3's `d.clone` and ops-DAG mechanisms; vlace's `[...]` list literal and map update lower to
  these.
- **`std:math`** supplies `abs`, `min`, `max`, `clamp`, `gcd`, `pow`, `sqrt` under their own names.
- **`std:string`** supplies `split`, `join`, `starts_with`, `ends_with`, case folding, `regex_match`
  — vlace's string functions, built on `value()` splices so they never copy (§4.5).
- **`std:logic`** supplies the `unify` that §5.4's pattern matching drives for destructuring, and
  the `mark`/`release` discipline that makes a failed match leave no bindings.

The rule from AGENTS.md — *new Vortex standard-library code reuses existing standard-library
functions unless it cannot* — is why §7.2 is short. The only genuinely new routines are the
evaluation discipline (`thunk`, `force`, `seq`), the binding and combinator basis, the record and
`IO` support, and the lazy-list tails. Everything else delegates.

### 7.4 The prelude: names in scope in a fresh program

A fresh vlace program opens with the prelude imported — these names need no `import`. The table is
the single enumeration an implementer and a programmer code against; each name cites the module that
exports it (§7.1–§7.3) or the prelude ADT it belongs to. Signatures use the types of §4.6/§5.5.

| name                                                                 | signature                                              | module / source               |
| -------------------------------------------------------------------- | ------------------------------------------------------ | ----------------------------- |
| `Bool`, `True`, `False`                                              | `data Bool = False \| True`                            | prelude ADT (§4.6)            |
| `Option`, `None`, `Some`                                             | `data Option a = None \| Some a`                       | prelude ADT (§6.6)            |
| `Result`, `Err`, `Ok`                                                | `data Result e a = Err e \| Ok a`                      | prelude ADT (§6.6)            |
| `List`, `[]`, `(:)`                                                  | `data List a = Nil \| Cons a (List a)`                 | prelude ADT (§5.1)            |
| `IO`, `pure`, `(>>=)`, `(>>)`                                        | the `IO` monad (§6.5)                                  | `std:fun/io`                  |
| `id`, `const`, `flip`, `(.)`, `($)`                                  | combinator basis                                       | `std:fun`                     |
| `fst`, `snd`                                                         | `(a, b) -> a` / `-> b`                                 | `std:fun`                     |
| `map`, `filter`, `foldr`, `foldl`, `zip`                             | over `List`                                            | `std:functional`              |
| `cons`, `head`, `tail`, `take`, `drop`, `repeat`, `iterate`          | lazy lists                                             | `std:fun` (§7.2)              |
| `length`, `reverse`, `append`/`(++)`                                 | over `List`                                            | `std:fun` + `std:collections` |
| `fromList`, `insert`, `lookup`, `delete`, `keys`, `values`, `toList` | `Map k v`                                              | `std:collections`             |
| `emptyMap`                                                           | `Map k v`                                              | `std:collections`             |
| `insertWith`                                                         | `(v -> v -> v) -> k -> v -> Map k v -> Map k v`        | `std:collections`             |
| `abs`, `min`, `max`, `clamp`, `gcd`, `pow`, `sqrt`                   | numeric                                                | `std:math`                    |
| `div`, `mod`                                                         | `Int -> Int -> Result ArithError Int`                  | `std:math` (§6.6)             |
| `toFloat`, `round`, `truncate`                                       | `Int`↔`Float` (§4.6)                                   | `std:math`                    |
| `words`, `lines`, `split`, `join`, `startsWith`, `endsWith`          | over `String`                                          | `std:string`                  |
| `toUpper`, `toLower`, `regexMatch`                                   | over `String`                                          | `std:string`                  |
| `show`                                                               | `a -> String` (render any value, §6.7)                 | `std:fun/show`                |
| `format`                                                             | `String -> List String -> String` (`{}`-interpolation) | `std:string`                  |
| `currentVersion`, `read`, `save`, `getLine`, `putStr`, `putStrLn`    | I/O (§6.5)                                             | `std:fun/io`                  |

The §9 examples draw only on this table. Where 1.0 used undefined names (`addWord`, `sortByValue`),
§9 now spells them from these primitives (`insertWith`, a `foldr` with a comparator) so the examples
honour §3's rule that every construct is defined.

______________________________________________________________________

## 8. What the C++ core actually has to gain

### 8.1 Nothing in the VM or the arena

The VM's existing opcodes (`vortex_vm.hpp`'s `OpcodeKind`: arithmetic, relational, the structural
`#Link`/`#Value`/`#New`/`#Splice`, control-flow `#Call`/`#Return`/`#Jump`, and the memoization the
VM already performs along `d.cache`) are everything vlace's evaluation needs. Function application
is `#Call`; a thunk force is `#Call` on a nullary body with a `d.cache` lookup, which the VM already
does for opt-in memoization; pattern matching reuses the `#Unify` the logic extension added. **No
new opcode. No new `ArenaManifold` method. No change to the trail, the mark, or `promote()`.** The
arena's `mark`/`release` that §5.4 needs for failed matches is the one Vlog and vicon already use;
vlace is its third consumer.

vlace mints no new system dimension at all — unlike vicon's `d.scan` or Vlog's `d.clause`, its
values live on the dimensions the manifold already has (`d.grab`/`d.step` for structure, `d.vars`/
`d.values` for scope, `d.spin` for bodies, `d.clone` for sharing, `d.cache` for memo). This is the
quietest of the four front ends against the core: it is pure composition of mechanisms that exist.

### 8.2 A front end: lexer, parser, type checker, compiler, driver

What vlace needs in C++ is a front end, structured like the four that exist
(`apps/common/xanadu/{vql,vpl,vprolog}` and the Vortex assembler), with one pass the others lack — a
Hindley–Milner type checker (§5.5):

```
apps/common/xanadu/vlace/
  token.hpp         // identifiers, constructors, keywords, literals, operators
  lexer.{hpp,cpp}   // ML lexical rules; -- line comments; layout-insensitive
  ast.hpp           // expression and declaration nodes
  parser.{hpp,cpp}  // precedence-climbing; application is juxtaposition
  types.{hpp,cpp}   // Hindley-Milner inference; names an error or erases types
  compiler.{hpp,cpp}// lowers the typed AST to +d.spin streams of link/value
  driver.{hpp,cpp}  // forces the top-level thunk; the REPL/batch loop
apps/vlace/main.cpp  // the REPL/runner, mirroring apps/vprolog/main.cpp
apps/vlacec/main.cpp // the standalone compiler, mirroring apps/vqueryc and apps/vplc
```

The compiler and type checker return outcomes, not void, and name their refusals:

```cpp
namespace xanadu::vlace {

/// One compiled vlace program: the thunk to force for its result, and the
/// names its top-level declarations bound. A program may be all declarations
/// and no final expression, so the result is optional rather than noCell --
/// an API-level absence is std::optional, per AGENTS.md (§8.3).
struct CompiledProgram {
  gleditor::cpp26::optional<zigzag::CellRef> resultThunk;
  std::vector<std::pair<std::string, zigzag::CellRef>> bindings;
};

/// Why a program was rejected -- named, so a REPL says which, rather than
/// returning a half-built program for a thing it could not accept. "Refuse
/// loudly, by number," applied to a compiler.
enum class CompileError : std::uint8_t {
  LexicalError,
  UnexpectedToken,
  UnboundIdentifier,
  TypeMismatch,
  OccursCheckFailed,     ///< an infinite type: a = a -> b
  NonLinearPattern,      ///< a pattern binds the same variable twice (§5.4)
  NonExhaustiveMatch,
  UnknownModule,         ///< an import names a module that does not resolve (§5.8)
};

class Compiler {
public:
  explicit Compiler(zigzag::vortex::VortexCore &core);

  /// Compile and type-check a whole program; std::expected so a rejection
  /// names its reason rather than throwing or half-building a stream.
  [[nodiscard]] std::expected<CompiledProgram, CompileError>
  compile(const Program &program);

  /// Compile one expression for the REPL, in the current scope.
  [[nodiscard]] std::expected<zigzag::CellRef, CompileError>
  compileExpression(std::string_view source);

  /// Setters chain: configure and get the compiler back (AGENTS.md).
  Compiler *withTypeChecking(bool on) noexcept {
    typeCheck_ = on;
    return this;
  }

  [[nodiscard]] zigzag::vortex::VortexCore &core() noexcept { return core_; }

private:
  zigzag::vortex::VortexCore &core_;
  zigzag::vortex::VortexVM vm_;
  zigzag::vortex::VortexStdLib stdlib_;
  bool typeCheck_{true};
};

} // namespace xanadu::vlace
```

The driver forces the result, performs the top-level `IO` action, and projects at the arena
boundary:

```cpp
/// Force the program's result thunk to weak head normal form, in an arena
/// (§6.7). nullopt is a bounded divergence -- a budget trip, a blackhole
/// re-entry (<<loop>>), or a trapped partial match / unsafeDiv (§6.6) -- not
/// a recoverable failure; recoverable failure is an Option/Result value.
[[nodiscard]] std::optional<zigzag::CellRef> Driver::evaluate();

/// Run a top-level IO action, performing its effects (read snapshots, save
/// promotions, console I/O) at the boundary in order. The pure fragment it
/// threads is evaluated per evaluate(); only this method performs effects.
[[nodiscard]] std::expected<zigzag::CellRef, RuntimeError>
Driver::runEffect(zigzag::CellRef ioAction);

/// Fully force and render a value to text (show/render, §6.7; the crossing
/// Version::materialize draws). Bounded by a depth limit so a cyclic or
/// infinite value (§9.2) does not loop. function_ref: the sink is called,
/// not kept.
Driver &Driver::show(gleditor::cpp26::function_ref<void(std::string_view)> sink);

/// The one persistent effect, performed by `save` inside runEffect(): compact
/// the value's reachable subgraph into the store, preserving d.clone sharing
/// (§6.5) and extending the given parent microversion (non-confluent, §6.5).
[[nodiscard]] std::expected<zigzag::vortex::VortexCore::PromoteResult,
                            PromoteError>
Driver::promote(zigzag::CellRef value, MicroversionId parent);
```

### 8.3 Coding standards the front end is held to

The front end is new C++ and is held to AGENTS.md's "Code style", which the sketch above follows:

- **Functional style with `std::optional`/`std::expected`.** `compile()` and `compileExpression()`
  return `std::expected<_, CompileError>` — a rejection names its reason by number; `evaluate()`
  returns `std::optional<CellRef>` (a bounded divergence is `nullopt`, not an exception); and an
  API-level "no result" is `optional` (e.g. `CompiledProgram::resultThunk`, for a program that is
  all declarations and no final expression), never a reserved `CellRef`. `noCell` (`== 0`, R5)
  remains the engine's *in-manifold* absence — a link that points nowhere — which is the
  wire/structure layer AGENTS.md exempts, not an API return value.
- **Named errors with a stated message format.** Each `CompileError` renders as
  `vlace: <file>:<line>:<col>: <code>: <detail>` — e.g.
  `vlace: wc.vl:3:14: NonExhaustiveMatch: case on Option a misses None`,
  `vlace: m.vl:7:1: NonLinearPattern: 'x' bound twice in one pattern`,
  `vlace: u.vl:2:9: UnknownModule: no module "mine:util" on d.modules`. "Refuse loudly, by number"
  (AGENTS.md) means the code is machine-greppable and the detail is for a human.
- **Chainable mutators.** A method that configures the object and has nothing else to return gives
  back the object (`withTypeChecking` returns `Compiler*`), as `ArenaManifold`'s setters do; one
  that can refuse returns `std::expected` of it.
- **C++23/26 via the polyfill.** `gleditor::cpp26::function_ref` for the `show` sink (called, not
  stored); `gleditor::cpp26::inplace_vector` for a constructor's fields where the arity is fixed by
  its `data` declaration (a fact of the design); `gleditor::cpp26::optional<T&>` for an optional
  reference to a binding's thunk. From `<gleditor/cpp26*.hpp>` as `gleditor::cpp26::*`; never test
  `__cpp_lib_*` directly (`design/cpp26-compatibility.md`).
- **Logging.** Named spdlog categories — `vlace.type`, `vlace.compile`, `vlace.force` — guarded so
  expensive traces cost nothing when disabled, as the engine's categories are.
- **Tests sit with the code, once.** `tests/xudu/vlace_lexer_test.cpp`, `vlace_parser_test.cpp`,
  `vlace_types_test.cpp`, `vlace_compiler_test.cpp`, `vlace_e2e_test.cpp`, beside the
  `vprolog_*_test.cpp` and `vpl_*_test.cpp` suites. vlace exercises the engine with no graphics
  device, so it belongs in `xudu_test`, not `gleditor_test`.

______________________________________________________________________

## 9. Worked examples

### 9.1 `map` and `fold` over a list

```ocaml
letrec map = fn f => fn xs =>
  case xs of
    []      => [] ;
    (h : t) => f h : map f t
in map (fn x => x * x) [1, 2, 3]
```

1. `map` is a thunk bound on `d.vars` before its body is built (`letrec`, §5.2), so its body may
   name it.
1. Each `case` alt is compiled as a marked unification (§5.4): the `[]` pattern compares against the
   argument's tag; the `(h : t)` cons pattern binds `h` to the head cell and `t` to the tail thunk
   by `d.clone` splice.
1. `f h : map f t` conses a head onto a **thunk** of the recursive call — the tail is not forced
   until demanded, which is laziness (§6.1) and is why this same code would work on an infinite list
   (§9.2).
1. The top-level forces the result to a list of three squared integers, all arena scalar cells
   (§4.5). Rendering prints `[1, 4, 9]`; no operation is minted.

`map` here is sugar; the real `map` is `std:functional/map` (§7.3), and this definition is how a
program would spell its own if it wanted a lazy tail.

### 9.2 An infinite stream, taken finitely

```ocaml
letrec from = fn n => n : from (n + 1)
in take 5 (from 0)
```

1. `from 0` conses `0` onto a thunk of `from 1`; the thunk is unforced, so the "infinite" list is a
   single cons cell and one suspended computation.
1. `take 5` (`std:fun/take`, §7.2) forces exactly five tails, each force a cache-miss that computes
   one more cons and memoizes it (§6.3), then stops — the sixth tail is never forced.
1. The result is the five-element list `[0, 1, 2, 3, 4]` in the arena. The non-termination of `from`
   is never reached, because call-by-need demanded only what `take 5` consumed.

A strict language cannot write this; a lazy language on a *mutable* heap writes it with a mutable
thunk indirection. vlace writes it with the arena's memoization island and promotes nothing.

### 9.3 A persistent map update that copies nothing

```ocaml
let m0 = fromList [(1, "a"), (2, "b"), (3, "c")]
in  let m1 = insert 4 "d" m0
    in  case lookup 2 m1 of
          Some v => v ;
          None   => "missing"
```

`insert` returns a *new* map `m1` and leaves `m0` usable — the defining property of a persistent
collection. `lookup` returns `Option v` (§6.6), so the miss case is a value the `case` must handle,
not a silent failure. Under the manifold (§2.3):

1. `m0` is a tree of cells on `std:collections`' map structure.
1. `insert 4 "d" m0` **mints** only the path from the new leaf to the root, $O(\log n)$ cells; every
   untouched subtree is **shared** with `m0` through a `d.clone` link — Vortex §5.2's structural
   sharing, not aliasing. (`lookup` itself still walks a rank per level, $O(n)$ until U1, §2.3/§11.)
1. `m0` and `m1` coexist, `m1` costing a logarithm of *minted* cells rather than a copy, and both
   are referentially transparent arena values.
1. If this map were promoted (§6.5), `promote()` must emit the shared subtrees once and link both
   roots, so the $O(\log n)$ sharing survives the boundary; it does not re-mint `m0`'s subtrees.

The path copying a functional implementation writes by hand is here the shape of the store: a branch
in the ops DAG (R8) shares its ancestors, and a `d.clone` rank shares a subtree. §2.3 is this
example stated as principle.

### 9.4 A word-frequency counter: the pure core and the typed effect edge

```ocaml
-- pure: a frozen document to a count map, using only prelude names (§7.4)
countWords = fn doc =>
  foldr (fn w => fn m => insertWith (fn old => fn _ => old + 1) w 1 m)
        emptyMap
        (words doc) ;

-- effectful: acquire a snapshot, read it (pure), save the result. Type: IO ().
main =
  currentVersion "chapter.xanadoc" >>= fn r =>
    case r of
      Err e  => putStrLn (format "cannot open: {}" [show e]) ;
      Ok ver => save "wordcounts" (countWords (read ver)) ;

main
```

1. `countWords` has type `Doc -> Map String Int` — **no `IO` in its type, so it is pure** and
   referentially transparent (§2.2). It is `foldr` + `insertWith` from the prelude (§7.4); `words`
   yields a lazy list of zero-copy quoting cells over the document's spans (§4.5).
1. `currentVersion "chapter.xanadoc" : IO (Result IOError MicroversionId)` is the **effect** —
   asking which version is current (§6.5). Its `Err` branch is handled as a value (§6.6); there is
   no hidden exception for a missing file.
1. `read ver : Doc` is **pure** — a frozen snapshot has one value (§2.1/§6.5), so
   `countWords (read ver)` is a pure expression even though `ver` was obtained effectfully.
1. `save "wordcounts" … : IO ()` is the one persistent effect, performed by the driver's `runEffect`
   (§8.2): `promote()` compacts the count map into the store under the current head, preserving the
   map's `d.clone` sharing (§6.5).

The type `main : IO ()` tells the reader and the checker this program has effects, exactly where —
the thing v1.0's untyped `save`-in-a-`let` could not express. Evaluate `countWords (read ver)` twice
and you get the same map (pure); run `main` twice and the store gains two results in some order
(effectful, non-confluent, §6.5). Purity and its boundary are now distinguished by type, not by
prose.

### 9.5 A record, with a functional update

```ocaml
data Point = Point { x :: Int, y :: Int } ;

moveRight = fn p => fn dx => p { x = x p + dx } ;

moveRight (Point { x = 1, y = 2 }) 10           -- Point { x = 11, y = 2 }
```

`Point` is a record (§5.7); `x p` is the field accessor; `p { x = … }` is functional update, which
copies `Point`'s constructor cell, replaces the `x` field, and shares `y` with `p` through a
`d.clone` link — one changed field, one new cell (§2.3). The result is a new `Point`; `p` is
unchanged, as a pure value must be.

______________________________________________________________________

## 10. What this buys that a conventional functional language does not have

- **Every intermediate value is addressable, if you ask.** A conventional pure language discards
  intermediates to the garbage collector. vlace discards them to the arena, which is the same — but
  evaluate in the *persistent* regime instead and every reduction step has a `MicroversionId`;
  `d.ops_dag` walks the reduction graph; `xudu-dump --section=ops` renders it. That is a step
  debugger and a reduction-trace profiler for a lazy language, which is notoriously hard to build,
  and nobody wrote it — it is what the store does with the operations it is given. **This regime is
  intensional**: because it records *that* a reduction happened and in what order, it observes more
  than the value, so it is a debugging view, not a referentially transparent one — you cannot have a
  full reduction trace and extensional purity at once, and the trace is the price. The arena regime
  is the one a REPL runs and the one §2.2's RT claim is about; the persistent regime is for when the
  reduction itself is the thing worth inspecting.

- **Persistent data structures are the substrate, not a library.** §2.3: structural sharing is the
  ops DAG and `d.clone`, so a vlace `Map` is not an implementation of a persistent tree, it is a
  view of the one the manifold already is. There is no library to get subtly wrong.

- **Laziness and memoization share one mechanism with pure-function caching.** §6.3: a thunk's
  forced value and a memoized function's result are the same `d.cache` island, so a program's lazy
  evaluation and its explicit `std:memoize` use one bounded, GC-reclaimable store rather than two.

- **Zero-copy strings and documents.** §4.5: a substring is a quoting cell, and a xanadoc read into
  vlace shares the document's spans rather than copying them (§9.4), with the R8 fence keeping a
  scratch address out of persistent structure. A text-processing program over a large document
  allocates no document bytes.

- **It composes with its siblings.** vlace values are Vlog terms (§4.2) and Vortex cells, so a vlace
  program can hand a value to vicon or drive a Vlog predicate with no marshalling — one manifold,
  four notations. Calling a *VQL query* is the weaker claim: it presumes VQL is exposed as a module
  on `d.modules` whose exports are query combinators (§5.8), which VQL's own spec does not yet
  define — so this particular bridge is proposed here, not grounded in a sibling, and any sibling
  invoked from vlace must run in vlace's arena (§3) so it cannot write persistently behind the type
  system's back.

______________________________________________________________________

## 11. Breakages and costs

- **Indexing and random access into a rank are `O(n)`, which is U1.** A vlace list or array indexed
  by position walks the rank — the same wall VPL hit from the array side, Vlog from the clause side,
  and vicon from the string side (vicon §11). vlace is a fourth independent consumer of convergence
  U1. The mitigation is identical: a pinned ephemeral index keyed by ordinal
  ([`enfilade-rank-indexing.md`](enfilade/enfilade-rank-indexing.md)). That four front ends now
  reach U1 from four directions is the strongest argument the design corpus contains for resolving
  it.

- **Lazy space leaks are the language's own, not the manifold's.** Call-by-need's classic hazard — a
  thunk retaining a large structure it will force cheaply — is present, and `!`/`seq` (§6.1) are the
  usual tools. The manifold makes the memory bounded by LRU trimming (§6.3), which softens but does
  not remove the leak; an implementer must still offer strictness analysis or the annotations.

- **Hindley–Milner is a real front-end pass.** §5.5's inference is the most front-end-heavy of the
  four languages — more than vprolog's compiler or VPL's glyph table. It asks nothing of the
  manifold, but it is where most of the C++ in §8.2 goes, and `types.cpp` is the file most likely to
  harbour subtle bugs; its tests should be the thickest.

- **Divergence, not failure — now precisely.** A *well-typed, total* vlace expression produces a
  value or diverges; it has no exception channel. But v1.1 no longer pretends failure does not
  exist: recoverable failure is an `Option`/`Result`/`IO` value in the program's type (§6.6), and
  only genuinely partial situations (a guarded non-exhaustive `case`, `unsafeDiv _ 0`) trap into a
  bounded divergence (`evaluate()` → `nullopt`, §8.2). A conforming REPL must offer a budget or an
  interrupt, because a pure language will cheerfully be asked to force an infinite value (§9.2).

- **Tail calls are constant-space (resolved, §6.7), but deep non-tail recursion is not.** v1.1 makes
  a tail `#Call` reuse the `d.stack` frame (§6.7), so tail-recursive loops run in bounded space —
  the gap v1.0 left silent. Non-tail recursion still builds a call chain; the usual accumulator-
  passing and strictness (`!`/`seq`) tools apply, and a lazy accumulator remains a space-leak hazard
  (above).

- **No typeclasses in 1.0.** §5.5: dictionary passing is deferred. Programs that want ad-hoc
  polymorphism write explicit function arguments until a later version specifies it. This is a
  front-end limitation; the manifold would carry a dictionary as an ordinary record cell.

______________________________________________________________________

## 12. Reconciliation with the convergence

- **R5 (`noCell == 0`, zero is absence).** vlace's `nullopt`/empty-slot checks are absence, never a
  cell holding zero; it reuses the convergence's meaning of absence.
- **R6 (scalar cells: bits plus rendering).** vlace numbers are scalar cells; arithmetic reads bits,
  text is spooled only at promotion, so numeric evaluation allocates no content (§4.5, §6.4).
- **R8 (navigation does not mutate; the ephemeral/persistent split).** This is vlace's load-bearing
  ruling: the pure fragment evaluates in an `ArenaManifold` and mints no operation, which *is* its
  referential transparency (§2.2); effects are confined to the typed `IO` boundary (§6.5), whose
  only persistent write is `promote()` and whose read takes an immutable `MicroversionId` snapshot.
  Zero-copy substrings are ephemeral quoting cells refused as persistent link targets (§4.5).
  Promotion is parent-relative and non-confluent, so the "byte-identical" RT claim is scoped to the
  pure fragment and `render()`, not to a `save` (§6.5).
- **R12 (a dimension is a cell on `d.dims`).** vlace mints no new dimension; it composes the
  existing ones (§8.1).
- **U1 (random access into a rank).** vlace is a fourth independent consumer, from the list-indexing
  side (§11); it does not resolve U1 and does not need to for correctness, only for the speed of
  indexed access.
- **V3 (`promote()` is a CSR compaction that writes ops as it goes).** vlace's one effect is exactly
  this pass, with deferred scalar renderings written out along the way (§6.5).

vlace changes nothing in `store.hpp`, `CompactOpNode`, the arena, or the VM. It is a front end and a
`std:fun` package, held to the same acceptance test as VQL, VPL, vicon, and Vlog, and resting
entirely on mechanisms those documents already built and this repository already tests. It is the
front end that turns the append-only operations spool — built to make a hypertext's history
inspectable — into the immutable-value substrate of a pure functional language, by observing that
the two were the same structure all along.

______________________________________________________________________

## Appendix: Versioning and Change History

The rule is the one its siblings use:

- **Major** — a change a conforming implementation could not ignore: a keyword or construct gains,
  loses, or changes meaning; a default changes what an existing program does (e.g. the laziness
  default, or the arena-evaluation commitment).
- **Minor** — an addition that breaks nothing, a refinement, a clarification, a correction to
  normative prose.

Editorial changes that alter no normative text bump neither.

| version | commit | date       | change                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| ------- | ------ | ---------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1.0     | —      | 2026-10-15 | Initial specification: purity as the arena regime (§2), values as cells and ADTs as Vlog terms (§4), the ML-shaped grammar (§5), call-by-need via thunks and the §6 memoization island (§6), the `std:fun` package (§7).                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  |
| 1.1     | *this* | 2026-10-15 | A typed effect boundary. Added `IO` monad with `read`/`save` typed as effects (§6.5); `Option`/`Result` prelude and the runtime error model (§6.6); operator grammar + fixity table and the `List` ADT (§5.1); production→lowering table (§5.6); records (§5.7); user modules/`import` (§5.8); numeric tower, `Bool`, `Char`, literals (§4.6); the prelude listing (§7.4); WHNF per cell kind, three-state blackholing, proper tail calls (§6.7). Corrected claims: identity-keyed memo with CSE non-normative and the persistent regime conceded intensional (§6.3, §10); normative HM with the let-generalization rule stated and the dynamic mode removed from conformance (§5.5); WHNF-forcing destructuring with non-linear patterns rejected and guards added (§5.4); promotion required to preserve `d.clone` sharing and stated non-confluent, RT scoped to the pure fragment (§6.5, §2.2). Doc hygiene: keyword list and `CompileError` message formats (§5.1, §8.3), §2 normative lead bullets, jargon glossed. |

[toc-01]: #2-purity-over-an-append-only-store-the-reconciliation-that-comes-first
[toc-02]: #21-the-spool-is-an-immutable-value-evaluation-is-a-pure-function
[toc-03]: #22-the-two-regimes-are-referential-transparency-and-its-absence
[toc-04]: #23-persistent-data-structures-are-the-ops-dag-and-dclone-unasked
[toc-05]: #41-a-value-is-a-cell-a-constructor-is-a-functor-on-the-inputs-wing
[toc-06]: #42-algebraic-data-types-are-a-tag-cell-and-an-argument-rank
[toc-07]: #43-a-function-is-a-cell-a-closure-is-its-captured-environment-rank
[toc-08]: #44-a-thunk-is-a-cell-with-an-unevaluated-dspin-body
[toc-09]: #52-let-letrec-and-lexical-scope-as-a-dvars-rank
[toc-10]: #53-lambda-application-and-the-dual-wing-calling-convention
[toc-11]: #54-pattern-matching-is-destructuring-unification-forced-to-whnf
[toc-12]: #62-laziness-is-a-thunk-forcing-is-running-a-dspin-stream
[toc-13]: #63-memoization-is-vortex-6-and-the-memo-key-is-thunk-identity
[toc-14]: #82-a-front-end-lexer-parser-type-checker-compiler-driver
[toc-15]: #93-a-persistent-map-update-that-copies-nothing
[toc-16]: #10-what-this-buys-that-a-conventional-functional-language-does-not-have
[toc-17]: #3-the-acceptance-test-link-value-and-nothing-else
[toc-18]: #61-lazy-by-default-strictness-is-an-annotation
[toc-19]: #64-recursion-sharing-and-why-the-spool-does-not-grow
[toc-20]: #65-effects-the-typed-io-boundary-and-promotion
[toc-21]: #73-what-is-reused-unchanged-from-the-core-modules
[toc-22]: #94-a-word-frequency-counter-the-pure-core-and-the-typed-effect-edge
[toc-23]: #51-lexical-structure-keywords-operators-and-the-ebnf-grammar
[toc-24]: #55-data-declarations-hindleymilner-and-let-generalization
[toc-25]: #46-the-numeric-tower-bool-characters-and-literals
[toc-26]: #57-records-named-fields-and-functional-update
[toc-27]: #58-modules-user-facing-import-and-qualified-names
[toc-28]: #74-the-prelude-names-in-scope-in-a-fresh-program
