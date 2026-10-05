# Surface as a Slice Projection

**Status:** strongest Slice-only counterdesign to the proposed
[Surface structure kind](surface-vision.md). This is a design, not an implemented projector. It
tests whether the final `Make` kind slot is needed before assigning it. It depends on the
[generic Make and context-chain plan](../generic-structure-make.md) for Cell birth containment; the
same graph can be projected from today's single Slice with weaker birth-containment provenance.

## Claim

One ordinary Slice can contain any number of Surface roots. Each Surface is a named **Cell** whose
subgraph encodes a typed coordinate domain, placements, media sources, and scene snapshots. All
persistent changes are existing `Make(Cell)`, `SetLink`, `SetValue`, or `Splice` Structure ops. A
versioned `sys:surface` Vortex package interprets the graph and returns an ephemeral spatial or
spatiotemporal projection. The store need not grow `Make(Surface)` or Surface-specific verbs to
express the behavior proposed so far.

This does not claim that arbitrary Cells are automatically well-formed Surfaces. A reader selects
the Slice version, validates the pinned `sys:surface` schema, and either projects a Surface root or
reports a specific invalid or unresolved claim. The projector is the semantic interpreter; the
Manifold remains the faithful fold of authored Cells and dimensions.

## Persistent graph

Mint one root Cell `R` with its local name as content. Put Surface roots on a `d.surfaces` rank off
the Slice home Cell, or find them through a typed schema link. A versioned schema Cell linked from
`R` identifies the `sys:surface` release and axis signature `XY`, `XYZ`, or `XYT`. The signature is
immutable for a given root. An OpHandle to `R` can carry branch-local `d.alias` and typed
`d.created` annotations exactly as other Cell births can. None of this uses a reserved Make kind.

Each placement is a Cell `P` born beneath `R` in the generic Make containment chain. `P` has a
source Cell `M` whose content is an ordinary addressable span of image, volume, video, or audio
primedia. Repeating a source in another placement mints a new `M` with the **same span**, not a new
copy of its bytes. A foreign source uses the store's scroll registry or an external reference; it is
never guessed from equal media contents. The source's media type, decoded geometry, and decoder
profile occupy their own linked Cells.

A placement state `T` is another Cell born beneath `P`. It links to scalar Cells for its source crop
or interval, destination bounds, layer, transform, and parameters. Signed `Int64` Cells hold
coordinates; a rational uses distinct numerator and denominator Cells, with zero denominators and
overflow refused by the projector. Relative media time has its own unit Cells and never uses the
absolute `Timestamp` kind merely because both can be counted in nanoseconds. The function reference
is either a pinned bundled `{sys:surface release, symbol}` or a pinned persistent
`{program store, version, entry birth}`. Separate Cells carry each field; no transform grammar is
hidden in the placement's display text. Each state owns its field Cells; if a numeric or function
record must be shared, use a per-state reference Cell rather than competing for one reciprocal
dimension slot on the shared Cell.

A scene snapshot `Q` contains an ordered `d.items` rank of *occurrence Cells*. Each occurrence is an
OpHandle `H` whose `value` names one `P` birth. Its `d.state` link reaches a fresh state-reference
OpHandle `F` whose `value` names the chosen `T` birth. There may be several occurrences of one
source or even one placement. Fresh `H` and `F` Cells avoid ZigZag's
one-posward/one-negward-neighbor limit: many scene snapshots never compete for a reciprocal link on
`P` or `T`. The order on `d.items`, explicit layer Cells, and a stable birth name break ties
deterministically. `R` has one `d.current-scene` link to the selected `Q`.

```text
Slice birth -> home -> R: "Harbor edit"
                       ├─ d.schema -> sys:surface release + XYT axes
                       ├─ P-video -> M-video (original primedia span)
                       ├─ P-audio -> M-audio (original primedia span)
                       └─ d.current-scene -> Q2
                                             ├─ H1: handle(P-video) -> F1: handle(T-video)
                                             └─ H2: handle(P-audio) -> F2: handle(T-audio)
```

The dimensions above are schema vocabulary, not privileged storage fields. Every one is an ordinary
dimension Cell on `d.dims`. The package release gives their names and types meaning; another
application remains free to mint more dimensions. Multiple Surface roots can share one Slice, and a
root can refer to another root through an occurrence with an exact pinned version. A generic nested
Cell birth can make the containment chain `T -> P -> R -> Make(Slice)`. Readers of older
direct-in-Slice Cell births can recover grouping through links, but lack that stronger birth path
until the generic Make plan lands.

## Editing without Surface verbs

To Place, mint `P`, its source Cell, a complete state `T`, a new scene `Q`, and an occurrence
carrying fresh handle references to `P` and `T`. Copy unchanged occurrences as fresh lightweight
handle pairs into `Q`; they continue to name the same placement births and state Cells without
sharing dimension neighbors. Build the entire candidate graph while it is unreachable from
`R.d.current-scene`. One final `SetLink(R, d.current-scene, Q)` publishes the new scene at a single
hypertime point. The candidate Cells exist on earlier microversions but do not affect the projected
Surface until that link changes.

Change builds a new `T` beneath the same `P`, then publishes a new `Q` selecting it. The source span
and `P` birth remain stable. Remove publishes a `Q` without an occurrence for that `P`; old scenes
and primedia remain. Rearrangement changes occurrence order or parameters in a new `Q`. Nothing
requires editing a published scene or mutating an existing state Cell. A reader validates that
published scene and state subgraphs have not been changed after selection; an invalid graph is
reported, not silently reinterpreted. This is a schema rule, enforced by the projector rather than
by a new wire verb.

For a synchronized video clip and backing track, `H1` and `H2` select distinct sources but map to
the same local time interval. At local `t = 3`, the projector resolves a frame from the larger video
and a sample from the backing audio's own source interval. Trimming both as one action builds two
new state Cells and publishes one new `Q`. If one logical Place identity is wanted, make `P` a group
Cell with an ordered `d.components` rank of video and audio component Cells; its chosen `T` contains
both component states. The same final scene-link operation publishes the group atomically. Audio
mixing remains a versioned package rule, not an accidental consequence of equal time ranges.

Every operation still has a hypertime parent. Under generic Make, the Cell's edit chain goes to its
Cell birth, then through nested birth containment to `R` and the Slice birth. The final `SetLink` is
an edit on `R`, so its subject predecessor and context edge follow `R` to the Slice birth. A fork
before that link can point `R` at a different `Q` on each branch. The Surface view for a selected
version never uses the store's globally latest scene.

## Projecting and querying

`sys:surface/project(root R, selected Slice version)` folds the ordinary Manifold at that version,
follows `R.d.current-scene`, validates schema and reachability, and reads each occurrence's pinned
placement and state handles. It builds an ephemeral index over destination bounds. Its key includes
the Slice version, root birth, selected scene-link op, package release, and pinned program and
source versions. This index is a cache, not a new kind of authored Cell or operation.

Validation must confirm that `R`, `Q`, every occurrence, each handle target, and each selected state
were born on the chosen ancestral path and belong to the expected nested Cell chain. An OpHandle's
stored target index alone does not establish branch membership. Check that the current scene and its
reachable state subgraphs were completed before selection and have no later mutation on the selected
branch. A malformed handle or off-branch state is a rejected projection, never a fallback to a
same-named Cell.

For an `XY` query, it returns ordered image contributors at `(x, y)`. For `XYZ`, it returns source
voxel coordinates at `(x, y, z)`. For `XYT`, it returns visual contributors at `(x, y, t)` and audio
contributors at `t`. Each result includes `P` and occurrence identities, source span and decoded
source coordinate, transform reference, and unresolved status when media or code is unavailable. The
numeric map can be an affine routine or a pinned `sys:surface` or user Vortex function. A function
without validated conservative bounds stays in every query candidate set. Vortex runs in the
restricted, deterministic, budgeted `surface.map` profile described in the
[Surface vision](surface-vision.md); only scratch computation is allowed during projection.

The bundled `sys:surface` package can also supply `project`, `sample`, `audio_mix`, volume-section,
time-window, and spatial-index routines. Each authored scene pins the package release used to
interpret it. Bootstrap `CellRef`s are process-local, so an operand stores release digest and symbol
rather than a transient arena index. A client update cannot change an earlier scene's
interpretation; a missing release gives an explicit unresolved view.

## Feature stress test

| Proposed Surface feature           | Slice-only representation                                                                                           |
| ---------------------------------- | ------------------------------------------------------------------------------------------------------------------- |
| Birth, name, rename, timestamp     | Root Cell birth, content span or `d.alias` annotation, and typed `d.created` annotation.                            |
| `XY`, `XYZ`, `XYT`                 | Immutable axis-schema Cell pinned to `sys:surface` release.                                                         |
| Place identity and history         | `P` Cell birth, chosen state `T`, and per-occurrence handles to both births.                                        |
| Crop, trim, rotate, retime, remove | New state or scene Cells; one final `SetLink` selects a complete scene.                                             |
| Hypertime forks                    | Different ancestral `d.current-scene` links select different snapshots.                                             |
| Reused source and provenance       | Distinct Cells with equal source `PrimediaSpan`; return both span and occurrence identity.                          |
| Video with backing audio           | Two synchronized occurrences, or one group `P` with two component sources.                                          |
| Vortex transforms anywhere         | Pinned function Cell/version or bundled release/symbol in state, evaluated on query.                                |
| Nested or quoted structures        | State names another root and selected version; projector checks cycles and depth.                                   |
| Interleaved Surfaces in one store  | Separate root Cells in one Slice and separate cache keys.                                                           |
| Missing media or program           | Explicit unresolved projection, not empty output or substitution.                                                   |
| Spatial or temporal links          | Region Cells plus linked claims in the Slice; classic span-only Links still need a media-region endpoint extension. |

The last row is a limit shared with the dedicated Surface proposal: a compressed image rectangle or
video time interval is generally not one encoded byte span. A Cell can name a decoded region and the
projector can carry that identity across quotations, but today's classic Link endpoint still names
primedia spans. Changing that endpoint model is independent of assigning kind 3.

## What this costs and what it does not provide

The Slice graph is larger than one SurfacePlace operand: scalar coordinates, source references,
state links, and new occurrence Cells take operations and memory. Creating a new `Q` can copy an
entire occurrence rank unless the package uses persistent shared subgraphs or chunked ranks. The
one-final-link pattern makes a scene update atomic for the projector, but intermediate draft Cells
are visible to generic Slice tools. Validation, spatial indexing, decoding, and Vortex execution
still need code; representing them as Cells does not make those algorithms free. Benchmark a large
scene and branch-heavy editing before choosing the graph's rank layout.

If copying an entire rank is too costly, make `Q` point through an OpHandle to the root of a
persistent ordered tree of occurrence chunks. A Change copies the affected chunk and its ancestor
path, then changes `R.d.current-scene` once; unchanged chunks keep their Cell births. Child pointers
are fresh OpHandle Cells naming chunk births, avoiding reciprocal-link conflicts when several scene
versions share a chunk. This reduces authored work toward the path length, at the price of a more
complex `sys:surface/project` traversal. It is an optimization to measure, not a new persistent
operation type.

The generic ops spool would not know that a Cell represents a Surface coordinate frame. Generic
`Insert`, `Delete`, and `Transclude` would not accept `(x, y, z)` or `(x, y, t)` as their `at`
coordinates. All spatial edits would be authored through `SetLink` and new Cells, then interpreted
by the pinned package. If a product requirement later demands a primitive OSMIC op that edits a
Surface coordinate directly, or a store-level typed Surface birth independently validated by every
OSMIC reader, this Slice-only design does not supply that primitive. That requirement, and a
measured failure of the Slice projection, would justify revisiting kind 3.

Until then, the proposed features can be represented without consuming the final Make-kind slot.

## Prototype and decision gate

Build one small `sys:surface` projector over the existing Manifold before changing the operation
format. Exercise two Surface roots in one Slice, a shared image source with two placements, a fork
that changes one placement, an `XYZ` crop, and an `XYT` video with independent backing audio. Verify
that one final scene link publishes each multi-Cell edit atomically, old branches retain old states,
and repeated placement or state references never evict one another's ZigZag links. Round-trip the
graph through save/load and publication, including renumbered OpHandle targets and pinned package
releases. Require explicit failures for off-branch references, altered published snapshots, missing
media, missing Vortex code, and recursive sources.

Measure operations and primedia bytes per edit, retained snapshot Cells per branch, cold projection
time, incremental update time, and point-query cost against the proposed dedicated Surface replay
product. The comparison should use the same authored scene and program semantics. Only an observable
capability gap or an unacceptable measured cost can make kind 3 more than a second spelling for this
Slice projection.
