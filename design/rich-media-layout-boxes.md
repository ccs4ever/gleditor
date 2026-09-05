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

## Stage 4: floating small images

Every media box up to this point used `BoxPlacement::Block`, even though the
layout engine has supported `FloatLeft`/`FloatRight` since Stage 2 — xudu
just never reached for it. This stage does: an image narrow enough to leave a
usable column beside it now floats, so a paragraph following it wraps instead
of stepping over it the way a full-width figure has to.

`Session::sourceFor()` (`apps/xudu/session.cpp`) decides per media span:

```cpp
const bool isImage = gleditor::MagicMimeDetector::isImageMime(stretch.mime);
constexpr float kFloatWidthFraction = 0.5F;
const bool floats =
    isImage && fit.width <= Doc::textWidthPx * kFloatWidthFraction;
```

Only images float; audio and video stay `Block` regardless of width, since
their transport chrome (play/pause, seek bar, title) wants the full column to
itself rather than a half-width sliver beside text. `kFloatWidthFraction` of
one half is deliberately simple: narrower than half the page and there is
still a usable column beside it; wider, and there is nothing left worth
wrapping text into, so it keeps stepping over it as a centred `Block` box
always has. A floated box gets no `BlockStyleRange` entry — `align` only
means something for a `Block` box's horizontal position; a float's `left`
comes from `placeFloat()` against whichever side it floats to.

Verified with a new fixture, `tests/samples/xudu/multimedia/11_floating_image`
(built via `--import tests/samples/sample_image.png` then `--insert-text
0:append:...floating_image_body.txt`, its own scroll, not the shared one the
`tools/create-sample-xanadocs.sh` generator uses): the 64x64 image floats
flush left, the paragraph's first few lines wrap narrowed beside it, and once
the paragraph clears the image's own height the column widens back to its
full measure — confirmed both visually (screenshot) and against the
library's own `TextWrapsBesideAFloat` test's invariants. `03_mixed_text_image`
now floats its own image too (same 64x64-vs-half-page-width math applies) —
it used to sit `Block`-centred under all the caption text; it now floats flush
left where its anchor lands, which happens to be after all the text since
that document's image comes last with nothing to wrap. `04_audio_doc` and
`05_video_doc` are unaffected, confirmed via `compare-backends.sh` and direct
screenshot comparison against their Stage 1d baselines.

### A pre-existing bug this stage exposed and fixed

Building the floating-image fixture surfaced a real, pre-existing defect
unrelated to boxes or floats: `Session::sourceFor()`'s box logic first
appeared to be dropping every byte of the appended paragraph — `concatext`
came back as just the 3-byte U+FFFC anchor, nothing else, no matter what
`LayoutBox`/`BoxPlacement` it was given.

The actual fault was two steps upstream, in how the *store* records what is
media. `Store::insertMedia()` is the only thing that populates
`localSegments` (`apps/xudu/core/store.cpp`), the table `classifyRun()` needs
to correctly re-split a piece where media coalesced with immediately-following
plain text (see that function's own comment on why a run can straddle a
segment boundary). `apps/xudu/main.cpp`'s `--insert-text DOC:POS:file`
handling already sniffs a file argument's MIME type and calls `insertMedia()`
when it is a media type — but the *first* `--import file` of a session
(`apps/xudu/main.cpp`'s `importFiles` loop) does not sniff at all: it trusts
`ContentPiece::mimeType` from `FileTextSource::pieces()`
(`src/text_source.cpp`), and that function's non-PDF branch always returned
one piece with `mimeType` empty, on the reasoning that "a plain file is
exactly one plain-text piece." True for a text file; false for a whole image,
audio, or video file handed to `--import` directly, which is exactly what
every existing multimedia sample does.

With no segment recorded, `classifyRun()`'s fallback — sniff the whole
run's bytes at once, since it has no boundary to split on — is correct only
when a run is homogeneously one type. `03_mixed_text_image`, `04_audio_doc`,
and `05_video_doc` never exercised the failure mode because each of their
runs *is* homogeneous (a run that is only the media file, or only text,
never both); `scrolls.spool` for all three is empty of `localsegment` lines,
and it never mattered. The instant a run mixes a directly-imported media
file with plain text immediately after it — a document not preceded by a
page break, i.e. anything a floating figure with body text below it would
naturally look like — the whole-buffer sniff sees the image's own leading
signature (PNG's magic bytes, in this case) and misclassifies the entire
run, image and paragraph both, as one media stretch. The paragraph never
reaches `concatext` at all; it is silently swallowed into a single 3-byte
anchor.

Fixed at the source: `FileTextSource::ensureLoaded()`'s non-PDF branch now
sniffs the whole file's own bytes with the same `MagicMimeDetector` used
everywhere else in this codebase, and tags the piece's `mimeType` when it
identifies as a media type — mirroring what a PDF's embedded figure
sub-piece, and `--insert-text`'s own file argument, already did. This routes
a whole-file `--import` of media through `Store::insertMedia()` (recording
the segment `classifyRun()` needs) exactly like every other media ingestion
path in the codebase, rather than being the one path that skipped it. No
caller of `pieces()` changes: a plain text file still gets `mimeType` empty
and one plain piece, exactly as before.
