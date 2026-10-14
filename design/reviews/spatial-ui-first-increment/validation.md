# Native checkpoint validation

This is the first opt-in prerequisite increment, based on `e9726a6b`. It is runnable and has 100
passing owning checks. It does not advance the M0–M6 cutover or Track H journey acceptance gates.

| Gate                                                         | Result                     | Evidence / limit                                                                                                                                                                                                                                                                   |
| ------------------------------------------------------------ | -------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Default native build                                         | Pass                       | All binaries, 443 objects and shaders built with `make -j3`, SDL2, Mesa and Vulkan. Behavior build uses `DEBUG_OPTS='-O0 -g0'` and system Poppler include paths.                                                                                                                   |
| Owning checks                                                | 100/100                    | 13 PDF/SVG, 4 ReadingPlace, 49 reference/navigation/activity, 34 panel/package UI. Initial two test-fixture failures are retained; final checks resolve them.                                                                                                                      |
| Actual new UI                                                | Bounded Pass               | UI-authored 2-by-3 Link, quoted repeated occurrence, complete member controls; `luna-1/anchor-final` shows same readable selected passage through inspect/fold.                                                                                                                    |
| Source and visit separation                                  | Bounded Pass               | `luna-1/no-write` has identical source dumps and unchanged visit dimension. `inspection-invariants` keeps both source spool hashes and two existing visit records unchanged through selection, inspection, member/occurrence preview and fold. Private activity metadata may grow. |
| Three Luna validators                                        | Complete scoped audit      | Separate reports/results for J17–J32 and J1–J16 controls. No complete spatial journey passes. J6–J9 are not independently rerun; J13 supplies a real PTY handoff subset.                                                                                                           |
| Independent UX / visual review                               | Bounded Pass               | Initial and final reports. Finalized inspector frames on OpenGL, OpenGL ES and Vulkan are legible and complete. Source anchor continuity is a separate same-document proof.                                                                                                        |
| Full library suite                                           | 781/782                    | Xvfb `-noreset` avoids SDL teardown/server reset. Unchanged O0 resize benchmark measures 4625 ns versus its 1000 ns threshold. Performance remains unaccepted.                                                                                                                     |
| Full store suite excluding separately run E2E/network suites | 1325/1327 initially        | Memoization benchmark fails its throughput threshold under O0. Native replays had grown a tracked sample permascroll; the fixture was restored exactly and its purity test then passes.                                                                                            |
| Full Xuzz application suite                                  | 99/99                      | `full-xuzz-final.log`.                                                                                                                                                                                                                                                             |
| Full native UI suite                                         | 232/233                    | Object-manager `FittedRowsShareSafeGeometryAndFullLabels` fails independently in isolation. Its owning product/test files are untouched; pinned full-binary comparison has not established baseline classification.                                                                |
| Renderer core comparison                                     | Pass                       | OpenGL/OpenGL ES/Vulkan document, overlay, picking, atlas growth, coarse and multithreaded recording checks. No thresholds changed. Small sample does not exercise split threaded recording.                                                                                       |
| Broader backend E2E extension                                | 32/36 on OpenGL; blocked   | Whole-page ink threshold, New Xanadoc control, drag destination and foreign-run reader-owned-store checks fail. This prevents later backend E2E comparisons. Their baseline classification is unresolved; retain failures rather than claiming preservation.                       |
| Network namespace suite                                      | Environment skip           | Runner cannot create root/unprivileged network namespaces. No two-peer network pass.                                                                                                                                                                                               |
| Formatting                                                   | Pass                       | `format-check-checkpoint.log`.                                                                                                                                                                                                                                                     |
| Lint                                                         | Existing baseline failures | Shell/YAML/config harmony and UI text policy pass. Markdown lint retains the same 37 diagnostic rows as pinned baseline, with no new diagnostics in touched docs.                                                                                                                  |
| Assistive technology                                         | Environment block          | AccessKit not compiled and `dbus-run-session` absent. Internal named tree/actions and real pointer/keyboard input are bounded evidence only.                                                                                                                                       |

The original offscreen full-suite attempt cannot instantiate Vulkan windows. The first Xvfb run also
hit a server reset between SDL initializations; these diagnostics remain in the evidence.
`make test` is therefore not reported green. The remaining suites were run separately to expose all
relevant failures instead of stopping with the library benchmark.

The dock paging assumption was reproduced using both pinned and current presenter code: at 640
pixels and 1.5 scale both show an incomplete floating panel and a complete compact unpaged dock. The
corrected test verifies exact prefixes and every command in either layout. The D1 identity fixture
now uses nonadjacent source addresses, since adjacent insertions merge.

Raw PPMs were converted losslessly to PNG; `evidence/capture-conversions.json` retains their hashes
and workspace archive locations. Original launch records keep the exact PPM paths used at runtime.
Retained Luna 2 profiles are outside Git with a hashed manifest. Full input logs and selected store
operation dumps remain in Git. Text views normalize CRLF/trailing whitespace; original affected
capture bytes are retained as deterministic gzip archives with `evidence/text-normalization.json`.
The two reproduction Python files in `evidence/` record this workspace's paths; adjust their
root/profile paths before replaying in a fresh session.

Next: keep the first increment opt-in; settle broader legacy failures against the pinned baseline,
complete distant/unavailable/branch/reopen J24–J25 variants, run narrow/enlarged text and reduced
motion, establish AT and optimized performance gates, then proceed through the dependency map in the
project status file. There is no full spatial journey or cutover acceptance.
