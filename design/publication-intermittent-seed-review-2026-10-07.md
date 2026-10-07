# Intermittent publication seed review failure

Status: open for investigation. Observed during UI text-fitting batch 6 on 2026-10-07, committed as
`d7a0a87`; the tested build was based on `359484e` with the batch 6 changes. No publication
implementation or publication test changes were included in that batch.

## Observation

`make -j$(nproc) test` passed 733 library, 1,255 engine, 59 Xuzz and 166 ZigZag tests, then all 12
namespace swarm tests. The final network test failed:

```text
PublicationOutboxNetworkTest.RemoteDhtAcknowledgesAndRetainsTheSignedPointer
```

Its keyboard-driven Xuzz reader resolved the signed publication magnet, then showed:

```text
Download failed (1/2 dependencies): publication seed file mi...
Publication is not ready to open
```

The truncated UI message corresponds to the exception prefix
`publication seed file missing or truncated:` in `publication_outbox.cpp`'s `checkedSeed()`. The
test expected `Ready to open` and an installed inbox copy; neither was present. The failed test took
56.9 seconds. Earlier steps in the same test passed: publishing, remote DHT resolution, direct
manifest reading, a separate-process download and offline reopening.

An immediate isolated retry passed without code changes:

```sh
make -j$(nproc) test/publication-swarm
```

The retry ran one test in 58.5 seconds. This is an intermittent failure, not a diagnosed cause or a
confirmed regression. The retry does not erase the failed full-test result.

## Evidence locations

The original full-test log is `/tmp/ui-text-fit-batch6-full-test-final.log`; the passing retry log
is `/tmp/ui-text-fit-batch6-publication-retry.log`. Both are temporary artifacts and may disappear.
The UI writes `build/publication-download/network-ui/download.log` and `queued.ppm`, `ready.ppm`,
`opened.ppm`; subsequent runs overwrite those paths. The failing test's temporary reader files were
removed, so the failing dependency's actual file size and complete exception suffix were not
retained.

The integration journey is in `tests/xudu/publication_outbox_test.cpp`, in the named test above. The
relevant implementation is `apps/common/xanadu/publication_inbox.cpp` (`DownloadTransport::fetch`
and dependency review) and `apps/common/xanadu/publication_outbox.cpp` (`checkedSeed`).

## Investigation hypothesis and next steps

A disk-write completion race is plausible but unproven. The downloader verifies pieces through
`readStream`, then immediately calls `reviewPublicationSeed`, which checks on-disk file existence
and length. It destroys the transport only after those reviews. The existing shutdown comment says
libtorrent flushes disk writes before installation; review therefore precedes that flush boundary.

1. Repeat the isolated namespace test and retain each failing reader cache before cleanup, with
   separate logs and captures per attempt. Start with the existing headless test target.
1. Capture the complete exception, dependency hash, expected/actual file lengths and transfer/disk
   completion state without logging document text or key material.
1. Determine whether validated in-memory pieces can precede complete disk files. Compare the review
   ordering with explicit disk-write completion or a retained-file barrier.
1. If confirmed, add a deterministic regression for that ordering and fix the completion boundary.
   Preserve piece verification, path/length checks, cancellation and offline reopening; do not mask
   the failure by accepting incomplete files or merely increasing the UI wait.
