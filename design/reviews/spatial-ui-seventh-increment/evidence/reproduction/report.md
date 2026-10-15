# Seventh overlay reproduction

This is a read-only reproduction at HEAD `67435ffdfbddbd536a7d99ea43c465ccbcdd432a`. The candidate
artifacts were unchanged across the run: `build/gleditor` SHA-256
`8ff8d62122112af470f9b6583f0f9a1029a6088aa95c5cac4bef569c674451aa`, `build/xuzz`
`404c21bc477c8dd5324bbba7957545f9de50ba52b0f96f38b94dfb3bd68a6cbd`, and `build/libgleditor.so.0`
`6964780c202365c8da5ada84fad51f0d21fc621a0c2db72ad0ebc792b75ffcf6`.

Six sequential captures used X11 under `xvfb-run -a -s '-noreset -screen 0 1440x1000x24'`, with
fresh XDG data/config/cache directories per invocation and software rendering. The four overlay
invocations used the same sample and arguments:

```sh
--profile --strict-diagnostics \
  --toast 'error:GL_INVALID_ENUM in glEnable(0xdead)' \
  --toast 'warning:overlay parity check' \
  --click 300,265 --select 20,34 --screenshot ... \
  tests/samples/quick_brown_fox.txt
```

Two used OpenGL and two OpenGLES. Two more OpenGL captures used the same arguments without the toast
options. Exact arguments, logs, exit codes, and PNGs are stored per run.

Every process exited 0. Repeated OpenGL overlay frames were byte-identical; repeated OpenGLES
overlay frames were byte-identical; repeated plain OpenGL frames were byte-identical. Both current
GL backends produced the same overlay frame. The overlay changed 17,840 pixels compared with the
plain frame, confirming that the toast and caret path appeared in the capture.

The historical comparison's OpenGL overlay PPM has SHA-256
`e217b9fb9c8800b089c035a94754a8fa1a4f301f6815679e9ad0822abeb78b2b`; its OpenGLES overlay PPM is
`7763cbc52147ac6319dff50735e179fe59488b45d2a1fc413b02b794b6a5eaa8`. Both current OpenGL overlay
captures have the latter hash, matching current OpenGLES. The historical OpenGL frame differs from
the current OpenGL frame at 6,220 pixels (maximum channel delta 23). The recorded sixth-gate wrapper
also explicitly set `SDL_VIDEODRIVER=x11` and ran under Xvfb, so the available evidence does not
support attributing this difference to offscreen versus X11 rendering. The older comparison ran its
backend sequence inside one Xvfb invocation; this reproduction used a fresh XDG profile and Xvfb
invocation per capture. The alternate glyph raster is present in the historical GL capture, but did
not recur within this reproduction's repeated captures. The precise state difference remains
unresolved. This investigation does not change the comparison threshold.

The six original PPMs remain under `/workspace/spatial-build/seventh-repro/raw`;
`raw-ppm-sha256.txt` records their hashes. The checked-in PNGs were converted losslessly from those
PPMs.
