# Publication download validation — 2026-10-05

The known-publication download path works through Ctrl+O on an isolated local DHT. A fresh reader
resolves an author/document magnet, downloads the signed manifest and its content/history carriers,
verifies the complete store, then opens the author's selected version. Completed snapshots survive
restart and reopen offline. This is a prerequisite for P2, not completion of P1–P7: author-key-only
catalog discovery, topic search, commentary/update notifications and link-package workflows remain
pending.

## Interface evidence

| Step                                                      | Affordance                                                                | Outcome | Evidence                                                                                       |
| --------------------------------------------------------- | ------------------------------------------------------------------------- | ------- | ---------------------------------------------------------------------------------------------- |
| Resolve and download from an empty reader profile         | Ctrl+O, Tab, publication magnet, Return                                   | Pass    | `build/publication-download/network-ui/download.log`, `queued.ppm`                             |
| Review completion                                         | Wait for background work, Return to refresh                               | Pass    | `network-ui/ready.ppm`, internal accessibility dump in `download.log`                          |
| Open complete publication                                 | Right to Open completed publication, Return                               | Pass    | `network-ui/opened.ppm`; visible Story Ideas text and selected reader tab                      |
| Verify retained model after UI exit                       | Native loader reads the downloaded store, with a fresh reader permascroll | Pass    | Namespace assertion checks signed store ID, exact operation count and selected text            |
| Reopen a completed snapshot after restart                 | Ctrl+O download entry, Open completed publication                         | Pass    | `tools/publication-local-test.py`, `download-reopen.log`, `download-reopen.ppm`                |
| Show a failure and retry                                  | Ctrl+O magnet without a configured swarm; Retry download                  | Pass    | `download-failure.log`, `download-failure.ppm`, `download-retry.ppm`                           |
| Keep local opening usable with a corrupt download receipt | Ctrl+O, then Custom path to a valid local reader                          | Pass    | `download-invalid-cache.log`, `download-invalid-cache.ppm`, `download-invalid-cache-local.ppm` |
| Open the same completed snapshot twice                    | Download picker in the same process                                       | Pass    | Two command results name the same store index; retained authored operations are unchanged      |

The live namespace test starts the publisher with explicit mock verification. The reader receives a
publication magnet and a named DHT bootstrap node; it has no direct BitTorrent peer address and no
preloaded content cache. The downloader uses DHT peer discovery for the manifest and dependencies.
The mock enrollment and node configuration are test setup, not a new production enrollment UI.

The offline picker check uses a previously verified cached reader fixture to isolate that control;
it is not claimed as an additional live transfer. Headless offscreen OpenGL captures and the
internal accessibility tree were inspected. Native desktop assistive-technology delivery and other
rendering backends were not exercised in this batch.

## Integrity and lifecycle checks

Seven inbox model tests cover complete-history restoration, signed key/salt/sequence mismatches,
corrupt dependencies, byte/count budgets, invalid retained receipts, cancellation, retry and worker
shutdown. A ready reader has the signed store identity and selected text without appending incoming
primedia to the reader's permascroll. Transport construction, use and destruction stay on the
worker.

Metadata-only magnets remain in upload mode until their file paths, piece shape and byte budget are
accepted. A separate namespace test proves that requesting reads before release obtains no payload;
after release, bytes arrive and remain readable after copied piece buffers are discarded. Pieces
larger than 16 MiB are refused. Complete-store installation still rechecks the retained files and
publication inventory/history. Cancellation is checked under the final status lock, preventing an
accepted cancellation from committing Ready.

Review also fixed snapshot deletion with empty temporary primary workspaces, competing Store
instances on repeated opens, render-loop termination on malformed retained receipts, and libtorrent
callback-state destruction before its session threads had joined. Snapshots live under the user's
XDG data directory, separately from primary stores. Authored editions and histories are preserved;
opening does not create a subscription or choose a different edition.

`make -j$(nproc) test` passed: 539 library tests, 1,249 engine tests, 57 Xuzz tests, 119 ZigZag
tests, 12 namespace transport/mutable-name tests and one publication namespace integration with the
live keyboard flow. The engine also reported one optional video skip and one disabled benchmark. The
focused inbox, outbox and inventory run passed 38 tests; 24 automation-script tests passed. The
local publication runner passed its signing/edition, cached opening, offline and download-control
checks.

After the final receipt commit-order fix, the 38 focused tests and the live namespace/UI integration
passed again. The Docker recipe refreshed the compiled local image with the affected inbox object
removed, forcing its rebuild; the resulting `gleditor-swarm-test:local` image (`8b0375f74c72`)
passed 95 smoke tests with networking disabled. Its inbox source digest matches the working tree,
and no fixture publication key state or XDG identity data was retained in the image. Repository
formatting and lint gates passed. Logs and captures are retained under `build/publication-download/`
and `build/publication-local/run-wtmyh7k4/`.

## Remaining findings

- Author-key-only following needs a signed author catalog at `bep46:<key>/catalog`; topic rendezvous
  and signed advertisement exchange are still missing. Discovery-result activation is wired to the
  downloader, but an empty production catalog has no authenticated remote results to activate.
- A crash before the atomic signed receipt is retained can leave an invisible partial cache/store.
  Normal failure and cancellation clean their output. Safe crash reclamation needs cross-process
  ownership protection or an explicit cleanup control; startup must not delete a live peer's work.
- Cancellation during local piece review or installation waits for that bounded operation to return.
  Add interruption hooks within verification/copying to improve responsiveness for large stores.
- The reader tab currently uses the native folder name `store`. Preserve a private display label
  from the signed publication title when catalog metadata is integrated.
- Long Custom path/magnet text can wrap near the Open form footer. Input remains complete, but the
  field should scroll or grow without colliding with other controls.
- Persistent subscriptions, missed-update recovery, backlinks/commentaries, independent link-package
  publication/discovery and private reader visibility toggles remain unimplemented. The complete
  seven-user-journey acceptance run is still pending.
