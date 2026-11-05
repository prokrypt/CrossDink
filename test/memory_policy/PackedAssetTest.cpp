#include <BuildScratch.h>
#include <PackedAsset.h>
#include <gtest/gtest.h>
#include <zlib.h>

#include <cstring>
#include <vector>

namespace {
std::vector<uint8_t> sample(size_t n) {
  std::vector<uint8_t> bytes(n);
  for (size_t i = 0; i < n; ++i) bytes[i] = uint8_t((i * 131 + i / 977) & 255);
  return bytes;
}

// Raw DEFLATE, matching the generator scripts (level 9, wbits -15).
std::vector<uint8_t> deflateRaw(const std::vector<uint8_t>& in) {
  z_stream z{};
  EXPECT_EQ(deflateInit2(&z, 9, Z_DEFLATED, -15, 9, Z_DEFAULT_STRATEGY), Z_OK);
  std::vector<uint8_t> out(deflateBound(&z, in.size()));
  z.next_in = const_cast<Bytef*>(in.data());
  z.avail_in = static_cast<uInt>(in.size());
  z.next_out = out.data();
  z.avail_out = static_cast<uInt>(out.size());
  EXPECT_EQ(deflate(&z, Z_FINISH), Z_STREAM_END);
  out.resize(z.total_out);
  deflateEnd(&z);
  return out;
}
}  // namespace

struct PackedAssetTest : testing::Test {
  void SetUp() override { fakeheap::reset(); }
  void TearDown() override {
    buildscratch::reclaim();
    EXPECT_TRUE(fakeheap::live.empty());
  }
};

TEST_F(PackedAssetTest, InflatesIntoPsramWithZeroedHeadroom) {
  const auto raw = sample(70000);
  const auto packed = deflateRaw(raw);
  const PackedAsset asset{packed.data(), uint32_t(packed.size()), uint32_t(raw.size())};
  {
    HeapByteBuffer out = inflatePackedAsset(asset, "T", PackedAssetFallback::PsramOnly, 16);
    ASSERT_TRUE(out);
    EXPECT_EQ(byteBufferPool(out.get()), MemoryPool::Psram);
    for (int i = 0; i < 16; ++i) EXPECT_EQ(out[i], 0);
    EXPECT_EQ(memcmp(out.get() + 16, raw.data(), raw.size()), 0);
  }
}

TEST_F(PackedAssetTest, PsramOnlyFailsInsteadOfTakingInternalRam) {
  const auto raw = sample(4096);
  const auto packed = deflateRaw(raw);
  fakeheap::external.fail = 1;
  const size_t internalFree = fakeheap::internal.free;
  HeapByteBuffer out = inflatePackedAsset({packed.data(), uint32_t(packed.size()), uint32_t(raw.size())}, "T");
  EXPECT_FALSE(out);
  EXPECT_EQ(fakeheap::internal.free, internalFree);
}

TEST_F(PackedAssetTest, PsramThenDefaultFallsBackWhenPsramIsFull) {
  const auto raw = sample(4096);
  const auto packed = deflateRaw(raw);
  fakeheap::external.fail = 1;
  HeapByteBuffer out = inflatePackedAsset({packed.data(), uint32_t(packed.size()), uint32_t(raw.size())}, "T",
                                          PackedAssetFallback::PsramThenDefault);
  ASSERT_TRUE(out);
  EXPECT_EQ(byteBufferPool(out.get()), MemoryPool::Internal);
  EXPECT_EQ(memcmp(out.get(), raw.data(), raw.size()), 0);
}

TEST_F(PackedAssetTest, NoPsramUsesDefaultHeap) {
  fakeheap::reset(false);
  const auto raw = sample(4096);
  const auto packed = deflateRaw(raw);
  HeapByteBuffer out = inflatePackedAsset({packed.data(), uint32_t(packed.size()), uint32_t(raw.size())}, "T");
  ASSERT_TRUE(out);
  EXPECT_EQ(memcmp(out.get(), raw.data(), raw.size()), 0);
}

TEST_F(PackedAssetTest, RejectsTruncatedStream) {
  const auto raw = sample(20000);
  auto packed = deflateRaw(raw);
  packed.resize(packed.size() / 2);
  HeapByteBuffer out = inflatePackedAsset({packed.data(), uint32_t(packed.size()), uint32_t(raw.size())}, "T");
  EXPECT_FALSE(out);
}
