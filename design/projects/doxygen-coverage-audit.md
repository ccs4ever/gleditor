# Doxygen symbol coverage: live follow-up

The initial configuration audit found that only `src/` and `apps/` were selected. This follow-up
built Doxygen locally from source, expanded extraction, generated real documentation and checked its
XML. The original Graphviz checkpoint covered all 923 supported first-party source files with 39,072
unique member entries in the generated index. The phase-11 baseline now covers 925 files and 39,766
entries. An optional-feature extraction probe increases that to 40,000 entries; the updated
full-generation gate is pending. This is source/index coverage, not a percentage of documented prose
or proof of an exhaustive compiler AST.

## Local generator

- Upstream [Doxygen 1.18.0 source](https://www.doxygen.nl/download.html), SHA-256
  `a1deed70a6785bbec95a2b2a9e419dc7f7b223a9d74a8644ae611c8e2dcdd354`, verified before extraction.

- Source and build: `/tmp/gleditor-doxygen-local/`; executable:
  `/tmp/gleditor-doxygen-local/build/bin/doxygen`. Nothing was installed globally or vendored into
  this repository.

- Built with upstream CMake/Ninja and libclang support. The repository still builds through GNU
  Make. A four-job compile pool limited build memory while the upstream build admitted 32 jobs.

- Validation command:

  ```sh
  make -j$(nproc) doc DOXYGEN=/tmp/gleditor-doxygen-local/build/bin/doxygen
  ```

The initial run had no Graphviz and generated all symbols without optional diagrams. A follow-up run
uses the subsequently installed Graphviz 16.1.0 with inheritance, collaboration, include, call and
caller graphs enabled. HTML, XML, tag, LaTeX-source and man outputs are generated; a PDF is not
built.

## Before and after

The old Doxyfile was also executed with this same local generator, using an isolated XML output and
disabling optional renderers. Old source roots and extraction settings were preserved.

| Metric                          | Original configuration | Expanded configuration |
| ------------------------------- | ---------------------: | ---------------------: |
| Authored source files indexed   |             484 of 923 |             923 of 923 |
| Public headers under `include/` |                0 of 99 |               99 of 99 |
| Unique member entries           |                 12,898 |                 39,072 |
| Indexed function entries        |                  5,619 |                 23,250 |
| Macro entries                   |                      3 |                     14 |

Current supported source inventory:

| Tree                                                 | Files indexed |
| ---------------------------------------------------- | ------------: |
| `include/`                                           |            99 |
| `src/` (including `config.h.in`)                     |            66 |
| `apps/`                                              |           419 |
| `tools/` (C++ and Python, including the new checker) |            31 |
| `tests/` (C++ and Python)                            |           300 |
| `assets/shaders/`                                    |             8 |

Eight Markdown input files are indexed in addition to the source inventory, including the five
explicit top-level manuals and README files inside source roots. Generated and vendored code are
excluded; the generated output directory is never recursively scanned as an input.

## Changes and gate

- Added the public include tree, tools, tests, GLSL and authored manuals as inputs. Enabled
  anonymous-namespace and internal documentation alongside existing private/static extraction.
- Selected C++ output, STL matching and GitHub-style Markdown heading ids. The obsolete
  `DOT_MULTI_TARGETS` setting was removed.
- Retained preprocessing and restricted macro expansion to shader layout annotations, so GLSL
  attributes are indexed as variables rather than bogus annotation functions. GLSL remains a C++
  approximation, not a native shader AST.
- Added XML and tag output plus a retained warning log. The coverage checker validates the source
  inventory, public/header-only types and functions, an anonymous/private checker member, a macro,
  shader entry points and a shader attribute.
- `make doc` now fails for a missing generator. Documentation-only goals bypass native package and
  compiler-dependency generation; mixed native/documentation goals retain native requirements.

## Observed verification

- Real Make documentation generation and the XML coverage gate passed: 923/923 files, no missing
  symbol probes or shader entry points.
- The original configuration was rejected by the new gate, including all 99 missing public headers.
- Removing one public-header file entry from a private copy of the index made the check fail. The
  test restored the original XML afterward.
- A nonexistent Doxygen executable made Make fail with an actionable message rather than silently
  skipping.
- A documentation-only Make dry run worked with empty pkg-config search paths and did not regenerate
  C++ dependencies. Native goals still require native packages.
- Representative built-in versus Clang-assisted parsing used flags derived from the actual
  compilation database: both produced 31 compounds and 195 unique member entries. No extra symbols
  were found in that sample, so normal generation does not require a Clang-enabled Doxygen package.
- A small conditional probe showed why preprocessing remains on: disabling it indexed both branches
  but lost macro symbols. The main reference keeps macros and normal conditional interpretation.

## Phase-11 optional-feature follow-up

The baseline full-generation gate passed with Doxygen 1.18.0 and Graphviz enabled: 925 of 925 source
files, 39,766 unique member entries, and no missing configured probes or shader mains. Enabling the
optional rendering/codec features in a separate XML extraction found 234 additional unique member
entries (40,000 total), including codec implementation helpers and PDF image extraction. Three
former stub variables became correctly parsed functions. Two unbalanced preprocessor warnings
disappeared; physical warning-log lines decreased from 708 to 653.

The configuration now enables those features for extraction without requiring their native build
packages. Representative JPEG, AV, Zstd, FLAC, TIFF and GIF implementation symbols are mandatory
probes. SDL3 remains the selected API; this single configuration cannot also select mutually
exclusive SDL2 or platform branches. The full-generation gate passed after this change: 925/925
files and 40,001 member entries, with all configured probes present. The old configuration fails the
new function probes; the enabled feature extraction passes them.

## Main-branch integration verification

After merging upstream lens and SQL work, the full documentation gate passed again with Doxygen
1.18.0 and Graphviz: 984/984 authored source files and 43,933 unique member entries, with every
configured symbol probe and shader entry point present. Extraction also enables the PostgreSQL and
systemd feature branches. These counts describe the merged tree; they do not expand the coverage
claim beyond the limits below.

## Remaining limits

The initial symbol-only run recorded 195 documentation/parser warnings and a missing `epstopdf`
converter for optional LaTeX image conversion. The Graphviz follow-up records 249 warnings (672
physical log lines), including the same documentation/parser backlog and 54 graphs skipped because
they exceed `DOT_GRAPH_MAX_NODES = 50`. Symbol extraction is unaffected by that graph-size limit.
The follow-up warning log contains no errors; a compiled LaTeX PDF is still unvalidated. Diagnostics
are retained rather than suppressed or represented as a clean warning gate. `EXTRACT_ALL` is
intentional for browsing and silences undocumented-member warnings; prose completeness is not
measured.

Doxygen chooses preprocessor branches. Inactive platform/native-fallback variants, macro-generated
compiler internals, and all C++ language constructs are not exhaustively validated by file coverage
or representative probes. Clang was compared on a sample, not every translation unit. There is no
claim that the generated index equals the complete compiled ABI or every build configuration.

Generated artifacts and JSON are under `docs/` and ignored by Git. The source configuration,
coverage checker and [build instructions](../../docs/testing.md#api-documentation-generation) are
kept in the repository. The local generator is temporary and can be replaced by a system install.
