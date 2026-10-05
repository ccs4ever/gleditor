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

## Complete inventory and edition review batch

This batch implements format 2 whole-store inventory and verified reconstruction, and adds explicit
edition review, creation and repointing to Publish. Existing designations remain unchanged by
default. Author annotation edits are kept separately from derived caches, so a reopened edition can
be repointed without inventing an annotation at its old target. Pending author decisions must be
sealed before a history publication is signed; branch folds do not override them.

Evidence root: `build/publication-inventory-fixes/`. UI evidence:
`build/publication-local/run-fo8o_3bc/`. The final form frame was inspected and fits the viewport;
its accessibility dump names the selected release and its old/new target versions. Platform
assistive-technology delivery remains untested.

| Scope                | Step and affordance                                                                       | Outcome       | Evidence                                                                    |
| -------------------- | ----------------------------------------------------------------------------------------- | ------------- | --------------------------------------------------------------------------- |
| P1 controls          | Add `release` in Publish with Ctrl+Shift+S, Tab and Right                                 | Pass          | `edition-1.log`, `publish-form-1.png` in the UI evidence root               |
| P1 controls          | Reopen and publish with Keep all existing editions                                        | Pass          | `edition-2.log`, `publish-form-2.png`                                       |
| P1 controls          | Repoint that release to the edited version in Publish                                     | Pass          | `edition-3.log`, `publish-form-3.png`                                       |
| P1 controls          | Verify readiness and execute retry with Ctrl+Shift+P                                      | Pass          | `status.log`, `status.ppm`, `results.json`                                  |
| Engine prerequisite  | Restore documents, slices, branches, editions and annotations; retain reader bytes        | Pass          | `engine-final.log`, ten PublicationInventoryTest cases                      |
| Network prerequisite | Fresh opposite-namespace process fetches the manifest and every dependency, then restores | Pass          | `swarm-final.log`; explicit mock identity and zero reader-permascroll bytes |
| P2/P3                | Full-store opening through author/topic discovery controls                                | No affordance | Catalog ingestion and remote-open integration remain the next batch         |

The full engine run executed 1,236 cases: 1,233 passed, one optional video check skipped, and two
failed. The memoization benchmark missed its fixed throughput threshold while Docker was building;
it passed without build load at 39,470 hits/second. The page-count check captured the first frame
and exited before the settled profile summary. That diagnostic now runs in profile mode, and both
failed cases passed in `broad-recheck.log`. Earlier broad-run failures also exposed obsolete
`--screenshot` options; capture tests now use `--capture`. One early run overlapped linking and
produced transient binary-launch failures; final checks ran after the build completed.

All 57 Xuzz and 119 ZigZag checks passed against the final model. The eleven existing namespace
transport/mutable-name checks passed separately from the full publication restoration check. The
first opposite-namespace reader attempt timed out on metadata with a wildcard publisher interface;
the final fixture binds its explicit namespace address and passes. DHT safeguards are unchanged.
Three UI publications passed signed topics, durable sequence advancement, incremental permascroll
seals, actual GPG signature/digest verification, zero-filled private ranges, edition controls and
retry. Temporary signing keys were removed by the driver.

The rebuilt `gleditor-swarm-test:local` image passed 78 build-time smoke checks, including complete
inventory restoration. Its image ID is
`sha256:f6769738e65a7e7039b7ebd1ff8ccf244b722d4b8b347f97aa877be1dac11462`. The image contains the
final application/model changes; the later profile-only pagination harness correction does not
affect its publication smoke filter. Repository formatting and lint are checked before this batch is
committed.

None of P1–P7 has passed end to end. Full-store restoration is currently an engine API and
diagnostic peer mode. The selected-EDL adoption path remains in Session. Durable reader deployment
bindings, remote-open UI, author catalogs/topic exchange, subscriptions, commentary discovery and
independent link-package publication/toggles still require implementation. Initial signing/sealing
also remains synchronous on the Session command path.

## Cached opening and offline reader batch

This batch connects complete restoration to ordinary cached opening and native offline reopening.
Author editions stay unchanged unless the reader explicitly edits their local copy; opening and
saving add no authored operations. Signed manifests that lack a complete history are refused by the
reader-opening pathway. Selected-EDL quotation remains an explicit core operation.

| Scope                          | Affordance or check                                                                          | Outcome | Evidence                                                 |
| ------------------------------ | -------------------------------------------------------------------------------------------- | ------- | -------------------------------------------------------- |
| Cached opening prerequisite    | `Ctrl+O`, Custom path, signed `.xanadoc`, Enter                                              | Pass    | Local runner `reader-open.log`, form/document captures   |
| Offline reopening prerequisite | `Ctrl+O`, retained native reader path, Enter while publisher seeds are unavailable           | Pass    | `reader-offline.log`, capture, unchanged `ops.nodes`     |
| Dependency rejection           | `Ctrl+O`, signed manifest without its carriers                                               | Pass    | `reader-rejection.log`, stderr diagnostic, no new reader |
| Full model restoration         | Documents, slices, branches, editions and annotations; reader-only edits; corruption refusal | Pass    | `publication-reader-fixes/engine-verified.log`           |
| Cross-veth deployment          | Diagnostic network child installs complete reader; parent reopens after child exits          | Pass    | `publication-reader-fixes/swarm-recheck.log`             |

Store tables change from version 3 to **4**, including ordered deployment descriptors and the
imported author-local root. Versions 2 and 3 are refused with `StoreTablesUnreadable`; native node,
operation-wire and signed publication formats are unchanged. Both sample generators were run, and
all eighteen fixture operation dumps were compared before/after. Sixteen are identical. Two core
hypertext fixtures now include current-edition operations from the preceding author-choice fixes;
regression checks verify their text, links and transclusions. Immutable media seed carriers are
retained with the regenerated multimedia fixtures. Generated private publication-state directories
are excluded from the commit.

The first full engine attempt ran 1,242 tests: 1,235 passed, one optional video check skipped and
six failed. Two orchestration fixtures still assumed EDL-only `--read` and implicit merging into
store zero. They were updated to complete-history opening and explicit curator quotation; the
separate bypass-rendering fixture uses its already quoted versions. Four query/compiler checks
launched old executables linked to table format 3; the full build relinks every program before final
validation. The initial failed log is retained for review.

This batch does not provide following by author key, DHT topic discovery, UI downloads, update
subscriptions, backlinks or independent package toggles. Telescope now explains that fetching is
unavailable instead of manufacturing a summary presented as a verified publication. Cached copying
and verification remain synchronous. These results are prerequisites; P1–P7 remain incomplete.
Platform accessibility delivery is still untested without AccessKit.

The first namespace installation check exposed imported scroll descriptors being automatically
registered as new author operations during native save. The loader's private bindings now carry
those descriptors without writing authored registry cells, including a quotation on another branch
whose slice registry never named it. The new regression passes, and the opposite-namespace reader
installs and reopens successfully after its network child exits (`swarm-recheck.log`). The eleven
legacy namespace checks passed in `swarm-final.log` before that publication check exposed the
defect.

Final keyboard evidence is `build/publication-local/run-ieq55szl/`: three signed publications,
explicit edition creation/repointing, status/retry, cached reader opening, offline native reopening
while publisher carriers are moved away, and missing-carrier rejection without a new reader store.
The form and both document frames were inspected. The very long custom-path value wraps into the
Open form footer; full keyboard entry still works. Fixing single-line field clipping/caret scrolling
is a remaining presentation finding. Native SDL message boxes are suppressed by the headless runner,
so the rejection message is validated on stderr rather than through a visible/accessibility dialog;
platform message-box delivery remains unverified.

Docker excludes generated `publication-state` directories as well as GPG homes, so temporary fixture
device keys do not enter the image context. Only immutable public carriers are retained with the
native sample stores.

Final native outcomes: `engine-verified.log` ran 1,243 tests with 1,241 passes, one optional video
skip and one throughput-threshold failure during concurrent UI/model checks. The cache-hit benchmark
measured 34,978 hits/s against a 35,000 threshold; its isolated recheck passed at 51,180 hits/s in
`benchmark-idle.log`. Thus all 1,242 executed cases succeeded either in the full run or that
recheck; this is not a claim of a single all-green final full run. `xuzz-verified.log` passed 57
tests and `zigzag-verified.log` passed 119. The 35 focused reader/table/outbox checks also passed in
`registry-focused.log`. Formatting and lint passed before the final report update; they are rerun
before committing. The image is rebuilt after the registry fix, rather than relying on the earlier
87-test image.

The final `gleditor-swarm-test:local` image passed **88 smoke tests**, including the
imported-registry regression and format-4 table checks (`docker-final.log`). Its image ID is
`sha256:7188a02b353195bb8309f898cd59b9e9e71f8c6fb22da4e252c6d28d40b71236`. A container with
networking disabled confirmed the three regenerated fixture permascrolls contain no
`publication-state` directories. Repository formatting and lint passed after the code/report
changes.
