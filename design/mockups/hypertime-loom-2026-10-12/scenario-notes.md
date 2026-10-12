# Hypertime Loom scenario studies

These are generated visual concepts, not app captures. Counts and ranges define the intended
scenarios; individual microglyphs and wire placement in the raster images are illustrative.

## Scenario 1: 100 xanadoc ops and three side branches

The main run is D001 through D100. Branch A has 20 new ops A01 through A20 and departs after D001.
Branch B has B01 through B20 and departs after D050. Branch C has C01 through C20 and departs after
D100. The shared fork operations are counted once: 100 + 20 + 20 + 20 = 160 unique ops. D100 remains
the named main tip while C01 begins a side branch; named branch tips are not the same concept as
leaves of the operation tree.

The three fork locations stand for the start, middle, and end of the requested span. Silver ancestry
stays traceable through coils; gold connections belong to content-span transclusion.

## Scenario 2: 30 interleaved ops, then slice-history tug

Use 15 xanadoc text operations and 15 slice Structure operations in one uninterrupted source chain.
X identifies xanadoc scope and S identifies slice scope, independent of the individual op kind.

```text
X01 → S02 → X03 → S04 → X05 → S06 → X07 → S08 → X09 → S10 → X11 → S12 → X13 → S14 → X15 → S16 → X17 → S18 → X19 → S20 → X21 → S22 → X23 → S24 → X25 → S26 → X27 → S28 → X29 → S30
```

The slice is named Slice Z1. Its projected history contains all 15 participating operations:

```text
S02 → S04 → S06 → S08 → S10 → S12 → S14 → S16 → S18 → S20 → S22 → S24 → S26 → S28 → S30
```

The combined source chain keeps its positions during the tug. Each projected S operation has a
matching-ID source marker and correspondence tether. The projected track uses a seamed ordering
guide with explicit omitted X-operation gaps. Its adjacency is filtered view order, not a claim that
S04 is a direct stored child of S02. X01 precedes the first participating slice operation; 14
further X operations lie between the successive S operations.

## Scenario 3: ten branch paths and many shared-content strands

Count the main continuation plus nine side branches as ten named current paths B01 through B10. B10
is the main continuation. The other nine paths leave the shared ancestral corkscrew. Exit collars
use branch identity rather than invented trunk indices: odd-numbered paths lead left, even-numbered
paths lead right. For visual consistency, each displayed path gets a 20-op segment; this count was
chosen for the mockup, not specified by the user.

Every exit remains individually visible and labelled. Gold strands connect matching shared-span
anchors on the branch state leaves and cross at different depths. Crossings create no new history
junction. This drawing shows an illustrative subset of shared-span connections, not an arbitrary
left-by-right pairing or an all-to-all link expansion.

## Final assets and visual checks

- `07-scenario-1-trunk-branches.png`: the 100-op run and three labelled 20-op side paths.
- `08-scenario-2-combined.png`: checked 30-chip sequence with all IDs appearing once.
- `09-scenario-2-slice-tug.png`: 30 source chips and 15 projected slice chips. The projected IDs are
  correct, but the upper source turn retains label artifacts, including X26 and a missing S28 label.
  Use the canonical sequence above for identity; correspondence wiring is illustrative.
- `10-scenario-3-ten-branches-v2.png`: ten matching source collars and heads, with B10 marked as the
  continuation. At this density, selected gold paths still need stronger emphasis in a working
  interface to be followed reliably.

These studies explore spatial packing and simultaneous contexts. They do not validate animation, hit
targets, occlusion handling, or performance with a real store. Exact submitted prompts are saved in
`scenario-prompts.md`; all images used the built-in image-generation tool.
