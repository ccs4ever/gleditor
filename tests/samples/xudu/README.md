# Xudu Sample Xanadocs & Permascroll Dataset

This directory contains reference Xanadoc stores and a master user permascroll (`000.scroll`) generated using the project's native C++ Xanadulogical engine (`xudu`).

All samples are generated directly from the external source assets in `tests/samples/` and `tests/samples/xudu/sources/` using `tools/generate_sample_xanadocs.cpp`.

---

## Directory Structure

```
tests/samples/xudu/
├── 000.scroll                                  # Sovereign author permascroll byte stream
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
│   └── 09_image_transclusion/                  # Image spatial sub-region (IDAT quadrant) transcluded
│
└── beams/                                      # Complex 3D Beam Topologies
    ├── 01_one_to_many/                         # 1 thesis span -> 3 observation spans
    ├── 02_many_to_many/                        # 2 thesis spans -> 2 observation spans with centroid leveling
    └── 03_multi_span_stacked/                  # Multi-span beam above 2 single-span beams
```

---

## 1. Core Hypertext Samples (`core_hypertext/`)

### The 8 Author Link Types (`ProminenceTier::Author`)
All links are bound to permanent primedia content coordinates with `ProminenceTier::Author`:

1. **`LinkType::Comment`**: Comments on bidirectional link theory in Document A from Document B's title.
2. **`LinkType::Illustration`**: Illustrates Doc A's Docuverse continuum with Doc B's Eightfold Relational Taxonomy.
3. **`LinkType::Disagreement`**: Dialectic critique contrasting hierarchical web links against fluid knowledge networks.
4. **`LinkType::Authorship`**: Author attribution linking chapter titles to Theodor Holm Nelson.
5. **`LinkType::Quotation`**: Explicit citation linking the quoted sentence to its origin.
6. **`LinkType::Other`**: Contextual associative connection between permascrolls and transclusion permanence.
7. **`LinkType::Format`**: Presentation attribute link applying `FormatAttribute::Bold` to the core thesis via `vocabularySpanFor(FormatAttribute::Bold)`.
8. **`LinkType::Dimension`**: Zigzag 2-rank dimensional manifold link with `owner = "dimension:d.concept"`.

### Emergent Transclusion
Both `xanadoc_a` and `xanadoc_b` share the identical primedia span:
> *"EVERYTHING IS DEEPLY INTERTWINGLED. In an important sense there are no 'subjects' at all; there is only all knowledge, since the brute facts, but the aspects of reality and the thoughts which have already been thought are interconnectable into the same great tangle."*

When opened together in `xudu` or evaluated via `placeTransclusions()`, the engine detects the shared coordinate overlap and renders an **Identity Gold** volumetric transclusion ribbon without allocating any duplicate text storage.

---

## 2. Multimedia Demonstrations (`multimedia/`)

- **`01_multipage_pdf`**: Ingests `tests/samples/multipage.pdf` into a `Store`, inserting `OpKind::PageBreak` ops at page boundaries so pages lay out identically to the original PDF.
- **`02_pdf_linked_xanadoc`**: Connects annotations on PDF Page 0 and Page 1 to commentary paragraphs in a companion analysis xanadoc.
- **`03_mixed_text_image`**: Interleaves raster image asset bytes (`sample_image.png`) with descriptive header and caption paragraphs on a single page.
- **`04_audio_doc` & `05_video_doc`**: Standalone audio (44.1 kHz PCM with 4 distinct tones) and video (MP4 container with 4 distinct scene keyframes) stream primedia documents.
- **`06_embedded_media_page`**: Demonstrates multi-page text flowing around embedded interactive audio and video widgets.
- **`07_audio_transclusion`**: Page 1 contains the 4-tone master audio recording; Page 2 transcludes the 1-second E5 (659.25 Hz) tone without copying audio bytes.
- **`08_video_transclusion`**: Page 1 contains the 4-scene master video stream; Page 2 transcludes the Scene Gamma clip.
- **`09_image_transclusion`**: Page 1 contains the master 64x64 quadrant image; Page 2 transcludes the compressed IDAT quadrant detail crop.

---

## 3. Complex Beams Demonstrations (`beams/`)

- **`01_one_to_many`**: Demonstrates centroid alignment where a single thesis span in Doc A links to 3 non-adjacent observation spans in Doc B.
- **`02_many_to_many`**: Demonstrates dual-anchor centroid leveling where 2 premise spans in Doc A connect to 2 conclusion spans in Doc B.
- **`03_multi_span_stacked`**:
  - **Link 101**: Broad multi-span link connecting non-contiguous top and bottom spans.
  - **Link 102**: Upper single-span link focused on top paragraphs.
  - **Link 103**: Lower single-span link focused on bottom paragraphs.
  - Demonstrates multi-span disambiguation spines and instance micro-hue shifts (`linkColourWithInstanceShift`).

---

## How to View and Run

### Run Unit Tests
```sh
make test TEST_FILTER='SampleXanadocsTest.*'
```

### Open in `xudu` (Interactive 3D Editor)
```sh
# View core hypertext documents side by side
./build/xudu tests/samples/xudu/core_hypertext/unified_store --version-id 1 --alongside 2

# View 1-to-many beams
./build/xudu tests/samples/xudu/beams/01_one_to_many --version-id 1 --alongside 2

# View stacked multi-span beams
./build/xudu tests/samples/xudu/beams/03_multi_span_stacked --version-id 1 --alongside 2
```

### Regenerate Samples
```sh
make generate-sample-xanadocs && ./build/generate-sample-xanadocs
```
