# Spatial journey prototype · J17–J32

Open [the interactive prototype](index.html) or [the captured journey gallery](evidence/index.html).
The gallery contains the complete action sequence for every journey, independent negative routes,
layout captures and identity/effect traces. It links each image back to its exact prototype state.

This represents the concepts before implementation. Publication discovery, search, cryptographic
verification, acquisition, media playback, authoring, commit status and lifecycle outcomes are
scripted fixtures. The prototype performs native browser actions, focus, list filtering, scrolling
and responsive layout. It does not contact stores or change the Xuzz application.

## Walk a journey

1. Select J17–J32 in the Journey guide.
1. Choose Main journey or an independent Fixture route.
1. Choose the next named action in Available actions. Its result replaces the surrounding panels;
   the reading page changes only when the route explicitly enters or restores a place.
1. Read the resulting state and proposed effect receipt, then select Next task.
1. Use List view, Larger text and Reduce motion to compare presentations. Inspect reference records
   or browse complete inventories to reach counted items outside the visible window.

The guide is a facilitator tool. It can restart routes or jump to a task by replaying its
prerequisite states. Scripted external events have separate simulator controls. Sequential action
gating makes the test script reproducible; it does not propose a mandatory wizard for the future
product.

The calm graphite faces, silver rims, violet selection, cyan Comment relations and gold
shared-source identity inherit the
[spatial design plan](../../3d-spatial-hypermedia-navigation-implementation-plan.md) and the
[earlier mockups](../../mockups/). Loom ancestry uses concrete fixture parents, with a separate
same-operation correspondence tether. Its legend distinguishes stored ancestry, view correspondence,
filtered ordering and actual shared source addresses.

## Review and contracts

- [Journey specification](../../ux_workflow_spatial_concepts.md)
- [Proposed identity, query and authoring contracts](contracts.md)
- [Final representation review](../../reviews/spatial-concept-journeys-2026-10-13-rerun/report.md)
- [Transition validation](evidence/validation.json)
- [Native browser run](evidence/run.json)
- [Original static review](../../reviews/spatial-concept-journeys-2026-10-13/report.md)

Each pack has an accompanying coverage map. The three requested Luna runners authored separate
packs, then reviewed another runner's journeys. The lead checked every transition and resolved
count, label, identity and missing-action findings before closing the review.

## Reproduce

No product build or Node dependencies are added to the repository. The page loads its compiled
fixture data through a plain script and can be served by a local static server:

```sh
python -m http.server --bind 127.0.0.1 8000 \
  --directory design/prototypes/spatial-journeys-2026-10-13
```

Open `http://127.0.0.1:8000/index.html`. To rebuild the fixture data and validate it:

```sh
python design/prototypes/spatial-journeys-2026-10-13/tools/build.py
python design/prototypes/spatial-journeys-2026-10-13/tools/validate.py
python design/prototypes/spatial-journeys-2026-10-13/tools/verify_evidence.py
```

`tools/render.cjs` records all routes with externally installed Playwright and `/usr/bin/chromium`.
Set `PLAYWRIGHT_MODULE` to that installation's module path. The renderer starts a temporary loopback
server because managed Chromium blocks file URLs. Use the repository's headless environment
settings; its native screenshot, accessibility-tree and focus evidence applies only to this
prototype. `JOURNEYS=J22` can refresh a corrected journey while retaining unaffected run records.

The post-implementation evidence column in the specification remains outstanding: real store diffs,
network/cache behavior, cryptography, actual media playback, authoring protocol reliability,
sovereign keymap integration, assistive-technology sessions and graphics-backend behavior.
