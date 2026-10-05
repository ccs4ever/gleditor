# WebAssembly build and accessibility

The browser platform adapter publishes the shared accessibility tree as native HTML controls beside
the canvas. The controls stay in the browser accessibility tree while the canvas supplies their
visual representation. Stable node IDs preserve focus across updates. Buttons, links, switches, text
fields, live messages and modal dialogs use browser semantics. Text selection offsets convert from
Unicode scalars to HTML's UTF-16 units. Browser actions queue into the same `ActionRequest`
interface as desktop adapters; the source must advertise each supported action.

Fields advertising `SetValue` use native input. Password edits replace the complete value; the DOM
never imports masking stars as a secret, and an unfocused password control clears its local value.
Document nodes currently advertise focus and activation. Their text and caret are mirrored, and
keyboard editing remains SDL's responsibility; the shared source does not yet accept replacement
text or selection actions from an assistive technology. Browser accessibility delivery does not
remove that source-level limitation.

## Independent platform validation

Activate an Emscripten SDK, initialize the repository submodules, and install Python Playwright with
Chromium. The validation runs headless and queries Chromium's accessibility API, rather than only
inspecting the internal tree:

```sh
python3 -m venv build/wasm-validation-venv
build/wasm-validation-venv/bin/pip install playwright
build/wasm-validation-venv/bin/playwright install chromium
packaging/wasm/build.sh --a11y-test build/wasm-a11y
build/wasm-validation-venv/bin/python packaging/wasm/test-accessibility.py \
  --compiled build/wasm-a11y/test-accessibility.html
```

`--browser PATH` reuses an installed Chromium. The suite checks roles, text, descriptions, live and
switch state, Unicode caret offsets, modal isolation, stable focus, removal of stale requests and
keyboard dispatch. Its compiled fixture sends a C++ tree through `Platform::update()`, reads the
result from Chromium, edits a native control, then checks the resulting C++ `ActionRequest`. The
maximum 64-bit node ID and multiline Unicode text exercise both directions of the bridge.

## Full application build

Set `VCPKG_ROOT` to a checkout at `f298fb4ef3a7ae09766b6a5beb92ce171a767927`, bootstrap it, and run
`packaging/wasm/build.sh [output-dir]`. The manifest cross-builds static text, PDF, MIME and crypto
dependencies in `build/wasm-dependencies`; `WASM_DEPENDENCIES` overrides that location. Host build
tools include autoconf, autoconf-archive, automake, libtool, pkg-config and Ninja. The compiler's
standard is taken from the main Makefile's probe. A generated overlay applies the small Poppler
26.04 libc++ incomplete-type constructor fix while retaining the pinned upstream port metadata; no
installed dependency or submodule source is modified. Full application builds use pthreads and
require HTTP response headers `Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp` when served.

The full distribution still has existing native dependencies without browser ports in this tree:
`src/media.cpp` requires LibVLC, and Xuzz additionally requires the native identity/swarm libraries
including RNP and libtorrent. The standalone platform fixture builds without them. Passing that
fixture proves browser accessibility delivery and action round trips; it does not certify that the
complete application distribution builds or that all desktop media and networking features work in a
browser. The packaging workflow runs this accessibility check before attempting the full build.
