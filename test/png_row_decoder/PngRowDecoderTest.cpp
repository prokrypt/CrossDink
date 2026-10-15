#include <PngRowDecoder.h>
#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <vector>

#include "PngRowDecoderFixtures.h"

namespace {
void expectRows(PngRowDecoder& png, const PngFixture& f) {
  ASSERT_EQ(png.width(), f.width) << f.name;
  ASSERT_EQ(png.height(), f.height) << f.name;
  ASSERT_EQ(png.colorType(), f.colorType) << f.name;
  ASSERT_EQ(png.bitDepth(), f.bitDepth) << f.name;
  ASSERT_TRUE(png.begin()) << f.name;
  const uint32_t rowBytes = png.rowBytes();
  for (uint32_t y = 0; y < f.height; ++y) {
    const uint8_t* row = png.nextRow();
    ASSERT_NE(row, nullptr) << f.name << " row " << y;
    EXPECT_EQ(memcmp(row, f.rows + static_cast<size_t>(y) * rowBytes, rowBytes), 0) << f.name << " row " << y;
  }
}

std::vector<uint8_t> bytesOf(const PngFixture& f) { return std::vector<uint8_t>(f.png, f.png + f.pngSize); }
}  // namespace

TEST(PngRowDecoder, DecodesEveryColorTypeAndDepthFromMemory) {
  for (const auto& f : kPngFixtures) {
    PngRowDecoder png;
    ASSERT_TRUE(png.openMemory(f.png, f.pngSize)) << f.name;
    expectRows(png, f);
  }
}

TEST(PngRowDecoder, DecodesEveryColorTypeAndDepthFromFile) {
  for (const auto& f : kPngFixtures) {
    FsFile file(std::make_shared<HostFileData>(HostFileData{bytesOf(f)}));
    PngRowDecoder png;
    ASSERT_TRUE(png.openFile(file)) << f.name;
    expectRows(png, f);
  }
}

TEST(PngRowDecoder, ReportsPaletteAlphaAndColorKeysLikePngdec) {
  for (const auto& f : kPngFixtures) {
    PngRowDecoder png;
    ASSERT_TRUE(png.openMemory(f.png, f.pngSize));
    if (strcmp(f.name, "palette_trns") == 0) {
      ASSERT_NE(png.palette(), nullptr);
      EXPECT_TRUE(png.hasAlpha());
      EXPECT_EQ(png.paletteEntries(), 16);
      EXPECT_EQ(png.palette()[3], 3);
      EXPECT_EQ(png.palette()[768 + 1], 64);
      EXPECT_EQ(png.palette()[768 + 4], 7);
      EXPECT_EQ(png.palette()[768 + 5], 255);  // past tRNS: opaque
      EXPECT_EQ(png.transparentColor(), -1);
    } else if (strcmp(f.name, "gray_trns") == 0) {
      EXPECT_TRUE(png.hasAlpha());
      EXPECT_EQ(png.transparentColor(), 0x5A);
    } else if (strcmp(f.name, "rgb_trns") == 0) {
      EXPECT_TRUE(png.hasAlpha());
      EXPECT_EQ(png.transparentColor(), 0x112233);
    } else if (strcmp(f.name, "c0_d8") == 0) {
      EXPECT_FALSE(png.hasAlpha());
      EXPECT_EQ(png.palette(), nullptr);
    }
  }
}

TEST(PngRowDecoder, RejectsTruncatedAndCorruptInput) {
  const auto& f = kPngFixtures[0];
  PngRowDecoder notPng;
  const uint8_t junk[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  EXPECT_FALSE(notPng.openMemory(junk, sizeof(junk)));

  // Cut inside the image data: open succeeds, a later row fails.
  PngRowDecoder truncated;
  ASSERT_TRUE(truncated.openMemory(f.png, f.pngSize - 40));
  ASSERT_TRUE(truncated.begin());
  bool failed = false;
  for (uint32_t y = 0; y < f.height && !failed; ++y) failed = truncated.nextRow() == nullptr;
  EXPECT_TRUE(failed);
}

TEST(PngRowDecoder, SampleHelpersReadPackedRows) {
  const uint8_t row[] = {0b10110100, 0b01111000};
  EXPECT_EQ(PngRowDecoder::sample(row, 0, 1), 1);
  EXPECT_EQ(PngRowDecoder::sample(row, 1, 1), 0);
  EXPECT_EQ(PngRowDecoder::sample(row, 1, 2), 0b11);
  EXPECT_EQ(PngRowDecoder::sample(row, 3, 4), 0b1000);
  EXPECT_EQ(PngRowDecoder::sample(row, 1, 8), 0b01111000);
  EXPECT_EQ(PngRowDecoder::sampleToByte(1, 1), 255);
  EXPECT_EQ(PngRowDecoder::sampleToByte(2, 2), 170);
}
