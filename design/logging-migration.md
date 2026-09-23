# Categorized logging migration

## Current foundation

`spdlog` is a required pkg-config dependency for the library and the standalone Xanadu engine
programs. `<gleditor/logging.hpp>` creates named stderr loggers on first use and loads the standard
`SPDLOG_LEVEL` setting. For example:

```sh
SPDLOG_LEVEL='warn,text.layout=debug,xudu.links=trace' ./build/xudu --headless
```

The default spdlog level is `info`, so debug and trace calls remain compiled in but silent until
enabled. The `GLEDITOR_LOG_*` macros check the category level before evaluating formatting
arguments. They do not use spdlog's compile-time debug macros, which would remove calls from the
default build. Warnings and errors remain visible by default. Categories already used are
`text.layout`, `render.scene`, `xudu.edit`, `xudu.links`, `zigzag.action`, `zigzag.edit`, and
`vortex.edit`.

## Conversion order

1. Inventory each remaining `std::cout`, `std::cerr`, `std::clog`, `printf`, `fprintf`, and SDL
   logging call in `src/` and `apps/`. Mark it as a diagnostic, user result, benchmark marker, or
   protocol output before editing. The initial audit found roughly 300 stream call sites; this
   number includes legitimate output and is not a count of logs to replace.
1. Convert diagnostic chatter in `src/doc.cpp`, `src/renderer.cpp`, `apps/xudu/session.cpp`, and
   `apps/xudu/beams.cpp` first. The initial conversions in those files establish `text.layout`,
   `render.scene`, `xudu.edit`, and `xudu.links`. Continue with `src/glyphcache/`,
   `src/render/{gl,vulkan}/`, `apps/xudu/main.cpp`, and `apps/zigzag/zigzag_visualizer.cpp`. Use
   more specific categories such as `render.gl`, `render.vulkan`, `render.timing`, `xudu.input`, and
   `zigzag.view` when a subsystem needs an independent switch.
1. Convert internal Xanadu engine diagnostics in `apps/common/xanadu/` to categories such as
   `xanadu.store`, `xanadu.swarm`, `vortex.vm`, and `vql.parser`. Keep the engine free of SDL and
   graphics dependencies. Errors that are returned to a caller should still be returned; a log
   message is not an error handling mechanism.
1. Route `src/render/diagnostics.cpp` to `render.driver` while preserving its deduplication,
   notification queue, `setLogging(false)`, strict mode, and no-throw driver callback. Keep
   collection and UI notification independent of the logger level.
1. Add live application settings after the diagnostic sites are classified. The plain editor should
   use its own configuration (currently `config.tsv`; migrate that to the documented YAML format
   separately). Xudu and Xuzz should define logging defaults and schema in `system://settings` via
   `defaultSettingSpecs()`, load them through `SettingsConfig`, and reapply levels when the settings
   store changes. Zigzag should use sovereign system slice cells with `d.schema` and `d.notes`. An
   explicit process environment setting should remain an emergency override available before a store
   loads.
1. Add a repository audit for newly introduced direct debug prints in library and engine code.
   Allowlist intentional stdout protocols and entry point output. Keep the audit about new
   diagnostics, not a blanket ban on streams.

## Output contracts to preserve

- REPL results and compiler output from `vquery`, `vpl`, and `vprolog` remain on stdout. `VPL`'s
  print operation is language behavior, not telemetry.
- `xudu-dump` output and `xudu-swarm-peer`'s `port` and `pubkey` lines remain machine-readable. The
  swarm test parses the `port` prefix.
- `First page rendered:` and `Complete render settled:` are parsed by the KJV benchmark scripts.
  Benchmark test output and `--do` command results also need classification before migration.
- Do not log document content, permascroll bytes, passwords, keys, or identity secrets. For edits,
  log operation type, offset, length, and version only.

## Verification for each batch

Run `make -j$(nproc)` and `make -j$(nproc) test` headless. For a category changed in the batch, run
a focused headless path with its level off and on; confirm that stderr gains the tagged diagnostic
only when enabled and stdout is unchanged. Recheck CLI and benchmark output contracts. For hot edit,
render, and driver paths, measure latency and allocations with debug off before adding expensive
formatting or asynchronous sinks.
