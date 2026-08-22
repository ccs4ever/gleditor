# The operations spool: a binary format, and sealing it like primedia

## What was slow, and why

The operations spool is the second of OSMIC's two spools -- "permits
replaying the operation" -- and it grew the way an append-only log should:
without bound, for as long as a document is edited. Two things in how it was
kept made that growth expensive rather than merely large.

**Every save rewrote the whole history.** `Store::save()` opened `ops.spool`
with `std::ios::trunc` and wrote one text line per operation this store had
ever recorded, every time it was called -- on Ctrl-S, on quit, on
`--publish`. A document with a hundred thousand edits paid for a hundred
thousand lines on the *next* keystroke's save, and the one after that, and
every one after that: save cost was linear in total history, not in what had
changed since the last save. For a document edited over months this is the
"unbounded growth" that motivated this change -- not that the file grows
(an append-only log has to), but that touching it at all became more
expensive the longer the document had existed.

**Every load re-parsed all of it as text.** `std::istringstream` per line,
`operator>>` per field, a string comparison per operation kind. None of that
is expensive once; all of it is expensive across a few hundred thousand
lines, and it happens on every `--read`, every open, every `--publish`.

Neither cost showed up in a short session, which is exactly why it is the
kind of bug that survives a while: it scales with something a quick test
never has much of.

## The binary record

`encodeOpRecord()` / `decodeOpRecord()` (`ops.hpp`) replace the text line
with a self-delimiting binary record: every fixed-size field is written
big-endian, and a `MicroversionId` is a byte giving its segment count
followed by that many `(branch letter, number)` pairs. "Self-delimiting"
matters more than "binary" here -- it is what lets records be concatenated
with nothing between them and still be read back one at a time, on disk or
inside a torrent, without a length prefix or a framing layer over the top.

`Store::opsLog()` returns the whole history encoded this way, prefixed by a
five-byte tag (`XUOP` and a version byte). `Store::save()` writes that same
tag once and then one record per operation to `ops.spool`; `Store::load()`
checks for the tag to tell a binary file from one written before this
existed.

**What this does not do**: change what is kept in memory. `Store` still holds
`std::map<MicroversionId, Op>`, unchanged, because nothing measured here
showed the in-memory shape costing anything close to what the two save/load
paths did, and reworking it touches every piece of code that already walks
that map (`rebuild()`, `opsFor()`, `children()`, `allVersions()`...) for a
win nothing here demonstrates it needs. `opOrder` (below) is the one addition,
and it is small: a `vector<MicroversionId>`, one entry per operation, already
the cheap kind of growth an append-only log is supposed to have.

## Making save() actually incremental

A binary record is faster to write than a text line, but writing the *whole
history* faster is still writing the whole history. The save cost that
mattered was `O(total history)` where it should have been `O(what changed)`,
and fixing the encoding alone does not fix that.

`Store` now keeps `opOrder`, the states named in `ops`, **in the order
`putOp()` recorded them** -- not `ops`'s own order, which is sorted by
`MicroversionId` and is what `save()` used to iterate. Those two orders
differ exactly when editing branches: `1a1` is recorded after `2` if `2`
already existed when `1` was edited again, but `1a1 < 2` in the map's
ordering. `save()` needs record order, not replay order, because nothing
about *reading* the file depends on the order records appear in it --
`Store::load()` has always rebuilt `ops` by keying each record on the
`MicroversionId` it names, regardless of what line it was on. Replay order is
recovered later, from `MicroversionId::path()`, not from file position. So
writing in record order changes nothing anybody depends on, and is the one
thing that makes an incremental append well-defined: `opOrder`'s tail past
whatever was already flushed *is* exactly what is new.

`opsFlushed` (how much of `opOrder` is already on disk) and
`flushedOpsDirectory` (which directory it agrees with) are the cursor.
`save()` opens `ops.spool` in append mode and writes `opOrder[opsFlushed..]`
when the directory matches what was last flushed there; otherwise -- first
save ever, or a save to a directory this in-memory store has not written to
before -- it truncates and writes everything, once, and the cursor now
agrees. `load()` sets the cursor to "everything just read is already durable"
when the file it read was the binary shape, and clears it (forcing one full
rewrite on the next save) when the file was the older text shape, so a
store's first save after being opened by an older build migrates it rather
than corrupting it by appending binary records after text lines.

The net effect: opening a document costs what it always cost (a full read of
its history -- there is no way around reading a document's whole history at
least once, and this was never the expensive path). Saving it costs what
changed since the last save, for the whole session after the first.

## Backward compatibility

`load()` reads the first five bytes of `ops.spool` and compares them against
the tag. A match means binary records follow; anything else -- a shorter
file, a mismatched tag, a file in the one-line-per-operation shape every
build before this wrote -- falls back to the original text parser unchanged,
line by line, exactly as before. `tests/xudu/store.cpp`'s
`aLegacyTextOperationsSpoolStillReads` and
`tests/xudu/resolver.cpp`'s `aStoreWrittenBeforeScrollsStillReads` (which
happens to also exercise a legacy `ops.spool`) both hold a store to this: a
document written by an older `xudu` still opens, and the first save migrates
it.

This is the same pattern `scrollsFile`/`originsFile` already established in
this file for the scroll table -- detect the old shape, read it, write the
new shape from then on -- applied to the file next to it.

**What is given up**: the old format's other property, stated in the comment
it replaced -- "readable without this program." A person could `cat
ops.spool` and see the edits made in order; a binary record is not meant to
be read that way. That is a real cost, not a rounding error, and it is
deliberate here because it is exactly the tradeoff the format was asked to
make: bytes-per-record and parse cost were the whole complaint, and both are
smaller when integers are four or eight raw bytes instead of decimal digits
with separators around them, and a record ends where its fields say it ends
instead of at the next `\n` an `std::getline` has to scan for.

## The primedia spool got the same treatment

The first version of this change left `primedia.spool` alone: `save()` still
opened it with `std::ios::trunc` and wrote `spool.bytes()` in full every time.
That was deliberate at first -- copying a `std::string` to disk was never the
cost that motivated this work, unlike re-encoding a few hundred thousand text
lines -- but it left the two spools inconsistent for no reason that survives
scrutiny: both are append-only in memory, both are read whole exactly once
(on open) and written whole on every save after, and nothing about the
primedia spool's format needed to change to fix that, only the same
"remember how much is already on disk" bookkeeping the operations spool
already has.

`primediaFlushed` and `flushedPrimediaDirectory` are that bookkeeping,
independent of `opsFlushed`/`flushedOpsDirectory` rather than sharing a single
cursor: the two spools are always saved to the same directory in practice
(one `Store::save(directory)` call writes both), but nothing requires that,
and keeping the trackers separate means neither block has to reason about the
other's state to know whether it can append. Unlike the operations spool,
`primedia.spool` has never had a second on-disk shape -- it has always been
exactly the bytes typed, nothing to detect or migrate -- so `load()` can mark
it flushed unconditionally, with no binary-tag check standing in for it.

## Sealing the ops log like the primedia spool

`sealLocalSpool()` bundles the local spool into a torrent as one of several
files -- content, authorship record, its signature -- under one info hash, so
the record of who wrote something cannot be separated from what they wrote.
`Store::opsLog()` makes it possible to add a fourth: the operations that
produced the content, in the same self-delimiting binary shape the local
`ops.spool` is written in. `SealedScroll` now carries a second `Scroll`,
`opsScroll`, alongside the content's, with its own `ScrollSegment` naming
where in the torrent's piece stream that fourth file begins.

**Why a second scroll and not a further segment of the first.** A
`ScrollSegment`'s `at`/`length` are offsets into *one* coordinate space --
the scroll's own. The primedia scroll's offsets are byte offsets into the
primedia spool; the ops log's are byte offsets into a wholly different
stream of bytes that starts at zero for an unrelated reason. Folding an ops
record's address into the content scroll would mean two different pieces of
content could claim the same offset, which is precisely the kind of
collision `scroll.hpp` exists to rule out by construction. So the ops log
gets its own publisher-key-and-salt identity -- the same key, salted
`<salt>+ops` -- which is a second, independent append-only sequence sealed
into the same torrent as the first, the same way two unrelated files can
share a `.tar` without their contents merging.

Both scrolls are still "one segment, the whole stream, starting at scroll
offset zero" -- the same shape `Scroll::ofTorrentFile()` already built for
content sealed with no publisher name at all (`scroll.hpp`'s pre-BEP-46
case). `sealLocalSpool()` reuses it for both rather than constructing a
`ScrollSegment` by hand twice: `Scroll::ofTorrentFile(hash, fileIndex, path,
streamOffset, length)` gives back the segment, and the publisher/salt that
make it *this machine's* scroll rather than an anonymous one are set
afterwards. What differs between the content and the ops log is exactly the
five arguments -- which file, where it starts in the piece stream, how long
it is, and what salt names it -- not the shape of the scroll each becomes.

**Why nothing resolves a span into it.** The primedia scroll exists because
`PrimediaSpan`s point into it -- a document's text is pointers into that
scroll, and sealing is what makes those pointers resolve on another machine.
Nothing points into the ops log the same way; no document ever transcludes
"operation 47". The ops scroll is sealed for a different reason: so that a
reader who has the torrent has enough to reconstruct this store's hypertime
-- every state, every branch -- without asking this machine for it, the same
durability the content already has. It travels with the content it explains
because the two were never meaningfully separable, but it is not part of the
document model and does not need to be.

**Why the whole log, every time, rather than only what is new.** This
mirrors what `sealLocalSpool()` already does for the primedia spool: every
call re-embeds `store.primedia().bytes()` from offset zero, not merely the
bytes written since the last seal. True incremental sealing -- a new segment
covering only what changed, so re-sealing a long-lived document does not cost
the whole thing again -- is explicitly future work; see
`design/btfs-and-permascrolls.md`'s "No append protocol" and "the actual
work" that remains. Building it in a way that considers the primedia case but
not the newly-added ops case would leave one spool ahead of the other for no
reason; building an incremental-sealing scheme for the ops log alone, ahead
of primedia's, would invent a second mechanism only to retire it once the
general one exists. So the ops log participates today exactly as primedia
does today -- one torrent, the whole history, every seal -- and whatever
solves incremental resealing for one spool is what should solve it for both.

## What is not here

- **Incremental (delta) resealing**, for either spool. Out of scope for the
  reason above; tracked in `design/btfs-and-permascrolls.md`.
- **A smaller in-memory representation.** `std::map<MicroversionId, Op>` plus
  `opOrder` is what changed; the map itself is untouched. If a store's memory
  footprint becomes the bottleneck -- as opposed to save/load latency, which
  is what was actually measured here -- that is a further, separable change,
  and one with a much larger blast radius: every reader of `ops` today
  assumes map iteration and `map::at`-style lookup.
- **Fetching a sealed ops log back into a `Store`.** `opsScroll` is produced
  and addressed; nothing yet reads one back and replays it into a fresh
  store. That is the natural next step once there is a reason to want it --
  recovering a document from its torrent alone, with no local spool files at
  all -- and it is a reader for a format this change already fixes in place,
  not a reason to have waited to fix the format.
