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

TEST_F(LMDBContentCacheTest, PutAndGet) {
  LMDBContentCache cache(cache_dir);
  PrimediaSpan span{1, 100, 50};
  std::string text = "hello world";

  cache.put(span, text);

  std::string cached_text;
  EXPECT_TRUE(cache.get(span, cached_text));
  EXPECT_EQ(text, cached_text);
}

TEST_F(LMDBContentCacheTest, GetNonExistent) {
  LMDBContentCache cache(cache_dir);
  PrimediaSpan span{1, 100, 50};

  std::string cached_text;
  EXPECT_FALSE(cache.get(span, cached_text));
}

TEST_F(LMDBContentCacheTest, PutEmptyDoesNotCache) {
  LMDBContentCache cache(cache_dir);
  PrimediaSpan span{1, 100, 50};

  cache.put(span, "");

  std::string cached_text;
  EXPECT_FALSE(cache.get(span, cached_text));
}

} // namespace xudu
