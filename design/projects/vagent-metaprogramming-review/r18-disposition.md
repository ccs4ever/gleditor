# Round 18 disposition — v1.17 → v1.18 (convergence-freeze round)

Round 18 was the bounded freeze round per the convergence-freeze plan: verify the r17
rulings landed *correctly* and hunt majors only in pre-existing mechanisms — no
new-defect hunting in v1.17's own new wording.

## Tally

| lane      | verdict        | B | M | m |
| --------- | -------------- | - | - | - |
| vortex    | CONVERGED      | 0 | 0 | 1 |
| zigzag    | NOT CONVERGED  | 0 | 1 | 2 |
| codebase  | NOT CONVERGED  | 0 | 1 | 4 |
| architect | NOT CONVERGED  | 0 | 1 | 2 |
| meta      | CONVERGED      | 0 | 0 | 2 |
| ergo      | NOT CONVERGED  | 0 | 1 | 2 |
| writer    | CONVERGED      | 0 | 0 | 0 |

Three lanes converged outright. The four majors are narrow and all qualify under the
freeze mandate — two verify R17-B's *application* is mechanized wrong, two are
pre-existing defects. This is not convergence yet, but the findings sit in one cluster
plus two small items, so a single targeted revision closes them.

## Verification results

- **R17-A (`+d.step` positional carrier): VERIFIED** by every lane that examined it —
  present at :393, :1078-1083, :1186-1189, :2131-2133, order matches
  `link(from,dim,dir,to)` (`arena_manifold.hpp:604`), `+d.name` correctly reserved and
  `+d.refs`-scoped. Zigzag verified the carrier survives `link`-eviction.
- **R17-B (dim filing): FAILED verification in two specific respects** — zigzag and
  codebase independently proved the enumeration as stated cannot yield the vocabulary.
  File-before-pin and file-before-first-resolution orderings themselves verified
  (architect audited every `ensureDimension` in the tree).

## Rulings

### R18-A — enumeration by name-table + explicit fixed list (zigzag M1 + codebase M1, convergent)

The stated enumeration "the `d.dims`-rank member set `dimensions()` walks" is doubly
broken: `ArenaManifold::dimensions()` is base∪`arenaDims_` and never reads the
arena-local `d.dims` rank (`arena_manifold.cpp:34-46`); and `ensureDimension` only
rank-appends when a base supplies `home` (:91-103), so emit-side mints are
rank-invisible — the walk is circular. Separately, the list mislabels
`vortex_core.cpp:50-69` as "the `mintNamedDimension` V10 names": that range mints the
nineteen **system** dims (`d.grab`…`d.role`), while V10's actual thirteen
(`d.behavior`, `d.skill`, `d.capacity`, `d.space`, `d.member`, `d.holon`,
`d.intention`, `d.subgoal`, `d.outbox`, `d.belief`, `d.pending`, `d.source`,
`d.extends` — `vagent-agent-language.md` V10) are `ensureDimension`-minted by the
adapter and appear nowhere in the list. Unfiled, every `+d.belief`/`+d.intention`
rank orphans at first checkpoint — the failure filing exists to kill.

**Ruling:** restate the enumeration as two sources: (a) the arena's recorded
**name table** — the surface `ensureDimension` writes and `dimensionNamed`/
`dimensions()` read — walked directly, not via the `d.dims` rank; (b) an explicit
fixed list: the nineteen system dims `vortex_core.cpp:50-69` mints (correctly
labeled system dims), the thirteen V10 `std:agent` dims (with ordering: the adapter's
V10 `ensureDimension` sweep runs before filing, or filing itself performs it), plus
`d.store-refs` (`arena_manifold.cpp:289-290`) and `dimConstant`/
`findOrMintDimension` mints (`vagent/compiler.cpp:769-772`). The coverage sentence
says "names whose edges can fold" (architect m2), which also resolves zigzag m3
(provenance/quote/import-scratch dims are non-folding — architect's audit confirmed).

### R18-B — declared-image int64 ref finalization: state the invariant (architect M1)

Genesis (e) is bounded to learn-registered heads + replay copies, so declared heads'
non-`dim` int64 ref slots (wing slots, `Call`/`Jump`/`Branch`/`Choice` targets) name
no writeback — the doc never says what finalizes them. What actually finalizes them
already exists in the spec: the positional-preservation invariant the canonical
adopted-band comparand verifies — if the comparand passes, every declared position
names the same cell; if it fails, resume refuses (OpHandle-valued slots ride
`promote`'s translation separately). **Ruling:** add the stating clause — declared
positional int64 refs are finalized by positional preservation under the comparand,
not by writeback — so an implementer doesn't hunt a ceremony that does not exist.

### R18-C — §8.1 junction-label member (ergo M1, pre-existing since v1.2)

The endpoint domain lists "a synapse junction's own label" but `sprout_synapse`
mints no label and no `*_named` resolver reaches `+d.synapse`. **Ruling:** drop the
member (no mint exists to name).

### R18-D — `+d.step` decode anchor (zigzag m1 + meta m1 + ergo m1, three-lane cluster)

The positional carrier's *decode* is unstated: the recipe chain is heterogeneous
(literal/capture/ref-capture records share `+d.step`), so raw chain position cannot
delimit tuple runs. Meta supplied the fix from inside stated vocabulary: the `dim`
member anchors each tuple — it is the unique member whose `+d.recipe` record carries
`+d.name`; `from` is its chain predecessor, `dir`/`to` its successors. **Ruling:**
state the anchor rule (one clause wherever the carrier is defined).

### Minor bundle

Vortex m1 (:1083 stale "(name → tuple position)" echo — needs the `+d.step` suffix);
codebase m1 (`d.source` walk misdescription — anchors state-side belief clauses),
m2 ("position in the chain" → "within its tuple run"), m3 (`set_weight` table
junction arm shorthand), m4 (annotate the 1.11 changelog entry); architect m1
("invisible to `dimensionNamed`'s base arm" mis-attribution — the post-pin hazard is
fold-order, not name-table visibility); meta nits (`d.op-label`/or_parallel dims —
the honest-residual clause already covers; leave); zigzag m2 folds into R18-D.
