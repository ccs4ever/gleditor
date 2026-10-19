# Twenty-first spatial UI checkpoint

Based on `cc26a174`, this increment adds exact logical-file sizing at the existing native-store and
retained-seed writers. The spatial overhaul remains open.

## Behavior

`Store::nativeFileSizes()` measures the full 65,536-byte operation header plus checked node extent
and the canonical encoded store tables. It refuses metadata that save would still need to seal,
including authored external-scroll registration. Repeated queries append no operations or primedia.
Both native and OSMIC save paths share their existing deployment-table construction; the table
writer and size query share one encoder. Existing format versions and fixture bytes are unchanged.
`PublicationSeed::retainedFileBytes()` includes payload and the metainfo sidecar with checked
addition. The installer measures reviewed seeds before copying, checks the copied footprint, sizes
its one restored store before saving, and reconciles actual native output lengths before promotion.

These are prepared native-install measurements after dependencies are available. They do not impose
pre-payload admission, an aggregate quota, or a whole-request peak bound. Logical file lengths
exclude filesystem allocation overhead, caller-owned permascroll flushing, manifest carriers and
duplicate download caches. No transport, cleanup policy, pin lifecycle, UI control or format is
changed.

## Validation

GNU Make passes, including the real swarm peer. Fresh headless UI 248/248 and Xuzz 107/107 pass. The
broad engine comparison passes 1,347 baseline tests and 1,352 final tests with no baseline cases
removed. One unchanged benchmark is disabled. The exact exclusion filter and XML are retained; broad
native E2E and the other network suites are excluded. Five new cases and extended existing cases
cover prepared/refused sizing, stable primedia, actual copied and native files, overflow,
empty-header extent and exact canonical bytes of three existing table fixtures. The earlier
1,351-case candidate run and focused worker run are retained separately. The worker initially used
formatter 23; root reformatted all touched C++ with the required clang-format 19 before canonical
builds and final checks. The initial root format command also selected the system formatter 23; the
final gate explicitly selects 19. One new archived-plan long line was shortened after lint reported
it. Both failed attempts are retained; no formatter or lint rule was relaxed.

The library and library-test binaries remain byte-identical to checkpoint 17, so its 799/799 and
mip/cache 53/53 evidence is reused rather than presented as new runs. Fresh strict GL/GLES/Vulkan
comparison passes unchanged criteria: GL/GLES ordinary and overlay are exact; Vulkan is 0.5104% /
0.5096% under the existing 1% limit with channel tolerance 1. The sample does not exercise actual
split recording. Final format/lint/text results are in `gates.json` and retained logs.

The [restart inspection](native/native-compat/restart-inspection.md) preserves the exact failed
request, attempt 1 and completed/current visit 1 through ordinary controls. The separate
[local signed import](native/local-import/validation.md) succeeds through Open Document's Custom
path field, exercising the changed installer. Its native files are 67,840 and 697 bytes; three
retained dependency trees total 61,232 logical bytes and exactly match the source closure. Source
operations, signed manifest and package visibility remain unchanged. Ordinary activity state writes
occur, with no additional completed visit. Both processes exit 0. This is local import, not network
acquisition. The final import frame shows clipped page text under an existing context overlay; it is
not full-page readability acceptance. Internal accessible trees are retained; this build lacks
native accessibility, so no AT-SPI pass is claimed. See the independent visual review.

A separately isolated real-peer metadata-only test passes under the existing veth harness and GDB.
The observed save directory is empty immediately before explicit `startDownload()`, then contains
the verified 218-byte payload after flush. This is one concrete filesystem observation, not a
universal no-write guarantee or normal-mode publication UI validation. The peer executed from
`build/xudu-swarm-peer` and is byte-identical to the recorded frozen peer. Raw runner output,
breakpoints, environment, hashes and the one-line private harness change are retained in
`metadata-observation/`.

## Exact next task

Derive exact native-install projection from signed publication inputs before dependency payload
transfer, sharing restore/table construction rather than inventing a copy factor. Add transport
metadata preflight before `startDownload()`, accounting for encoded metainfo sidecars. Then enforce
a governed aggregate logical-file budget on the existing worker across caches, attempts, retained
copies, native files and manifest carriers; reconcile before writes. Preserve installed roots,
receipts, pin consumers and explicit cleanup. Do not expose Forget before the ownership/release
protocol is complete. Normal service configuration and real identity authority remain separate
prerequisites; do not substitute a mock verifier. Complete J1–J32, D1–D5/S1–S3/H, native AT-SPI,
remote variants and optimized performance remain open. The optimized measurement plan records the
historical O0 failures without treating current default-build success as optimized acceptance.
