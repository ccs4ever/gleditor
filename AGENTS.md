# gleditor

A GPU-rendered text editor library (`libgleditor`) and programs built on it. C++23, GNU Make +
pkg-config (**no CMake**); OpenGL, OpenGL ES, and Vulkan from one pipeline. See
[README.md](README.md), [docs/rendering.md](docs/rendering.md), [docs/xanadu.md](docs/xanadu.md),
and [docs/testing.md](docs/testing.md).

| program                   | what it is                                                                                       |
| ------------------------- | ------------------------------------------------------------------------------------------------ |
| `apps/gleditor`           | plain text editor; shares no Xanadu/ZigZag/Xuzz code and keeps its own TSV config                |
| `apps/xuzz`               | sovereign unified Xanadu + ZigZag app (`build/xudu`, `build/zigzag` symlinks)                    |
| `apps/vquery` / `vqueryc` | VQL query REPL/runner over stores, and standalone VQL-to-Vortex compiler                         |
| `apps/vpl` / `vplc`       | VPL array-language REPL/runner (APL/J syntax, lattice grids), and compiler                       |
| `apps/vprolog`            | Prolog REPL over the Vortex/Vlog logic runtime                                                   |
| `apps/vlace` / `vlacec`   | purely functional ML/Haskell-shaped language over Vortex (`ArenaManifold`) and compiler          |
| `apps/vicon` / `viconc`   | Icon-shaped goal-directed language over Vlog choice points and compiler                          |
| `build/vagent`            | vagent typed agent-language checker and single-node runner ([guide](docs/vagent-user-guide.md))  |
| `build/store-dump`        | (`build/xudu-dump` symlink) reads a store or `ops.nodes` without the loader; format diffing tool |
| `build/xudu-swarm-peer`   | peer driven by the network-namespace swarm tests                                                 |

## Core architectural boundaries

- **`src/`, `include/gleditor/`, `apps/gleditor`:** `libgleditor` is generic only (zero
  Xanadu/ZigZag references; never include `apps/`). `apps/gleditor` uses `libgleditor` only.
- **Where xanalogical code goes:** `apps/common/xanadu/` is device-free and must never include
  `apps/xuzz/`, `apps/common/ui/`, or device-requiring `<gleditor/*>` headers (device-free headers
  like `<gleditor/cpp26*.hpp>`, `<gleditor/spatial.hpp>`, `<gleditor/draw_budget.hpp>` allowed). UI
  lives in `apps/common/ui/` (`xanadoc/`, `slice/`); program wiring in `apps/xuzz/`. Never create
  `apps/xudu/` or `apps/zigzag/`.
- **Promotion & tests:** move reusable code down to `apps/common/xanadu/` or `libgleditor`. Tests
  sit with their code once (`tests/lib/`, `tests/xuzz/`, `tests/zigzag/`, `tests/fuzz/`).

## Universal invariants

- **Headless execution:** never open a window or touch a display/GPU/audio device unless visual
  confirmation is explicitly requested. `make` exports `SDL_VIDEODRIVER=offscreen`,
  `SDL_AUDIODRIVER=dummy`, `LIBGL_ALWAYS_SOFTWARE=1`, and `XDG_*` under `build/xdg/`; set those (or
  `xvfb-run` / `--headless`) when invoking binaries directly.
- **Submodules:** run `git submodule update --init --recursive` in fresh clones/worktrees or before
  debugging missing `thirdparty/` headers; never hand-edit under `thirdparty/`.
- **Sovereign keymap, config & Vortex:** all `xuzz` (`xudu`/`zigzag`) key bindings live in
  `system://keymap` using Vortex actions (`apps/gleditor` exempt); runtime tunables belong in
  `system://*` stores/slices (no naked magic numbers). In `xuzz` and the engine, new C++ must
  justify why it isn't Vortex (drivers, rendering, allocators, raw OS events), and new Vortex stdlib
  code must reuse existing stdlib functions.
- **No backwards-compatibility shims:** not in production — bump on-disk versions and delete old
  readers while keeping structural invariants (64B `CompactOpNode`, 64 KiB Merkle pieces,
  append-only permascroll).
- **License:** GPL-3.0 (`LICENSE`). `-only` vs `-or-later` is unconfirmed; do not resolve in headers
  or packaging metadata.

## When to load which skill

| Task area                                                                                         | Skill                                                                    |
| ------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------ |
| Submodules, Makefile targets/flags, headless tests, fixtures                                      | [`build-and-test`](.claude/skills/build-and-test/SKILL.md)               |
| C++23/26, GLSL, Markdown/YAML/Shell/Nix style, logging, formatters, linters, CI gates             | [`code-style-and-lint`](.claude/skills/code-style-and-lint/SKILL.md)     |
| Engine types, `Store`/`Manifold`, language runtimes, `system://*`, on-disk formats, `design/` map | [`xanadu-engine`](.claude/skills/xanadu-engine/SKILL.md)                 |
| `libgleditor` text shaping, glyph atlas, GL/GLES/Vulkan, AccessKit a11y, Layoutfilade, hot loops  | [`rendering-and-text`](.claude/skills/rendering-and-text/SKILL.md)       |
| Xuzz pages, slice views, links/navigation, visual UX & a11y                                       | [`xuzz-ui-design`](.claude/skills/xuzz-ui-design/SKILL.md)               |
| End-to-end UX journey audits (`xuzz`, `vquery`, `vpl`, `vprolog`)                                 | [`ux-validation`](.claude/skills/ux-validation/SKILL.md)                 |
| libtorrent, BEP 10/46, Merkle identity ledger, transcopyright                                     | [`bittorrent-expert`](.claude/skills/bittorrent-expert/SKILL.md)         |
| Layer promotion, config audits, perf profiling, magic numbers                                     | [`system-architect`](.claude/skills/system-architect/SKILL.md)           |
| Nelsonian ideals vs. systems realism dialectic on new designs                                     | [`xanadu-deep-reasoning`](.claude/skills/xanadu-deep-reasoning/SKILL.md) |
