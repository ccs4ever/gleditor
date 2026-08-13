# BTFS as a storage server for transcluded content and permascrolls

An investigation, not a proposal to adopt. The short answer is that BTFS is a
good tool that does not fit either job, but the second question it raises --
how a permascroll can live on BitTorrent at all -- has a good answer, and the
code is already shaped for it.

Measurements below were taken on Ubuntu 24.04 with `btfs` 2.24 and
libtorrent-rasterbar 2.0.10.

## Which BTFS

Two unrelated projects use the name, and they want different things.

**btfs**, by Johan Gunnarsson: a FUSE filesystem that mounts a `.torrent` or a
magnet link as a read-only directory, fetching pieces on demand as a reader
reads them. Built on the same libtorrent this project already uses. Packaged
for Debian, Arch, Fedora and Homebrew. This is the one investigated here.

**BTFS**, by BitTorrent Inc. and TRON: a fork of IPFS with a token incentive
layer, addressed by IPFS CIDs rather than by BitTorrent info hashes. Assessed
briefly at the end; it is a different system that happens to share a name.

## What btfs does, measured

```
$ btfs --data-directory=... sample.torrent mnt
$ ls -l mnt
-r--r--r-- 0 root root 218 Aug  8 09:09 sample.txt
```

The file appears immediately, at the right size, before any content has been
fetched: the size comes from the torrent's metadata. Reads are what trigger
fetching.

Two things follow from that listing and from what happened next.

**It is read-only.** The permission bits say so and the README says so.

**A read of content it cannot obtain does not fail; it blocks.** Two attempts
to `cat` a file whose content no reachable peer held hung until an external
timeout killed them, at 10 and 20 seconds. There is no deadline: a POSIX read
either returns bytes or does not return.

A third observation, from the option list rather than an experiment: btfs has
no way to be told about a peer. It finds them through the DHT, a tracker, or
local discovery, or not at all. In a closed network -- which is what a private
permascroll server would be -- there is nothing to point it at. This is also
why the positive path could not be demonstrated here: with no DHT, no tracker,
and multicast discovery not working in this container, btfs had no way to reach
the seeder that was running beside it. That is a limitation of the environment
rather than evidence that btfs fails to read when it can find peers -- fetching
content is what it is for and it plainly does it.

## Role one: reading transcluded content

The question is whether btfs is a better `ContentSource` than what is here now.

Structurally it drops straight in, and that is worth saying plainly: mount a
torrent, point `DirectoryContentSource` at the mount point, and the existing
`Resolver` reads and verifies through it with **no new code at all**. The seam
is in the right place, and btfs fits it.

It is still the wrong choice, for four reasons, in descending order of how much
they matter.

**Blocking reads break a property the renderer depends on.** `Resolver` returns
nothing when content cannot be obtained, so a document quoting something nobody
is seeding renders with the quotation blank. That is a deliberate behaviour and
it is tested. Through btfs the same situation is an uninterruptible read on the
loader thread. The document would not render blank; it would not render.
`SwarmContentSource` has a deadline for exactly this reason.

**It fetches more than a quotation needs.** A quotation is usually a sentence
out of something long. `SwarmContentSource` requests only the pieces the range
touches, by setting a deadline on each. btfs decides for itself what to fetch
and read-ahead around a read, so a reference would cost more than it should --
and the whole argument for a reference over a copy is that it costs less.

**One mount per torrent.** A store's origin table can name many torrents. Each
would need a mount, a mount point, and a lifecycle: mounted when, unmounted by
whom, cleaned up after a crash how. `SwarmContentSource` holds many swarms in
one session.

**It adds FUSE to the deployment.** `/dev/fuse`, `fusermount`, and on many
systems a `user_allow_other` decision, on every machine that reads a document.
libtorrent is a library.

None of this is a criticism of btfs. Streaming a video out of a torrent without
waiting for it is exactly what it is good at, and it is good at it. It is a
program for a person, not a library for a program, and this needs a library.

## Role two: permascrolls

This is the more interesting question and the answer is not about btfs at all.

A permascroll is append-only and unbounded: content goes in, gets an address,
and stays at that address forever. A torrent is the opposite kind of object. Its
info hash is the SHA-1 of its info dictionary, which fixes the file list, the
piece length and every piece hash. **Appending to a torrent produces a different
torrent.** That is not a limitation to be worked around; it is the property that
makes the hash a stable name in the first place.

So BitTorrent cannot *be* a permascroll, and btfs -- read-only -- cannot even be
the write path to one. But it can carry one, in a shape that fits Xanadu
comfortably:

- The permascroll is an ordered sequence of **immutable segments**.
- A segment, once sealed, becomes a torrent. Its content is addressed by hash,
  verifiable forever, and servable by anyone who has it.
- The **live tail** -- content appended since the last seal -- is an ordinary
  append-only file on the storage server. It is the one part that cannot be a
  torrent, because it is still growing.
- A stable name for the whole scroll is a **BEP 46 mutable torrent**: a DHT item
  signed by the publisher's key, whose payload is the info hash of the current
  index. It is written `magnet:?xs=urn:btpk:<public key>`, and the publisher
  updates it by republishing with a higher sequence number. The key is the
  permanent name; what it points at moves as segments are sealed.

The reason this is a good fit rather than a workaround: sealing is exactly what
publishing already means in Nelson's model. Content that has been sealed is
content that can be quoted, and quoting content that is still being typed was
never coherent anyway.

### What this codebase would need

Very little of the addressing, which is the encouraging part.

`Origin` already names a file inside a torrent, and the store already keeps a
table of them. A permascroll segment is just another origin, so a document
quoting a sealed segment needs **no change at all** -- `SwarmContentSource`
fetches and `Resolver` verifies it today.

Three things were missing when this was written. One of them no longer is.

**BEP 46 is now implemented**, both halves: `MutableLink` reads and writes the
`urn:btpk` form and works out where in the DHT the pointer lives,
`SwarmContentSource::resolveMutable` asks and checks the answer's signature
against the name itself, and `publishMutable` puts one. A name resolves to an
info hash, and everything below that is unchanged -- which was the point of
putting the indirection here rather than deeper.

**A span cannot cross a seal.** `Version::occurrencesOf` merges adjacent extents
only when their addresses are consecutive within one origin, which is right for
one torrent and wrong across segments. A quotation spanning a seal boundary
would show as two shaded runs where a reader sees one passage.

The fix first written here -- teach the merge that segment *n* ends where
*n+1* begins -- is the wrong one, and the section below says why.

**No append protocol.** The server side -- accept content, append to the tail,
seal periodically, seed the result, republish the pointer -- does not exist.
This is the actual work, and it is the part BitTorrent contributes nothing to.

### Addresses have to be in scroll coordinates

A permascroll is read constantly and appended to in small pieces at typing
speed. Neither a torrent nor a DHT item can be anywhere near that write path: a
BEP 44 value is capped under a kilobyte, is rate limited, expires in hours, and
is ordered only by a sequence number, and sealing per keystroke would mint a
swarm per keystroke. So the address of a byte cannot be the address of the
thing currently distributing it.

Fix the address at write time instead, in the only coordinate system that never
moves: `(scroll, offset, length)`, where a scroll is one publisher's
append-only sequence. That costs no round trip, and because a scroll only grows,
no later append can invalidate an address already handed out. Where those bytes
can currently be fetched from is then a separate, replaceable question -- an
index mapping offset ranges to whichever torrent carries them -- and sealing
republishes an index without touching a single existing reference.

`Origin` today names `(info hash, file index, file offset)`, which binds a
span's identity to the container it happens to be distributed in. That is the
change to make, and it dissolves the seal-boundary problem rather than
special-casing it: in scroll coordinates a quotation spanning a seal is
contiguous, and the seal is invisible to transclusion.

Distribution then falls into tiers, each with a different number of parties:

| tier           | latency         | who holds it              | what makes it trustworthy     |
| -------------- | --------------- | ------------------------- | ----------------------------- |
| local append   | a keystroke     | the author                | fsync                         |
| tail shipping  | seconds         | a few subscribed replicas | signed by the publisher's key |
| sealed segment | minutes         | anyone, through a swarm   | piece hashes                  |
| index pointer  | tens of minutes | the DHT                   | BEP 46 signature              |

The faster the tier, the fewer the parties and the weaker the guarantee. Reads
are overwhelmingly of sealed segments, which are immutable and content
addressed and therefore need no coherence protocol at all; only the short live
tail needs the fast path. And there is no consensus problem anywhere in it,
because one scroll has exactly one appender -- documents drawing on several
authors are an edit decision list over several scrolls.

The key implemented for BEP 46 is the same instrument the tail tier needs: what
signs the pointer can sign each appended run, so a replica can check a fresh
append long before anything is sealed.

### Where it would sit

`ContentSource` stays the seam. A permascroll client is a `ContentSource` that
resolves a `urn:btpk` name to an index, the index to a segment, and the segment
to the swarm it already knows how to talk to. Nothing above it changes, for the
same reason nothing above it changed when the swarm was added: the resolver
verifies whatever it is handed, so it does not need to know where it came from.

## The other BTFS

TRON's BTFS is a fork of IPFS with a BTT token incentive layer. Adopting it
would mean addressing content by IPFS CIDs instead of BitTorrent info hashes,
which discards the bencode, info hash and piece verification code here and the
property that a reference can be handed to any ordinary BitTorrent client.

It is worth being blunt about the incentive layer. The durability BTFS offers is
economic rather than technical: content persists while somebody is paid to keep
it. A permascroll's promise is that an address resolves forever, and "forever"
underwritten by a token price is a weaker claim than the one Xanadu makes.
Plain BitTorrent makes no such promise either -- content survives while somebody
seeds it -- but it does not charge for the disappointment, and its addressing is
the one already implemented here.

Not recommended.

## Conclusion

Do not adopt btfs. It fits the existing seam and would work, and it is still
worse than the swarm source already written on the point that matters most:
unreachable content must fail quickly, and a filesystem read cannot.

The permascroll question is worth pursuing on its own terms. Segments sealed
into torrents, a growing tail that is not one, and a BEP 46 key as the permanent
name is a design that suits both BitTorrent's constraints and Nelson's model,
and most of the addressing for it is already here. The first step named here --
resolving a `urn:btpk` name -- has since been taken, and it went where it was
expected to: a name resolves to an info hash and nothing below that changed.

The next step is not the append protocol, which is the largest piece, but the
smallest one that the rest depends on: moving a span's address off the torrent
that happens to carry it and into scroll coordinates. Every other tier is
buildable afterwards and none of them is buildable before, because until an
address survives sealing there is nothing for a reference to be stable against.
