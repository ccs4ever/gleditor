# Twentieth spatial UI checkpoint

Based on `806c464b`, this increment adds an explicit partial-download cleanup action to the existing
publication status form and Inbox worker. It does not complete the spatial overhaul.

## Behavior

Failed and Cancelled requests offer Discard partial data. The action keeps the durable request, full
pin or observed mutable lower bound, original failure, phase and attempt counter. It queues cleanup
on the existing worker; the UI performs no filesystem scan or deletion. Pending cleanup blocks Retry
and duplicate cleanup. The form keeps its six rows and full-pin identity, leads with the cleanup
outcome, and exposes the full original and cleanup diagnostics through accessibility.

The worker validates every owned attempt before deleting any files. Only exact canonical
`<request-id>.attempt-N` direct children with canonical decimal N at or below the persisted counter
are eligible. Internal symlinks and special files refuse cleanup. Installed `<request-id>` roots,
other requests, malformed/future attempts, receipts and subscriptions are preserved. Logical regular
file lengths are checked for overflow. Iterative deletion checks shutdown between entries;
successful completion syncs the inbox parent where supported and reports the actual removed logical
bytes, including zero. A failure reports that cleanup may be incomplete, independently of the
request error.

Cleanup progress/results are transient and do not change XPI1. Restart never resumes deletion or
starts networking. If shutdown interrupts cleanup, remaining owned attempts require another explicit
action. Retry retains its identity and uses the next attempt number after cleanup. No Forget,
automatic eviction, aggregate disk quota or production transport is added. The private inbox has one
application writer; concurrent hostile local filesystem mutation is outside this path-based
protocol.

## Validation

Canonical GNU Make passes. Fresh headless full UI 248/248, Xuzz 107/107 and owning
publication/package 77/77 pass. The focused cleanup preservation test also passes under syscall
tracing on workspace ZFS: the Inbox worker removes only the owned attempt and syncs the inbox
parent. The main test thread subsequently tears down the whole fixture; those later removals are not
Inbox cleanup. The failed-attempt fixture measures 1,360 logical bytes, not a representative
production budget. Initial fixture timing and state failures were corrected without changing product
behavior.

The default library and library-test hashes still match checkpoint 17, allowing reuse of 799/799 and
mip/cache 53/53 evidence. Fresh strict comparison passes unchanged criteria: GL/GLES ordinary and
overlay are exact; Vulkan is 0.5104% / 0.5096% under the existing 1% limit and channel tolerance 1.
The sample does not exercise actual split recording. Format, lint and UI-text policy pass.

The [native zero-byte route](native-zero-discard-validation.md) uses the actual Discard control,
reports zero bytes and preserves the exact failed request/attempt after restart. Both processes exit
0\. Source/package hashes and the completed-visit/current state remain unchanged at 1; shutdown adds
activity metadata. The settled result fits three lines with all six rows and footer visible at
1440×1000. Independent pixel review rejects an initial missing-letter impression: the full capital D
is present. The intermediate pending frame is a partial redraw and is not pending-layout evidence.
Nonzero and failed-cleanup visuals, native AT-SPI and other backend form visuals remain unverified.

The first complete format check identified captured runner transcripts stored with a source `.sh`
extension in checkpoints 19 and 20. This increment renames them to the existing `.sh.txt` evidence
convention without changing their captured bytes, updates checkpoint 19's path manifest, and retains
the failing format log plus the correction's hashes. Formatter criteria are unchanged.

## Remaining work

Explicit cleanup relieves accumulated partial data but does not bound admission or aggregate disk
use. First derive exact pre-write native-store and retained-copy sizes from shared writer logic, and
verify estimates against actual installs. Then use the
[next staging policy plan](next-staging-policy-plan.md) to implement governed admission before
normal network traffic. Audit completed-root pin ownership before exposing Forget or freeing request
slots. Real identity authority/enrollment and BEP46 binding, normal configured remote acquisition
variants, complete J1–J32, D1–D5/S1–S3/H, native AT-SPI parity and optimized performance remain
open.
