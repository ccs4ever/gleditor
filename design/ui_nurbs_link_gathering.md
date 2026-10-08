# NURBS paths and grouped link ribbons

A many-to-many link has two ordered endsets and one identity. Its drawing should expose that
structure without suggesting that the author paired individual left and right members.

## Chosen presentation

Visible occurrences branch into a left gathering point and out of a right gathering point. A short
common trunk joins the gathering points. Each branch attaches only to its exact range; gaps between
members remain gaps. Gathering points are view geometry, not cells, endpoints or new stored objects.
The panel protects the active document occurrence (or current reading point) by choosing a safe
strip around it and fitting its text and controls there when space permits. Narrower panels use
fewer button columns when the height budget allows. There is no new click target or navigation mode:
a ribbon selects the whole link, while the existing link context chooses members and occurrences
independently before explicit entry.

The panel draws above the overview and below menus and dialogs. A missing representative endpoint
does not suppress another member's visible branch; the trunk is staged once when both gathering
points have visible attachments.

Different links retain separate trunks, type hues and authority. Selecting one raises its opacity
and lets the others recede. Authored links use quiet stationary filament shading. Transclusion keeps
its optical glass surface, so similarity of geometry does not erase the distinction between an
explicit relation and shared primedia. Tethers remain narrow tapered feedback with a clear pointer
pixel and the existing cancellation behavior.

A single hub was rejected because it collapses the two endsets into one junction. A carousel would
hide unchosen members and add a browsing mode beside controls that already exist. It remains a
possible presentation for a future dense endpoint panel, rather than the default beam arrangement.

## Geometry and ownership

The library owns borrowed clamped NURBS paths of degree one to three, positive rational weights,
analytic tangents, and bounded sampling. It accepts interior knots; application routes currently use
single clamped spans. Unit weights are the default. Samples carry cumulative chord distance, so
taper and longitudinal fade follow distance instead of spending equal distance on unequal parameter
intervals. Adjacent ribbon segments share their endpoint normals.

Sampling uses fixed stack storage with at most 256 segments per path. Staging exact visible link
attachments uses the sum of the endsets rather than a Cartesian product. Representatives retained
for anchor resolution are not authored pairs. GPU buffers keep their allocations between settled
frames. Existing margin layout and endpoint discovery have their own costs; this change does not
claim that the entire renderer is allocation-free or that endpoint indexing has constant cost.

Native geometry belongs in C++ because it prepares GPU instances and owns device buffers. Link
semantics, provenance, navigation commands and reader activity stay in the application engine. No
store format or authorial edition decision changes.

## Live configuration

`system://layout` exposes `beams.curveHandleShare`, `beams.curveWeight`, `beams.curveSegments`,
`beams.gatheringShare` and `beams.inactiveLinkAlpha`, with schema defaults and bounded ingestion.
`curveSegments` replaces the former `bypassSegments` control. The drag tether keeps its live sag,
width, texture, pointer-clearance and reduced-motion settings.

## Review and acceptance

The UI skill's independent UX and aesthetics agents were requested, but both stopped at a service
usage limit. The implementation therefore receives separate UX and aesthetics passes by the lead,
with this limitation recorded instead of an independent approval claim.

Acceptance requires captured GL, GLES and Vulkan frames, repeated segment-join and rational-curve
checks, a high-fanout staging check, actual keyboard and pointer endpoint browsing, and a comparison
of visited stores before and after navigation. Full J16 authoring, distant-cell activity branching
and live assistive-technology operation need separate evidence; a prepared native link fixture is
not proof of authoring through the interface.
