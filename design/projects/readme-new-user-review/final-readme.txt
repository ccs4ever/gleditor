# gleditor

A GPU-rendered text editor library, a plain editor, and `xuzz` — one application for Xanadu
documents and ZigZag spaces — with query and language tools over their shared store. The project
uses C++23, GNU Make, and pkg-config; there is no CMake build.

[![C/C++ CI][ci-badge]][ci-workflow]

The project is under active development. The [design notes](design/) explain architectural
decisions; [AGENTS.md](AGENTS.md) is the contributor runbook.

## Programs

| Program                         | Purpose                                                                        |
| ------------------------------- | ------------------------------------------------------------------------------ |
| `build/gleditor`                | Plain file editor built on `libgleditor`; it has no Xanadu dependency.         |
| `build/xuzz`                    | Xanadoc editing (transclusion, links, publishing, hypertime) and ZigZag cells. |
| `build/vquery`, `build/vqueryc` | VQL query runner/REPL and standalone compiler.                                 |
| `build/vpl`, `build/vplc`       | VPL array-language runner/REPL and compiler.                                   |
| `build/vprolog`                 | Prolog REPL over the Vortex/Vlog logic runtime.                                |
| `build/vagent`                  | vagent source-checking frontend; compiler and actor runtime are in progress.   |
| `build/store-dump`              | Inspect a store or `ops.nodes` directly, including one the loader refuses.     |
| `build/xudu-swarm-peer`         | Peer used by the network-namespace swarm tests.                                |

`xudu` and `zigzag` are retired as separate applications; `xuzz` is the one program for both.
`build/xudu` and `build/zigzag` are compatibility symlinks to it, and the name it is started under
only picks the opening view (`--view xanadoc`, `--view zigzag`; the default is `unified`). The
[unified traversal vision](design/xuzz-unified-link-traversal-vision.md) and
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
signed publication. For Ubuntu, the
[CI build job's apt package list](.github/workflows/c-cpp.yml#L134) is a tested Linux dependency
set; other distributions use their equivalent library and tool packages.

```sh
make -j$(nproc) lib       # shared library
make -j$(nproc) gleditor  # plain editor
make -j$(nproc) xuzz      # xanadocs and ZigZag; `xudu` and `zigzag` make its symlinks
make -j$(nproc) vquery vqueryc vpl vplc vprolog
make -j$(nproc) shaders   # GLSL validation and SPIR-V when Vulkan is enabled
```

The default target also builds the four test binaries and `build/compile_commands.json`. The
Makefile chooses `clang++` by default and probes the supported language-standard flag. Set `CXX=g++`
to use GCC. Useful switches are `GLEDITOR_SDL=2|3`, `GLEDITOR_DISABLE_VULKAN=1`,
`GLEDITOR_ENABLE_A11Y=1|0`, and `DEBUG=1`. Changing backend or debug flags rebuilds affected
objects; `ccache` is used automatically when installed. The Makefile lists all targets and switches.

Open an existing text file with `./build/gleditor file.txt`, or start Xuzz with `./build/xuzz`. With
no store argument, Xuzz opens the default store in the user's data directory (normally
`$XDG_DATA_HOME/xuzz/xanadocs/default`); edits are saved there on normal exit and reopened from that
path. A positional store path can select or create another store. `xuzz --view zigzag` opens on the
cell view and `--view xanadoc` on the pages. `xuzz --xanadoc STORE` projects a xanadoc store into
the cell view. For batch and diagnostic options hidden from everyday help, run
`./build/xuzz --help-all`; `--headless` (also `--batch`) runs batch commands without the GUI.
`--raster` prints text from a slice/store; add `--lens PATH` to print a filesystem projection, for
example `./build/xuzz --lens docs --raster`. The query and language tools also offer `--help` and
headless execution.

The [vagent user guide](docs/vagent-user-guide.md) introduces its typed agent language and explains
which commands are implemented. The current frontend can be built and checked with:

```sh
make -j$(nproc) vagent vagent_frontend_test
make -j$(nproc) test/vagent-frontend
./build/vagent check tests/samples/vagent/mini_01.vagent
```

Source checking does not execute an agent. Vortex lowering, the actor adapter, `run`, and the REPL
remain tracked work in the [implementation progress](design/vagent-implementation.md).

## Library and rendering

`libgleditor` provides text layout, pagination, glyph caching, rendering, input and accessibility.
The plain editor uses it without any Xanadu dependency.

The [rendering reference](docs/rendering.md) covers the shared OpenGL/GLES/Vulkan pipeline, text
fitting, clipping and picking, extension interfaces, C++26 compatibility, and backend verification.
Its [first-file steps](docs/rendering.md#first-saved-file) show how to create, save and reopen a
plain text file.

## Xanadu store and application

Xuzz presents documents and ZigZag cells over the shared engine in `apps/common/xanadu/`. Each
author has one append-only permascroll; a store holds edit decisions and contains no primedia, so a
bare store directory is not portable alone.

The [Xanadu and Xuzz reference](docs/xanadu.md) describes the store formats, hypertime, transclusion
and link identities, publishing and reader layers, system configuration, and language runtimes. See
its [disposable-store and backup steps](docs/xanadu.md#create-and-back-up-a-disposable-store) to
seed, reopen, and back up a scratch store with its permascroll.

## ZigZag and Xuzz presentation

Xuzz combines page and cell presentations, shared-span beams, animated neighborhood views and GPU
picking. The current bridge is a foundation for the proposed unified navigation journey; the
prototype gallery is not a set of running-UI screenshots.

See [ZigZag and Xuzz presentation](docs/xanadu.md#zigzag-and-xuzz-presentation) for view behavior,
projection commands and UI sizing.

## Accessibility and SDL

The library maintains an accessibility tree alongside GPU rendering and uses a shared focus manager
for keyboard navigation, modal input and accessibility actions. SDL2 and SDL3 share one
compatibility layer.

See [accessibility and SDL](docs/rendering.md#accessibility-and-sdl) for platform bindings, input
methods and headless UI automation.

## Tests and diagnostics

```sh
make -j$(nproc) test
make -j$(nproc) format-check lint
```

Make runs tests headlessly with isolated XDG state. The [testing reference](docs/testing.md) covers
that environment for direct binary runs, focused and network tests, fixture regeneration, backend
comparisons, quality gates and logging. [AGENTS.md](AGENTS.md) is the contributor runbook.

## Installing and project layout

`make install DESTDIR=/tmp/stage prefix=/usr` installs the library, headers, `gleditor`, `xuzz`
(with `xudu` and `zigzag` symlinks), assets, desktop integration, and man pages. It installs the
C++26 fallback headers selected by the library build so external users see the same type layout. The
other programs build in-tree; the install target names only `gleditor` and `xuzz`. `make dist`
produces a source tarball with submodules included. Packaging definitions live under `packaging/`
for Arch, Debian, Fedora, macOS, Windows, Nix, Android, and WebAssembly; see each target's own files
for its build scope.

| Path                                                        | Contents                                                                 |
| ----------------------------------------------------------- | ------------------------------------------------------------------------ |
| `src/`, `include/gleditor/`                                 | Public library implementation and headers.                               |
| `apps/common/xanadu/`                                       | Shared Xanadu store, identity, enfilades, ZigZag manifold, and runtimes. |
| `apps/gleditor/`, `apps/xuzz/`                              | The plain editor and the Xanadu/ZigZag application.                      |
| `apps/common/ui/`                                           | Xanadoc and cell presentation components linked into `xuzz`.             |
| `apps/vquery*`, `apps/vpl*`, `apps/vprolog/`                | Language and query front ends.                                           |
| `tests/lib/`, `tests/xudu/`, `tests/xuzz/`, `tests/zigzag/` | GoogleTest suites.                                                       |
| `assets/shaders/`, `tools/`                                 | Shader sources and build, fixture, and diagnostic tools.                 |
| `design/`                                                   | Architecture, decisions, workflows, and interface prototypes.            |
| `thirdparty/`                                               | Git submodules; do not edit their contents in place.                     |

The [store/slice convergence](design/store-slice-convergence.md) is the central model for cell
identity and hypertime. The [bridge design](design/xudu-zigzag-unified-hypermedia-bridge.md) covers
xanadoc/ZigZag composition. The [C++26 compatibility note](design/cpp26-compatibility.md) explains
native-versus-fallback selection. See the
[API documentation note](docs/testing.md#api-documentation-generation) for `make doc`'s Doxygen
prerequisite, output location, and scope limits.

## License

The repository includes the GNU General Public License version 3; see [LICENSE](LICENSE).

[ci-badge]: https://github.com/ccs4ever/gleditor/actions/workflows/c-cpp.yml/badge.svg
[ci-workflow]: https://github.com/ccs4ever/gleditor/actions/workflows/c-cpp.yml
