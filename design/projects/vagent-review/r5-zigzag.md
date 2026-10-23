# Round 5 critique: ZigZag purist (vagent-agent-language.md v3.2)

"Doc" is `design/vagent-agent-language.md`; code paths are under `apps/common/xanadu/`.

## Status of round-4 BLOCKING and MAJOR findings

| R4      | Topic                                 | Status                                                                                                                                                                                                     |
| ------- | ------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1 MAJOR | `d.source` used but never minted      | RESOLVED. V10/§5.1 list it, with `d.extends`, as std:agent-minted; thirteen is now the count everywhere I grepped (V10, §10 genesis, §16 R12). The citation of the minting mechanism is wrong (finding 3). |
| 2 MAJOR | No layout for lists, maps, records    | PARTLY. §5.9 gives one. It is a third list layout beside two that already exist in the code, and §10 claims reuse of `std:collections` that the layout does not allow (finding 2).                         |
| 3 MAJOR | "Ready" not carried by the checkpoint | RESOLVED. `ready` stamp from `$turn_count` (§5.6), ready list derived from cells only (§4.2 step 10, §5.6).                                                                                                |

R4 minors 4-8 are addressed: guard on `+d.grab`, body on `+d.spin` (§5.7); `d.subgoal` is "chain";
reserved fields complete; per-intention cost priced (§4.4); legend reads "rank head = master". The
`extends` part of R4 minor 4(b) was fixed by adding `d.extends`, and that fix is itself defective
(finding 1).

## 1. BLOCKING. `d.extends` as a one-link dimension breaks the two-degree rule (§3 V10 …

Full title: 1. BLOCKING. `d.extends` as a one-link dimension breaks the two-degree rule (§3 V10,
§5.1, §5.5, §5.7, §8.9)

§5.1 declares `d.extends`: "event type or capacity name cell -> its supertype's", shape **link**. A
link is reciprocal (V11 says so itself): `A +d.extends -> B` writes `B -d.extends -> A`, and a cell
has one negward neighbour per dimension. Therefore:

- A supertype can have only one subtype. `event Alarm extends TargetDetected` and
  `event Warning extends TargetDetected` (§8.5 line 1058 shows the first form): the second `link`
  evicts `Alarm` from `TargetDetected`'s negward end. `ArenaManifold::link` is a bare link and
  evicts silently, which §5.1 itself warns about two paragraphs later.
- `CapacityDecl ::= 'capacity' Ident [ 'extends' IdentList ]` (§8.9 line 1175) allows several
  supertypes, but a cell has one posward neighbour per dimension, so a capacity can name only one.
  `capacity extends` is also what §5.3 installs one instance per extended capacity for.
- The chain `Alarm -> TargetDetected -> Event` walked by §5.7's "each supertype" then also threads
  through `TargetDetected`'s one negward slot, which is already taken by `Alarm`.

The table's refusal of `d.clone` ("would make sibling subtypes look like each other's subtypes")
applies, in inverted form, to the chosen design. This is not cosmetic: the first program with two
subtypes of one event loses an inheritance edge, and dispatch to the lost subtype's handlers on the
supertype silently stops.

Fix. Make it a **rank per subtype**, not a link between type cells. A subtype's name cell heads its
own `+d.extends` rank of fresh *reference cells*, each with content = a supertype's qualified name
(V11, V12: references are names, copied), listed nearest first and flattened transitively by the
compiler. The rank costs one cell per ancestor per type, is read by one walk, handles `IdentList`,
and places no constraint on how many types share a supertype (they hold copies of its name, not a
link to its cell). Change the §5.1 row to "type name cell -> a rank of its ancestors' names
(copied)", shape rank, and rewrite §5.5 ("the receiver resolves the chain along `d.extends`") and
§5.7 ("the ranks for the event's type and each supertype") to say the dispatcher walks the
ancestor-name rank and looks each name up in the symbol table. Same shape, same dimension, no new
C++.

## 2. MAJOR. §5.9's list and map layouts disagree with the lists and maps the code …

Full title: 2. MAJOR. §5.9's list and map layouts disagree with the lists and maps the code already
has, and with §10's reuse claim (§5.9, §10, §8.7)

The code has three candidate list layouts:

1. `std:collections` lists: a rank on `d.step` headed by the first element, built by `chainCells`
   (`vortex/vortex_stdlib.cpp:30-36`), defaulted by `listDim` (`vortex_stdlib.cpp:2681-2683`),
   walked and extended by `pushBack`/`pushFront`/`popBack`/`listToVector`
   (`vortex_stdlib.cpp:2499-2535`).
1. Prolog cons lists, `'.'(H, T)` ending in `[]` (`VortexStdLib::makeCons`/`makeList`,
   `vortex_stdlib.cpp:4032-4046`), which is what `[H|T]` unification needs.
1. §5.9's: a head cell, first element on `+d.grab`, the rest chained on `+d.step`.

§10 says `std:agent` "reuses ... `std:collections` (lists)", and AGENTS.md requires reuse "unless it
cannot". Layout 3 is not layout 1: `std:collections` functions take the first element as the head,
not a head cell, so `*xs`, `!xs` and `++` (§12.1) cannot be reused without a step through `+d.grab`,
and `pushBack` cannot extend an empty §5.9 list (no first element to be the head). Layout 3 is also
not layout 2, so "a list is a term, so `#Unify` works" holds only for equal-length lists unified
element-wise; `[H|T]` patterns do not. Maps: `std:collections` already has a map, name cells on
`d.vars` with the value on `+d.values` (`mapKeyCell`, `vortex_stdlib.cpp:2685-2692`); §5.9 invents
"a list of `{key, value}` records", and §8.7 says `Map` is "from `std:agent`", which duplicates it.

Fix. Pick one, say why, and state the consequence:

- Preferred: keep §5.9's head cell, but say its `+d.grab` target is the first element of a
  `std:collections` list, so every `std:collections` function applies to the rank headed there, and
  the empty list is a head cell with no `+d.grab` (the only case `std:agent` handles itself). Add a
  sentence that `#Unify` over lists is element-wise, length-matched, and that vagent has no `[H|T]`
  pattern.
- For `Map<K, V>`: either reuse the `std:collections` map (record-shaped, text keys, O(n)) and
  restrict `K` to `Str`/`Int`/names, or justify the generic-key list-of-records by a stated need.
  The price table in §5.9 already says O(n) for both, so the justification is only about `K`.

## 3. MINOR. V10 cites a private function as the minting mechanism (§3 V10, §10 genesis)

V10: "`VortexCore::mintNamedDimension` looks a name up before minting it". It is declared after
`private:` (`vortex/vortex_core.hpp:241, 256`), as is `baseDimensionNamed`; `std:agent` is Vortex
code, and no opcode mints a dimension (`OpcodeKind`, `vortex_vm.hpp:27-87`). The mechanisms that
exist: the public `ArenaManifold::ensureDimension` (`zigzag/arena_manifold.hpp:283`,
`arena_manifold.cpp:81-97`: find in the base, else in the arena's table, else make the cell and link
it onto `d.dims`), which the C++ loader can call (it is new code, §11.1; `mesh_value.cpp:462`
already calls it on resume), or a Vortex walk of the `d.dims` rank by name followed by `#New` at its
end (Vlog §6.1's "`MakeCell` plus `SetLink`"). No proposal is needed either way. Fix: replace the
citation by `ensureDimension` and say in §10 genesis whether the loader or the Vortex genesis
routine performs it. If the loader does, `std:agent`'s genesis row should say "receives the thirteen
`DimRef`s", because Vortex code needs them as numbers for `#Link`'s second input
(`vortex_vm.cpp:526-531`).

## 4. MINOR. §5.1's attachment recipe names C++ helpers that Vortex code cannot call (§5.1, §9.1)

"Attachment uses `ArenaManifold::insertIntoRank` or `zigzag::rankTail` followed by `link`." The
dispatcher, beliefs, deliberation and relay in `std:agent` (§10) are Vortex; no opcode exposes
either helper, and the `#Link` write form is a bare `arena_.link` (`vortex_core.cpp:164-166`), which
evicts. The adapter (C++) may use the helpers. For Vortex the recipe is: read the tail with
`#Link`'s read form (`vortex_core.cpp:135-137`), mint with `#New`, and for a middle insertion write
the two links in the order that re-seats the evicted neighbour; `std:collections::pushBack`
(`vortex_stdlib.cpp:2507-2513`) already does the tail append. Say which side performs each of
`adopt`, outbox entry, source add and removal, and that removal is read-neighbours then link
predecessor to successor then `#Break`. No proposal is needed, but §15's "appends walk ranks" cost
is then Vortex steps against the fuel limit, not a C++ loop, and should say so.

## 5. MINOR. Two small inconsistencies in §5.6 and §4.4

(a) `delegator` is "a peer cell and correlation id". A peer cell sits on one rank (`d.holon` or
`d.member`) and cannot also hang off an intention (V11). Say "the delegator's name and the
correlation id", as V16 already treats identities.

(b) §4.4 says an intention costs "ten attribute pairs"; §5.6 now lists twelve attributes
(`id kind goal plan tried step waiting ready delegator blocked_until faulted frame`). Absent ones
are not minted, so "about thirty" is plausible, but say "up to twelve, of which about six are
present at rest".

## Register check (§11.2), within my lane

No dependency on an unregistered change to existing zigzag or Vortex code remains. Everything
structural the design relies on already exists: `ArenaManifold::insertIntoRank`
(`arena_manifold.hpp:617`), `cloneMaster` (`arena_manifold.hpp:523`; `VortexCore::value` writes
through the clone master, `vortex_core.cpp:176-177`, consistent with the skill frame),
`ensureDimension`, `promote` reachability and the `mesh_value.cpp:393-395` dimension carry-over (the
dimension cells travel in a checkpoint even though no link reaches them, so V10's "reuses the
restored dimensions" holds), and `setStateRoot`. Findings 3 and 4 need no proposal; I note that so a
reader does not add P10/P11. The register's justification rows and "Without it" columns are not my
lane beyond that.

## Things checked and found sound

- Agent state is one cell with ranks off it; no state cell links into the program (V12); two peer
  cells per child on `d.holon` and `d.member` respect one neighbour per direction; `d.source` rank
  entries are each their own peer cell, so no cell is on two source ranks.
- Skill instances: one rank per dimension, frame at the head of its `d.clone` rank.
- The §5.9 compound-term layout matches Vlog's (`vlog-logic-extension.md:81,142-155`: arguments on
  `+d.grab`, then chained on `+d.step`), and record/`Opt` placement collides with nothing.
- Dead cells after unlink are described correctly (arena frees only by `release` truncation;
  `compact()` frees no cell); reclamation by checkpoint and resume agrees with R8 and R7.
- Handler guard on `+d.grab` and body on `+d.spin` do not collide: `#Call` takes an opcode cell as
  its target (`vortex_vm.cpp:665-683`), so the guard's entry opcode is callable wherever it hangs.

VERDICT: NOT CONVERGED
