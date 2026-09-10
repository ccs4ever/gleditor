# Xudu Sample Xanadocs & Permascroll Dataset

This directory contains reference Xanadoc stores and the master user permascroll (`permascroll/`)
they are written against, generated using the project's native C++ Xanadulogical engine (`xudu`).

All samples are generated directly from the external source assets in `tests/samples/` and
`tests/samples/xudu/sources/` using `tools/create-sample-xanadocs.sh`.

**The permascroll is not optional.** None of these stores holds any primedia: each is an edit
decision list whose local spans are addresses in `permascroll/active.primedia`, so a store opened
without it comes back as a document whose every version renders empty. That is also what makes the
shared quotation below a transclusion rather than a copy -- `xanadoc_a` and `xanadoc_b` name the
same offsets in the same scroll. Open one with `--permascroll tests/samples/xudu/permascroll`, or
construct a `UserPermascroll` pointed at that directory and hand it to the `Store`, as
`SampleXanadocsTest` does.

______________________________________________________________________

## Directory Structure

```
tests/samples/xudu/
├── permascroll/                                # Sovereign author permascroll -- the content itself
│   └── active.primedia                         # every byte these documents' spans address
├── README.md                                   # This documentation file
│
├── core_hypertext/                             # 8 Author Link Types + Emergent Transclusions
│   ├── xanadoc_a/                              # Primary document ("The Nature of Hypertext")
│   ├── xanadoc_b/                              # Secondary document ("Xanadulogical Synthesis & Manifolds")
│   └── unified_store/                          # Unified store holding both microversions
│
├── multimedia/                                 # Multimedia Primedia Demonstrations
│   ├── 01_multipage_pdf/                       # Multi-page PDF source doc with forced page breaks
│   ├── 02_pdf_linked_xanadoc/                  # Multi-page PDF doc linked to an analytical xanadoc
│   ├── 03_mixed_text_image/                    # Page sharing interleaved text and image primedia
│   ├── 04_audio_doc/                           # Standalone audio media document (44.1kHz PCM)
│   ├── 05_video_doc/                           # Standalone video media document (MP4 container)
│   ├── 06_embedded_media_page/                 # Embedded audio & video with flowing multi-page text
│   ├── 07_audio_transclusion/                  # Audio temporal subspan (E5 659Hz tone) transcluded
│   ├── 08_video_transclusion/                  # Video temporal clip (Scene Gamma) transcluded
│   ├── 09_image_transclusion/                  # Image spatial sub-region (IDAT quadrant) transcluded
│   └── 10_svg_static_image/                    # Static SVG vector primedia, rasterized via ThorVG
│
└── beams/                                      # Complex 3D Beam Topologies
    ├── 01_one_to_many/                         # 1 thesis span -> 3 observation spans
    ├── 02_many_to_many/                        # 2 thesis spans -> 2 observation spans with centroid leveling
    └── 03_multi_span_stacked/                  # Multi-span beam above 2 single-span beams
```

Each store directory holds `ops.nodes` (the operations), `store.tables` (scrolls, links, current
versions and annotations) and, where `--export-osmic` was used, `ops.export`. `11_floating_image`
has its own `11_floating_image.permascroll/` beside it rather than sharing the one above, because
what it demonstrates is a layout decision about one image and one paragraph.

______________________________________________________________________

## 1. Core Hypertext Samples (`core_hypertext/`)

### The 8 Author Link Types (`ProminenceTier::Author`)

All links are bound to permanent primedia content coordinates with `ProminenceTier::Author`:

1. **`LinkType::Comment`**: Comments on bidirectional link theory in Document A from Document B's
   title.
1. **`LinkType::Illustration`**: Illustrates Doc A's Docuverse continuum with Doc B's Eightfold
   Relational Taxonomy.
1. **`LinkType::Disagreement`**: Dialectic critique contrasting hierarchical web links against fluid
   knowledge networks.
1. **`LinkType::Authorship`**: Author attribution linking chapter titles to Theodor Holm Nelson.
1. **`LinkType::Quotation`**: Explicit citation linking the quoted sentence to its origin.
1. **`LinkType::Other`**: Contextual associative connection between permascrolls and transclusion
   permanence.
1. **`LinkType::Format`**: Presentation attribute link applying `FormatAttribute::Bold` to the core
   thesis via `vocabularySpanFor(FormatAttribute::Bold)`.
1. **`LinkType::Dimension`**: Zigzag 2-rank dimensional manifold link with
   `owner = "dimension:d.concept"`.

### Emergent Transclusion

Both `xanadoc_a` and `xanadoc_b` share the identical primedia span:

> *"EVERYTHING IS DEEPLY INTERTWINGLED. In an important sense there are no 'subjects' at all; there
> is only all knowledge, since the brute facts, but the aspects of reality and the thoughts which
> have already been thought are interconnectable into the same great tangle."*

When opened together in `xudu` or evaluated via `placeTransclusions()`, the engine detects the
shared coordinate overlap and renders an **Identity Gold** volumetric transclusion ribbon without
allocating any duplicate text storage.

______________________________________________________________________

## 2. Multimedia Demonstrations (`multimedia/`)

- **`01_multipage_pdf`**: Ingests `tests/samples/multipage.pdf` into a `Store`, inserting
  `OpKind::PageBreak` ops at page boundaries so pages lay out identically to the original PDF.
- **`02_pdf_linked_xanadoc`**: Connects annotations on PDF Page 0 and Page 1 to commentary
  paragraphs in a companion analysis xanadoc.
- **`03_mixed_text_image`**: Interleaves raster image asset bytes (`sample_image.png`) with
  descriptive header and caption paragraphs on a single page.
- **`04_audio_doc` & `05_video_doc`**: Standalone audio (44.1 kHz PCM with 4 distinct tones) and
  video (MP4 container with 4 distinct scene keyframes) stream primedia documents.
- **`06_embedded_media_page`**: Demonstrates multi-page text flowing around embedded interactive
  audio and video widgets.
- **`07_audio_transclusion`**: Page 1 contains the 4-tone master audio recording; Page 2 transcludes
  the 1-second E5 (659.25 Hz) tone without copying audio bytes.
- **`08_video_transclusion`**: Page 1 contains the 4-scene master video stream; Page 2 transcludes
  the Scene Gamma clip.
- **`09_image_transclusion`**: Page 1 contains the master 64x64 quadrant image; Page 2 transcludes
  the compressed IDAT quadrant detail crop.
- **`10_svg_static_image`**: Interleaves static SVG vector primedia (`sample_image.svg`, four
  quadrant rects plus a circle) with descriptive text, rasterized via ThorVG rather than a raster
  decoder -- otherwise the same layout `03_mixed_text_image` demonstrates for a raster PNG.

______________________________________________________________________

## 3. Complex Beams Demonstrations (`beams/`)

- **`01_one_to_many`**: Demonstrates centroid alignment where a single thesis span in Doc A links to
  3 non-adjacent observation spans in Doc B.
- **`02_many_to_many`**: Demonstrates dual-anchor centroid leveling where 2 premise spans in Doc A
  connect to 2 conclusion spans in Doc B.
- **`03_multi_span_stacked`**:
  - **Link 101**: Broad multi-span link connecting non-contiguous top and bottom spans.
  - **Link 102**: Upper single-span link focused on top paragraphs.
  - **Link 103**: Lower single-span link focused on bottom paragraphs.
  - Demonstrates multi-span disambiguation spines and instance micro-hue shifts
    (`linkColourWithInstanceShift`).

______________________________________________________________________

## How to View and Run

### Run Unit Tests

```sh
make test TEST_FILTER='SampleXanadocsTest.*'
```

### Open in `xudu` (Interactive 3D Editor)

```sh
# --permascroll is required: the stores hold no content of their own.
PERMA=tests/samples/xudu/permascroll

# View core hypertext documents side by side
./build/xudu --permascroll $PERMA \\
  tests/samples/xudu/core_hypertext/unified_store --version-id 1 --alongside 2

# View 1-to-many beams
./build/xudu --permascroll $PERMA \\
  tests/samples/xudu/beams/01_one_to_many --version-id 1 --alongside 2

# View stacked multi-span beams
./build/xudu --permascroll $PERMA \\
  tests/samples/xudu/beams/03_multi_span_stacked --version-id 1 --alongside 2
```

### Regenerate Samples

```sh
make -j$(nproc) xudu
./tools/create-sample-xanadocs.sh          # core_hypertext, multimedia, beams, permascroll/
./tools/create-floating-image-sample.sh    # 11_floating_image, which the above deletes
```

Both, in that order: the first script opens by removing the whole `multimedia/` directory, which
takes `11_floating_image` with it.
