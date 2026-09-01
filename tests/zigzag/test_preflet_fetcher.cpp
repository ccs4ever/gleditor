/**
 * @file test_preflet_fetcher.cpp
 * @brief Unit tests for BitTorrent PrefletFetcher.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <zigzag/core/preflet_fetcher.hpp>

namespace {

using zigzag::Preflet;
using zigzag::PrefletFetcher;

TEST(PrefletFetcherTest, LifecycleAndInitialState) {
  PrefletFetcher fetcher;
  EXPECT_FALSE(fetcher.busy());
  EXPECT_EQ(fetcher.progress().status, PrefletFetcher::Status::Idle);
  EXPECT_FLOAT_EQ(fetcher.progress().fraction, 0.0F);
  EXPECT_TRUE(fetcher.progress().slice_path.empty());
  EXPECT_TRUE(fetcher.progress().message.empty());

  // Move constructor
  PrefletFetcher moved(std::move(fetcher));
  EXPECT_FALSE(moved.busy());
  EXPECT_EQ(moved.progress().status, PrefletFetcher::Status::Idle);

  // Move assignment
  PrefletFetcher assigned;
  assigned = std::move(moved);
  EXPECT_FALSE(assigned.busy());
  EXPECT_EQ(assigned.progress().status, PrefletFetcher::Status::Idle);
}

TEST(PrefletFetcherTest, CacheRootResolution) {
  const auto root = PrefletFetcher::cacheRoot();
  EXPECT_FALSE(root.empty());
}

TEST(PrefletFetcherTest, RejectInvalidMagnetUri) {
  PrefletFetcher fetcher;
  Preflet preflet;
  preflet.resource_identifier = "invalid://not-a-magnet";

  std::string error;
  EXPECT_FALSE(fetcher.begin(preflet, error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(fetcher.busy());
}

TEST(PrefletFetcherTest, BeginValidMagnetUriPollAndCancel) {
  PrefletFetcher fetcher;
  Preflet preflet;
  preflet.resource_identifier =
      "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&"
      "dn=test_slice.yaml";
  preflet.metadata.push_back({"preferred_filename", "test_slice.yaml"});

  std::string error;
  ASSERT_TRUE(fetcher.begin(preflet, error)) << "begin failed: " << error;
  EXPECT_TRUE(fetcher.busy());
  EXPECT_EQ(fetcher.progress().status, PrefletFetcher::Status::Fetching);

  // Calling begin while busy fails
  std::string error2;
  EXPECT_FALSE(fetcher.begin(preflet, error2));
  EXPECT_FALSE(error2.empty());

  // Non-blocking poll
  fetcher.poll();

  // Cancel aborts fetch
  fetcher.cancel();
  EXPECT_FALSE(fetcher.busy());
  EXPECT_EQ(fetcher.progress().status, PrefletFetcher::Status::Idle);

  // Acknowledge resets status back to idle
  fetcher.acknowledge();
  EXPECT_EQ(fetcher.progress().status, PrefletFetcher::Status::Idle);
}

} // namespace
