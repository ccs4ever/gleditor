# Reader package layers: validation (2026-10-07)

This batch makes an independently signed package a private reader contribution. Review offers
Enable/Disable reader layer and Select link for navigation. Enabling does not adopt links, register
scrolls or append operations to a visited store. Each choice pins one immutable carrier; a newer
curator revision starts disabled. Author edition choices remain authoritative.

## Interaction contract

Selection uses the existing link panel, command boundary and activity store. It exposes both
complete ordered endsets and independent member/occurrence cursors. Selecting does not move focus;
entering focuses the explicitly chosen occurrence. Unresolved endpoints remain listed. A target
closed before entry is refused before an activity visit is recorded. Disabling dismisses the live
package selection, retains focus and preserves prior activity; a queued accessibility selection is
refused after disable.

Rendering marks exact spans and draws a representative linear scaffold between resolved occurrences.
The scaffold does not pair authored members: navigation retains the original many-to-many link.
Curator identity is shown separately from the source owner. The review labels signature verification
and unchecked enrollment separately.

Activity uses a domain-separated SHA-256 derivative of the full immutable carrier hash as its
existing 128-bit authority, plus the signed link ordinal. The verified reverse map retains the full
hash. Native store identities are author-controlled, so hydration, opening and enabling explicitly
refuse native/package authority overlap. Missing payloads retain their identity reservation and
preferences. Transient 64-bit render handles never become activity identities.

## Scope and interface evidence

The rootless namespace integration extends the existing independent-package publication test. Devin
publishes through the graphical form and seeds the signed package and catalog. Bob and Carl each use
a fresh profile containing Alice's signed source manifest, without an incoming curator key, catalog,
package or direct peer preload. Each discovers Devin through scroll rendezvous, fetches/reviews the
package, downloads its pinned source and opens it through the UI. Offline keyboard runs then enable
the layer, select its link, enter the second left member and cross to the right slice cell, restart
with the same exact package enabled, and disable it. Native `ops.nodes` and `store.tables` bytes
must remain unchanged. Evidence lives under `build/publication-layers/network-ui/`.

The fixture supplies Alice's store and commentary links; it does not certify authoring the complete
P1–P7 story through the UI. Its provenance fixture uses placeholder OpenPGP authorship, while
package and catalog signatures, DHT exchange and torrent piece integrity are real. Enrollment/Oracle
election remain mocked. The separate local runner checks real temporary GPG signing and publication
controls.

An initial namespace run incorrectly asserted that the right endpoint was unavailable. The signed
snapshot also contains its Research slice cell, which the resolver correctly found and entered. The
harness now checks that cell focus. A separate UI unit closes the target before entering and
verifies refusal without a new activity visit. No model behavior was weakened to satisfy the
fixture.

## Implementation and checks

Canonical per-store global scroll keys prevent equal numeric deployment slots from being confused.
One interval index covers the relevant documents and cells; repeated quotations, discontinuities,
partial coverage and unresolved signed members survive resolution. Package matching filters verified
retained packages against the selected publication document's actual byte spans.

The engine registry stores bounded format-1 reader preferences with an atomic rename and typed
version refusal. It preserves malformed files. These checks cover process restart, not power-loss
durability. New C++ handles signature/torrent identity, native-store address resolution, filesystem
persistence and renderer/accessibility integration; it reuses the existing navigation commands.

The touched legacy UI directories have moved to `apps/common/ui/xanadoc/` and
`apps/common/ui/slice/`, as required by the repository layering rule. Make and WebAssembly source
collection now use the shared UI tree. No native store, publication or package wire format changed.
WebAssembly wiring has not been compiled with Emscripten in this environment.

Final host build passed. The final focused run passed 17 engine cases and the full slice/UI suite
passed 192 cases. The library and Xuzz suites passed 748 and 61 cases respectively. The stable full
engine run executed 1,317 cases: 1,298 passed, 17 were skipped (network peers/optional video
absent), and two unchanged timing benchmarks failed while other work was running. Both passed their
original assertions on the quieter isolated rerun: memoization reached 44,897 hits/sec against
35,000, and Arrayfilade reached 10× against 5×. One benchmark remains disabled. Initial runs that
overlapped linking reported executable permission failures; stable runs cleared those orchestration
failures.

The corrected namespace package journey passed in 618 seconds, and all 12 separate namespace
transport/name tests passed. The real temporary-GPG local runner passed three UI publications,
edition choices, cached opening, offline reopening and download controls, with evidence in
`build/publication-local/run-5ul8r2c0/`. The final resolver exercise indexed 50,000 pieces in 8.4 ms
and resolved 2,000 members in 4.4 ms; these measurements exclude rendering and include the test's
carrier identity work.

Docker image `gleditor-swarm-test:local` (`e259f1177c18`) passed 135 smoke cases during the build
and again with networking disabled, plus both reader UI cases. All 71 changed C++/shader source
hashes match the host; the audit found no private identity artifacts or retired C++ sources. The
build reused local dependency/compiled layers and executed the tracked recipe's Make/test tail. It
is not a cold Debian installation result. Existing overlay test assertions needed braces and several
range loops needed references for Debian's compiler; no assertion was relaxed. Owned intermediate
containers and cache tags were removed. Format-check and lint passed with clang-format 19.

Framebuffer/picking, atlas, minification and text comparisons passed across OpenGL, OpenGL ES and
Vulkan. All 33 Xuzz orchestration cases passed on OpenGL and OpenGL ES. Vulkan passed 29 and failed
four: modal focus/pouch toggle, pouch divider, selection drag and scripted pouch clasp. Three
failures report exhaustion of `DeviceVK`'s 64-pipeline descriptor pool when creating another
`canvas-image` pipeline; selection drag timed out after 120 seconds. The overall backend comparison
therefore remains red. Package-specific interaction acceptance is OpenGL-only in this report.

The [subsequent Vulkan validation](ux_publication_vulkan_validation_2026-10-07.md) resolves all four
failures and passes the complete backend comparison. The results above remain the evidence for this
earlier batch.

## Remaining work

- Individual link selection/editing during package preparation and package creation independent of
  an existing signed source publication.

- Full commentary document creation, Alice's reply citing both commentaries, update notifications
  and P1–P7 acceptance through the interface.

- Matching currently scopes to the selected document's published spans, not every slice cell or
  historic edition in the store. Scroll rendezvous remains a bounded peer query.

- Large occurrence fanout can still cost rendering work. Representative scaffolding avoids a
  Cartesian product; the measured resolver exercise is not a whole-frame latency guarantee.

- Captured navigation panels occupy much of the 800×600 viewport, with the destination page small
  behind the panel. These captures verify command state and exact focus, not comfortable reading of
  the target text. Panel/page framing remains a presentation finding.

- Platform AccessKit delivery and package-specific Vulkan/GLES interaction acceptance remain
  unverified. The actual package journey uses OpenGL and the accessible-tree harness.

- Enrollment, Oracle election, user creation/login and key revocation remain deferred.
