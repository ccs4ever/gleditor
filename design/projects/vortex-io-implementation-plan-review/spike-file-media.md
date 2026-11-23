# M5 spike: mutable files and bounded media

Date: 2026-11-23\
Environment: Linux workspace, repository fixtures, `/tmp/vortex-io-source-SdEoS6`\
Scope: independent behavior probes for the M5 file/media acceptance gate and source questions
U-7/U-8. No production code, workspace build, or test suite was changed or run.

## Findings

### Mutable file observations

An isolated Python probe created `live.bin` with `abcdefghij`, read and retained those bytes, then
mutated the file in place, truncated it, and renamed/replaced its pathname. Results:

| Operation                                                  | Subsequent path read / metadata                 | Previously materialized bytes          |
| ---------------------------------------------------------- | ----------------------------------------------- | -------------------------------------- |
| Initial read                                               | `abcdefghij`                                    | `abcdefghij`                           |
| Overwrite first three bytes in place                       | `XYZdefghij`                                    | `abcdefghij`                           |
| Truncate to four bytes                                     | size 4                                          | still 10 bytes; retained tail `efghij` |
| Rename original aside, create replacement at original path | path reads `REPLACED`; old file contains `XYZd` | prior observation still `abcdefghij`   |

The probe also opened the four-byte old file immediately before rename. After the pathname was
replaced, reading that descriptor returned `XYZd`, while reopening the pathname returned `REPLACED`.
This demonstrates ordinary Linux locator behavior, not an atomic read guarantee: an fd is tied to an
inode across rename, but another writer can still mutate that inode in place. Materializing an
observation is sufficient to retain the exact bytes actually read; it does not prove that a
multi-read acquisition was a coherent point-in-time snapshot.

### Large offsets

A sparse file was extended by seeking to offset `4,294,967,419` (4 GiB + 123), writing `END!`, then
seeking back and reading four bytes. The file size was `4,294,967,423`, the read returned `END!`,
and allocated blocks were 8 (512-byte blocks). This confirms the host filesystem and Python 64-bit
seek/read path support this offset. It does not validate any proposed Vortex API implementation;
that still needs checked `uint64_t` arithmetic and adapter tests.

### Immutable Git object versus working-tree locator

In the isolated repository at `/tmp/vortex-io-source-SdEoS6/repo`, committed `blob.txt` contained
`committed bytes\n`; its blob ID was `dfeaea624076aa3499d09ccbf144c0a101e3b233`. Rewriting the
working-tree path to `working tree mutable\n` did not change `git cat-file blob <id>`, which
continued to return the committed bytes. This is direct evidence that a Git blob object ID names
immutable object content while a checked-out pathname remains mutable. It does not mean the existing
Git lens can read blobs; the reviewed source analysis says it currently exposes commit metadata, not
blob-byte access.

### Media seek, decode, and range selection

Available tools were `/usr/bin/ffmpeg` and `/usr/bin/ffprobe`. Repository fixtures:

| Fixture                                   |  Bytes | Probe result                          |
| ----------------------------------------- | -----: | ------------------------------------- |
| `tests/samples/sample_video_seekable.mp4` | 21,778 | H.264 video, 3 seconds, MP4 container |
| `tests/samples/sample_audio_seekable.mp3` | 81,127 | MP3 audio, 5 seconds                  |

Both a video frame seek/decode at 1.0 seconds and a 0.25-second audio decode starting at 1.0 seconds
succeeded. A 4,096-byte slice beginning at byte 10,889 in the MP4 was written as a standalone file;
`ffprobe` rejected it with `Invalid data found when processing input`. This small fixture therefore
demonstrates that arbitrary selected encoded bytes need not form a playable artifact. It does not
measure large-media throughput, decode memory, GPU surfaces, cancellation latency, or a decoder
cache.

## Reproduction

The mutation and sparse-offset probe was run with Python 3 using a unique temporary directory
created by `mktemp -d /tmp/vortex-io-source-XXXXXX`. Its key operations were:

```python
observed = path.read_bytes()
with path.open("r+b") as f:
    f.seek(0)
    f.write(b"XYZ")
with path.open("r+b") as f:
    f.truncate(4)
fd = os.open(path, os.O_RDONLY)
os.rename(path, path.with_suffix(".old"))
path.write_bytes(b"REPLACED")
with sparse.open("wb") as f:
    f.seek((1 << 32) + 123)
    f.write(b"END!")
```

The isolated Git experiment used `git init`, `git add`, `git commit`, `git hash-object`, then
compared `git cat-file blob <id>` against the rewritten worktree path.

Media commands, run headless against repository fixtures:

```sh
ffprobe -v error -show_entries format=format_name,duration,size \
  -show_entries stream=index,codec_name,codec_type,duration -of compact=p=0:nk=0 \
  tests/samples/sample_video_seekable.mp4
ffprobe -v error -show_entries format=format_name,duration,size \
  -show_entries stream=index,codec_name,codec_type,duration -of compact=p=0:nk=0 \
  tests/samples/sample_audio_seekable.mp3
ffmpeg -nostdin -v error -ss 1.0 -i tests/samples/sample_video_seekable.mp4 \
  -frames:v 1 -f null -
ffmpeg -nostdin -v error -ss 1.0 -i tests/samples/sample_audio_seekable.mp3 \
  -t 0.25 -f null -
```

The range probe copied `[10889, 14985)` from the 21,778-byte MP4 and ran `ffprobe` on that
standalone range; it was rejected as invalid input.

## Implications for M5 / U-7 / U-8

- Keep pathname/inode/open-handle locator identity distinct from a retained `ReadObservation`. Once
  exact bytes are materialized, leases can preserve those bytes through later overwrite, truncate,
  rename, or replacement; reads started later against the path can observe different content.
- A same-path reread must not reuse a prior live-source cache entry without backend-proven shared
  immutable version/snapshot evidence. A before/after `stat`, watch event, or stable fd is
  insufficient evidence of an atomic snapshot or external-writer exclusion.
- Bind a Git blob read capability to the immutable object ID and object availability. Keep
  working-tree reads live and generation-scoped. Current lens metadata alone does not provide blob
  bytes.
- Use checked 64-bit byte ranges. Separately account encoded input leases, decoder state/output,
  indexes, and GPU/device surfaces. Seek/cancel can retire derived work only when the
  decoder/backend acknowledges release; this spike did not exercise those lifetimes.
- Treat arbitrary selected media intervals as encoded source data, not as a playable export. A
  playable capture needs adapter-identified container headers/indexes and dependency closure, with
  refusal when the adapter cannot establish or collect that closure. Never infer whole-file or
  playable capture from a successful range read.

## Limits and next implementation evidence

These are OS/tool observations and a proposed-model assessment, not implementation verification. No
CAS or snapshot property was measured or demonstrated. The probes did not race an external writer
during a read, inject service-write uncertainty, test generations/typed outcomes, test
cancellation/eviction under active leases, or establish playable dependency closure. M5 still needs
implementation-level tests for replacement, truncation, in-place/external mutation, service writes
and queued generation-bound reads; offset overflow and >4 GiB reads; bounded random seek/decode,
cancellation acknowledgement, cache eviction while leases remain, and explicit dependency-closure
success/refusal. For mutable file adapters without a proven atomic/versioned facility, strict
snapshot/CAS requests should refuse and consistency must describe the bytes actually observed.
