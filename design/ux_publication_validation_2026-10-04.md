# Publication validation, 2026-10-04

## Result

None of P1–P7 passed end to end. Local text creation, the publication form and signed local sealing
worked; remote publication, discovery, responses, notifications and link layers remain incomplete.
The initial namespace prerequisite failed at veth creation, and the initial image attempt found no
Docker daemon. After those host prerequisites were supplied, all 11 host network tests passed. The
resumed image and container results are recorded below; no infrastructure result substitutes for a
user journey pass.

Contract: [publication journeys](ux_workflow_publication.md). Revision: `6fddffa` (full revision in
the evidence directory). Product code was unchanged in the initial journey runs. Resumed image
validation required small compilation fixes: explicit numeric cell IDs in rank diagnostics, an
unused-field annotation for the SVG cache when optional ThorVG is absent, and assertions that
consume the results of two existing benchmarks. The registry lifetime test now reconstructs a live
store in the same storage instead of querying through a destroyed object. Two aggregate
initializations use equivalent explicit construction to avoid GCC 16 diagnostics.

Initial evidence root: `/tmp/publication-journeys-20261004`. The initial artifacts are no longer
present in this resumed session; their outcomes below are the previously recorded results. Fresh
infrastructure evidence is available under `resume/`. Four independent profiles had distinct XDG
data/config paths and disposable OpenPGP signing keys. Alice's local publishing action also minted
an Ed25519 identity. No mock Oracle adapter was available, so no approved/pending/rejected verifier
test was performed. No four-user swarm or bootstrap topology started successfully during the initial
run.

## Executed checks

- Initialized recursive submodules and ran `make -j$(nproc)`; built the missing peer explicitly with
  `make -j$(nproc) xudu-swarm-peer` before retrying the network prerequisite.
- Ran 48 tests across PublicationTest, BinaryOpsTest, MutableLinkTest, LinkPackageTest and
  LinkPackageInteractionTest. All passed. See `unit-tests.log`; these are local prerequisite tests.
- Ran seven headless OpenGL UI sessions, including a corrected publication retry, using the
  offscreen driver, dummy audio, software rendering and isolated profile directories. Inspected
  captured frames and retained scripted accessibility descriptions. The build reports
  “accessibility: not in this build”, so platform assistive-technology delivery was not validated.
- Ran `tools/swarm-netns-test.sh`: `Error: Unknown device type.` at veth creation. No remote piece
  transfers or DHT resolutions ran; script cleanup removed its temporary namespaces.
- Ran `make -j$(nproc) swarm-image`: Docker could not connect to `/var/run/docker.sock`. Image
  compilation and container smoke tests remain unvalidated. The target's dry run also succeeded with
  a failing `pkg-config` shim, proving host native dependencies do not gate this target.

## Journey findings

Rows group steps stopped by the same unmet prerequisite. “Blocked” means the steps were not run, not
that they independently failed. Missing affordances are corroborated by source inspection.

| Journey | Step                                                      | Affordance used or sought                        | Outcome       | Evidence                                                                                    |
| ------- | --------------------------------------------------------- | ------------------------------------------------ | ------------- | ------------------------------------------------------------------------------------------- |
| P1      | Identity/mock verification                                | Profile identity panel and verifier boundary     | Missing       | No adapter/control found; profile public keys in `alice/public-keys.txt`                    |
| P1      | Type a new xanadoc                                        | `Ctrl+N`, typed sentence                         | Pass          | `alice/ui.log`, `alice/xanadoc.png`                                                         |
| P1      | Create slice in that same store                           | `Ctrl+Alt+N`                                     | Fail          | `alice/controls.log`, `alice/new-slice.png`; action creates another store                   |
| P1      | Import, topology, branch fixture                          | Same-store slice/content workflow                | Blocked       | Required combined-store UI path not established                                             |
| P1      | Review dependencies and topic tags                        | `Ctrl+Shift+S`                                   | No affordance | `alice/publish-form.png`, form description in `alice/ui.log`                                |
| P1      | Sign/seal local xanadoc, diagnostic subset                | Form fields and Enter                            | Pass          | `alice/publish-corrected.log`, `alice/decoded-manifest.json`, `alice/provenance-verify.log` |
| P1      | Seed/announce complete store and reopen publication state | Publish form submission                          | Missing       | Local `.xanadoc` written; no DHT/catalog announcement in `Session::publishDocument()`       |
| P2      | Add Alice key/follow/browse catalog                       | `F3`, inspected controls                         | No affordance | `bob/catalog.png`, `bob/controls.log`; demo catalog only                                    |
| P2      | Fetch/read/subscribe/resume                               | Author-catalog workflow                          | Blocked       | P1 announcement absent; namespace setup failed; subscription control absent                 |
| P3      | Search Ideas                                              | `F3`, click search bar at `400,94`, type `Ideas` | Fail          | `carl/search-before.png`, `carl/search-after.png`, `carl/controls.log`                      |
| P3      | Open remote result and subscribe                          | Topic discovery                                  | Blocked       | No real metadata ingestion or published result                                              |
| P4      | Quote Alice and publish two commentaries/backlinks        | Quotation and package workflows                  | Blocked       | Alice not remotely retrievable; backlink publishing controls absent                         |
| P5      | Discover responses/revise/publish/notify/reconnect        | Response discovery and update subscription       | Missing       | No UI package discovery or sequence polling/acknowledgement integration found               |
| P6      | Create/review/publish Devin's package                     | Command menu and package editor                  | No affordance | `devin/commands.png`, `devin/controls.log`; core package APIs lack UI workflow              |
| P7      | Discover/preview/toggle/reopen package layers             | Reader package-layer controls                    | No affordance | No package identity/filter/preference integration found in Xuzz                             |

## Reproduced failures and proposed fixes

1. **Same-store creation.** `apps/xuzz/xuzz_app.cpp` registers New slice by first calling
   `views.newDocument()`, allocating another store. Offer an explicit current-store structure
   creation path and prove both structures survive save/reopen before preparing P1's fixture.
1. **Publication review and network completion.** `apps/xudu/views.cpp` asks only for name, title,
   author, email, signing key, passphrase, rights and note. `Session::publishDocument()` writes a
   local manifest and seals history, without seeding/announcing the complete dependency closure. Add
   store/scroll/permascroll scope review, topic metadata, real seeding, mutable announcements and a
   user-visible completion state. The local diagnostic used a real temporary signing key; GPG
   verification passed. It is not evidence of whole-store globalization.
1. **Search input edits the document.** The search-bar pick reports overlay tag `14006`, but typing
   `Ideas` changes the underlying store's text and leaves the displayed `#hypertext author:nelson`
   query unchanged. Route text/keyboard input to `SwarmTelescopeOverlay` while its search field is
   focused. Include the overlay in accessibility descriptions and expose author-key following.
1. **Discovery metadata is demonstration data.** `SwarmCatalog` seeds sample entries on
   construction; Xuzz instantiates it without signed remote catalog/topic ingestion. Implement the
   `catalog` and topic-rendezvous transport before claiming Carl or Bob retrieved Alice's
   publication.
1. **Commentary, package and notification integration.** Signed package primitives and rendezvous
   hashing exist, but Xuzz lacks editor/review/announce/discover controls. `adoptLinkPackage()` adds
   persistent store links and does not provide reversible private layers. Add package identity and
   filtering with per-reader preferences; add persisted mutable-sequence polling and notification
   acknowledgement. Scroll-sealed broadcasts do not satisfy store update notices.
1. **Infrastructure prerequisites.** Host veth support and the Docker daemon were supplied for the
   resumed run. Containers on an internal network can manage peer isolation; missing publication UI
   remains a separate finding. See the [container instructions](../packaging/docker/README.md).

## Implementation follow-up

The first fixes and their fresh verification are recorded in
[publication implementation progress](publication-implementation-progress.md). Findings above remain
the historical validation result. The seven full journeys still need an implementation acceptance
run.

## Resumed infrastructure validation

Evidence is retained under `/tmp/publication-journeys-20261004/resume`.

- `swarm-netns.log`: 11 tests across SwarmTest and MutableNameTest passed after veth was loaded.
  This includes network piece transfer, mutable-name resolution, retrieval using only a name,
  unknown-name timeout and salt isolation.

- `zigzag-tests.log`: all 119 tests in the native ZigZag suite passed after the numeric-diagnostic
  compatibility fix.

- `registry-tests.log`: all seven DimensionRegistry tests passed with the corrected lifetime check.

- `initialization-tests.log`: all 27 SystemDocs, VQLCompiler and benchmark tests passed natively.

- The first successful contact with Docker exposed an unavailable Arch RNP package. Debian stable
  then exposed missing Poppler private headers, SDL3_image, external-header warnings, optional SVG
  warnings and an older Poppler API. The image now uses Debian forky with the required packages,
  treats external Poppler headers as system headers, and keeps project warnings enabled.

- `docker-build-success.log`: `make -j$(nproc) swarm-image` completed and tagged
  `gleditor-swarm-test:local`. Its build-time publication/history/link-package smoke tests passed
  all 50 tests; `docker-smoke.log` records the same default command passing with no network.

- `docker-image.json`: image identity
  `sha256:d3e87aeb92d2d777200f1cc55cc6044f7d9f816d523eb8b1eab2b436ccede0b0`, size 2,457,437,732
  bytes. This developer image retains source, tools and build objects.

- `docker-initialization-tests.log` and `docker-registry-tests.log`: 25 system/VQL compiler tests
  and seven registry tests passed inside the image.

- `docker-ui.log` and `docker-ui/smoke.png`: headless software OpenGL rendered the typed text
  `Container publication test`, also present in the accessibility description. The captured frame
  was inspected. This checks editing and rendering, not the publication flow or platform AT.

- `docker-swarm-writable/`: one seeder and three simultaneous reader containers on an internal
  Docker network. Each reader passed all 11 SwarmTest/MutableNameTest checks (33 total), including
  piece transfers and retrieval by mutable name. Separate container filesystems held downloads; the
  seeder minted a disposable Ed25519 key. The topology, public key, image ID and logs are retained.
  The runner removed its own four containers and network afterward.

- The first container attempt in `docker-swarm/` timed out during transfers with read-only seed
  storage and a wildcard listen address. The successful retry used writable private seed storage and
  an explicit container IP. Both attempts remain recorded. This was a transport fixture, not Alice's
  whole-store publication, and did not supply a mock Oracle.

- Repository lint and formatting of edited C++/new Markdown passed. Repository-wide `format-check`
  failed on untouched `apps/xuzz/xuzz_app.cpp:1943` with the available clang-format 23. The failure
  is retained in `format-check.log`.

## Evidence limitations

The initial run recorded Alice's native header/operation dumps, decoded signed manifest and seal
prefix in `alice/store-header.txt`, `alice/store-ops.txt`, `alice/decoded-manifest.json` and
`alice/seal-header.txt`. There was no remote reconstruction against which to compare them.
Incremental restart, whole-store metadata, unpublished local-scroll dependency handling, offline
notification deduplication, mock verification rejection and third-party package toggles were not
validated. Vulkan, GLES and platform accessibility were not tested.

The first publication submission attempt selected the newly created slice and did not submit: Enter
opened the signing-key dropdown. Its log is retained as `alice/publish-submit.log`. The corrected
attempt reopened the actual text store, replaced fields with backspaces and submitted from the
passphrase field. The recorded local publication pass came from `alice/publish-corrected.log`.
Temporary private signing keys were removed after the initial evidence collection. Its public
records and captures are no longer available in the current temporary directory; resumed logs remain
available.
