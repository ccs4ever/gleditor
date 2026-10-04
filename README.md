# gleditor

A GPU-rendered text editor library and a family of programs for Xanadu documents, ZigZag spaces, and
computation over their shared store. The project uses C++23, GNU Make, and pkg-config; there is no
CMake build.

[![C/C++ CI][ci-badge]][ci-workflow]

The project is under active development. The [design notes](design/) explain architectural
decisions; [AGENTS.md](AGENTS.md) is the contributor runbook.

## Programs

| Program                         | Purpose                                                                       |
| ------------------------------- | ----------------------------------------------------------------------------- |
| `build/gleditor`                | Plain file editor built on `libgleditor`; it has no Xanadu dependency.        |
| `build/xudu`                    | Versioned xanadoc editor with transclusion, links, publishing, and hypertime. |
| `build/zigzag`                  | Multidimensional cell visualizer over a store-backed ZigZag manifold.         |
| `build/xuzz`                    | Xudu and ZigZag in one scene, with a shared presentation bridge.              |
| `build/vquery`, `build/vqueryc` | VQL query runner/REPL and standalone compiler.                                |
| `build/vpl`, `build/vplc`       | VPL array-language runner/REPL and compiler.                                  |
| `build/vprolog`                 | Prolog REPL over the Vortex/Vlog logic runtime.                               |
| `build/xudu-dump`               | Inspect a store or `ops.nodes` directly, including one the loader refuses.    |
| `build/xudu-swarm-peer`         | Peer used by the network-namespace swarm tests.                               |

`apps/xuzz/main.cpp` includes xudu's main program with `XUZZ_BUILD`, which enables ZigZag
presentation at compile time. It is one integrated application, not a second copy of the store
engine. The [unified traversal vision](design/xuzz-unified-link-traversal-vision.md) and
[interaction prototypes](design/ui/prototypes/README.md) describe proposed link selection, exact
endpoint entry, activity walks, and the Flap. Those complete interactions are **not yet
implemented**.

## Quick start

Clone with submodules, then build with the repository's parallel Make invocation:

```sh
git clone --recurse-submodules https://github.com/ccs4ever/gleditor.git
cd gleditor
make -j$(nproc)
```

In an existing checkout or a new worktree, run `git submodule update --init --recursive` before
investigating missing headers under `thirdparty/`. Those directories are git submodules; edit the
pointer rather than their contents.

The build needs a C++23 compiler, Make, pkg-config, FreeType, HarfBuzz, libunibreak, FriBidi,
Fontconfig, GLM, spdlog, SDL2 or SDL3, libtorrent-rasterbar, OpenSSL, LMDB, and the OpenGL headers.
GoogleTest and GoogleMock are needed for tests. Vulkan and `glslangValidator` are used when
available; AccessKit is optional unless explicitly required. `gnupg` is needed at run time for
signed publication. The [CI workflow](.github/workflows/c-cpp.yml) records a working Linux
dependency set.

```sh
make -j$(nproc) lib       # shared library
make -j$(nproc) gleditor  # plain editor
make -j$(nproc) xudu xuzz zigzag
make -j$(nproc) vquery vqueryc vpl vplc vprolog
make -j$(nproc) shaders   # GLSL validation and SPIR-V when Vulkan is enabled
```

The default target also builds the four test binaries and `build/compile_commands.json`. The
Makefile chooses `clang++` by default and probes the supported language-standard flag. Set `CXX=g++`
to use GCC. Useful switches are `GLEDITOR_SDL=2|3`, `GLEDITOR_DISABLE_VULKAN=1`,
`GLEDITOR_ENABLE_A11Y=1|0`, and `DEBUG=1`. Changing backend or debug flags rebuilds affected
objects; `ccache` is used automatically when installed. The Makefile lists all targets and switches.

Start with `./build/gleditor file.txt`, `./build/xudu [store]`, `./build/zigzag [store]`, or
`./build/xuzz [store]`. With no ZigZag store, `zigzag` opens a built-in sample.
`zigzag --xudu STORE` projects a xanadoc store into its cell view; `--raster` prints a text
projection and exits. Each program's `--help` shows everyday options and `--help-all` shows batch
and diagnostic options. `xudu --headless` runs batch commands without a window. The query and
language tools also offer `--help` and headless execution.

## Library and rendering

`libgleditor` owns text layout, pagination, the glyph cache, rendering, SDL integration, input,
accessibility data, and the application loop. It names no xanadoc, cell, link, or document format.
The plain editor keeps its own YAML configuration and shares no Xanadu code.

### One pipeline, three backends

The graphics backend is selected at run time with `--backend opengl|opengles|vulkan`. OpenGL 3.3
core is the default, OpenGL ES 3.0 is supported, and Vulkan 1.0 is compiled when its development
tools are found. All use `render::RenderDevice`, the same glyph instances, and shader bodies from
`assets/shaders/`. Backend-specific shader preambles and build-time SPIR-V generation keep those
bodies aligned. GL entry points come from `SDL_GL_GetProcAddress`; the build does not link `-lGL`.

The glyph atlas holds FreeType's 8-bit coverage. HarfBuzz shapes runs, FriBidi resolves text
direction, libunibreak chooses line breaks, and Fontconfig resolves fonts.
`TextLayout::layoutPage()` limits work to lines that fit a page; immutable `PageShaping` values pass
from page building to the render thread. Editing rebuilds the affected page and continues only until
pagination resynchronizes. Caret and selection offsets are document byte positions, while picking
and highlighting account for character boundaries inside shaped clusters such as ligatures.

Pages outside the view are culled conservatively in clip space. Distant pages switch to per-line
coarse bars according to projected size. `--no-cull` and `--coarse-below` expose both paths for
comparison. `Doc::collect()` submits page draws across open documents as one list. Vulkan can record
chunks on worker threads; GL and GLES record in order on their context thread. Vulkan measures the
choice at run time; `GLEDITOR_RECORD_THREADS` overrides it for comparison.

Picking writes an identity attachment beside color. `requestPickingTag()` queues an asynchronous
read and `takePickingTag()` returns a completed result with its pixel coordinates. GL/GLES use a PBO
ring and fences; Vulkan uses per-frame host-visible buffers. The renderer also collects driver
messages in `render::DiagnosticSink`, logs them by category, and shows significant messages as
notifications. `--strict-diagnostics` makes them fatal for automated checks.

Programs extend the library through `TextSource`, `DocumentObserver`, `SpanDecorator`,
`FrameContributor`, `PickObserver`, and the application/command interfaces in `include/gleditor/`.
`Canvas` draws text and geometry in world or screen coordinates through the same glyph pipeline.
`runWithState()` moves commands onto the render thread. Xudu uses these hooks for its versioned
text, shared-span shading, and links; ZigZag contributes cells and picking without putting its model
in `libgleditor`.

The public C++26 compatibility facades select native library facilities or pinned fallbacks for
`function_ref`, optional references and ranges, and `inplace_vector`. Scoped adapters provide
checked span access and a two-span `views::concat`. Vortex `map`/`filter`/`fold` and
`WorkerPool::run` use `function_ref`; VQL `!both` uses the concat view.
`GLEDITOR_CPP26_FORCE_FALLBACK=1` tests the fallback paths. Installed headers record the library's
native/fallback choices so clients use the same type layout. See the
[C++26 compatibility note](design/cpp26-compatibility.md) for the exact boundary.

For a rendering regression, build and run `./tools/compare-backends.sh` under `xvfb-run`. It
compares software-rendered PNGs, picking tags, and Vulkan's single- and multi-thread recordings. A
process exiting zero while drawing nothing is not a rendering test.

## Xanadu store and applications

The xanalogical engine lives in `apps/common/xanadu/`. The headers under `apps/xudu/core/` and
`apps/zigzag/core/` forward to it. The editor and visualizer use the same store, but each owns its
presentation.

### Content, operations, and hypertime

Each author has one append-only `UserPermascroll` for primedia. A `Store` is an edit decision list
over local slot 0 and registered external scrolls. A store directory holds `ops.nodes` and
`store.tables`; it contains **no primedia** and is not portable without the permascroll it
addresses. The default permascroll lives under `$XDG_DATA_HOME/xudu/permascroll/`;
`xudu --permascroll` binds an explicit one. Publishing packages the content and references others
need to read, instead of copying a bare store directory.

`ops.nodes` starts with a format header, followed by immutable, 64-byte `CompactOpNode` records in
64 KiB Merkle-piece-aligned segments. `store.tables` holds scroll registration, links, designated
versions, and annotations. A format the loader does not understand is refused by name and version;
`xudu-dump --section=header STORE` or `--section=ops` inspects the bytes without that loader.
Human-readable OSMIC or compact binary operations are optional `ops.export` output, not another
store file.

A microversion is rebuilt by replaying its ancestral path through the append-only operation tree.
Editing an earlier state branches rather than discarding its descendants. Deletion removes a
reference from the current version; it does not erase primedia or earlier versions. Transclusion
adds another reference to the same content address. Shared addresses, rather than equal strings,
identify shared content across documents and versions.
`xudu --version-id ID --alongside OTHER STORE` shows two branches for comparison; `--map` opens the
hypertime map.

`OpKind::Structure` is OSMIC's sixth hyperop. It mints ZigZag cells and changes their links or
content. A cell's persistent identity is the operation that created it, and later changes chain to
that operation. The same spool produces two replay views: `Version` for document concatext and
`zigzag::Manifold` for cell structure. Typing into a xanadoc does not itself mint cells.
`ArenaManifold` supplies an ephemeral copy-on-write space for queries, Vortex execution, and logic
choice points; explicit promotion mints persistent Structure operations. Navigation and speculative
work do not silently write hypertime.

A ZigZag dimension is itself a cell. Links on each dimension give a cell at most one posward and one
negward neighbor, forming ranks. A cell's text is a run of primedia spans; roles, MIME type, media
paths, and other structured properties live on dimensions rather than hidden markup in that text.
`sliceToStore()` mints a structure DTO as operations, and `storeToSlice()` projects a manifold back
to a DTO. The old YAML slice format and its loader are gone.

### Links, discovery, and publication

A Xanadu link has one identity, type, attribution, and ordered left and right endsets of
`PrimediaSpan`s. Neither side is intrinsically a source or destination. An endset may contain
multiple discontinuous spans; a stored link does not pair each left member with one right member.
Links attach to content addresses, so quotations can reveal the same link in another manifestation.
The Spanfilade indexes span occurrences across documents and cells for transclusion discovery.

Xudu renders explicit links as cyan/magenta ribbons and shared-content transclusions as gold prisms.
Its beam layout and margin brackets help distinguish dense, overlapping passages. Xuzz's
`BridgeCoordinator` brings ZigZag cell anchors and an accessibility source into the xudu scene.
Current beam activation still follows a rendered strand directly; the exact, many-to-many traversal
interface is described as a proposal in the
[Xuzz navigation workflow](design/ui_workflow_xuzz_navigation.md).

Publication signs an authorship record, seals content and operations for sharing, and can publish
mutable names through BEP 46. Global spans and operation references map published identities back to
local stores. A Merkle identity ledger and BEP 10 extensions support author lookup and peer
verification; Hashcash gates costly requests. Live collaboration exchanges operation descriptors and
global spans rather than injecting raw remote text into an author's local permascroll. See the
[publication and discovery design](design/bep46-publication-and-discovery.md) and
[identity model](design/oracle-identity-model.md) for protocol details.

### Configuration and computation

Xudu and Xuzz keep keymaps, settings, layout, UI state, and pouches in five sovereign system
xanadocs (`system://keymap|settings|layout|ui|pouches`). They are validated structure, with
defaults, rather than hardcoded application keymaps. ZigZag uses sovereign-store-backed system
slices. `gleditor` remains separate and uses its own YAML configuration. See the
[system xanadocs design](design/system-xanadocs-customization-and-metasystem.md).

The Vortex VM operates over manifolds. VQL queries and VPL array expressions compile to Vortex;
`vquery`/`vqueryc` and `vpl`/`vplc` expose their runners and compilers. `vprolog` runs a Prolog
front end over Vortex/Vlog logic operations. The ZigZag UI also has Vortex and VQL palettes. VQL can
compose multiple stores in one ephemeral arena; this does not make persistent cross-store cell links
automatic. The [Vortex](design/vortex-hyperstructural-runtime.md),
[VQL](design/vql-query-language.md), [VPL](design/vpl-array-language.md), and
[Vlog](design/vlog-logic-extension.md) notes document their models and remaining work.

## ZigZag and Xuzz presentation

`ZigzagVisualizer` owns a `UnifiedTransclusionEngine` over a `Manifold`. It renders the focused cell
and its rank neighborhood with animated transitions, GPU picking, and accessibility descriptions.
Cell Content View emphasizes the content of each cell; Topology View emphasizes its dimensional
connections. The view can cycle dimensions and use execution, scope, contract, logic, and standard
library bundles. `zigzag --raster STORE` prints a non-graphical projection. The visualizer can adopt
a store-backed slice or project xanadoc versions via `--xudu`.

Xuzz composes this presentation with xudu's pages and beams. The current bridge supplies cell
anchors, a shared scene, and cell activation. It is a foundation for the proposed unified link
journey, not a completed persistent activity navigator. The
[prototype gallery](design/ui/prototypes/README.md) contains generated mock-ups rather than
screenshots of the running UI.

## Accessibility and SDL

The GPU draws quads, which do not describe themselves to a screen reader. A separate accessibility
tree is built from the same document and UI state. It describes documents as line-sized text runs,
caret and selection offsets in characters, notifications, forms, links, hypertime, and ZigZag cells.
Actions return through a queue to the thread that owns the target state. `include/gleditor/a11y/`
defines platform-independent nodes; `src/a11y/` sends them through AccessKit to AT-SPI on Linux, UI
Automation on Windows, and NSAccessibility on macOS. A build without AccessKit still builds the
internal tree and uses a no-op platform adapter. `GLEDITOR_ENABLE_A11Y=1` requires the binding;
`ACCESSKIT_DIR` can point to a local accesskit-c installation. `--dump-a11y` prints the settled tree
for headless inspection.

The Makefile selects SDL3 when pkg-config finds it, otherwise SDL2; use `GLEDITOR_SDL=2|3` to pin a
major. `include/gleditor/sdl_compat.hpp` normalizes their input, window, and text-input APIs. SDL2
needs version 2.0.22 or newer. SDL supplies windows and message boxes, not a widget toolkit:
editable forms such as xudu's publishing dialog are rendered with `gleditor::Form`. Text input area
updates position input-method candidate windows beside the focused field.

## Tests and diagnostics

`make` exports offscreen SDL video, dummy audio, software GL, and XDG data/config/cache directories
under `build/xdg/`. This keeps tests headless and keeps their primedia, system xanadocs, and cache
out of your personal directories. For a binary run outside Make, set these explicitly:

```sh
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
  XDG_DATA_HOME="$PWD/build/xdg/data" XDG_CONFIG_HOME="$PWD/build/xdg/config" \
  XDG_CACHE_HOME="$PWD/build/xdg/cache" ./build/xudu_test
```

```sh
make -j$(nproc) test                 # four gtest binaries, then rootless swarm tests
make test TEST_FILTER='MediaTest.*'  # focused gtest filter
make test/e2e-orchestration          # whole-program xudu orchestration
make -j$(nproc) format-check lint    # formatting and text lint gates
make -j$(nproc) shaders              # shader validation
make profile                         # library test coverage
```

`gleditor_test` links the real shared library; `xudu_test` and `xuzz_test` exercise the engine
without a graphics device; `zigzag_test` covers the slice model and transclusion engine. The swarm
step uses rootless network namespaces and needs the `veth` kernel module. If it ends with
`Error: Unknown device type.`, inspect the four gtest summaries before diagnosing the failure. Run
`xvfb-run -s "-screen 0 1024x768x24" ./tools/compare-backends.sh` for the backend image and picking
comparison.

Binary sample stores live under `tests/samples/xudu/` and `tests/samples/xuzz/`. When an operation
layout or format changes, regenerate them in the same commit:

```sh
make -j$(nproc) xudu
./tools/create-sample-xanadocs.sh
./tools/create-floating-image-sample.sh
```

The second script restores the floating-image sample deleted by the first. Its permascroll is part
of the fixture. `xudu-dump --section=ops STORE` is the semantic diff to use across format changes;
`--section=header` shows the format version.

`make format-check`, `make lint`, `make analyze`, and `make check` are the broader quality gates. If
a lint tool is missing, install it locally when possible and rerun the gate; a skipped tool is not a
pass. `AGENTS.md` has formatter and linter setup details. Logging uses named spdlog categories;
`SPDLOG_LEVEL='warn,text.layout=debug,xudu.links=trace'` enables selected diagnostics on stderr.

## Installing and project layout

`make install DESTDIR=/tmp/stage prefix=/usr` installs the library, headers, `gleditor`, `xudu`,
assets, desktop integration, and man pages. It installs the C++26 fallback headers selected by the
library build so external users see the same type layout. The other programs build in-tree; the
install target currently names only `gleditor` and `xudu`. `make dist` produces a source tarball
with submodules included. Packaging definitions live under `packaging/` for Arch, Debian, Fedora,
macOS, Windows, Nix, Android, and WebAssembly; see each target's own files for its build scope.

| Path                                                         | Contents                                                                 |
| ------------------------------------------------------------ | ------------------------------------------------------------------------ |
| `src/`, `include/gleditor/`                                  | Public library implementation and headers.                               |
| `apps/common/xanadu/`                                        | Shared Xanadu store, identity, enfilades, ZigZag manifold, and runtimes. |
| `apps/gleditor/`, `apps/xudu/`, `apps/zigzag/`, `apps/xuzz/` | User-facing editors and visualizers.                                     |
| `apps/vquery*`, `apps/vpl*`, `apps/vprolog/`                 | Language and query front ends.                                           |
| `tests/lib/`, `tests/xudu/`, `tests/xuzz/`, `tests/zigzag/`  | GoogleTest suites.                                                       |
| `assets/shaders/`, `tools/`                                  | Shader sources and build, fixture, and diagnostic tools.                 |
| `design/`                                                    | Architecture, decisions, workflows, and interface prototypes.            |
| `thirdparty/`                                                | Git submodules; do not edit their contents in place.                     |

The [store/slice convergence](design/store-slice-convergence.md) is the central model for cell
identity and hypertime. The [bridge design](design/xudu-zigzag-unified-hypermedia-bridge.md) covers
Xudu/ZigZag composition. The [C++26 compatibility note](design/cpp26-compatibility.md) explains
native-versus-fallback selection. `make doc` generates API documentation under `docs/`.

## License

The repository includes the GNU General Public License version 3; see [LICENSE](LICENSE).

[ci-badge]: https://github.com/ccs4ever/gleditor/actions/workflows/c-cpp.yml/badge.svg
[ci-workflow]: https://github.com/ccs4ever/gleditor/actions/workflows/c-cpp.yml
