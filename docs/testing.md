# Tests and diagnostics

[Project overview](../README.md) · [Build setup](../README.md#quick-start) ·
[Contributor runbook](../AGENTS.md)

Commands and source paths below are relative to the repository or worktree root.

## Headless execution

`make` exports offscreen SDL video, dummy audio, software GL, and XDG data/config/cache directories
under `build/xdg/`. This keeps tests headless and keeps their primedia, system xanadocs, and cache
out of your personal directories. For a binary run outside Make, set these explicitly:

```sh
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
  XDG_DATA_HOME="$PWD/build/xdg/data" XDG_CONFIG_HOME="$PWD/build/xdg/config" \
  XDG_CACHE_HOME="$PWD/build/xdg/cache" ./build/xuzz_test
```

## Test commands

```sh
make -j$(nproc) test                 # three gtest binaries, then rootless swarm tests
make test TEST_FILTER='MediaTest.*'  # focused gtest filter
make test/e2e-orchestration          # whole-program xuzz orchestration
make -j$(nproc) test/publication-local # three local UI publications with disposable signing keys
make -j$(nproc) test/publication-swarm # outbox acknowledgement/restart on separate network stacks
make -j$(nproc) format-check lint    # formatting and text lint gates
make -j$(nproc) shaders              # shader validation
make profile                         # library test coverage
```

`gleditor_test` links the real shared library; `xuzz_test` exercises the engine without a graphics
device; `zigzag_test` covers the slice model and transclusion engine. The swarm step uses rootless
network namespaces and needs the `veth` kernel module. If it ends with
`Error: Unknown device type.`, inspect the three gtest summaries before diagnosing the failure. Run
`xvfb-run -s "-screen 0 1024x768x24" ./tools/compare-backends.sh` for the backend image and picking
comparison.

## Binary fixtures

Binary sample stores live under `tests/samples/xudu/` and `tests/samples/xuzz/`. When an operation
layout or format changes, regenerate them in the same commit:

```sh
make -j$(nproc) xuzz
./tools/create-sample-xanadocs.sh
./tools/create-floating-image-sample.sh
```

The second script restores the floating-image sample deleted by the first. Its permascroll is part
of the fixture. `./build/store-dump --section=ops STORE` is the semantic diff to use across format
changes; `--section=header` shows the format version.

## Quality gates and logging

`make format-check`, `make lint`, `make analyze`, and `make check` are the broader quality gates. If
a lint tool is missing, install it locally when possible and rerun the gate; a skipped tool is not a
pass. [AGENTS.md](../AGENTS.md) has formatter and linter setup details. Logging uses named spdlog
categories; `SPDLOG_LEVEL='warn,text.layout=debug,xuzz.links=trace'` enables selected diagnostics on
stderr.

## API documentation generation

Install Doxygen and Python 3, then build the reference from the repository root:

```sh
make -j$(nproc) doc
```

For a local Doxygen executable, pass `DOXYGEN=/path/to/doxygen` to that same Make command. The
configuration was validated with Doxygen 1.18.0. Documentation-only targets need no native project
development packages or compiled application. Graphviz `dot` adds diagrams when present; Make
explicitly disables those optional diagrams when it is absent, without reducing symbol extraction.

Output includes `docs/html/index.html`, `docs/xml/index.xml`, `docs/gleditor.tag`, and
`docs/doxygen-warnings.log`. LaTeX sources and man output are also generated; this target does not
compile a PDF. Complete LaTeX image output can additionally require TeX utilities such as
`epstopdf`; inspect the warning log before using that output. Generated output remains ignored by
Git alongside the authored Markdown files.

The target fails if Doxygen is missing or generation fails. It then runs
`tools/check-doxygen-coverage.py`, which compares XML against authored C/C++ headers and sources,
Python, the configuration-header template and GLSL under `include/`, `src/`, `apps/`, `tools/`,
`tests/` and `assets/shaders/`. Representative public, private, anonymous-namespace, header-only,
macro and shader symbols are checked too. The machine-readable result is `docs/coverage.json`;
`make -j$(nproc) doc-check` is an alias for the same rebuild-and-check gate.

The default reference includes undocumented, private, static and anonymous-namespace symbols. This
maximizes browsing coverage; `EXTRACT_ALL` disables undocumented-member warnings, so it is not a
prose-quality measurement. Other parser/documentation warnings remain in the warning log. Optional
rendering and codec features are enabled for documentation extraction independently of installed
native packages. SDL3 is the selected interface; mutually exclusive SDL2/platform and
native/fallback variants still require separate configurations. GLSL uses a C++ parser
approximation. The [live coverage audit](../design/projects/doxygen-coverage-audit.md) records the
measured results, negative checks and remaining limits; file coverage does not prove an exhaustive
compiler AST.
