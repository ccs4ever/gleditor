# Selected occurrence anchor motion

Implemented in the native xuzz presentation on 9 October 2026. This extends the current link-colour
margin brackets toward the selected occurrence's highlight contour. It does not change the NURBS
inter-document ribbon arrangement or enable the deferred shallower-angle solver.

## Behaviour

- The active side's explicitly selected document occurrence extends its true top and bottom ends.
  Changing side or occurrence retracts the outgoing ends while the new occurrence extends. Dismiss
  retracts both ends. Camera motion and caret focus alone do not choose another occurrence.
- Motion uses a monotone smoothstep over 0.25 seconds. A reversal starts from the current progress.
  State is bounded to the current and one outgoing occurrence; a third rapid distinct target
  replaces the least visible outgoing track.
- Rails and ordinary caps remain in place. Thin, darker strokes retain the link hue and use the
  existing bracket weight. The extension follows blank line-edge space to the margin-facing covered
  contour on the terminal text line. Square turns retain the bracket's visual language.
- Page continuations retain their open ends. A viewport boundary creates no new terminal. Sparse
  endset gaps acquire no new stroke or semantic coverage.
- Accents write colour only, preserving the underlying picking tag and depth. Their path uses the
  named glyph-surface depth so blank glyph rectangles do not occlude it in Vulkan. They write no
  store operations or completed-reader activity visits.
- `beams.anchorReachSeconds` and `beams.anchorReducedMotion` are live layout settings. Duration is
  bounded to 0–2 seconds; zero duration and reduced motion cut to the final state without replay.
  Native reduced-motion evidence uses a prepared settings fixture; changing this setting through the
  user interface has not been validated in this batch.

## Geometry and safe fallback

Highlight fractions and retained uploaded glyph quads supply the meeting point. Raster ink rows,
computed while copying the glyph bitmap, distinguish blank inter-line space from the full-height
highlight rectangle. Other glyph ink blocks a route. A terminal outside its text band, incomplete
multi-quad cluster geometry, media or cell content keeps the existing bracket and glow.

The lane allocator reserves corridor clearance plus half a stroke beyond every authored terminal,
independent of selection progress. Continued paper-edge ends retain their previous bounds. This
addresses the new extension's extent; it does not certify the allocator's existing dense overlap or
ordinary stroke-tip accounting.

Retained per-cluster metadata grows from the previous 12-byte byte/character record to 28 bytes:
quad centre, packed extent and two ink-row bounds. This is ephemeral page memory, not an on-disk
format change. No reshaping or GPU readback is required during the animation.

## Validation

The full build, formatting and lint pass. All 82 focused tests pass: 34 library geometry/automation,
15 motion/link-panel and 33 settings/margin-lane checks. These cover geometry, automation parsing,
exact selection identity, extension/retraction/reversal, bounded settings, reduced-motion cuts and
finite settling. Native captures use isolated headless software-rendered environments and prepared
KJV/notes fixtures.

Diagnostic captures `build/anchor-reach-opengles-04/` and `build/anchor-reach-vulkan-04/` use a
one-second duration to expose intermediate states in this slow software renderer. Their motion
contact sheets show Left → Right → Left → Dismiss. Vulkan's initial `left.png` is intermediate; its
later returned-left state reaches the glow. They establish state progression, not measured default
frame smoothness.

Final default-duration reruns are in `build/anchor-reach-{opengl,opengles,vulkan}-10/`. Each
captures Left → Right → Left → Dismiss in an isolated environment. Saved source-store hashes remain
identical.

The reduced-motion run `build/anchor-reach-opengl-07/` uses a prepared one-second duration with
reduced motion enabled. Its first and intermediate Cross/Dismiss anchor-region images are
pixel-identical to the final corresponding state: there is no growing/retracting intermediate
geometry.

The OpenGL sparse/continuous run `build/anchor-reach-continuous-08/evidence/` has eight captures,
exact authored focus ranges and unchanged source hashes. Its continuous Psalm passage keeps the page
gap open between pages 622 and 623; an obstructed terminal retains the ordinary cap. This verifies
the sampled continuation geometry, not animation timing at every page break.

The generic renderer comparison in `build/anchor-reach-final-backend-compare/` passes across all
three backends, including glyph/page picking. This run excludes the broad orchestration battery,
which passed in the preceding beam-inspector batch.

Independent UI and UX reviewers accept the sampled same-hue contour connection, readable blank-space
routing, stationary rails and retraction. Tiny stepped joins are a minor polish issue. Broader dense
four-colour arrangements, partial ligature boundaries, RTL/bidi, italic overhang and multi-quad
fallback still need dedicated native evidence. This is scoped anchor-motion validation, not a full
J16 or navigation accessibility pass.

## Follow-up findings

The final09 Vulkan capture exposed partial accent occlusion by full-height glyph depth rectangles.
The final implementation places accents on the named glyph surface (`Doc::VBORow::depthStep`) while
retaining depth testing and colour-only writes. Current page transforms preserve world Z; future
rotated page planes must transform this local surface offset with their page frame.

An initially reported GLES09 text-row defect was retracted after original-image inspection: the
instruction paragraph is readable and pixel-identical between Left and Right on GLES04, 06, 09 and
10\. The apparent corruption was confined to the resampled preview.
