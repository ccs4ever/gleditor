# Decode Index Spike

A prototype settling the open questions the multimedia pipeline plan's Phase
5 named and deliberately left unanswered: what a "decode index" for
compressed primedia would need to mean, per format, before any of it becomes
load-bearing in `Store`/`ScrollSegment`/`PrimediaSpan`. The code lives in
`tools/decode-index-spike.cpp`, built via `make decode-index-spike` (only
when `libavformat`/`libavcodec`/`libavutil`/`zlib`/`libjpeg`/`libwebp` are
found by pkg-config; optional, not part of `all` or `make test` — see the
Makefile's own comment above that target).

Five formats now, five different answers to "how much of this is a
library's problem": FFmpeg solves audio/video seeking (video and, with one
real caveat, MP3) entirely; PNG needs a real checkpoint this spike builds
by hand on top of zlib's own primitives; JPEG's restart markers, paired
with a modern libjpeg-turbo API, turn out to need *no* bespoke code at all,
provided the encoder opted in to a rarely-used setting; and WebP is the
outlier that breaks the pattern of "there's always some answer" — its
public API offers a crop feature that looks like exactly what this spike
needs and turns out to buy nothing. That range is the actual finding —
"manifold formats" was the premise, and the honest answer is that "rely on
libraries" means something different for each one, including "there is
nothing to rely on."

## The problem, restated

A byte range inside a compressed stream names no region of anything. PNG's
IDAT is a zlib stream of filtered scanlines; an MP3 or H.264 stream is frames
whose decode depends on the frames before them. Two implementations that
disagreed on what the *uncompressed* coordinate space means for a given
format — row order, bit depth, sample format, colour space — would compute
different addresses for the same nominal span, and every transclusion
between them would silently point somewhere else. That is the same failure
mode as this codebase's Merkle domain-separation and nonce-convention bugs:
an implicit agreement two parties can hold differently without either
noticing until content disagrees.

## The problem shapes are not the same problem

**Audio/video is a solved problem, and the library is FFmpeg — with one
real caveat for lossy audio.** `libavformat`/`libavcodec` already
generalise "seek to a point in decoded time" across every container and
codec they support — MP4, MKV, WAV, MP3, OGG/FLAC, and dozens more — via
`av_seek_frame()`, which uses each container's own keyframe/index tables
(`AVIndexEntry`) internally. The decode index for this half of the problem
is simply FFmpeg's own seek index. No bespoke restart-state logic was
written for the video case at all — `avDecodeIndexSpike()` calls
`av_seek_frame()` and compares the result against a linear decode, and
that comparison is the entire implementation.

**MP3 uses the identical mechanism, and it works — but needs a measured
"discard the lead-in" step the video case did not.** `mp3DecodeIndexSpike()`
opens the audio stream instead of the video one and calls the same
`av_seek_frame()`, but seeking mid-MP3-stream and decoding immediately
does not give correct output right away: investigating this printed
frame-by-frame PCM output from both a linear decode and a seeked one side
by side, and found the seeked decoder returns pure silence for its first
two frames, a transitional (neither-silent-nor-correct) third frame, and
only becomes bit-exact identical to the linear decode from the fourth
frame onward. This is `mp3float`'s own decoder-priming delay — a real,
measured quantity for this decoder and fixture (3 frames), not something
derived from the format spec or asserted from documentation. The plan's
own Phase 5 text anticipated this in the abstract ("for lossy formats...
spans snap outward to the nearest restart and the decoder discards the
lead-in"); this is a concrete, measured instance of exactly that, and the
first sign that "FFmpeg solves seeking" is not quite the same claim for
every codec it supports.

**Compressed images have no native random access, and no library changes
that.** PNG's IDAT is one zlib stream, and its Up/Paeth scanline filters are
differential across rows: unfiltering row *n* needs row *n − 1*'s own
already-unfiltered bytes. No library exposes "decode from this compressed
byte offset, at this specific row," because no self-consistent restart point
exists without recording that previous row. This is not a gap in library
coverage — it is inherent to the format, which is why Mark Adler's `zran.c`
(distributed as a worked example with zlib itself, not invented for this
spike) is the canonical answer for exactly the same problem in gzip. The
library still does the hard part — raw DEFLATE decompression, and cheap
resumable-state cloning via zlib's own public `inflateCopy()`. What zlib
does not and cannot abstract — PNG's five scanline filter types (`None`,
`Sub`, `Up`, `Average`, `Paeth`) — is a well-documented ~30-line per-byte
predictor, not a compression algorithm, and is the one genuinely bespoke
piece of code this spike contains.

**JPEG needs no bespoke code either, but only because of an encoder option
almost nothing turns on.** Unlike PNG, a JPEG's restart markers (`RSTn`,
inserted every `DRI`-declared number of MCUs) exist for exactly this
purpose: at each one, every component's DC predictor is reset to zero by
the format's own definition, so the entropy stream resynchronises with
*zero* carried-over state — a genuinely different, and cheaper, shape than
PNG's row-differential filters. Because of that, libjpeg-turbo ships a
public API built for it —
`jpeg_skip_scanlines()` — that fast-forwards whole MCU rows using those
markers internally. `jpegDecodeIndexSpike()` calls it and nothing else;
there is no checkpoint structure of this spike's own for JPEG at all. The
catch, named rather than glossed over: this only works *fast* when the
encoder chose to emit restart markers, which most encoders do not by
default (Pillow, `cjpeg`, and ffmpeg's own `mjpeg` encoder all require it
requested explicitly, and ffmpeg's build in this environment does not
expose the option at all — the fixture here was made with Pillow's
`restart_marker_blocks`). Confirmed empirically, not assumed: the API still
returns byte-correct output on a JPEG with no restart markers at all, at
the cost of decoding every skipped row internally instead of fast-forwarding
past them — always correct, only sometimes cheap.

**WebP breaks the pattern: its own public API offers what looks like the
answer, and measurably is not one.** `WebPDecoderConfig` has a
`use_cropping` option (`crop_left`/`crop_top`/`crop_width`/`crop_height`)
that reads, from its own header comment, like exactly the mechanism this
spike keeps finding in other formats. It is not. Timing a full decode
against an 8-row crop of the same file — first on a large (2048×2048),
deliberately incompressible investigation fixture not committed to this
repo, then reproduced by `webpDecodeIndexSpike()` on the small committed
one — found no meaningful difference either way (large fixture: full
~0.21s, cropped ~0.20s, statistically indistinguishable; the small
committed fixture is fast enough in both directions that its own timing
numbers are noise-dominated and are printed with a comment saying so,
specifically so a future reader does not mistake a favourable ratio on a
sub-millisecond decode for evidence of savings). `WebPDecode()` decodes the
*entire* image internally regardless of what region was asked for, and
`use_cropping` only trims the output copy afterward. Unlike zlib's
`inflateCopy()` or JPEG's restart markers, libwebp exposes no public API to
snapshot or resume its internal decoder state mid-image — for VP8 lossy,
that decoder's own intra-prediction chains genuinely need the reconstructed
pixels of the block above and to the left, a same-shape dependency to
PNG's row filters but with no equivalent of zlib's public checkpoint
primitive to build a spike on top of; for WebP lossless, the custom
LZ77-style entropy coder has the same absence of an exposed resumable
state. Building a real checkpoint for either variant would mean
reimplementing meaningful parts of libwebp's internals rather than relying
on the library, which is precisely the tradeoff Phase 5's own text
anticipated ("if the index proves too costly for some format, the honest
fallback is to support whole-media transclusion for it and say so").
That is the conclusion for WebP: no partial decode, full stop, at least
without abandoning "rely on libraries" as the ground rule.

A second, smaller finding, investigated but not committed as a fixture or
assertion (the same "reported, not hidden" treatment as the PNG spike's own
dead end): the crop *is* byte-identical to the corresponding region of a
full decode for lossless WebP (confirmed, and what
`webpDecodeIndexSpike()` actually asserts), but for **lossy** WebP the
cropped region came out close to, not identical with, the same rows from a
full decode — small per-pixel differences at the crop boundary, most
likely from loop-filtering or chroma-upsampling context that differs
between "this row is the image's real edge" and "this row is merely where
cropping happened to stop". Even where WebP's crop is fast enough to
matter, this is a second reason not to lean on it as a checkpoint
primitive: for the one variant where it changes nothing about decode
cost, it is also the one variant where its output is not guaranteed to be
exactly reproducible.

## Canonical uncompressed coordinate, per format

- **Image (PNG, JPEG, WebP):** byte offset into a top-to-bottom raster —
  RGBA8 for PNG (`width * 4` bytes/row) and WebP (also `width * 4`, set
  explicitly via `WebPDecoderConfig::output.colorspace = MODE_RGBA`, since
  the zero-initialised default is `MODE_RGB` — a real bug this spike found
  in itself, see below), RGB8 for the JPEG fixture (`width * 3` bytes/row,
  no chroma subsampling). The PNG/WebP convention matches
  `ImageCache::decodeImageBuffer()`'s own existing one
  (`src/image_cache.cpp`'s `surfaceToDecodedImage()`, also row-major
  top-to-bottom RGBA8), so a real integration would not need a second
  coordinate convention to reconcile against the rendering path that
  already exists. A real integration would still need to settle what PNG,
  JPEG and WebP spans share, if anything — this spike does not need to,
  since each format's check stands on its own (and for WebP, per above,
  there is no partial-decode mechanism to address into regardless).
- **Audio (WAV, MP3):** PCM sample-frame index at the stream's own native
  sample rate and channel layout. Matches `fragmentTimeRange()`'s existing
  linear byte-to-second scaling for uncompressed PCM (`src/media.cpp`),
  which is already exact for that case — this spike does not re-litigate
  it. For MP3, translated to a frame index via the format's fixed 1152
  samples/frame (MPEG-1 Layer III) — exact for the fixture's constant
  128kbit/s CBR encode; a VBR MP3 varies frame size with the local bitrate,
  which this spike does not attempt to handle.
- **Video:** decoded frame number at the stream's own native frame rate.

**Left open, not solved:** resampling to one canonical rate across streams
that differ (two audio files at different sample rates transcluding into
each other) is a real question a production integration would have to
answer; this spike only proves seeking within a single stream's own native
rate. For MP3 specifically, translating an arbitrary decoder's own warmup
delay (measured here as 3 frames for `mp3float`, on one fixture) into
something a real integration could rely on generically — rather than a
constant hardcoded after watching one decoder's behaviour — is also left
open.

## What the spike actually does

`tools/decode-index-spike.cpp`, five independent checks:

1. **`pngDecodeIndexSpike()`** — over `tests/samples/sample_image.png`
   (64×64, 8-bit RGBA, non-interlaced — the one PNG shape this spike
   handles; interlaced, lower-bit-depth, and palette/greyscale PNGs are a
   real follow-up, not attempted here). Parses the PNG's chunks by hand
   (chunk CRCs are not checked — this reads a fixed, trusted test fixture,
   not untrusted input), concatenates every `IDAT` chunk into one zlib
   stream, and decodes it row by row via `inflate()`, manually unfiltering
   each scanline. Every eighth row, it clones the live inflate state via
   `inflateCopy()` and records the previous row's own unfiltered bytes
   alongside it — a checkpoint. It then picks a row range a few rows past a
   checkpoint boundary (landing exactly on one would prove less than
   decoding a handful of thrown-away rows forward from an earlier one),
   clones that checkpoint, decodes forward from it, and diffs the result
   byte-for-byte against the same rows from the full top-to-bottom decode.

2. **`avDecodeIndexSpike()`** — over a new fixture,
   `tests/samples/sample_video_seekable.mp4` (3 seconds of guaranteed-motion
   H.264 at 10 fps, `-bf 0 -g 10`, generated via `ffmpeg -f lavfi -i
   "testsrc=size=320x240:rate=10:duration=3"` — a real fixture gap, since
   `sample_video.mp4` has no actual stream inside it, per the multimedia
   plan's own Phase 3 finding). Three keyframes land at t=0.0/1.0/2.0s.
   Decodes linearly from the start to frame 15 (t=1.5s, a P-frame, reached
   from the keyframe at t=1.0s), then independently seeks with
   `av_seek_frame(..., AVSEEK_FLAG_BACKWARD)` to the same target and decodes
   forward from wherever that lands, comparing the two frames' raw decoded
   planes (Y/U/V, packed without linesize padding) byte-for-byte.

3. **`jpegDecodeIndexSpike()`** — over a new fixture,
   `tests/samples/sample_image_restart.jpg` (512×512, procedural test
   pattern, no chroma subsampling, generated via Pillow with
   `restart_marker_blocks=64` — one restart marker per 8-pixel MCU row,
   confirmed by parsing the file's own `DRI`/`RSTn` markers: 63 markers for
   64 rows). Decodes the whole image once as the reference, then calls
   `jpeg_skip_scanlines(cinfo, 88)` followed by `jpeg_read_scanlines()` for
   8 rows, and diffs against the same rows from the full decode.

4. **`mp3DecodeIndexSpike()`** — over a new fixture,
   `tests/samples/sample_audio_seekable.mp3` (5 seconds of a 440Hz sine
   tone, mono, 44.1kHz, 128kbit/s CBR, generated via `ffmpeg -f lavfi -i
   "sine=frequency=440:duration=5" -c:a libmp3lame -b:a 128k`). Decodes
   linearly to frame 88 (target frame 85 plus the 3-frame warmup margin,
   see below) as the reference, then independently seeks to frame 85's pts
   with `av_seek_frame(..., AVSEEK_FLAG_BACKWARD)`, discards 3 decoded
   frames, and compares the next 5 frames' raw PCM samples against the
   linear reference.

5. **`webpDecodeIndexSpike()`** — over a new fixture,
   `tests/samples/sample_image.webp` (512×512, the same procedural
   gradient pattern as the PNG fixture, lossless — 166 bytes on disk, since
   the pattern is highly predictable and lossless WebP compresses it
   accordingly). Decodes the whole image once, then decodes again with
   `WebPDecoderConfig::options.use_cropping` set to the last 8 rows, and
   diffs the cropped output against the same rows of the full decode.
   Timing for both decodes is printed, with a comment explaining why the
   numbers from this particular fixture should not be over-read (see
   below).

`png`, `av`, `jpeg` and `mp3` print `PASS`/`FAIL` per check; the tool
exits non-zero on any mismatch. `webp` also asserts and reports PASS/FAIL
for its one correctness check (lossless crop matches), and separately
prints timing as pure information, not a pass/fail gate. As of this
writing all five checks pass.

## A dead end, reported rather than hidden

The first attempt at the JPEG spike did not use `jpeg_skip_scanlines()` at
all — it tried to reproduce the PNG spike's own approach, splicing raw
bytes: keep the file's header/tables/SOS segment, then append the
entropy-coded bytes starting exactly at a chosen `RSTn` marker, terminate
with `EOI`, and decode the result as a small standalone JPEG. Prototyped in
Python against `Image.open()` (Pillow, also backed by libjpeg-turbo) before
touching any C++, on the theory that a restart marker's own zero-DC-predictor
reset should make this valid.

It produced consistently wrong output — correct-looking luma with exactly
neutral (flat, zero-chroma) colour for every pixel past the splice point,
regardless of which restart marker was chosen. Renumbering the spliced-in
`RSTn` marker values so the first one in the excerpt reads as `RST0` (a
known technique for other JPEG-splitting tools, on the theory that a fresh
decode session might validate the marker's cyclic sequence number against
its own internal counter) made no measurable difference at all — same
mismatch count, same pixel values, byte for byte. That non-effect is itself
informative: whatever is actually wrong is not the marker sequence
numbering, and this spike did not chase it further once
`jpeg_skip_scanlines()` was found to solve the same problem correctly
through a five-line call to a public, documented API instead. The
byte-splice attempt is not committed anywhere — this paragraph is the
record of it, kept so the next person does not spend an afternoon
rediscovering that the naive approach looks plausible and isn't.

## A real bug found and fixed while building this, not assumed away

`Checkpoint` originally held its `z_stream` **by value** inside a struct
kept in a `std::vector<Checkpoint>`. Running the spike under
AddressSanitizer showed exactly half of an 8-checkpoint vector leaking
~42 KB each — specifically, every checkpoint that had survived at least one
vector reallocation. `inflateEnd()` on those returned `Z_STREAM_ERROR`
rather than freeing anything.

Root cause: zlib-ng's internal inflate state keeps its own back-pointer to
the `z_stream` struct's address, checked by `inflateEnd()`. A `z_stream`
held by value inside a vector element gets bytewise-copied to a new memory
address every time the vector reallocates — even with a correct C++ move
constructor moving the *struct's contents* correctly, zlib is never told
the struct itself now lives somewhere else, so its internal back-pointer
goes stale. `inflateEnd()` then detects the mismatch and refuses to free.
Fixed by heap-allocating the `z_stream` (`std::unique_ptr<z_stream>`) so the
`Checkpoint` wrapper can be freely moved by the vector while the `z_stream`
itself never changes address for its whole lifetime. Verified clean under
`-fsanitize=address,undefined` after the fix.

The lesson generalises beyond this one tool: **a `z_stream` — and, by
extension, any per-format decoder state a production decode index would
need to keep many of, in a container that might reallocate — must be
heap-allocated with a stable address, never stored by value in a
vector-like container.** A real integration keeping a checkpoint per
segment in some index structure needs to get this right from the start.

**A second, smaller bug found while writing the WebP spike:**
`webpDecodeIndexSpike()`'s first version left `WebPDecoderConfig::output.
colorspace` at whatever `WebPInitDecoderConfig()` zero-initialises it to.
That default is `MODE_RGB` (3 bytes/pixel) — `MODE_RGB` is enum value `0`,
so a zeroed struct silently selects it — not the 4-bytes/pixel `MODE_RGBA`
this spike's stride arithmetic assumed. The lossless crop-correctness
check failed outright on first run because of it (comparing 3-byte-strided
reads against 4-byte-strided writes lines up nothing). Fixed by setting
`config.output.colorspace = MODE_RGBA` explicitly. A small thing, but the
same class of mistake as the `z_stream` one above in spirit: a library's
"empty"/zero-initialised default is not always the default a reader would
guess, and the fix in both cases is to stop relying on an implicit value
and say what is meant.

## What this settles, and what it deliberately does not

Settled: the canonical uncompressed coordinate per format (above); that the
video half of audio/video needs no bespoke code because FFmpeg already
solves it; that the same is true for MP3, with a real, measured warmup
caveat FFmpeg's mechanism does not absorb on its own; that PNG needs a real
zran-style checkpoint (not something to keep deferring) because its
filters are differential; that JPEG needs *no* bespoke code either
whenever the encoder emitted restart markers, because the format's own
zero-state resync point plus libjpeg-turbo's `jpeg_skip_scanlines()`
already solves it; that a checkpoint's decoder state must be
heap-allocated to survive being moved around (found for zlib's `z_stream`,
worth checking again for any other format's own opaque decoder handle
before trusting it in a container); and, distinctly, that WebP has *no*
usable answer via its public API at all — a fourth, genuinely different
outcome that is the concrete evidence "how much of this is a library's
job" has to be answered per format rather than assumed from one example,
or even assumed to always have a positive answer.

Not attempted, and not silently assumed to be a small remaining step:

- Wiring any of these mechanisms into `Store::insertMedia()` /
  `ScrollSegment` / `Session::classifyRun()`. This spike is deliberately
  standalone — `Store` gains nothing from this file's existence, on
  purpose, so a mistake in the coordinate convention costs a rewrite of one
  tool rather than a migration of real stores.
- Interlaced, lower-bit-depth, greyscale, or palette PNGs; JPEGs without
  restart markers reaching a genuinely large image (correctness was
  confirmed for that case — see the `jpeg_skip_scanlines()` section above —
  but this spike did not measure how slow the internal fallback decode
  actually gets); progressive JPEG (restart markers interact with
  spectral selection/successive approximation differently, not
  investigated here); VBR MP3 (frame size varies with the local bitrate,
  breaking this spike's fixed 1152-samples/frame arithmetic); cross-stream
  sample-rate canonicalisation.
- Durable serialization of a checkpoint index (what would actually ship in
  a segment record, and how it would be transmitted/verified over BitTorrent
  the way `ScrollSegment` already is) — this spike's checkpoints live only
  in the process's own memory for the duration of one run.
- Checkpoint interval tuning ("granularity is a space/time dial," per the
  original plan text) — every eighth row (PNG) or one restart marker per
  MCU row (JPEG) was picked to guarantee more than one checkpoint on a
  small fixture, not measured against any real size/seek-latency tradeoff.
- Whether encoders in the wild can be relied on to emit JPEG restart
  markers at all, given every encoder tried while building this fixture
  (Pillow, `cjpeg`, ffmpeg's `mjpeg`) needed it requested explicitly and
  ffmpeg's own build here did not expose the option — a real deployment
  question this spike surfaces but does not answer.
- Deriving MP3's decoder-warmup delay from anything other than watching
  one decoder's behaviour on one fixture — a real integration would need a
  principled answer (or a conservative fixed margin, accepting some waste)
  rather than the empirical constant `3` this spike hardcodes.
- Any animated-WebP consideration (multiple frames, their own timing) —
  this spike only looked at single-image WebP, lossy and lossless.

## Later: promoted into a real library component (not WebP)

The PNG/JPEG/video/MP3 mechanisms this spike proved now live in
`gleditor::decode_index` (`include/gleditor/decode_index.hpp`,
`src/decode_index.cpp`) — a real, tested component alongside `ImageCache`/
`MediaPlayer`, built into the core library behind three optional
dependency flags (`GLEDITOR_HAVE_DECODE_INDEX_ZLIB`/`_LIBJPEG`/`_LIBAV`,
each gated by its own `pkg-config --exists` check the same way
`HAVE_SDL_IMAGE` already works). `gleditor::buildDecodeIndex()` reports a
format's `DecodeIndex` (seekable/durableIndex/seekPoints); PNG additionally
gets `gleditor::PngCheckpoints`, a separate type rather than a field on
`DecodeIndex`, since its checkpoints are in-process-only handles, not
durable bytes — see the header's own comments for why that distinction is
load-bearing. `tools/decode-index-spike.cpp` was converted to call this
library rather than containing its own copy of the PNG/JPEG/video/MP3
logic, proving the shipped implementation instead of a standalone
duplicate; WebP's own check is untouched, since no library equivalent
exists or is planned for it. Tests live in `tests/lib/decode_index_test.cpp`
(always compiled, skipping gracefully via `GTEST_SKIP()` when a build
lacks one of the three optional dependencies).

Explicitly **not** done in this pass, same as the spike itself never did:
wiring `buildDecodeIndex()`/`PngCheckpoints` into `Store`/`ScrollSegment`/
`Session::classifyRun()`, and making PNG checkpoints durable/serializable
(still needs the full `Z_BLOCK` + `inflatePrime()` zran technique, per the
finding above — investigated, found nontrivial, left as a named
follow-up).

A real bug was found while writing `tests/lib/decode_index_test.cpp`, not
assumed away: `PngCheckpoints::build()` originally parsed a PNG's IDAT
bytes into a `build()`-local `std::vector`, but every checkpoint's cloned
`z_stream` keeps zlib's own `next_in`/`avail_in` pointing straight into
those bytes — `inflateCopy()` copies the `z_stream` struct, not what it
points at. The local vector was destroyed when `build()` returned, so any
later `decodeRows()` call dereferenced a dangling pointer; a test that
called `decodeRows()` in a separate statement after `build()` returned
(rather than inline, where the now-freed memory often still reads back
correctly by accident) caught it immediately. Fixed by storing the IDAT
bytes in `PngCheckpoints::Impl`, alongside the checkpoints that reference
them, so they live exactly as long as the object does — the same
"heap-owned, never relocated for the object's own lifetime" discipline the
`z_stream`-by-value lesson above already established, applied to what the
`z_stream` points at rather than the `z_stream` itself. Confirmed fixed
under AddressSanitizer/UndefinedBehaviorSanitizer with the original bytes
buffer explicitly freed before decoding, not just by re-running the test.

## Later still: gzip, bzip2, zstd — three more genuinely different answers

Investigated whether the same idea extends to general-purpose archive
compression (gzip, bzip2, zstd), since these show up constantly, especially
gzip wrapping tar archives. Real experimentation (small standalone C/C++
programs, not just documentation-reading) turned up three more distinct
cases, none matching the five formats above exactly:

- **gzip**: the *best* result of any format here. Unlike PNG, plain
  deflate has no per-scanline filter state to also carry, so the full
  zran technique — `inflate(Z_BLOCK)` to find deflate block boundaries,
  `inflatePrime()` for the leftover sub-byte bits, `inflateSetDictionary()`
  with the preceding 32KB of output — produces a checkpoint expressible as
  pure bytes (byte offset + bit count/value + dictionary), with **no live
  decompressor state at all**. Built 55 checkpoints across a 34MB gzip
  file and resumed from every one of them in a fresh `z_stream`, byte-
  identical to a full decode; repeated on incompressible random data too.
  This is the "meaningfully more work" PNG's own zran attempt deferred —
  it turns out to actually work once PNG's filter complication is removed.
  Not implemented in `gleditor::decode_index` in this pass; a real
  follow-up given how cleanly it worked.
- **bzip2**: the best *format property*, but with no library shortcut.
  bzip2 blocks (100KB–900KB uncompressed, chosen at compress time) are
  **fully independent** — no cross-block state whatsoever, unlike gzip's
  LZ77 back-references or PNG's row filters, and unlike JPEG's restart
  markers this is not encoder-optional; it is how the format's Burrows–
  Wheeler blocks fundamentally work. Confirmed with `bzip2recover` (shipped
  with bzip2, itself just a bit-level scanner for the block magic, not a
  special API): split a 34MB/343-block file into 343 independent one-block
  files, decompressed each separately, and concatenating them reproduced
  the original exactly. The catch: `libbz2`'s public API has no
  `jpeg_skip_scanlines()`-equivalent for this — building it means
  bit-level magic scanning (bzip2recover's own technique, not exposed as a
  library call) plus reconstructing a valid single-block stream to hand to
  `BZ2_bzDecompress()`. Not implemented; more manual work than gzip's zran,
  though the payoff (works on any bzip2 file, no per-checkpoint dictionary
  storage needed) is arguably better.
- **zstd**: mixed, and closer to WebP's outcome than gzip's. Frame
  boundaries are free via the stable `ZSTD_findFrameCompressedSize()` API,
  but only useful if the file has multiple frames — plain `zstd` CLI never
  produces more than one regardless of input size; `pzstd` does split into
  frames, but the frames it produces do not declare their own decompressed
  size, so even that case needs an extra pass to fill one in. The official
  answer for real seeking, zstd's own `seekable_format` (`ZSTD_seekable_*`,
  compressed+decompressed offset per frame, no extra pass needed), is not
  part of the core library and is not packaged by this system's
  distribution — confirmed via `nm -D` against the installed `libzstd.so.1`
  and the Arch package database. The one function that superficially
  resembles zlib's `inflateCopy()`, `ZSTD_copyDCtx()`, is explicitly
  documented as deprecated ("misleading... very limited utility"), slated
  for removal, and gated to static-linking-only — a dead end, not a
  shortcut. This *was* implemented (see below), by vendoring the seekable
  format's own source rather than depending on a system package for it.

### Zstd: vendored, and wired into gleditor::decode_index

`thirdparty/zstd` is a git submodule (pinned to `v1.5.7`, matching the
system `libzstd` this build otherwise links, to keep an ABI mismatch —
same symbols, different behaviour, not a compile error — from being able
to drift in silently). Only two of its files are actually compiled:
`contrib/seekable_format/zstdseek_compress.c` and `zstdseek_decompress.c`,
which implement the stable, public `ZSTD_seekable_*` API declared in that
same directory's `zstd_seekable.h`. Verified before writing any of this
into the library: compiled those two files standalone against the system
`libzstd`, compressed a 34MB file into 131 seekable frames, then called
`ZSTD_seekable_decompress()` at an arbitrary offset 3/4 through the file —
it decompressed only that one frame and returned bytes matching the
original exactly. Separately confirmed that a *plain* streaming
`ZSTD_decompressStream()`, with no seekable-format awareness at all,
correctly and transparently skips a seekable file's own trailing seek-table
frames (ordinary zstd "skippable frames," which the format itself requires
every decoder to pass over) — this is what makes one code path handle
re-encoding regardless of whether the input was already seekable.

`gleditor::buildDecodeIndex()` gained a fifth format,
`DecodeIndexFormat::Zstd`, behind its own `GLEDITOR_HAVE_DECODE_INDEX_ZSTD`
flag (`libzstd` found by `pkg-config`) — same "check `seekable` before
relying on anything else" contract as the other four. Most zstd files
found in the wild are not in the seekable format (confirmed: the ordinary
`zstd` CLI never produces one), so `seekable`/`durableIndex` are false for
them, honestly, not because no mechanism exists but because none was used
at encode time.

That asymmetry is why `gleditor::reencodeZstdSeekable()` exists: given any
zstd bytes (seekable already or not), it decompresses fully via the plain
streaming API and recompresses into the seekable format at a configurable
frame size, so a caller — a publish/seal step, per the request that led to
this — can convert a non-seekable zstd asset before it becomes part of
something committed, rather than only ever reporting on files that already
happen to qualify. This is a real transcode (full decode, full recompress),
not a header patch, and is meant to be called once per asset rather than
on every read.

Verified in `tests/lib/decode_index_test.cpp`: a plain zstd fixture reports
not seekable/not durable; a seekable one (its own seek table built by
calling `reencodeZstdSeekable()` on the plain fixture, so the fixture-
generation step is itself a test of the function) reports a durable index
whose seek points have strictly increasing compressed and decompressed
offsets and whose total decompressed extent matches the original content's
size exactly; and the round-trip test decompresses the re-encoded bytes
independently and compares them byte-for-byte against the original,
confirming this is a real, content-preserving transcode and not just a
metadata change.

Not attempted in this pass: gzip's zran (a real, working mechanism, but a
separate follow-up) and bzip2's block-boundary scanning (more manual work,
no library shortcut) — see above for why both are believed tractable, just
not yet built.

## Later still: FLAC — the same shape as zstd, no vendoring needed

FLAC turned out to be the same asymmetry as zstd: a native seek-table
concept exists (the SEEKTABLE metadata block, part of the format's own
spec, unlike anything gzip or WebP has), but most encoders write one
sparsely or not at all, since it costs bytes for a benefit only a seeking
reader gets. Unlike zstd's seekable format, this needed no vendoring —
`libFLAC++` (the `flac++` package, already commonly packaged) implements
both reading and writing SEEKTABLE blocks directly, via `FLAC++/decoder.h`,
`FLAC++/encoder.h`, and `FLAC++/metadata.h`.

Verified before writing any of this into the library, mirroring the zstd
investigation: a standalone program built a placeholder seek table
(`FLAC::Metadata::SeekTable::template_append_spaced_points_by_samples()`),
fed it to `FLAC::Encoder::Stream` alongside custom in-memory write/seek/tell
callbacks (the seek/tell pair is what lets the encoder patch the
placeholders with real byte offsets once whole frames are written — its own
documentation calls this out explicitly: "update the metadata... if output
seeking is possible"), and confirmed three things independently: every
placeholder resolved to a real, monotonically increasing byte offset; a
full decode of the result matched the original PCM exactly; and
`FLAC__stream_decoder_seek_absolute()`, using the resolved table, landed on
samples matching the tail of a from-the-start linear decode. A second,
separate check confirmed libFLAC can still `seek_absolute()` correctly on a
file with **no** seek table at all — frame sync codes make this possible
without an index, the same way MP3's own bitrate-position estimate does,
just slower.

`gleditor::buildDecodeIndex()` gained a sixth format, `DecodeIndexFormat::Flac`,
behind `GLEDITOR_HAVE_DECODE_INDEX_FLAC` (`flac++` found by `pkg-config`).
`buildFlacIndex()` uses `process_until_end_of_metadata()` — the same
"read only the header, never touch the audio frames" discipline JPEG's
`jpeg_read_header()` and PNG's IHDR-only parse already established — to
read STREAMINFO (for `uncompressedExtent`, in samples) and SEEKTABLE (for
`seekPoints`) without decoding a single frame. One real wrinkle: FLAC's own
`SeekPoint::stream_offset` is relative to the start of the first audio
frame, not the file — `get_decode_position()` immediately after
`process_until_end_of_metadata()` returns is exactly that frame's starting
byte, which is what makes the offsets this component hands back absolute
positions in the original file bytes rather than an offset nothing else
could interpret. `seekable` is `true` unconditionally (frame-sync seeking
always works); `durableIndex` is `true` only when a real SEEKTABLE was
found — the same "seekable does not imply durable" shape MP3 already has,
for the same underlying reason (an estimation/scan-based fallback that
works but produces nothing worth storing).

`reencodeFlacSeekable()` mirrors `reencodeZstdSeekable()`'s shape: full
decode to PCM (per-channel buffers, matching `FLAC__StreamEncoder::process()`'s
own expected input layout so nothing needs interleaving/de-interleaving),
full re-encode with a freshly built, generously-spaced seek table
(`secondsPerSeekPoint`, defaulting far denser than the 10-second-or-more
spacing common in the wild — "generous," per the request that led to this).
Only STREAMINFO and the seek table carry across; VORBIS_COMMENT tags and
embedded pictures a source file may have are not preserved in this pass, a
named gap rather than a silent one.

Verified in `tests/lib/decode_index_test.cpp` the same way as zstd: a
`--no-seektable` fixture reports seekable-but-not-durable; a fixture
produced by calling `reencodeFlacSeekable()` on that same plain file
reports a durable index with strictly increasing offsets and the correct
total sample count; and the round-trip test decodes the re-encoded bytes
independently and compares PCM samples exactly. The seekable fixture was
additionally cross-checked against `metaflac`/`flac` — tools entirely
outside this codebase — confirming the resolved seek table and decoded PCM
both match what this component's own tests already asserted.

## A note on "just decompress and record offsets" as a general strategy

Asked directly, while scoping FLAC: could a slow path — fully decompressing
a file, or walking its compressed bytes recording an offset at every
desired uncompressed position — work generically across every format above,
rather than needing six different per-format mechanisms?

Two different things hide in that question. "Fall back to a full,
unindexed decode when nothing better exists" is always possible and is not
something to build: it is exactly what `seekable == false` already means,
for any format, today. But "walk a linear decode and record a byte offset
for every desired uncompressed position" is *not* generically valid — this
whole investigation's central, repeated finding is exactly why. An
arbitrary compressed byte offset is only resumable if it happens to land
on a genuine, format-defined state-reset boundary, and recording one
without whatever compensating state that format needs fails every time:
gzip needs a dictionary and leftover bits attached (proven this session);
PNG needs the live decompressor state itself; JPEG and zstd only resync at
restart-marker/frame boundaries an encoder chose, not a byte-count
heuristic; MP4's index already is "record offsets, but only at keyframes";
MP3's own bit-reservoir spillover is why a warm-up margin was needed even
at frame boundaries; only bzip2's blocks are close to free, since they
carry no cross-block state at all. "Decompress once, walk it, record
checkpoints" is the technique behind zran, `PngCheckpoints`, and now the
zstd/FLAC reencode paths — but the checkpoint is never *just* a byte
offset, which is exactly why this component has six different
implementations rather than one generic walker.

## Later still: TIFF — already seekable by design, but not always usefully

TIFF is a different case from every format above: it needs no new index
concept added at compress time at all. Strips (or tiles) are how TIFF pixel
data is organised in the first place, and each one's byte offset and length
are recorded directly in the file's own IFD tags (`StripOffsets`/
`StripByteCounts`, or `TileOffsets`/`TileByteCounts`) — `libtiff` exposes
these, and `TIFFReadEncodedStrip()`, unlike anything WebP offers, decodes
exactly one strip without touching any other. Confirmed empirically, not
assumed: an isolated `TIFFReadEncodedStrip()` call against a *fresh* file
handle — no prior strip read, no state carried over — returns bytes
identical to that same strip in a full top-to-bottom decode, for both LZW
and Deflate/ZIP compression checked separately.

The gap is encoder behaviour, not a missing format feature — and it is a
real gap, not a hypothetical one: `tiffcp -r <height>` (forcing one strip
per image, something real tools do produce for small images or simplicity)
yields exactly one strip, one seek point, at row 0. Not usefully seekable
at all, the same shape as an unspaced FLAC file or a non-seekable-format
zstd file — seekable and durable in principle, useless in practice.

`gleditor::buildDecodeIndex()` gained a seventh format,
`DecodeIndexFormat::Tiff`, behind `GLEDITOR_HAVE_DECODE_INDEX_TIFF`
(`libtiff-4` found by `pkg-config`). `buildTiffIndex()` reads `IMAGELENGTH`
and, for a stripped (non-tiled) file, `StripOffsets` directly — no
decoding at all, the same "read only the header" discipline JPEG's restart-
interval check and PNG's IHDR-only parse already established. One
deliberate scope limit, named rather than discovered as a gap: tiled TIFFs
report `seekable` true (`TIFFReadEncodedTile()` still works, and this
function's own reencode always produces stripped output regardless of what
the source used) but `durableIndex` false — a tile's position is two-
dimensional, and this component's flat, single-scalar `uncompressedPosition`
has no way to carry that without conflating tiles that happen to start at
the same row.

`reencodeTiffSeekable()` mirrors the zstd/FLAC functions' shape: full
decode via `TIFFReadRGBAImageOriented()` (which normalises whatever
photometric interpretation, bit depth, and planar configuration the source
used into 8-bit RGBA — the same scope-narrowing choice `PngCheckpoints`
makes for PNG colour type 6), full re-encode as Deflate-compressed,
contiguous-planar RGBA with a small, forced `RowsPerStrip` (`rowsPerStrip`,
defaulting to 8 — "generous," per the request that led to this, denser
than any default a general-purpose strip-size heuristic would choose).
Colour profiles, EXIF/GPS metadata, and alternate photometric
interpretations a source file may have carried are not preserved, the same
named-gap choice `reencodeFlacSeekable()` makes for VORBIS_COMMENT/pictures.

Two real, minor correctness issues were found and fixed while building
this, both surfaced by `libtiff`'s own warnings rather than assumed away:
the legacy `COMPRESSION_DEFLATE` identifier triggered a warning recommending
`COMPRESSION_ADOBE_DEFLATE` instead (used); and leaving `ExtraSamples`
unset for the synthesized alpha channel left its meaning ambiguous per the
TIFF spec, not just noisy — fixed by setting `EXTRASAMPLE_UNASSALPHA`
explicitly.

Verified in `tests/lib/decode_index_test.cpp` the same way as zstd/FLAC: a
single-strip fixture (built with `tiffcp -r`) reports exactly one seek
point at row 0; a fixture produced by calling `reencodeTiffSeekable()` on
that same file reports 64 strictly-increasing seek points (512 rows / 8
rows-per-strip) and the correct total row count; and the round-trip test
decodes both files' RGBA pixels independently and compares them exactly.
The seekable fixture was additionally cross-checked against `tiffinfo` and
Pillow — tools entirely outside this codebase — confirming the strip
count, compression scheme, `ExtraSamples` tag, and decoded pixels all match
what this component's own tests already asserted.
