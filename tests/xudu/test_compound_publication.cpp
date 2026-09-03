/**
 * @file test_compound_publication.cpp
 * @brief Unit tests for MediaManager staging and compound multi-torrent
 * publication.
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <xudu/core/media_manager.hpp>
#include <xudu/core/publication.hpp>
#include <xudu/core/store.hpp>

namespace {

TEST(CompoundPublicationTest, StageAndSealCompoundTorrents) {
  const auto tempDir =
      std::filesystem::temp_directory_path() / "test_compound_pub";
  std::filesystem::remove_all(tempDir);
  std::filesystem::create_directories(tempDir);

  // 1. Create a dummy media file
  const auto imageFile = tempDir / "diagram.png";
  {
    std::ofstream out(imageFile, std::ios::binary);
    // Minimal valid 1x1 PNG
    const std::vector<std::uint8_t> png = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00,
        0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00,
        0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49,
        0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
    out.write(reinterpret_cast<const char *>(png.data()), png.size());
  }

  // 2. Stage with MediaManager
  xudu::MediaManager mediaMgr(tempDir / "staged");
  const auto staged = mediaMgr.stageMediaFile(imageFile);
  EXPECT_EQ(staged.suggestedName, "diagram.png");
  EXPECT_EQ(staged.mimeType, "image/png");
  EXPECT_EQ(mediaMgr.stagedAssets().size(), 1U);

  // 3. Create Store and populate text
  xudu::Store store;
  store.insert(xudu::MicroversionId{}, 0,
               "Quoting diagram from external source.");

  const auto keys = xudu::createMutableKeys();
  xudu::SignedProvenance prov{
      .yaml      = "author: Test Author\n",
      .signature = std::string(64, '0'),
  };

  // 4. Seal Compound
  const auto compound =
      xudu::sealCompound(store, keys, "test_salt", (tempDir / "pub").string(),
                         prov, {staged.localPath});

  EXPECT_FALSE(compound.mainSeal.hash.isZero());
  ASSERT_EQ(compound.mediaTorrents.size(), 1U);
  EXPECT_FALSE(compound.mediaTorrents[0].hash.isZero());
  EXPECT_EQ(compound.mediaTorrents[0].mimeType, "image/png");
  EXPECT_FALSE(compound.mediaTorrents[0].provenance.yaml.empty());
  EXPECT_FALSE(compound.mediaTorrents[0].provenance.signature.empty());

  // Verify .torrent and files were written to publish directory
  EXPECT_TRUE(std::filesystem::exists(tempDir / "pub" / "test_salt.torrent"));
  EXPECT_TRUE(std::filesystem::exists(tempDir / "pub" / "test_salt"));
  EXPECT_TRUE(std::filesystem::exists(tempDir / "pub" / "diagram.png.torrent"));
  EXPECT_TRUE(
      std::filesystem::exists(tempDir / "pub" / "diagram.png" / "diagram.png"));
  EXPECT_TRUE(std::filesystem::exists(tempDir / "pub" / "diagram.png" /
                                      xudu::provenanceFileName));
  EXPECT_TRUE(std::filesystem::exists(tempDir / "pub" / "diagram.png" /
                                      xudu::provenanceSigName));

  // 5. Test AuthorCatalog for transcopyright & published body of work tracking
  xudu::AuthorCatalog catalog;
  catalog.recordWork(compound.mainSeal.hash.hex(),
                     compound.mainSeal.provenance);
  catalog.recordWork(compound.mediaTorrents[0].hash.hex(),
                     compound.mediaTorrents[0].provenance);

  EXPECT_EQ(catalog.allWorks().size(), 2U);
  const auto authorWorks = catalog.worksByAuthor("Test Author");
  EXPECT_EQ(authorWorks.size(), 2U);

  const auto mediaWork =
      catalog.workForTorrent(compound.mediaTorrents[0].hash.hex());
  ASSERT_TRUE(mediaWork.has_value());
  EXPECT_EQ(mediaWork->title, "diagram.png");
  EXPECT_EQ(mediaWork->mimeType, "image/png");
  EXPECT_TRUE(mediaWork->transcopyrightPermitted);
  EXPECT_FALSE(mediaWork->contentSha256.empty());

  const auto contentLookup =
      catalog.workForContentHash(mediaWork->contentSha256);
  ASSERT_TRUE(contentLookup.has_value());
  EXPECT_EQ(contentLookup->title, "diagram.png");

  std::filesystem::remove_all(tempDir);
}

} // namespace
