# Surface: a Hypertime Coordinate Map

**Status:** vision and replay contract, not an implemented format. This note explores assigning
reserved `StructureKind = 3` to Surface after
[`generic-structure-make.md`](../generic-structure-make.md) establishes typed births and context
chains. The existing [Structure hyperop vision](../structure-hyperop-vision.md) and
[convergence rulings](../store-slice-convergence.md) remain its architectural basis.

## Why a Surface might earn a birth

A Slice can hold Cells describing images, rectangles, transforms, and layer order. That is useful,
but it does not make a pixel coordinate an operation coordinate. A Surface earns its own kind only
if an operation can ask the store to change a *spatial structure map* and replay can answer, at a
selected hypertime version and surface coordinate, which source media contributes there. If the same
rules can be supplied by a Slice projection without a separate edit target, keep kind 3 reserved.

`Make(Surface)` would mint a named, persistent coordinate frame. Its op index is the Surface's
identity; its content span holds its initial local name, as it does for Slice and Xanadoc. Its axis
signature is fixed at birth: the initial prototype is `XY`, while `XYZ` and `XYT` are possible later
signatures. A proposed birth encoding puts the axis signature in `Make(Surface).value` (`0 = XY`,
`1 = XYZ`, `2 = XYT`) with `ValueKind::None`; other values are refused. Its name remains in the
span, and no later edit changes the axis signature. A `d.alias` annotation could rename it without
changing that identity. A Surface may be top-level or born inside another structure through the
generic Make containment edge. Its coordinates are local to its own frame, not document byte
offsets, permascroll byte offsets, or screen pixels. The viewport, zoom, and pointer focus are
reader state and append no operations.

The source media remains in the author's permascroll or a registered external scroll. The Surface
stores decisions about where that media is shown; it does not rewrite an image whenever the author
crops or rotates it. Its replay product is distinct from `Version` (ordered text spans) and
`Manifold` (Cells and dimensions).

## One editing walk

An author imports a scanned map as one addressable media span, makes `Surface S`, and places the
image on S. The placement operation has its own stable birth identity, P. Later operations crop P,
rotate it, and move it above another placement. The author then forks before the rotation:

```text
Make(S) -> Place(P, source map) -> Change(P, crop) -> Change(P, rotate)
                                  \
                                   -> Change(P, alternate alignment)
```

Both branches still refer to the same source media span. Asking for S at either branch produces a
different spatial map; asking for P's history shows the author's decisions without inventing new
primedia for the unchanged pixels. A later placement Q can quote the same source span and show it
again at a different size. The two occurrences share source provenance but have distinct placement
identities and histories.

## A third axis: depth or media time

The same placement idea can select a *range* from a richer source. A spatial `XYZ` Surface can place
a sub-volume from a scan and transform it into a larger three-dimensional arrangement. An `XYT`
Surface can place a video interval in a scene, or an audio interval on its time axis. Two branches
can choose different depth slices or media intervals while referring to the same source blob. The
Surface's third axis must be typed: spatial depth has length units, while media time has duration
units. Neither is a `d.created` UTC timestamp; a temporal Surface's zero is local to its own
arrangement.

| Surface domain | Source selection in a Place                                  | Replay query                                                     |
| -------------- | ------------------------------------------------------------ | ---------------------------------------------------------------- |
| `XY`           | Image crop in intrinsic pixel coordinates                    | At `(x, y)`, ordered image contributors.                         |
| `XYZ`          | Volume box in intrinsic voxel coordinates                    | At `(x, y, z)`, source voxels and transforms.                    |
| `XYT`          | Video frame region and time interval, or audio time interval | At `(x, y, t)`, visual contributors; at `t`, audio contributors. |

For example, one branch of an `XYZ` Surface can show source slices `z = [40, 60)` while another
shows `z = [45, 55)` at the same destination depth. Likewise, an `XYT` Place can map source video
seconds `[12, 20)` to local seconds `[0, 8)`; a branch can select `[12, 16)` and map it across the
same destination interval for slower playback. Each result still names the original media span and
the exact Place birth that made it visible.

The Place operand must declare its source domain and half-open source range, then map that range
into the Surface's declared domain. Spatial transforms may mix `X`, `Y`, and `Z`. A temporal
transform maps source media time to destination time with exact rational scale and offset; spatial
axes do not rotate into time. A Change can alter the selected source range and transform while
preserving the source media identity and placement birth. Source range selection for compressed
video or audio needs a decode index; it cannot be implemented by guessing a byte subspan. Audio
mixing, depth compositing, and effects are separate replay or rendering rules to specify before
those domains become writable.

This extension also tests the name. If the persistent object becomes a general spatial or
spatiotemporal field, `Surface` may be too narrow; keep the kind number unallocated until the
coordinate contract and name are chosen. The two-dimensional raster contract below is the first case
to prove, not a promise that a depth or time axis is just another layer number.

## Logical Surface operations

Reserve `Structure` verbs 4–6 for a proposed Surface family; verb 7 remains unused. These are
logical operations, with the field encoding below proposed rather than implemented. They are
dispatched by verb, not inferred by parsing Cell contents.

| Verb                | Replay effect                                                                                   |
| ------------------- | ----------------------------------------------------------------------------------------------- |
| `SurfacePlace = 4`  | Mint a placement whose identity is this op; install its source, range, transform, and layer.    |
| `SurfaceChange = 5` | Replace one placement's crop, transform, or layer, retaining its source and placement identity. |
| `SurfaceRemove = 6` | Remove one placement from the selected Surface view; earlier versions keep it.                  |

Each op's context edge names the preceding edit to **S** on that branch, or `Make(S)` for the first.
The edge can pass through a rename or other valid annotation event that targets S; replay ignores
the annotation's geometry effect while validation retains the unbroken context chain. For these
non-Cell Structure verbs, use the generic plan's `sourceAt` context field. `sourceOpIndex` has no
Cell subject and must be zero. `parentIndex` still records hypertime ancestry and cannot replace the
Surface context edge. The op must be ancestral to the selected version; neither a global latest
pointer nor a numerically smaller op index is sufficient.

For this proposed encoding, flags select only the Surface verb: direction and `ValueKind` bits are
zero. `sourceAt` is the context predecessor, `value` is the target Place birth for Change and Remove
(zero for Place), and `span` is the typed operand for Place or Change (empty for Remove). The
presently unused `at`, `length`, `to`, `sourceLength`, and `linkId` fields are zero. No Surface verb
reinterprets document byte geometry as image coordinates.

`SurfacePlace` uses its own op index as the placement identity. `SurfaceChange` and `SurfaceRemove`
name that exact Place birth in `value`; on export, the target is serialized by its microversion name
and localized on adoption, never by a raw spool index. A Change requires a live placement on the
selected ancestral branch; Remove makes it absent. Changing a removed placement is refused.
Reintroducing the same source creates a new Place identity rather than mutating the removed one. A
change cannot retarget a placement to different source media: remove and place again, making the
provenance change explicit.

The first milestone admits decoded raster images with finite dimensions and a built-in affine
transform. A Place operand contains:

- one addressable source media span and its declared decoded width, height, orientation, and media
  type;
- a half-open crop rectangle in the image's intrinsic sample coordinates;
- an invertible affine map from those coordinates to local Surface coordinates; and
- an explicit layer number, with the Place birth's stable microversion name breaking ties.

A first-milestone Change operand replaces the source crop, affine map, and layer together. This is
one named edit: replay never exposes a half-updated crop or transform. The source span and decoded
geometry remain those of the Place. Coordinates and affine coefficients require an exact, bounded
numeric encoding, not host-dependent floating-point serialization. A versioned operand schema should
specify rational coefficients, half-open boundaries, pixel-center sampling, overflow limits, and
rejection of singular maps before the wire format is chosen. General warps, masks, blend modes, and
vector source geometry are later extensions; silently treating them as affine raster placements
would change what old readers display.

## Vortex-defined transforms

A later Place or Change operand may select a Vortex transform instead of a built-in affine map.
Vortex programs are already representable as persistent opcode Cells: `VortexHost` can promote an
entry opcode subgraph into a Store. A Surface operand must pin the **entry Cell birth and the exact
program version** that supplies its complete opcode and dependency graph. A local symbol name or the
latest version of a module is insufficient: editing the program later must not silently change an
earlier Surface version. Export and adoption must translate program references by stable operation
and version names, with all dependencies available or explicitly unresolved. The first
implementation can require the program to be in the same store; cross-store calls need the
publication boundary designed separately.

The proposed function contract maps an output query to a source query:

```text
surface.map(destination coordinate, source-domain metadata, immutable parameters)
    -> outside | one or more typed source coordinates with weights
```

The destination coordinate carries the Surface's fixed axis signature (`XY`, `XYZ`, or `XYT`). A
result carries the source's declared axes and must lie within the Place's selected source range.
Mapping from destination to source handles folds and many-to-one projections without demanding an
inverse function. A transform could bend a volume through space, select a non-planar slice, or remap
a video interval through time. A future Place source could itself be a pinned Surface or other
spatial structure rather than raw media; replay would evaluate the returned coordinate in that
source's own versioned coordinate frame. Recursive structure references need a cycle check and
bounded evaluation depth. A Change may choose another pinned function snapshot or parameters while
retaining the placement birth and source identity.

For example, a pinned function could map each `(x, y, z)` in a destination volume to
`(x, y, z + height(x, y))` in a source scan, where the height field is another pinned input. The
result is a curved section through unchanged source voxels. A later branch can reference a revised
function or height field, while the earlier branch still means the original mapping.

General Vortex execution is too broad for authoritative replay: it can change arena Cells, and
`VortexVM::run()` bounds cycles but does not alone establish a pure spatial function. Define a
versioned `surface.map` profile that reads a frozen program snapshot, exposes no clock, randomness,
network, mutable Store, or UI state, and allows only bounded scratch computation discarded after the
call. Give it instruction, memory, recursion, and output-size budgets. Numeric operations used for
authoritative coordinates need specified cross-platform behavior; device shader approximations may
draw the result but cannot decide source provenance. Missing programs, contract violations, budget
exhaustion, and cycles produce explicit unresolved placement results, never empty space or fallback
to a different function.

A built-in affine placement has conservative transformed bounds for a spatial index. An arbitrary
Vortex function may have no cheap inverse or provable bounds. Without validated conservative
destination bounds, replay must include that placement in each query rather than culling it and
silently losing possible source material. Optional bounds are an optimization claim checked against
the function's contract, not an authority to redefine its output. A future synthesis-capable profile
would need provenance saying its samples derive from the pinned program and inputs; it could not
masquerade as a quotation of bytes that do not exist.

The present 64-byte `CompactOpNode` does not hold a source address, a crop, and a full affine map
inline. The proposed compact node therefore points through its ordinary `span` to one immutable,
versioned binary *operation operand* in primedia. This is not text to parse from a Cell: the Surface
verb gives the bytes their type, and the op atomically applies the decoded operand. The operand can
contain source-span references; export must globalize them by scroll identity and import must
localize them, without treating local scroll IDs or op indices as portable bytes. If rewriting such
references would change the operand's authored bytes, the final wire format must carry a separate
typed operand record instead. Preflight the decoded operand, numeric bounds, and referenced scroll
identity before appending its bytes, then append operand and op as one validated edit. Source media
bytes may be unavailable to a reader even when their address and declared geometry are known; that
is an unresolved rendering state, not permission to forget the placement. Whether a later format can
fit a useful restricted transform inline should be measured before committing to this encoding. The
64-byte node, 64 KiB Merkle pieces, and append-only spool remain invariants.

## Replay at a selected version

`rebuildSurface(surfaceBirth, selectedVersion)` walks the selected version's ancestral path in
hypertime order. It validates the named Surface birth and considers only operations whose resolved
context chain belongs to that birth. Other Surfaces, Xanadoc text ops, Cell edits, and unrelated
Links do not change its map. It then folds the relevant Surface verbs:

1. Start with an empty map. The birth establishes coordinates and identity, not visible pixels.
1. On Place, validate the operand and add a live placement keyed by that Place birth.
1. On Change, find the live placement on this branch and replace its spatial parameters atomically.
1. On Remove, mark that placement absent in this view without deleting its op or source primedia.
1. Sort live placements by layer and stable birth name for a deterministic front-to-back order.
   Build an ephemeral spatial index over their transformed bounds for queries and rendering.

At a Surface point, a query finds covering placements, applies each inverse affine map, tests the
half-open source crop, and returns the ordered source samples plus placement identities and
transforms. Alpha compositing is a renderer concern; an occluded or transparent sample can still
have a provenance candidate. An unavailable decoder or source yields an explicit unresolved
placement, not an empty region that pretends no source was authored. The spatial index and decoded
textures are replay caches, never authored Cells or operations. Incremental application must agree
with a full ancestral rebuild, including after a fork or a Change that moves a placement across the
indexed plane.

For a Vortex placement, the same query calls its pinned `surface.map` function in the restricted
profile and resolves each returned source coordinate. The replay fold stores the function reference
and parameters; it does not execute the program while folding unrelated ops. Query caches include
the Surface version, placement birth, program snapshot, source identity and any source-structure
snapshot, parameters, and coordinate. A program edit or a branch cannot retroactively change an
earlier cache entry.

For a future `XYZ` or `XYT` birth, this fold is still selected by Surface identity and hypertime
version, but its coverage index has the birth's declared axes. A query supplies a coordinate in
those axes, applies each placement's affine or Vortex destination-to-source mapping, and returns
typed source contributors. The fold must not answer a time query with a depth value, or fabricate a
pixel result for an audio-only placement. A branch-local transform changes only the projection of
source samples into that Surface; the source stream and its intrinsic coordinates remain fixed.

This contract makes hypertime transforms inspectable: a reader can hold one source region fixed
while comparing its transformed occurrences at two versions. Undo is selection of an earlier
version, and an alternate transform is a branch. No operation mutates a prior node or the source
bytes. Surface rendering at a particular resolution may use different sampling hardware, but the
authored crop, transform, ordering, and source identity must replay identically.

## Addressability and links

A compressed PNG or JPEG region is generally not a contiguous interval of its encoded bytes. A
volume box or a video or audio interval may have the same problem. Quoting a crop by reusing the
whole image's `GlobalSpan` preserves *blob-level* source identity; it does not by itself make every
pixel a separately addressable primedia span. A Surface occurrence therefore needs both the source
media span and a decoded source-region coordinate. Link discovery must not claim that today's
span-only Link endpoint automatically follows an arbitrary pixel rectangle through a transform.

A later media-region endpoint or tile-addressed manifest could make sub-image links precise. It
would name the source media, a stable intrinsic region, and perhaps the source decoder geometry; the
Surface's inverse transform would project that endpoint into every visible occurrence. Until then,
links to the whole media span are valid, while region-specific link behavior is explicitly unbuilt.
The same distinction applies to quotations: two Surface placements may share one image span without
sharing their placement histories or claiming byte-level identity for every cropped pixel.

## Boundaries, formats, and proof obligations

This vision depends on generic Make and its ancestry-validated context chains. Allocating kind 3 to
Surface would replace its reserved status in that plan. The existing text `Insert`, `Delete`,
`Rearrange`, and `Transclude` coordinate fields are one-dimensional byte offsets; dispatch must
never feed a Surface op to `Version::insert()` or reinterpret `at` as pixels. `Manifold` must also
ignore Surface verbs rather than minting CellSlots. Adding Surface verbs and an operand schema
requires a versioned `ops.nodes` and export/import change with loud refusal by older readers;
generic Make's planned V5 may need a further bump if it lands first. `xudu-dump` should show the
Surface birth, placement births, target references, operand version, and context chain.

The minimum evidence for promotion from vision to implementation plan is:

- two interleaved Surfaces in one store, each rebuilding only its own placements;
- forks changing the same placement differently, with replay and incremental results agreeing;
- crop, translation, scale, and rotation retaining source-span and placement provenance;
- a pinned Vortex transform that bends a volume or remaps media time, gives the same typed query
  result after save/load and publication, and stays unchanged when the program later branches;
- explicit unresolved results for absent programs, failed contracts, fuel exhaustion, and cycles,
  plus correct queries when a function has no safe spatial bound;
- an exploratory `XYZ` volume box and `XYT` video or audio interval that keep spatial depth and
  relative media time typed and distinct from UTC annotations;
- deterministic ordering of overlapping placements before and after export/import renumbers ops;
- refusal of off-branch targets, wrong-Surface placements, broken context chains, malformed or
  unavailable operands, invalid media geometry, singular transforms, and numeric overflow; and
- an explicit comparison with a Slice of placement Cells showing which Surface operation or query
  needs its own replay contract rather than only a different UI projection.

The last check is decisive. If a Slice projection provides the same authored operations, stable
identity, branch behavior, and coordinate queries at acceptable cost, Surface should remain a
projection and the final Make-kind slot should remain free.
