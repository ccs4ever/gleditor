# Round 2 critique: Xanadu / ZigZag Purist (vagent-dynamic-source-weighting-and-epistemic-arbitration.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 4 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R1.1 (§6.1, line 660) Uncloned fan-in on `EpistemicProvenanceToken`: RESOLVED — Token stores unlinked scalar bits (`kTokenPredicate`, `kTokenKeyHash`) and authentic clone view cells on `+d.clone`, with multiple tokens chaining on `+d.step`.
- R1.2 (§2.4, line 306) Base-cell shadowing of source profiles violates Rule V12: RESOLVED — Source profiles and calibration descriptors reside exclusively in the agent's ephemeral `ArenaManifold` overlay on `+d.trust`; base manifold holds no profiles.
- R1.3 (§5.4, line 650) `arena.discard(m_0)` claim contradicts `ArenaManifold::discard`: RESOLVED — Query candidate scoring runs under an inner evaluation mark $m_{\text{eval}}$, with `arena.release(m_{\text{eval}})` cleanly truncating 100% of scratch cells prior to unifying the winner under $m_0$.
- R1.4 (§2.1, line 259) Stringified list literal in `$weights` violates cell purity: RESOLVED — Vector weights are represented as a canonical rank of R6 float scalar cells along `+d.step` off a head cell, supporting zero-allocation in-place updates.
- R1.5 (§2.1, line 232) Total omission of `d.clone` ranks for source provenance: RESOLVED — Source profiles on `+d.trust` are designated clone masters, asserting sources on `+d.source` are clone instances on `+d.clone`, giving $O(1)$ dereference via `ArenaManifold::cloneMaster(cell, d.clone)`.
- R1.6 (§4.3, line 548) Unspecified 1D rank excision on `confirm` and `refute`: RESOLVED — Explicitly specifies 1D rank excision for belief retraction on `refute` and for pruning inactive sources, grounded in P18.3 prerequisites.

## BLOCKING

None.

## MAJOR

None.

## MINOR

### 1. (§6.1, line 681) Multi-source corroboration topology on `kTokenSources`

Full title: Multi-source corroboration topology on `kTokenSources` should explicitly specify chaining along `+d.step`

**Problem:** §6.1 states:
> "- `kTokenSources`: Clone view cell on `+d.clone` linking the asserting source profile masters."

In ZigZag, every cell maintains strict 2-degree connectivity ($A.\text{pos}(d) = B \iff B.\text{neg}(d) = A$). A single clone view cell has exactly one negative neighbor on `d.clone` and can therefore link to exactly one clone master on `+d.trust`. When a belief is corroborated by multiple sources (e.g. both `"sensor:gps"` and `"sensor:odometry"`), a single cell cannot link to multiple profile masters simultaneously.

**Evidence:**
- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:681`
- `apps/common/xanadu/zigzag/arena_manifold.hpp:531–533`

**Fix:**
Clarify in §6.1 that when multiple sources corroborate a belief, `kTokenSources` links along `+d.values` to the first source's clone instance on `+d.clone`, with subsequent source clone instances chained posward along `+d.step` (or `+d.source`).

______________________________________________________________________

### 2. (§8.5, line 917) Token memory footprint pricing in Proposal P19.5

Full title: Token cell memory footprint pricing in Proposal P19.5 should reflect 4 cells (256 bytes)

**Problem:** Proposal P19.5 states:
> "- Pricing: 1 token cell + 3 link cells per active intention frame (~128 bytes)."

In the hyperstructural layout, each `CompactOpNode` / `CellSlot` occupies 64 bytes (`compact_op.hpp:78`). A token consisting of 1 token cell plus 3 metadata cells (`kTokenPredicate`, `kTokenKeyHash`, `kTokenSources`) totals 4 cells, which occupies $4 \times 64 = 256$ bytes (or $64 \times (4 + K)$ bytes for an intention corroborated by $K$ sources), not 128 bytes (which would be only 2 cells).

**Evidence:**
- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:917`
- `apps/common/xanadu/compact_op.hpp:78`: `static_assert(sizeof(CompactOpNode) == 64);`

**Fix:**
Update the pricing line to:
`- Pricing: 1 token cell + 3 field cells per active intention frame (~256 bytes, +64 bytes per additional corroborating source).`

______________________________________________________________________

### 3. (§4.3, line 563) Typo in `confirm` comment regarding $\beta$ update

Full title: `confirm` code comment indicates decrementing $\beta$ instead of incrementing $\beta$ on conflicting sources

**Problem:** §4.3 illustrates `confirm` with the comment:
```vagent
// 1. Confirm: Verifies that the asserted fact is veridical.
// Increments alpha for all asserting sources; decrements beta for conflicting sources.
confirm at(self, ?dest_x, ?dest_y)
```

In the Beta distribution $\text{Beta}(\alpha, \beta)$ formulation (§3.2, line 397), $\beta$ represents cumulative negative evidence (refutations/errors). To penalize conflicting sources that asserted contradictory facts for the confirmed key tuple, their $\beta$ count must be *incremented* ($\beta \leftarrow \beta + 1.0$), not decremented. Decrementing $\beta$ would reduce negative evidence and erroneously increase expected trust $\frac{\alpha}{\alpha + \beta}$.

**Evidence:**
- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:563`
- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:330`: "`$beta` | Cumulative negative evidence / refutations ($\ge 1.0$)"

**Fix:**
Change the comment line to:
`// Increments alpha for all asserting sources; increments beta for conflicting sources.`

______________________________________________________________________

### 4. (§2.1, lines 296–300) Head cell link dimension to first list element in Mermaid diagram

Full title: Mermaid diagram shows `$weights` head cell linking directly via `+d.step` rather than `+d.grab`

**Problem:** In the §2.1 diagram:
```mermaid
        F_Weights["$weights (Head)"] -->|"+d.step"| W1["0.82"]
        W1 -->|"+d.step"| W2["-0.15"]
        W2 -->|"+d.step"| W3["0.44"]
```

Under canonical `runtime_abi.hpp:22–25`:
> "List: a cell whose content is `kListContent` (`"[]"`), first element on `+d.grab`, the rest chained on `+d.step`."

Linking the list head directly along `+d.step` instead of `+d.grab` diverges slightly from the runtime ABI convention for ZigZag lists.

**Evidence:**
- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:296–300`
- `apps/common/xanadu/vagent/runtime_abi.hpp:22–25`

**Fix:**
Update the Mermaid diagram edge from `F_Weights` to `W1` to use `+d.grab`:
`F_Weights["$weights (Head)"] -->|"+d.grab"| W1["0.82"]`

______________________________________________________________________

VERDICT: CONVERGED
