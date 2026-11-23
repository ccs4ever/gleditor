# Mutable files and rich media: source analysis and owner extension

This is a bounded addendum to the v0.7 Vortex I/O redesign, incorporated as §2.4, §8.5, §9.3, P7,
U-7/U-8, and additional §11.2 scenarios in v0.8. It is not a rewrite of the historical review
reports. The six-person v0.7 review remains historical; those critics did not review this extension.

## Source facts

- `apps/common/xanadu/zigzag/filesystem_lens.cpp:21-25,49-62` derives `fs:<device>:<inode>` node
  keys and records metadata during its walk. `FilesystemLens::pathFor` exposes the walked path as a
  locator; this lens does not read file bytes or stamp a byte-content generation.
- `apps/common/xanadu/zigzag/git_lens.cpp:150-173` polls HEAD/ref/packed-refs using mtime and size
  and combines those stamps with `FileWatch` notifications; its node/edge enumeration exposes refs,
  commit SHA, ancestry, and commit metadata (`:255-340`). A Git commit SHA names an immutable Git
  object, but this GitLens has no blob-byte operation. Working-tree files are mutable independently
  of the commit graph.
- `apps/common/xanadu/zigzag/file_watch.cpp:29-64` implements notification hints on Linux; other
  branches use OS notifications or an inert watcher. It does not return a snapshot token or
  serialize arbitrary external writes with a content read.
- `apps/common/xanadu/zigzag/projection_session.hpp:75-105` reconciles cells by external key and
  restates content; `apps/common/xanadu/zigzag/projection_lens.hpp:219-224` separately drops
  lens-side page buffers. Neither hook supplies byte-range leases.
- `src/media_stream.cpp:49-90,126-170` shows `MemoryMediaStream` owning shared immutable bytes,
  while `FileMediaStream` obtains file size at construction, reopens the named path for each
  random-access read, and constructs subspans from that path. The latter is a live path reader, not
  a retained snapshot.
- `apps/common/ui/xanadoc/views.cpp:1824-1830` reads the imported file into a whole `std::string`.
  `apps/common/xanadu/publication.cpp:699-724` similarly reads staged media in full before hashing
  and publication. Those paths do not define a bounded large-file capture service.
- `apps/common/xanadu/user_permascroll.hpp:131-148` describes append-only user primedia;
  `apps/common/xanadu/segmented_primedia_spool.hpp:132-151` says published positions are never
  rewritten and clear/adopt/open are not concurrent-reader operations. Store directories contain no
  primedia. Permanent coordinates retain meaning through the one author's permascroll.

## Design disposition

A path, inode, file descriptor, mapping, and byte offset are locators or access mechanics. They do
not by themselves establish immutable source identity. On partial or uncertain service-write
outcome, mark the live endpoint generation unknown, invalidate its live caches, and refuse
operations bound to that generation until fresh revalidation or rebind; retained immutable
observations remain readable. A filesystem watcher may prompt a refresh; matching before/after size
and timestamp do not prove an atomic read. The same inode may be rewritten in place, and a path may
be replaced between operations.

Separate the mutable live source from retained data. A service read that materializes bytes creates
an immutable read observation with its own origin identity, byte interval, and declared consistency.
Those retained bytes remain readable while leased after live-source invalidation. Future reads
against an obsolete generation return typed stale/conflict/unknown outcomes. Raw-range caches key by
source generation or observation plus byte range; decoded entries also key by decode parameters.
Generation change retires prior entries from live lookup without revoking retained observations or
reclaiming their bytes until cell, choice, continuation, capture, cache, decoder, and
native-operation leases end. Two reads of the same path are different origins absent a proven shared
immutable snapshot/version; overlapping captures preserve identity only within the same observation
or a backend-proven version. Equal content is never a deduplication key.

Source adapters advertise version/snapshot and conditional-write capabilities. A mutable read may
legitimately return a byte-stable observation without guaranteeing that the observed interval was
atomic with respect to external in-place writes; the adapter states that consistency. Strict callers
refuse if the backend cannot provide the requested guarantee. Service-owned writes serialize and
carry expected generations, but cannot prevent an external race without backend atomic
compare/write. Report uncertainty explicitly. Do not assume that an open fd, mmap, watch event, or
agreeing stat values closes that race.

Keep permanent permascroll immutable. A future editor may model edits as an append-only virtual EDL
over pinned base observations and new immutable write observations; export or commit to an OS file
is separate and conflict-checked. An external file is never a writable permascroll.

Where a supported media adapter uses large video/audio or other rich media, it needs byte-addressed
bounded ranges, random access, seek and cancellation. Use integer byte offsets with checked
range/stride arithmetic, including large offsets beyond 4 GiB; time-to-byte mapping, frame/sample
interpretation, container indexes, codec state, and dependency invalidation live in adapters. The VM
gets no media-specific opcode. This extension does not assert that every rich-media format is
currently supported or propose a codec overhaul. Decoded frames and audio samples are derived
objects with separately bounded cache and GPU/device lifetimes. Selected encoded-range capture is
not necessarily playable on its own. Self-contained playable capture is explicit and includes the
adapter-identified headers, index, and dependency closure. No implicit whole-file staging or
permanent capture is allowed; a user-selected full-file capture still uses bounded preparation and
can backpressure or refuse.

## Targeted architect review

Reviewed the v0.8 target `design/vortex-io-redesign.md` at SHA256
`28e5aca38135e335774add9b6d72de888ef8820609ca7b643bb331b395aadfb3`. The architect found no remaining
must-fix items after the draft clarified source-generation cache keys, derived decoder parameters,
invalidation after partial/uncertain writes, and continued readability of retained observations. The
review confirmed the locator/observation/snapshot distinction, backend-declared consistency and
write capabilities, bounded random-access media and decode lifetimes, and explicit playable-artifact
dependency capture. This is a targeted review of the new extension, not a new six-person review of
v0.8; the historical v0.7 convergence remains unchanged.
