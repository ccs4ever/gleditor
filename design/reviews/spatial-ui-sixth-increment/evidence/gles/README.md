# GLES journey evidence

These captures use the frozen candidate executable
(`404c21bc477c8dd5324bbba7957545f9de50ba52b0f96f38b94dfb3bd68a6cbd`) and shared library
(`6964780c202365c8da5ada84fad51f0d21fc621a0c2db72ad0ebc792b75ffcf6`) on GLES at 800×600 with the UI
font scale set to 1.5 through its keyboard control. Each run used fresh data, config, and cache
directories. `argv.txt` and `session.log` record the input stream and diagnostics; lossless PNGs are
beside them. `raw-ppm-sha256.txt` identifies the original PPM captures kept outside the repository.
The PNG/PPM comparison reports zero changed pixels for every final frame.

The normal fixture contains 80 lines and 7,439 bytes. The forced fixture contains the same 80 lines
and 7,438 bytes, with an authored Ctrl+Return at byte 3719. In both runs the native preview reports
the target occurrence across pages 1–2 of 2, the Enter capture shows the source selected, and the
incremental wheel captures reach visible continuation text (P041 and later). The forced run shows
the continuation after its authored page break. `normal/07-pan-4.png` and `forced/09-pan-6.png` are
representative continuation frames.

The first forced run's activity `before` snapshot was not proven to precede Enter, so it does not
establish an Enter visit delta. `supplement-preenter/` is a fresh forced-break run with
line-buffered logs and an explicit 20-second wait between preview capture and Enter. Its pre-Enter
snapshot contains the initial root visit only; the Enter snapshot contains one added `visit2` record
and current visit index 2; the post-run snapshot still has two visit records. The source stores'
operation dumps are byte-identical at all three snapshots. In the supplement, Back visibly returns
selection to `default`, and Origin then selects `Doc 3`.

Earlier exploratory runs are retained in the `preliminary-*` directories and are not acceptance
captures.
