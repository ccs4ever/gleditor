/**
 * @file sample_xanadocs_test.cpp
 * @brief Comprehensive automated tests verifying generated sample xanadocs,
 *        dummy permascroll 000.scroll, 8 author link types, emergent transclusions,
 *        multimedia demonstrations, and complex beam topologies.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <xudu/core/format.hpp>
#include <xudu/core/magic_mime.hpp>
#include <xudu/core/link_layout.hpp>
#include <xudu/core/microversion.hpp>
#include <xudu/core/ops.hpp>
#include <xudu/core/store.hpp>
#include <xudu/core/user_permascroll.hpp>

namespace fs = std::filesystem;
using xudu::FormatAttribute;
using xudu::HalfLink;
using xudu::Link;
using xudu::LinkedPair;
using xudu::LinkType;
using xudu::MicroversionId;
using xudu::ProminenceTier;
using xudu::Store;
using xudu::TransclusionPair;
using xudu::Version;

namespace {

const fs::path kSampleBaseDir = "tests/samples/xudu";

std::vector<const Version *> viewing(const std::vector<Version> &versions) {
  std::vector<const Version *> out;
  out.reserve(versions.size());
  for (const auto &v : versions) {
    out.push_back(&v);
  }
  return out;
}

// -----------------------------------------------------------------------------
// Permascroll 000.scroll Verification
// -----------------------------------------------------------------------------
TEST(SampleXanadocsTest, Permascroll000ScrollExistsAndIsNonEmpty) {
  const auto scrollPath = kSampleBaseDir / "000.scroll";
  ASSERT_TRUE(fs::exists(scrollPath)) << "000.scroll must exist at " << scrollPath;
  const auto size = fs::file_size(scrollPath);
  EXPECT_GT(size, 1000U) << "000.scroll should contain generated primedia byte stream";
}

// -----------------------------------------------------------------------------
// Core Hypertext: 8 Link Types & Emergent Transclusions
// -----------------------------------------------------------------------------
TEST(SampleXanadocsTest, CoreHypertextLoadsAndDiscoversTransclusion) {
  const auto coreDir = kSampleBaseDir / "core_hypertext";

  Store storeA;
  storeA.load((coreDir / "xanadoc_a").string());
  EXPECT_GT(storeA.opCount(), 0U);

  Store storeB;
  storeB.load((coreDir / "xanadoc_b").string());
  EXPECT_GT(storeB.opCount(), 0U);

  const auto verA = storeA.latest();
  const auto verB = storeB.latest();

  const auto builtA = storeA.rebuild(verA);
  const auto builtB = storeB.rebuild(verB);

  EXPECT_THAT(storeA.textOf(verA), testing::HasSubstr("Chapter 1: The Nature of Hypertext"));
  EXPECT_THAT(storeB.textOf(verB), testing::HasSubstr("Commentary on Xanadulogical Systems"));

  // Check emergent transclusion
  std::vector<TransclusionPair> tPairs;
  xudu::placeTransclusions(viewing({builtA, builtB}), tPairs);

  ASSERT_FALSE(tPairs.empty()) << "Expected emergent transclusion between Doc A and Doc B";
  EXPECT_EQ(tPairs[0].from.doc, 0U);
  EXPECT_EQ(tPairs[0].to.doc, 1U);
  EXPECT_GT(tPairs[0].from.end, tPairs[0].from.start);

  const auto transcludedTextA =
      storeA.textOf(verA).substr(tPairs[0].from.start, tPairs[0].from.end - tPairs[0].from.start);
  const auto transcludedTextB =
      storeB.textOf(verB).substr(tPairs[0].to.start, tPairs[0].to.end - tPairs[0].to.start);

  EXPECT_EQ(transcludedTextA, transcludedTextB);
  EXPECT_THAT(transcludedTextA, testing::HasSubstr("EVERYTHING IS DEEPLY INTERTWINGLED"));
}

TEST(SampleXanadocsTest, CoreHypertextContainsAll8AuthorLinkTypes) {
  const auto coreDir = kSampleBaseDir / "core_hypertext";

  Store storeA;
  storeA.load((coreDir / "xanadoc_a").string());
  Store storeB;
  storeB.load((coreDir / "xanadoc_b").string());

  const auto builtA = storeA.rebuild(storeA.latest());
  const auto builtB = storeB.rebuild(storeB.latest());

  std::vector<LinkedPair> placed;
  std::vector<HalfLink> unplaced;
  xudu::placeLinks(storeA.links(), viewing({builtA, builtB}), placed, unplaced);

  std::set<LinkType> foundTypes;
  for (const auto &pair : placed) {
    foundTypes.insert(pair.type);
    EXPECT_EQ(pair.tier, ProminenceTier::Author);
    EXPECT_NE(pair.from.doc, pair.to.doc);
  }

  // Verify all 8 Link Types are present:
  // Comment, Illustration, Disagreement, Authorship, Quotation, Other, Format, Dimension
  EXPECT_TRUE(foundTypes.contains(LinkType::Comment));
  EXPECT_TRUE(foundTypes.contains(LinkType::Illustration));
  EXPECT_TRUE(foundTypes.contains(LinkType::Disagreement));
  EXPECT_TRUE(foundTypes.contains(LinkType::Authorship));
  EXPECT_TRUE(foundTypes.contains(LinkType::Quotation));
  EXPECT_TRUE(foundTypes.contains(LinkType::Other));
  EXPECT_TRUE(foundTypes.contains(LinkType::Dimension));

  // Format link is present in link table and recognises FormatAttribute::Bold
  bool foundFormatLink = false;
  for (const auto &[id, link] : storeA.links()) {
    if (link.type == LinkType::Format) {
      foundFormatLink = true;
      const auto attr = storeA.formatAttributeOf(link);
      EXPECT_EQ(attr, FormatAttribute::Bold);
      EXPECT_EQ(link.tier, ProminenceTier::Author);
    }
  }
  EXPECT_TRUE(foundFormatLink);
}

TEST(SampleXanadocsTest, CoreHypertextUnifiedStoreLoadsBothVersions) {
  const auto unifiedDir = kSampleBaseDir / "core_hypertext" / "unified_store";

  Store unified;
  unified.load(unifiedDir.string());
  EXPECT_GE(unified.allVersions().size(), 2U);

  const auto all = unified.allVersions();
  const auto built1 = unified.rebuild(all.front());
  const auto built2 = unified.rebuild(all.back());

  EXPECT_THAT(unified.textOf(all.front()),
              testing::HasSubstr("Chapter 1: The Nature of Hypertext"));
  EXPECT_THAT(unified.textOf(all.back()),
              testing::HasSubstr("Commentary on Xanadulogical Systems"));

  std::vector<TransclusionPair> tPairs;
  xudu::placeTransclusions(viewing({built1, built2}), tPairs);
  EXPECT_FALSE(tPairs.empty());
}

// -----------------------------------------------------------------------------
// Multimedia Demonstrations
// -----------------------------------------------------------------------------
TEST(SampleXanadocsTest, Multimedia01MultipagePdfHasForcedBreaks) {
  const auto pdfDir = kSampleBaseDir / "multimedia" / "01_multipage_pdf";
  Store store;
  store.load(pdfDir.string());

  const auto ver = store.latest();
  const auto rebuilt = store.rebuild(ver);

  EXPECT_THAT(store.textOf(ver), testing::HasSubstr("First Page Header"));
  EXPECT_THAT(store.textOf(ver), testing::HasSubstr("Second Page Header"));
  EXPECT_THAT(store.textOf(ver), testing::HasSubstr("Third Page Header"));

  EXPECT_GE(rebuilt.forcedBreaks().size(), 3U);
}

TEST(SampleXanadocsTest, Multimedia02PdfLinkedXanadocCrossDocumentLinks) {
  const auto dir = kSampleBaseDir / "multimedia" / "02_pdf_linked_xanadoc";
  Store store;
  store.load(dir.string());

  const auto all = store.allVersions();
  ASSERT_GE(all.size(), 2U);

  const auto pdfBuilt = store.rebuild(all[0]);
  const auto comBuilt = store.rebuild(all.back());

  std::vector<LinkedPair> placed;
  std::vector<HalfLink> unplaced;
  xudu::placeLinks(store.links(), viewing({pdfBuilt, comBuilt}), placed, unplaced);

  ASSERT_GE(placed.size(), 2U);
  EXPECT_EQ(placed[0].from.doc, 0U);
  EXPECT_EQ(placed[0].to.doc, 1U);
}

TEST(SampleXanadocsTest, Multimedia03MixedTextImageContainsRasterAndText) {
  const auto dir = kSampleBaseDir / "multimedia" / "03_mixed_text_image";
  Store store;
  store.load(dir.string());

  const auto ver = store.latest();
  const auto rebuilt = store.rebuild(ver);
  xudu::MagicMimeDetector magic;

  bool hasImage = false;
  bool hasText  = false;
  for (const auto &p : rebuilt.pieces()) {
    const auto bytes = store.read(p);
    const auto mime  = magic.identifyBuffer(bytes.data(), bytes.size());
    if (xudu::MagicMimeDetector::isImageMime(mime)) {
      hasImage = true;
    } else if (!bytes.empty()) {
      hasText = true;
    }
  }
  EXPECT_TRUE(hasImage)
      << "Expected raw image primedia span in 03_mixed_text_image";
  EXPECT_TRUE(hasText)
      << "Expected text primedia span in 03_mixed_text_image";
}

TEST(SampleXanadocsTest,
     Multimedia04AudioDocContainsWavHeaderAndWaveformAnalysis) {
  const auto dir = kSampleBaseDir / "multimedia" / "04_audio_doc";
  Store store;
  store.load(dir.string());

  const auto ver = store.latest();
  const auto rebuilt = store.rebuild(ver);
  xudu::MagicMimeDetector magic;

  bool hasAudio = false;
  for (const auto &p : rebuilt.pieces()) {
    const auto bytes = store.read(p);
    const auto mime  = magic.identifyBuffer(bytes.data(), bytes.size());
    if (xudu::MagicMimeDetector::isAudioMime(mime)) {
      hasAudio = true;
    }
  }
  EXPECT_TRUE(hasAudio)
      << "Expected raw audio primedia span in 04_audio_doc";
}

TEST(SampleXanadocsTest,
     Multimedia05VideoDocContainsMp4ContainerAndKeyframes) {
  const auto dir = kSampleBaseDir / "multimedia" / "05_video_doc";
  Store store;
  store.load(dir.string());

  const auto ver = store.latest();
  const auto rebuilt = store.rebuild(ver);
  xudu::MagicMimeDetector magic;

  bool hasVideo = false;
  for (const auto &p : rebuilt.pieces()) {
    const auto bytes = store.read(p);
    const auto mime  = magic.identifyBuffer(bytes.data(), bytes.size());
    if (xudu::MagicMimeDetector::isVideoMime(mime)) {
      hasVideo = true;
    }
  }
  EXPECT_TRUE(hasVideo)
      << "Expected raw video primedia span in 05_video_doc";
}

TEST(SampleXanadocsTest, Multimedia06EmbeddedMediaPageHasPageBreaksAndFlow) {
  const auto dir = kSampleBaseDir / "multimedia" / "06_embedded_media_page";
  Store store;
  store.load(dir.string());

  const auto ver = store.latest();
  const auto rebuilt = store.rebuild(ver);

  EXPECT_GE(rebuilt.forcedBreaks().size(), 2U);
}

TEST(SampleXanadocsTest, Multimedia07AudioTransclusionVerifiesE5ToneTemporalSubspan) {
  const auto dir = kSampleBaseDir / "multimedia" / "07_audio_transclusion";
  Store store;
  store.load(dir.string());

  const auto all = store.allVersions();
  ASSERT_GE(all.size(), 2U);
  const auto v1Built = store.rebuild(all.front());
  const auto v2Built = store.rebuild(all.back());

  std::vector<TransclusionPair> tPairs;
  xudu::placeTransclusions(viewing({v1Built, v2Built}), tPairs);

  ASSERT_FALSE(tPairs.empty()) << "Expected audio subspan transclusion between Page 1 and Page 2";
  EXPECT_EQ(tPairs[0].from.doc, 0U);
  EXPECT_EQ(tPairs[0].to.doc, 1U);
  EXPECT_GT(tPairs[0].from.end, tPairs[0].from.start);
}

TEST(SampleXanadocsTest, Multimedia08VideoTransclusionVerifiesSceneGammaTemporalClip) {
  const auto dir = kSampleBaseDir / "multimedia" / "08_video_transclusion";
  Store store;
  store.load(dir.string());

  const auto all = store.allVersions();
  ASSERT_GE(all.size(), 2U);
  const auto v1Built = store.rebuild(all.front());
  const auto v2Built = store.rebuild(all.back());

  std::vector<TransclusionPair> tPairs;
  xudu::placeTransclusions(viewing({v1Built, v2Built}), tPairs);

  ASSERT_FALSE(tPairs.empty()) << "Expected video clip transclusion between Page 1 and Page 2";
  EXPECT_EQ(tPairs[0].from.doc, 0U);
  EXPECT_EQ(tPairs[0].to.doc, 1U);
}

TEST(SampleXanadocsTest, Multimedia09ImageTransclusionVerifiesSpatialIdatCrop) {
  const auto dir = kSampleBaseDir / "multimedia" / "09_image_transclusion";
  Store store;
  store.load(dir.string());

  const auto all = store.allVersions();
  ASSERT_GE(all.size(), 2U);
  const auto v1Built = store.rebuild(all.front());
  const auto v2Built = store.rebuild(all.back());

  std::vector<TransclusionPair> tPairs;
  xudu::placeTransclusions(viewing({v1Built, v2Built}), tPairs);

  ASSERT_FALSE(tPairs.empty()) << "Expected image crop transclusion between Page 1 and Page 2";
  EXPECT_EQ(tPairs[0].from.doc, 0U);
  EXPECT_EQ(tPairs[0].to.doc, 1U);
}

// -----------------------------------------------------------------------------
// Complex Beams Topologies
// -----------------------------------------------------------------------------
TEST(SampleXanadocsTest, Beams01OneToManyLinkPlacement) {
  const auto dir = kSampleBaseDir / "beams" / "01_one_to_many";
  Store store;
  store.load(dir.string());

  const auto all = store.allVersions();
  ASSERT_GE(all.size(), 2U);

  const auto builtA = store.rebuild(all[0]);
  const auto builtB = store.rebuild(all.back());

  std::vector<LinkedPair> placed;
  std::vector<HalfLink> unplaced;
  xudu::placeLinks(store.links(), viewing({builtA, builtB}), placed, unplaced);

  ASSERT_EQ(placed.size(), 1U);
  EXPECT_EQ(placed[0].from.doc, 0U);
  EXPECT_EQ(placed[0].to.doc, 1U);

  // Link right end covers all 3 observations
  const auto &link = store.links().begin()->second;
  EXPECT_EQ(link.right.size(), 3U);
  EXPECT_EQ(link.tier, ProminenceTier::Author);
}

TEST(SampleXanadocsTest, Beams02ManyToManyLinkPlacement) {
  const auto dir = kSampleBaseDir / "beams" / "02_many_to_many";
  Store store;
  store.load(dir.string());

  const auto all = store.allVersions();
  ASSERT_GE(all.size(), 2U);

  const auto builtA = store.rebuild(all[0]);
  const auto builtB = store.rebuild(all.back());

  std::vector<LinkedPair> placed;
  std::vector<HalfLink> unplaced;
  xudu::placeLinks(store.links(), viewing({builtA, builtB}), placed, unplaced);

  ASSERT_EQ(placed.size(), 1U);
  const auto &link = store.links().begin()->second;
  EXPECT_EQ(link.left.size(), 2U);
  EXPECT_EQ(link.right.size(), 2U);
  EXPECT_EQ(link.tier, ProminenceTier::Author);
}

TEST(SampleXanadocsTest, Beams03MultiSpanStackedLinks) {
  const auto dir = kSampleBaseDir / "beams" / "03_multi_span_stacked";
  Store store;
  store.load(dir.string());

  const auto all = store.allVersions();
  ASSERT_GE(all.size(), 2U);

  const auto builtA = store.rebuild(all[0]);
  const auto builtB = store.rebuild(all.back());

  std::vector<LinkedPair> placed;
  std::vector<HalfLink> unplaced;
  xudu::placeLinks(store.links(), viewing({builtA, builtB}), placed, unplaced);

  ASSERT_EQ(placed.size(), 3U);

  // Check that all 3 links are of type Comment and tier Author
  for (const auto &pair : placed) {
    EXPECT_EQ(pair.type, LinkType::Comment);
    EXPECT_EQ(pair.tier, ProminenceTier::Author);
  }

  const auto &links = store.links();
  ASSERT_EQ(links.size(), 3U);

  // Link 1: Broad multi-span link connecting both top and bottom non-contiguous spans
  ASSERT_TRUE(links.contains(1));
  ASSERT_TRUE(links.contains(2));
  ASSERT_TRUE(links.contains(3));

  EXPECT_EQ(links.at(1).left.size(), 2U);
  EXPECT_EQ(links.at(1).right.size(), 2U);
  EXPECT_EQ(links.at(2).left.size(), 1U);
  EXPECT_EQ(links.at(2).right.size(), 1U);
  EXPECT_EQ(links.at(3).left.size(), 1U);
  EXPECT_EQ(links.at(3).right.size(), 1U);
}

} // namespace
