# Rich-Media Layout: Boxes, Alignment, and the U+FFFC Anchor

Media in gleditor used to be a run of `\n` characters sized to the widget's
pixel height (`apps/xudu/session.cpp`'s old `placeholderFor()`), drawn by a
second overlay system that guessed its position as
`anchorFor(offset).y - (height + anchorGapPx)`. The text engine did not know
media existed; the overlay did not know where the text went; the two agreed
only by discipline, and a figure's height decided the document's byte offsets
— change a font and every offset after a figure moved.

This note records the replacement: a generic `LayoutBox`/`BlockStyleRange` box
model in the library (`include/gleditor/layout_box.hpp`), and xudu's migration
from N-newline placeholders to a single U+FFFC (OBJECT REPLACEMENT CHARACTER)
anchor per media span.

## Why U+FFFC, and why one byte's worth of anchor rather than N

U+FFFC is the Unicode character meant for exactly this: a placeholder for an
embedded, non-textual object in a text stream. Using it (rather than, say, a
NUL byte or a private-use codepoint) means the anchor's meaning is legible to
any tool that inspects the raw bytes, not just this codebase.

It is 3 bytes in UTF-8 (`\xEF\xBF\xBC`) — fixed, regardless of the media's
pixel size. The old scheme's placeholder byte-length was `ceil(heightPx /
lineHeight)` newlines, which meant a widget's on-screen size and the
document's byte offsets were the same number wearing two hats. Shrinking a
font, or changing DPI, changed how many newlines a given figure needed, which
moved every byte offset after it. The box model breaks that coupling: `Doc`
learns a box's pixel size from `LayoutBox::widthPx/heightPx`, carried
alongside the text rather than encoded into it. The anchor byte(s) only need
to be found again, not sized.

**Before committing to this, the probe described in the plan's "prove, don't
assume" section was run**: does HarfBuzz shape U+FFFC to a visible glyph in
the fallback fonts this codebase's `layoutPage()` fallback path
(`src/text/layout.cpp`, `glyphIdx == 0` fallback chase) would reach for it?
It does, for at least one fallback font — confirmed empirically, not
assumed. That is why `layoutPage()`'s Block/Float/Inline box handling
(`src/text/layout.cpp`) suppresses the `GlyphEntry` for a box's anchor byte
by construction rather than relying on U+FFFC failing to shape: the box's
"line" gets an empty glyph range (`startGlyph == endGlyph`), so the assembly
loop that turns shaped glyphs into `GlyphEntry`s never iterates over the
anchor at all, regardless of what any given font would have drawn for it.
This was checked directly against real xudu samples during Stage 1d
verification by dumping every `GlyphEntry::chr` gleditor ever shaped while
rendering `03_mixed_text_image`, `04_audio_doc`, and `05_video_doc` — no
U+FFFC byte sequence (`ef bf bc`) ever appeared in the shaped output, only in
`shaping.boxes`, confirming the suppression holds for the real anchor bytes
xudu emits, not just for the synthetic single-newline anchors the library's
own unit tests use as a stand-in.

## Coordinate-space safety

Before shrinking the placeholder, it had to be proven that no persisted xudu
data encodes concatext (assembled-text) byte offsets, since those are exactly
what shrinking a placeholder's length would silently corrupt. `Link::left`/
`Link::right` are `PrimediaSpan`s — scroll-relative, content-addressed,
immutable primedia coordinates — not concatext offsets. Forced page breaks
are rebuilt fresh per-version in concatext space rather than persisted with a
fixed byte length. So only the ephemeral output of `Session::sourceFor()`
changes shape; every persisted store is unaffected, because none of them
ever depended on how many bytes a placeholder happened to occupy.

## What changed in xudu

`Session::sourceFor()` now appends exactly `kMediaAnchor` (the 3-byte U+FFFC
sequence) per media span, and records one `LayoutBox{.placement = Block}` and
one `BlockStyleRange{.align = Centre}` covering it — Block, not Float or
Inline, since a figure steps the flow around it the same way the old
many-newline placeholder always did; wrapping text beside a figure or sharing
its line is future work this migration does not attempt.
`Session::mediaSpansFor()` computes the same anchor-following offsets by the
same "3 bytes, not `ceil(heightPx/lineHeight)` newlines" arithmetic, so a
widget's `docOffset` and the box's `anchor` agree by construction rather than
by two independent formulas that could drift.

`ImageOverlay::bottomLeftOf()` and `MediaWidget::bottomLeftOf()` each
collapse to a single `doc->boxFor(offset)` lookup. `Doc::BoxRect::y` is
documented as the box's bottom edge in the same up-positive,
page-centre-relative space `Doc::Anchor` already used — confirmed by reading
`Page::boxGeometry()`'s own arithmetic before writing the simplified
`bottomLeftOf()` bodies — so neither caller needs the
`anchorFor(offset).y - (height + anchorGapPx)` arithmetic by hand any more.
`MediaWidget::anchorGapPx` survives as `LayoutBox::marginPx`: space the flow
now genuinely reserves as part of the box's own height, rather than a fudge
applied only at draw time.

## `AtomicRange` deletion

`AtomicRange{start, end, minWidthPx}` was the layout engine's only reserved-
space concept before this: one-dimensional, and only able to block a page
split and widen the page background — it reflowed nothing. Everything it did,
`LayoutBox` does, without coupling media pixel size to byte offsets, so it
was deleted outright: the type itself
(`include/gleditor/glyphcache/types.hpp`), `LayoutOptions::atomicRanges`,
`TextSource::atomicRanges()`, `Doc::atomicRanges`, the
`atomicRangeLinesStartingAt()` helper and its page-break check in
`src/text/layout.cpp`, the width-widening scan over it, and zigzag's
`ShapingKey::atomicRanges` field (plus its hash mixing and key construction).

Three of the four `AtomicRange` unit tests it had
(`tests/lib/text_layout.cpp`) already have a Stage 1b box-test successor
covering the same behavior with a `LayoutBox` instead.
`AtomicRangeMinWidthWidensThePage` has **no successor by design**: widening a
page to fit a content-less reserved run stops being meaningful once pages are
a fixed size by default and a box reports its own width directly — there is
no equivalent "empty run that widens the page" concept left to test.

## What to expect visually

Every prior stage in this redesign (1a page sizing, 1b block boxes, 2 floats,
3 inline boxes, 1c alignment) was verified byte-identical against a Stage 1a
screenshot baseline via `cmp`, because xudu did not yet populate any of the
new box/float/inline/alignment machinery. Stage 1d is the first stage that
breaks that invariant on purpose: it is the first time xudu actually emits
`LayoutBox`/`BlockStyleRange` data, so the render changes for the first time
by design. Verified visually instead of via `cmp`: `03_mixed_text_image`
(image now Centre-aligned via `BlockStyleRange`, positioned by `boxFor()`),
`04_audio_doc`, and `05_video_doc` — in each, the widget sits centered inside
the page, does not overlap surrounding text, and the page background covers
it. `./tools/compare-backends.sh` shows no divergence beyond the pre-existing
~1.2% Vulkan/OpenGL gap this codebase already carries independent of this
work.
