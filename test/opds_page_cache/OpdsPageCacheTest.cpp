#include <OpdsPageCache.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

// Native builds have no PSRAM pool, so tests back pages with internal memory.
OpdsPageBuffer makePage(const size_t bytes, const size_t maxBytes = 1024 * 1024) {
  OpdsPageBuffer page(MemoryPool::Internal, maxBytes);
  const std::vector<uint8_t> data(bytes, 'x');
  EXPECT_TRUE(page.append(data.data(), data.size()));
  return page;
}

}  // namespace

TEST(OpdsPageBufferTest, GrowsAcrossChunksAndKeepsBytes) {
  OpdsPageBuffer page(MemoryPool::Internal, 1024 * 1024);
  std::string expected;
  for (int i = 0; i < 100; ++i) {
    const std::string chunk = std::to_string(i) + std::string(1000, static_cast<char>('a' + i % 26));
    ASSERT_TRUE(page.append(reinterpret_cast<const uint8_t*>(chunk.data()), chunk.size()));
    expected += chunk;
  }
  ASSERT_EQ(page.size(), expected.size());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(page.data()), page.size()), expected);
}

TEST(OpdsPageBufferTest, OverflowFailsAndReleases) {
  OpdsPageBuffer page(MemoryPool::Internal, 100);
  const std::vector<uint8_t> data(60, 'x');
  ASSERT_TRUE(page.append(data.data(), data.size()));
  EXPECT_FALSE(page.append(data.data(), data.size()));
  EXPECT_TRUE(page.failed());
  EXPECT_EQ(page.size(), 0u);
  EXPECT_EQ(page.data(), nullptr);
  EXPECT_FALSE(page.append(data.data(), 1));
}

TEST(OpdsPageCacheTest, StoresAndFindsByUrl) {
  OpdsPageCache cache(64 * 1024);
  ASSERT_TRUE(cache.store("https://a/opds", makePage(1000)));
  const OpdsPageBuffer* found = cache.find("https://a/opds");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->size(), 1000u);
  EXPECT_EQ(cache.find("https://a/other"), nullptr);
  EXPECT_EQ(cache.bytesUsed(), 1000u);
}

TEST(OpdsPageCacheTest, EvictsLeastRecentlyUsedToFitBudget) {
  OpdsPageCache cache(3000);
  ASSERT_TRUE(cache.store("p1", makePage(1000)));
  ASSERT_TRUE(cache.store("p2", makePage(1000)));
  ASSERT_TRUE(cache.store("p3", makePage(1000)));
  ASSERT_NE(cache.find("p1"), nullptr);  // p2 is now least recently used
  ASSERT_TRUE(cache.store("p4", makePage(1000)));
  EXPECT_TRUE(cache.contains("p1"));
  EXPECT_FALSE(cache.contains("p2"));
  EXPECT_TRUE(cache.contains("p3"));
  EXPECT_TRUE(cache.contains("p4"));
  EXPECT_EQ(cache.bytesUsed(), 3000u);
}

TEST(OpdsPageCacheTest, EvictsWhenSlotsRunOut) {
  OpdsPageCache cache(1024 * 1024);
  for (size_t i = 0; i <= OpdsPageCache::MAX_PAGES; ++i) {
    ASSERT_TRUE(cache.store("p" + std::to_string(i), makePage(10)));
  }
  EXPECT_EQ(cache.pageCount(), OpdsPageCache::MAX_PAGES);
  EXPECT_FALSE(cache.contains("p0"));
  EXPECT_TRUE(cache.contains("p" + std::to_string(OpdsPageCache::MAX_PAGES)));
}

TEST(OpdsPageCacheTest, ReplacesSameUrlAndRejectsOversized) {
  OpdsPageCache cache(2000);
  ASSERT_TRUE(cache.store("p", makePage(500)));
  ASSERT_TRUE(cache.store("p", makePage(700)));
  EXPECT_EQ(cache.pageCount(), 1u);
  EXPECT_EQ(cache.bytesUsed(), 700u);
  EXPECT_FALSE(cache.store("big", makePage(2500)));
  EXPECT_TRUE(cache.contains("p"));
  cache.erase("p");
  EXPECT_EQ(cache.pageCount(), 0u);
  EXPECT_EQ(cache.bytesUsed(), 0u);
}

namespace {
OpdsPageBuffer pageOf(const std::string& body) {
  OpdsPageBuffer page(MemoryPool::Internal, 1024 * 1024);
  EXPECT_TRUE(page.append(reinterpret_cast<const uint8_t*>(body.data()), body.size()));
  return page;
}
}  // namespace

TEST(OpdsPageCacheTest, SameFeedIgnoresUpdatedStamps) {
  const auto a = pageOf("<feed><updated>2026-09-30T20:00:00Z</updated><entry><title>A</title></entry></feed>");
  const auto b = pageOf("<feed><updated>2026-09-30T20:05:31+00:00</updated><entry><title>A</title></entry></feed>");
  const auto c = pageOf("<feed><updated>2026-09-30T20:00:00Z</updated><entry><title>B</title></entry></feed>");
  const auto d = pageOf("<feed><updated>x</updated><entry><title>A</title></entry></feed><extra/>");
  EXPECT_TRUE(OpdsPageCache::sameFeed(a, b));
  EXPECT_FALSE(OpdsPageCache::sameFeed(a, c));
  EXPECT_FALSE(OpdsPageCache::sameFeed(a, d));
  EXPECT_TRUE(OpdsPageCache::sameFeed(pageOf("x<updated>1"), pageOf("x<updated>22")));
}

TEST(OpdsPageCacheTest, IsStaleByStoreTime) {
  OpdsPageCache cache(1024 * 1024);
  EXPECT_FALSE(cache.isStale("u", 100000, 30000));
  ASSERT_TRUE(cache.store("u", makePage(10), true, 1000));
  EXPECT_FALSE(cache.isStale("u", 30999, 30000));
  EXPECT_TRUE(cache.isStale("u", 31000, 30000));
}
