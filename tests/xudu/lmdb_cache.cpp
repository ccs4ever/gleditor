#include "../../apps/xudu/core/lmdb_cache.hpp"
#include <filesystem>
#include <gtest/gtest.h>
#include <string>

namespace xudu {

class LMDBContentCacheTest : public ::testing::Test {
protected:
  std::filesystem::path cache_dir = "test_cache";

  void SetUp() override {
    if (std::filesystem::exists(cache_dir)) {
      std::filesystem::remove_all(cache_dir);
    }
  }

  void TearDown() override {
    if (std::filesystem::exists(cache_dir)) {
      std::filesystem::remove_all(cache_dir);
    }
  }
};

TEST_F(LMDBContentCacheTest, PutAndGetXanadocSpan) {
  LMDBContentCache cache(cache_dir);
  const PrimediaSpan span{1, 100, 11};
  const std::string text = "hello world";

  cache.put(span, text);

  std::string cached_text;
  EXPECT_TRUE(cache.get(span, cached_text));
  EXPECT_EQ(text, cached_text);
}

TEST_F(LMDBContentCacheTest, PutAndGetExternalTorrentSpan) {
  LMDBContentCache cache(cache_dir);
  const auto hash =
      InfoHash::fromHex("0123456789abcdef0123456789abcdef01234567");
  const std::string text = "the quick brown fox jumps over the lazy dog";

  cache.put_external(hash, 50, text);

  std::string cached_text;
  EXPECT_TRUE(cache.get_external(hash, 50, text.size(), cached_text));
  EXPECT_EQ(text, cached_text);
}

TEST_F(LMDBContentCacheTest, SubspanSharesPieceWithoutDuplication) {
  LMDBContentCache cache(cache_dir);
  const PrimediaSpan parent{1, 0, 11};
  const std::string full_text = "hello world";

  cache.put(parent, full_text);

  // Extract backing piece hash and index from parent virtual record
  VSpanRecord vrec{};
  ASSERT_TRUE(cache.get_vspan(parent, vrec));
  InfoHash piece_hash{};
  piece_hash.bytes = vrec.hash;

  EXPECT_EQ(cache.ref_count(piece_hash, vrec.piece_index), 1U);

  // Create subspan [6, 11) -> "world"
  const PrimediaSpan subspan{1, 6, 5};
  EXPECT_TRUE(cache.put_subspan(subspan, parent));

  // Refcount should be incremented to 2
  EXPECT_EQ(cache.ref_count(piece_hash, vrec.piece_index), 2U);

  // Retrieve subspan: should return sliced "world"
  std::string sub_text;
  EXPECT_TRUE(cache.get(subspan, sub_text));
  EXPECT_EQ(sub_text, "world");

  // Retrieve parent: should still be "hello world"
  std::string parent_text;
  EXPECT_TRUE(cache.get(parent, parent_text));
  EXPECT_EQ(parent_text, "hello world");
}

TEST_F(LMDBContentCacheTest, SharedPieceDeduplicationAcrossXanadocAndExternal) {
  LMDBContentCache cache(cache_dir);
  const auto hash =
      InfoHash::fromHex("abcdef0123456789abcdef0123456789abcdef01");
  const std::string piece_data = "CHAPTER 1: The Beginning of Transclusion";

  // 1. Store raw verified piece with initial ref_count = 0
  EXPECT_TRUE(cache.put_piece(hash, 0, piece_data, 0));
  EXPECT_EQ(cache.ref_count(hash, 0), 0U);

  // 2. Link Xanadoc span
  const PrimediaSpan xanadoc_span{42, 100, 9};
  EXPECT_TRUE(cache.put_vspan(xanadoc_span, hash, 0, 11, 9)); // "The Begin"

  // 3. Link External span
  EXPECT_TRUE(cache.put_ext_span(hash, 0, 9, 0, 0)); // "CHAPTER 1"

  // 4. Retrieve Xanadoc span slice
  VSpanRecord x_rec{};
  EXPECT_TRUE(cache.get_vspan(xanadoc_span, x_rec));
  std::string piece_str;
  EXPECT_TRUE(cache.get_piece(hash, 0, piece_str));
  EXPECT_EQ(piece_str.substr(x_rec.chunk_offset, x_rec.length), "The Begin");

  // 5. Retrieve External span slice
  VSpanRecord ext_rec{};
  EXPECT_TRUE(cache.get_ext_span(hash, 0, 9, ext_rec));
  EXPECT_EQ(piece_str.substr(ext_rec.chunk_offset, ext_rec.length),
            "CHAPTER 1");
}

TEST_F(LMDBContentCacheTest, ReferenceCountedEvictionOnZeroRefs) {
  LMDBContentCache cache(cache_dir);
  const PrimediaSpan parent{1, 0, 11};
  const PrimediaSpan subspan{1, 6, 5};
  const std::string full_text = "hello world";

  cache.put(parent, full_text);
  EXPECT_TRUE(cache.put_subspan(subspan, parent));

  VSpanRecord vrec{};
  ASSERT_TRUE(cache.get_vspan(parent, vrec));
  InfoHash piece_hash{};
  piece_hash.bytes = vrec.hash;

  EXPECT_EQ(cache.ref_count(piece_hash, vrec.piece_index), 2U);

  // Erase parent span -> ref_count drops to 1, piece still exists
  EXPECT_TRUE(cache.erase(parent));
  EXPECT_EQ(cache.ref_count(piece_hash, vrec.piece_index), 1U);

  std::string sub_text;
  EXPECT_TRUE(cache.get(subspan, sub_text));
  EXPECT_EQ(sub_text, "world");

  // Erase subspan -> ref_count drops to 0, piece is automatically evicted
  EXPECT_TRUE(cache.erase(subspan));
  EXPECT_EQ(cache.ref_count(piece_hash, vrec.piece_index), 0U);

  std::string dead_piece;
  EXPECT_FALSE(cache.get_piece(piece_hash, vrec.piece_index, dead_piece));
}

TEST_F(LMDBContentCacheTest, GarbageCollectionSweep) {
  LMDBContentCache cache(cache_dir);
  const auto hash1 =
      InfoHash::fromHex("1111111111111111111111111111111111111111");
  const auto hash2 =
      InfoHash::fromHex("2222222222222222222222222222222222222222");

  // Store one active piece (refcount 1) and one orphaned piece (refcount 0)
  EXPECT_TRUE(cache.put_piece(hash1, 0, "active content", 1));
  EXPECT_TRUE(cache.put_piece(hash2, 0, "orphaned content", 0));

  // Sweep should evict 1 piece
  EXPECT_EQ(cache.evict_unreferenced(), 1U);

  std::string out;
  EXPECT_TRUE(cache.get_piece(hash1, 0, out));
  EXPECT_FALSE(cache.get_piece(hash2, 0, out));
}

TEST_F(LMDBContentCacheTest, GetNonExistentAndEmpty) {
  LMDBContentCache cache(cache_dir);
  const PrimediaSpan span{1, 100, 50};

  std::string cached_text;
  EXPECT_FALSE(cache.get(span, cached_text));

  cache.put(span, "");
  EXPECT_FALSE(cache.get(span, cached_text));
}

} // namespace xudu
