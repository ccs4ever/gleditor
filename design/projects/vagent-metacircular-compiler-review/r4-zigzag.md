# Round 4 critique: Xanadu/ZigZag Purist (vagent-metacircular-compiler.md)

Tally: 0 BLOCKING, 4 MAJOR, 6 MINOR.

## MAJOR

### 1. (§3.2 E1, D3) Registration status is invisible to the space in-session
opcodeMap_ is untrailed per-WorkerContext C++ state; a program can validate a
graph yet never learn whether the VM will execute it — it must try and observe
silent Nop. Every load-bearing distinction must be space-visible. Fix: add a
`kind_registered`/`kindOf` reflect verb reading carrier ∪ pending ∪ committed.

### 2. (§2) The doc never names the map-as-index principle
opcodeMap_/cursorPC_/memoizedOps_ are derived per-context indices over space
truth (same species as the transclusion index). State it once; it justifies
the whole D3 apparatus. Reframe `register` as "graduation into the program's
immutability class" (P15 freeze) — the map is really the immutability
registry; a d.ops/d.frozen rank is the space-visible form. Also: RunCall's
map-membership check is a derived guard, not a primitive — the +d.entry chain
already asserts the terminus is an op.

### 3. (§4 D1) Definition-of-done never tests metacircularity; staging can strand it
Stage 3 satisfies "runtime source→graph" pragmatically before any in-language
stage; "metacircularity must earn its keep" gives no criterion. Add: emitter
reproduces the bootstrap pair + lowering-time corpus validated-equal (C++
emitter demoted to oracle); terminal exit = compiler compiles its own source
indistinguishably; pin stage-3's interface identical to the in-language
pipeline's so the C++ service is a drop-in.

### 4. (§2/D3) Provenance travels as name-strings, not links
reify mints independent twins; the back-pointing +d.entry edge is overwritten
by recipe replay; `reify:<src>:<serial>` is provenance-by-name — nothing lets
a walker ask a source head "what twins exist?" Fix: `+d.lineage`/`d.reified-from`
edge at reify (twin → source; -d.lineage enumerates twins free); say why
per-copy recipes re-mint rather than share.

## MINOR
- F4: AST kind vocabulary should be filed cells (kArity precedent), builder
  input unified with AST schema — avoid two program-form intermediates.
- F5: std:meta's C++ body is a standing substrate violation; name in-language
  re-expression of validate/reify/grafts as the direction.
- F6: pending-registration machinery could dissolve into trailed rank-append +
  lazy map fill — evaluate, state whichever wins and why.
- F7: +d.validate tag residue is observer-effect on a read; first/last-match
  descriptor divergence needs one ordering contract per rank (doc already
  calls the fix).
- F8: carrier + name-keyed d.code re-resolution IS the (lineage, label)
  transclusion unit for U-M2/xanadoc code transclusion — strengthen §7.
- F10: "indistinguishable" inherits the int64 shadow-pointer contract layer;
  say contract-level reform (link-carried operands?) is a separate purity
  project.

VERDICT: NOT CONVERGED
