# Metadata-only filesystem observation plan

## Question and scope

Observe the real filesystem immediately before `SwarmContentSource::startDownload()` in the existing
`SwarmTest.MetadataOnlyMagnetWaitsForExplicitDownloadAndCanDiscardPieces` test, then again after its
`flushDownload()` and file-content assertion, before `TearDown()` removes the test's download
directory. This can establish whether this test leaves regular files on disk while the magnet is
metadata-only, and what files exist after explicit download and flush. It does not prove production
UI reachability, network behavior outside this test, or general absence of other files.

The test body at `tests/xudu/swarm.cpp:191-210` obtains real metadata from the isolated peer,
asserts an empty pre-download `readStream`, calls `startDownload`, waits until the received bytes
match the peer fixture, calls `flushDownload`, checks the downloaded file, and then discards only
cached piece buffers. Its fixture uses a test-named `xudu-swarm-` directory under
`std::filesystem::temp_directory_path()` and deletes that directory in `TearDown()`. Set a fresh,
dedicated `TMPDIR` so both checkpoints observe a known path without scanning or touching the user's
`/tmp` contents.

## Frozen inputs and execution boundary

- Test binary: `build/validation-twenty-first/frozen-pre-change/xudu_test`, SHA-256
  `e9d09b0ebe522040c43daea295f8e80d5d242bd68bb2a094623e5d056ab599ea`.
- The genuine peer must be supplied as `build/xudu-swarm-peer` from the authorized frozen build. It
  is currently missing. Do not synthesize a peer, substitute an in-process fixture, or build while
  source changes are underway.
- Use the repository's `tools/swarm-netns-test.sh`; it creates its own seed and test network
  namespaces connected only by veth, starts the actual peer process, and passes the actual peer
  address, port, torrent, and text to the test. Do not run this on the host network namespace.
- When authorized to execute, enter one explicit outer `unshare -Urnm` namespace. Inside it the
  runner sees uid 0 and skips its own non-root self-wrapping branch. Mount a private `/var/run`
  tmpfs and create `/var/run/netns` as the runner does in its own namespace setup. This avoids
  nesting a second user/network namespace while preserving the runner's test and peer setup.
- Prepare an ignored private copy of the runner and record a unified diff. Its sole content change
  is the test-binary invocation: launch GDB on the frozen binary instead of invoking
  `./build/xudu_test` directly. Keep the peer invocation, namespace topology, peer options, test
  filter, and all other runner bytes unchanged. Set
  `XUDU_SWARM_TEST_FILTER=SwarmTest.MetadataOnlyMagnetWaitsForExplicitDownloadAndCanDiscardPieces`
  outside the script so the original filter mechanism selects exactly the relevant test.
- Before launch, record SHA-256 hashes for the frozen test binary, peer binary, copied runner, GDB
  command file, source test, and peer fixture input. Preserve the actual shell argument vector,
  environment values, runner output, and process exit status. The missing peer is a hard stop, not a
  passing or skipped measurement.

## Isolation and environment

Use six fresh roots under `build/validation-twenty-first/metadata-observation/`: `data`, `config`,
`cache`, `tmp`, `gnupg`, and a private X server/display root if Xvfb is needed. Export the six
headless/XDG settings (`SDL_VIDEODRIVER=offscreen`, `SDL_AUDIODRIVER=dummy`,
`LIBGL_ALWAYS_SOFTWARE=1`, and the three XDG roots), plus `TMPDIR` pointing at the fresh temp root.
No display is needed for this test binary; no GUI or GPU is launched. Keep all roots private to this
run. Record the final expanded environment with secret values omitted; no key is needed or to be
generated for this test.

## Filesystem checkpoints

Break in GDB at the entry to `xanadu::SwarmContentSource::startDownload(const xanadu::InfoHash&)`.
At that point the test has already called `waitForMetadata()` and performed its empty `readStream()`
assertion, but has not requested the payload. Stop all threads and inventory the exact test
directory under `TMPDIR`. Record every regular file's relative path, byte length, allocated bytes
(`st_blocks * 512`), and SHA-256; also record directories and symlinks separately. A missing
directory is recorded as missing; an empty directory is recorded as present with zero regular files.
Do not infer the filesystem state from `readStream()`.

Continue to the entry to `discardCachedPieces(const InfoHash&)`. In the test this call follows a
successful `flushDownload()` and the exact downloaded-file comparison, so it is the post-flush,
pre-teardown checkpoint. Stop all threads and take the same inventory. This distinguishes downloaded
retained files from the in-memory buffers which the next line discards.

The GDB command file should use symbol breakpoints with pending breakpoints disabled after symbol
resolution, stop at each checkpoint exactly once, print checkpoint markers and PID, and run a small
read-only inventory command against the known directory. Verify before the run that both symbols
resolve in the frozen binary. If they do not, stop and revise the observation mechanism without
changing the test or production binary. Preserve the GDB command file byte-for-byte and its hash.

## Result interpretation

The pre-download checkpoint can support only the observed regular-file inventory in the test's save
path at that instant. A zero-byte or absent file is not inferred from an empty content read; it must
be shown by the filesystem walk. The second checkpoint can support the observed retained files after
a successful explicit download and flush. Neither checkpoint justifies claims about the XDG cache,
unrelated process files, failed downloads, crash consistency, production settings, or installs using
other paths.

No execution has been performed as part of this plan. The missing frozen peer prevents an actual
observation until the parent supplies it and authorizes the run.
