# Independent pixel review

The historical OpenGL/OpenGLES overlay mismatch is localized to document glyph pixels. The two toast
panels and the rest of the frame are byte-identical, the glyph cores align, and the changed RGB
values are nearly balanced around zero. This is consistent with a small glyph edge-coverage or
intensity difference; it does not resemble a global opacity or color shift. The available captures
do not identify the cause.

I inspected the lossless PPM captures from the sixth comparison, its baseline-control run, the
sequential rerun, the final rerun, and the seventh X11 reproduction. Pixel counts below use the
800×600 RGB frames (480,000 pixels). The historical GL/GLES PPMs and raw per-run PPMs remain in
`/workspace/spatial-build`; the checked-in reproduction PNGs and commands are linked in the
[seventh reproduction report](evidence/reproduction/report.md).

| Comparison                                    | Changed pixels | Maximum channel delta | Spatial result                                              |
| --------------------------------------------- | -------------: | --------------------: | ----------------------------------------------------------- |
| Historical GL vs GLES overlay                 | 6,220 (1.296%) |                    23 | Half-open box `[173,554) × [309,424)`, within document text |
| Historical GL vs current seventh GL overlay   | 6,220 (1.296%) |                    23 | Same glyph-region pattern                                   |
| Current seventh GL vs GLES overlay            |              0 |                     0 | Exact match                                                 |
| Historical GL vs GLES toast panels, y=482–572 |              0 |                     0 | Panels and surrounding pixels match exactly                 |
| Plain Vulkan vs GL reference                  | 1,193 (0.249%) |                     4 | Within the existing Vulkan allowance                        |

The historical GL/GLES overlay differences have a mean signed GL-minus-GLES delta of only
`[+0.020, +0.021, +0.015]` per RGB channel over the changed pixels. There are 3,250 changed pixels
with a negative net RGB delta and 2,970 with a positive one. In 6,179 of 6,220 changed pixels the
three channels move in the same direction, and 5,812 changed pixels are approximately grayscale in
both images. This pattern is localized neutral intensity variation, rather than a directional tint
or a frame-wide opacity shift.

The darkest glyph core is unchanged: at a mean-RGB cutoff below 32, both historical images have the
same 516 pixels. At cutoff 64 their glyph masks have an intersection-over-union of 0.989; at cutoff
128 it is 0.945. The disagreement grows at lighter edge cutoffs. That combination—matching dark
cores and larger differences among lighter pixels—points to antialiasing/coverage detail at glyph
edges. RGB screenshots alone cannot distinguish changed sampled coverage from another source of edge
intensity, so this is a description of the rendered pixels, not a diagnosis of the responsible
stage.

The comparison is repeat-sensitive. Historical sixth-gate baseline-control and candidate captures
reproduce the same GL/GLES hashes and the same 6,220-pixel mismatch. A separate sequential capture
instead produced exact GL/GLES equality. The sixth-gate wrapper explicitly selected X11 under one
Xvfb invocation. The seventh reproduction also selected X11, but used a fresh Xvfb and XDG profile
per capture; both backends and repeated captures then matched byte-for-byte. Thus the data do not
support an offscreen-versus-X11 explanation, and the fresh-session difference alone does not
establish a cause. The comparison threshold remains unchanged: GL/GLES require exact equality;
Vulkan retains its existing 1% allowance with channel tolerance 1.

There is also a distinct Vulkan repeatability observation. The candidate's sequential Vulkan
thread-1 and thread-4 frames differ in 6,238 pixels (1.300%), with maximum channel delta 23 and the
same document-glyph bounds. The baseline-control thread pair is byte-identical. This warrants
retaining the Vulkan thread comparison as a separate open finding; it does not establish that the
OpenGL/OpenGLES and Vulkan observations share a cause. Plain Vulkan versus GL remains within the
configured gate.

As source context, GL, GLES, and Vulkan use the shared glyph fragment shader, which samples a
single-channel glyph texture and uses its coverage to mix foreground and background colors. Their
texture setup and mip generation are backend-specific. That makes sampled glyph coverage one
relevant part of the render path, but the captures do not include atlas coordinates, mip contents,
or draw uniforms. No deterministic same-glyph/different-cache-order experiment was established by
this pixel review, so I make no source-level cause claim.

The visual comparison is narrow: it diagnoses where and how the captured RGB pixels differ, and
confirms stable exact equality in the fresh seventh reproduction. It does not resolve why the
historical GL image differs, or why the sequential Vulkan thread pair differs from its baseline
control. The pixel thresholds have not been relaxed.
