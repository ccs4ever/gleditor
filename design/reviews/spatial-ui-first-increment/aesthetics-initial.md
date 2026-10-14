# First native spatial increment: independent visual preparation

Reviewed 2026-10-13 by the independent aesthetics reviewer, before product implementation. This is
composition guidance from inspected design assets and existing code, not a native application pass.

## Sources inspected

- `.agents/skills/xuzz-ui-design/SKILL.md`, including the independent aesthetics role.
- `design/view-system-implementation-plan.md` §5 and
  `design/projects/view-reviews/04-aesthetics.md`.
- `design/projects/start-spatial-ui-overhaul.md` and the prototype's `contracts.md`.
- Actual final `investigation-workspace-2026-10-13/01-quiet-reading.png` and
  `02-expanded-investigation.png`; their concepts and refinement notes.
- Actual final `spatial-elements-2026-10-13/04-layer-console.png` and its concept notes.
- Existing `ui::Theme` and `LinkContext`/Walks interfaces. The native palette uses quiet dark
  surfaces, a light text hierarchy, cyan accent, theme type roles and em-based metrics. LinkContext
  already owns the real navigator boundary and exposes retained selection, preview and activity.

## Composition decisions for the implementation

1. Give the anchored reading page the strongest readable content surface. Put optical effects on
   tool edges, away from glyph backing. Use the theme's title/label/caption roles and metrics
   instead of copying the concept images' bright purple ornamental rims and background reflections.
1. Quiet mode retains one compact selected-Link face: identity, active side, independent left/right
   cursor/count summaries, unavailable membership and explicit Enter/Return. Expanded mode exposes
   individually reachable member and occurrence rows. Folding changes presentation only.
1. Preserve the native page's host bounds and reading position while tools fold. The generated quiet
   and expanded images shift and resize the page; their own concepts file calls this out. Those
   images therefore establish composition intent, not an anchor-position acceptance result.
1. Stored pages/cells keep sharp content bounds. View-only tablet chrome and controls use the shared
   softer/seamed treatment. A rectangular stored-content face inside a soft tool shell is coherent;
   rounding the actual stored page to imitate the concept surround would erase that distinction.
1. Retain the authored Link hue. The inspected frames correctly converge the cyan Comment through
   the One Link port; no conduit terminates at a package checkbox or invents member pairing. Gold
   remains reserved for actual shared source addresses. State emphasis uses
   outline/weight/brightness with a non-color cue, preserving hue identity.
1. Preserve the quiet frame's complete unavailable third right member and the expanded frame's
   explicit all-members action. Density reduces visible rows in a bounded list; it does not reduce
   stored membership or silently drop the retained selection.
1. Keep reading place, selected/previewed endpoint and activity origin visibly distinct. The
   expanded concept's pinned comparison state is labelled separately from current visit. Native
   compact and list equivalents must preserve that distinction even before comparison services are
   available.

## Evidence required for final native review

Inspect actual quiet and expanded application captures on the same reading state, member and
occurrence browsing, unavailable members, Enter then Return/Back, narrow and enlarged text,
reduced-motion transitions, and the compiled-backend comparison frames. Check legibility, label
collision, theme consistency, anchor stability, full endset access and one focal hierarchy.

No native frame has been inspected yet. No implemented journey, contrast threshold, backend parity
or motion acceptance claim is made by this preparation review.
