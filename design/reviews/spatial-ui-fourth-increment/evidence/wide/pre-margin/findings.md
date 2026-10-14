# Settled-build 800×600 pre-margin candidate

The settled build completes the UI flow at 800×600 on OpenGL and OpenGL ES. The 654-byte selected
source spans all eight explicit lines vertically, and the 23-byte destination is present in the link
state. `Doc 2` is selected with `reading: left member 1`; source-operation hashes remain unchanged
before/after.

The pixel-identical OpenGL/OpenGL ES `01-enter` frames have PNG SHA-256
`5637be5770fab11c5f358789031ce06523f2252b867ec533e97b0e379a1e8028` and raw PPM SHA-256
`c1219387b35df4cb0e17c491af2b14875247648ca065cd9cbc9cf749768ca006`. OCR recovers all eight lines,
but drops the initial `W` in lines 4 and 5 because those glyphs meet the screen's x=0 edge; the
image pixels show the text beginning clipped there. This is a pre-margin candidate, not accepted
final evidence.

The captures and run records are under this directory. Raw PPMs and private profiles are outside
Git; hashes and locations are recorded in `manifest.sha256`, `profile-manifest.tsv`, and the wide
archive manifest. Vulkan and the 640×480 / 1440×1000 OpenGL cases remain unrun pending the shared
`UiMetrics` safe-margin adjustment.
