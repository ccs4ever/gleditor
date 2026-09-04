#!/usr/bin/env bash
# tools/create-sample-xanadocs.sh
# Generate all sample Xanadocs by invoking the xudu application directly in headless mode.

set -euo pipefail

XUDU="./build/xudu"
BASE_DIR="tests/samples/xudu"
SOURCES_DIR="${BASE_DIR}/sources"
SCROLL="${BASE_DIR}/000.scroll"

if [[ ! -x "${XUDU}" ]]; then
  echo "Error: ${XUDU} not found or not executable. Build it first with: make -j\$(nproc) xudu" >&2
  exit 1
fi

echo "==> Creating sample Xanadocs using ${XUDU}..."

# Clean target directories and old scroll
rm -rf "${BASE_DIR}/core_hypertext" "${BASE_DIR}/multimedia" "${BASE_DIR}/beams" "${SCROLL}"
mkdir -p "${BASE_DIR}/core_hypertext" "${BASE_DIR}/multimedia" "${BASE_DIR}/beams"

# -----------------------------------------------------------------------------
# 1. Core Hypertext
# -----------------------------------------------------------------------------
echo "--> Creating Core Hypertext sample stores..."

# xanadoc_a (Doc A base)
${XUDU} "${BASE_DIR}/core_hypertext/xanadoc_a" \
  --import "${SOURCES_DIR}/literary_machines_doc_a.txt" \
  --dump-permascroll "${SCROLL}" \
  --export-osmic --headless

# xanadoc_b (Doc B with transcluded quote from Doc A)
${XUDU} "${BASE_DIR}/core_hypertext/xanadoc_b" \
  --permascroll "${SCROLL}" \
  --import "${SOURCES_DIR}/intertwingularity_doc_b_prefix.txt" \
  --open-store "${BASE_DIR}/core_hypertext/xanadoc_a" \
  --transclude-text "1@EVERYTHING IS DEEPLY INTERTWINGLED,0:append" \
  --insert-text "0:append:${SOURCES_DIR}/intertwingularity_doc_b_suffix.txt" \
  --dump-permascroll "${SCROLL}" \
  --export-osmic --headless

# Update xanadoc_a with all 8 author links pointing to Doc B spans
${XUDU} "${BASE_DIR}/core_hypertext/xanadoc_a" \
  --permascroll "${SCROLL}" \
  --open-store "${BASE_DIR}/core_hypertext/xanadoc_b" \
  --link "0@Links must be two-way and visible:35,1:0:60:comment:author:Theodor_Holm_Nelson" \
  --link "0@The Docuverse Continuum:50,1@Section C: The Eightfold Relational Taxonomy:44:illustration:author:Theodor_Holm_Nelson" \
  --link "0@By 'hypertext' I mean:40,1@Section B: Dialectic Critique:45:disagreement:author:Theodor_Holm_Nelson" \
  --link "0:0:47,1:0:60:authorship:author:Theodor_Holm_Nelson" \
  --link "0@EVERYTHING IS DEEPLY INTERTWINGLED,1@EVERYTHING IS DEEPLY INTERTWINGLED:quotation:author:Theodor_Holm_Nelson" \
  --link "0@Transclusion is not copy-and-paste:35,1@Xanadu replaces static URLs:40:other:author:Theodor_Holm_Nelson" \
  --format-link "0@EVERYTHING IS DEEPLY INTERTWINGLED:34:bold:author:Theodor_Holm_Nelson" \
  --dimension-link "0@The Docuverse Continuum:37,1@Section C: The Eightfold Relational Taxonomy:34:dimension:d.concept" \
  --dump-permascroll "${SCROLL}" \
  --export-osmic --headless

# unified_store (both docs, transclusion, and all 8 link types in one store)
${XUDU} "${BASE_DIR}/core_hypertext/unified_store" \
  --permascroll "${SCROLL}" \
  --import "${SOURCES_DIR}/literary_machines_doc_a.txt" \
  --import-branch "${SOURCES_DIR}/intertwingularity_doc_b_prefix.txt" \
  --transclude-text "0@EVERYTHING IS DEEPLY INTERTWINGLED,1:append" \
  --insert-text "1:append:${SOURCES_DIR}/intertwingularity_doc_b_suffix.txt" \
  --link "0@Links must be two-way and visible:35,1:0:60:comment:author:Theodor_Holm_Nelson" \
  --link "0@The Docuverse Continuum:50,1@Section C: The Eightfold Relational Taxonomy:44:illustration:author:Theodor_Holm_Nelson" \
  --link "0@By 'hypertext' I mean:40,1@Section B: Dialectic Critique:45:disagreement:author:Theodor_Holm_Nelson" \
  --link "0:0:47,1:0:60:authorship:author:Theodor_Holm_Nelson" \
  --link "0@EVERYTHING IS DEEPLY INTERTWINGLED,1@EVERYTHING IS DEEPLY INTERTWINGLED:quotation:author:Theodor_Holm_Nelson" \
  --link "0@Transclusion is not copy-and-paste:35,1@Xanadu replaces static URLs:40:other:author:Theodor_Holm_Nelson" \
  --format-link "0@EVERYTHING IS DEEPLY INTERTWINGLED:34:bold:author:Theodor_Holm_Nelson" \
  --dimension-link "0@The Docuverse Continuum:37,1@Section C: The Eightfold Relational Taxonomy:34:dimension:d.concept" \
  --dump-permascroll "${SCROLL}" \
  --export-osmic --headless

# -----------------------------------------------------------------------------
# 2. Multimedia Demonstrations
# -----------------------------------------------------------------------------
echo "--> Creating Multimedia sample stores..."

# 01_multipage_pdf
${XUDU} "${BASE_DIR}/multimedia/01_multipage_pdf" \
  --permascroll "${SCROLL}" \
  --import "tests/samples/multipage.pdf" \
  --dump-permascroll "${SCROLL}" \
  --export-osmic --headless

# 02_pdf_linked_xanadoc
${XUDU} "${BASE_DIR}/multimedia/02_pdf_linked_xanadoc" \
  --permascroll "${SCROLL}" \
  --import "tests/samples/multipage.pdf" \
  --import-branch "${SOURCES_DIR}/multipage_pdf_review.txt" \
  --link "0:0:19,1:0:70:comment:author:reviewer" \
  --link "0:20:18,1:100:65:quotation:author:reviewer" \
  --dump-permascroll "${SCROLL}" \
  --export-osmic --headless

# 03_mixed_text_image
${XUDU} "${BASE_DIR}/multimedia/03_mixed_text_image" \
  --permascroll "${SCROLL}" \
  --import "tests/samples/sample_image.png" \
  --import-branch "${SOURCES_DIR}/mixed_text_image.txt" \
  --transclude "0:0:228,1:append" \
  --dump-permascroll "${SCROLL}" \
  --export-osmic --headless

# 04_audio_doc
${XUDU} "${BASE_DIR}/multimedia/04_audio_doc" \
  --permascroll "${SCROLL}" \
  --import "tests/samples/sample_audio.wav" \
  --dump-permascroll "${SCROLL}" \
  --export-osmic --headless

# 05_video_doc
${XUDU} "${BASE_DIR}/multimedia/05_video_doc" \
  --permascroll "${SCROLL}" \
  --import "tests/samples/sample_video.mp4" \
  --dump-permascroll "${SCROLL}" \
  --export-osmic --headless

# 06_embedded_media_page
${XUDU} "${BASE_DIR}/multimedia/06_embedded_media_page" \
  --permascroll "${SCROLL}" \
  --import "${SOURCES_DIR}/embedded_media_page_p1.txt" \
  --insert-text "0:append:tests/samples/sample_audio.wav" \
  --import-break "${SOURCES_DIR}/embedded_media_page_p2.txt" \
  --insert-text "0:append:tests/samples/sample_video.mp4" \
  --import-break "${SOURCES_DIR}/embedded_media_page_p3.txt" \
  --insert-text "0:append:tests/samples/sample_image.png" \
  --dump-permascroll "${SCROLL}" \
  --export-osmic --headless

# 07_audio_transclusion
${XUDU} "${BASE_DIR}/multimedia/07_audio_transclusion" \
  --permascroll "${SCROLL}" \
  --import "tests/samples/sample_audio.wav" \
  --import-branch "${SOURCES_DIR}/audio_transclusion_b_prefix.txt" \
  --transclude "0:20000:30000,1:append" \
  --insert-text "1:append:${SOURCES_DIR}/audio_transclusion_b_suffix.txt" \
  --dump-permascroll "${SCROLL}" \
  --export-osmic --headless

# 08_video_transclusion
${XUDU} "${BASE_DIR}/multimedia/08_video_transclusion" \
  --permascroll "${SCROLL}" \
  --import "tests/samples/sample_video.mp4" \
  --import-branch "${SOURCES_DIR}/video_transclusion_b_prefix.txt" \
  --transclude "0:50:200,1:append" \
  --insert-text "1:append:${SOURCES_DIR}/video_transclusion_b_suffix.txt" \
  --dump-permascroll "${SCROLL}" \
  --export-osmic --headless

# 09_image_transclusion
${XUDU} "${BASE_DIR}/multimedia/09_image_transclusion" \
  --permascroll "${SCROLL}" \
  --import "tests/samples/sample_image.png" \
  --import-branch "${SOURCES_DIR}/image_transclusion_b_prefix.txt" \
  --transclude "0:20:100,1:append" \
  --insert-text "1:append:${SOURCES_DIR}/image_transclusion_b_suffix.txt" \
  --dump-permascroll "${SCROLL}" \
  --export-osmic --headless

# 10_svg_static_image
${XUDU} "${BASE_DIR}/multimedia/10_svg_static_image" \
  --permascroll "${SCROLL}" \
  --import "tests/samples/sample_image.svg" \
  --import-branch "${SOURCES_DIR}/mixed_text_svg.txt" \
  --transclude "0:0:428,1:append" \
  --dump-permascroll "${SCROLL}" \
  --export-osmic --headless

# -----------------------------------------------------------------------------
# 3. Complex Beams Topologies
# -----------------------------------------------------------------------------
echo "--> Creating Beams sample stores..."

# 01_one_to_many
${XUDU} "${BASE_DIR}/beams/01_one_to_many" \
  --permascroll "${SCROLL}" \
  --import "${SOURCES_DIR}/hypertext_theses_doc_a.txt" \
  --import-branch "${SOURCES_DIR}/supporting_evidence_doc_b.txt" \
  --link "0@Complex ideas synthesize insights:50,1@Observation Alpha:40+1@Observation Beta:40+1@Observation Gamma:40:quotation:author:synthesizer" \
  --dump-permascroll "${SCROLL}" \
  --export-osmic --headless

# 02_many_to_many
${XUDU} "${BASE_DIR}/beams/02_many_to_many" \
  --permascroll "${SCROLL}" \
  --import "${SOURCES_DIR}/hypertext_theses_doc_a.txt" \
  --import-branch "${SOURCES_DIR}/supporting_evidence_doc_b.txt" \
  --link "0@Thesis 1:40+0@Thesis 2:40,1@Observation Alpha:40+1@Observation Beta:40:disagreement:author:dialectic_analyst" \
  --dump-permascroll "${SCROLL}" \
  --export-osmic --headless

# 03_multi_span_stacked
${XUDU} "${BASE_DIR}/beams/03_multi_span_stacked" \
  --permascroll "${SCROLL}" \
  --import "${SOURCES_DIR}/hypertext_theses_doc_a.txt" \
  --import-branch "${SOURCES_DIR}/supporting_evidence_doc_b.txt" \
  --link "0@Thesis 1:35+0@Thesis 2:35,1@Observation Alpha:35+1@Observation Gamma:35:comment:author:multi_envelope" \
  --link "0@Thesis 1:35,1@Observation Alpha:35:comment:author:upper_focus" \
  --link "0@Thesis 2:35,1@Observation Gamma:35:comment:author:lower_focus" \
  --dump-permascroll "${SCROLL}" \
  --export-osmic --headless

echo "==> All sample Xanadocs successfully created by xudu application!"
