#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "ProgressRecord.h"

using namespace progress_record;

namespace {

Fields position(const uint16_t spine, const uint16_t page, const uint16_t count = 40) {
  Fields fields;
  fields.spineIndex = spine;
  fields.pageNumber = page;
  fields.pageCount = count;
  fields.hasPageCount = true;
  return fields;
}

Slot versioned(const Fields& fields, const uint32_t seq) {
  uint8_t bytes[RECORD_BYTES];
  encode(fields, seq, bytes);
  return decode(bytes, sizeof(bytes));
}

Slot legacy(const uint16_t spine, const uint16_t page) {
  const uint8_t bytes[6] = {static_cast<uint8_t>(spine), 0, static_cast<uint8_t>(page), 0, 40, 0};
  return decode(bytes, sizeof(bytes));
}

}  // namespace

TEST(ProgressRecordTest, RoundTripsWithAndWithoutVisibleTextOffset) {
  Fields withOffset = position(3, 7, 21);
  withOffset.hasVisibleTextOffset = true;
  withOffset.visibleTextOffset = 0x01020304;
  const Slot a = versioned(withOffset, 9);
  ASSERT_EQ(a.kind, SlotKind::Versioned);
  EXPECT_EQ(a.seq, 9u);
  EXPECT_TRUE(sameFields(a.fields, withOffset));

  const Slot b = versioned(position(3, 7, 21), 10);
  ASSERT_EQ(b.kind, SlotKind::Versioned);
  EXPECT_FALSE(b.fields.hasVisibleTextOffset);
  EXPECT_FALSE(sameFields(a.fields, b.fields));
}

TEST(ProgressRecordTest, FirstTenBytesStayReadableByOlderFirmware) {
  Fields fields = position(0x0102, 0x0304, 0x0506);
  fields.hasVisibleTextOffset = true;
  fields.visibleTextOffset = 0x0708090A;
  uint8_t bytes[RECORD_BYTES];
  encode(fields, 1, bytes);
  // Older firmware reads at most 10 bytes and parses them as the legacy record.
  const Slot asLegacy = decode(bytes, 10);
  ASSERT_EQ(asLegacy.kind, SlotKind::Legacy);
  EXPECT_TRUE(sameFields(asLegacy.fields, fields));
}

TEST(ProgressRecordTest, TornOrCorruptRecordIsInvalid) {
  uint8_t bytes[RECORD_BYTES];
  encode(position(1, 2), 5, bytes);
  bytes[2] ^= 0x40;
  EXPECT_EQ(decode(bytes, sizeof(bytes)).kind, SlotKind::Invalid);

  encode(position(1, 2), 5, bytes);
  bytes[10] = 0;
  EXPECT_EQ(decode(bytes, sizeof(bytes)).kind, SlotKind::Invalid);

  encode(position(1, 2), 5, bytes);
  EXPECT_EQ(decode(bytes, 12).kind, SlotKind::Invalid);
}

TEST(ProgressRecordTest, LegacySizesDecode) {
  const uint8_t bytes[10] = {1, 0, 0xFF, 0xFF, 9, 0, 4, 0, 0, 0};
  const Slot four = decode(bytes, 4);
  ASSERT_EQ(four.kind, SlotKind::Legacy);
  EXPECT_FALSE(four.fields.hasPageCount);
  const Slot ten = decode(bytes, 10);
  ASSERT_EQ(ten.kind, SlotKind::Legacy);
  EXPECT_TRUE(ten.fields.hasVisibleTextOffset);
  EXPECT_EQ(ten.fields.visibleTextOffset, 4u);
  EXPECT_EQ(decode(bytes, 5).kind, SlotKind::Invalid);
}

TEST(ProgressRecordTest, SavesAlternateSlotsAndNewestWins) {
  Slot primary;
  Slot backup;
  // Fresh book: progress.bin first.
  ASSERT_EQ(target(primary, backup), PRIMARY);
  primary = versioned(position(1, 1), nextSeq(primary, backup));
  EXPECT_EQ(newest(primary, backup), PRIMARY);

  ASSERT_EQ(target(primary, backup), BACKUP);
  backup = versioned(position(1, 11), nextSeq(primary, backup));
  EXPECT_EQ(newest(primary, backup), BACKUP);

  ASSERT_EQ(target(primary, backup), PRIMARY);
  primary = versioned(position(1, 21), nextSeq(primary, backup));
  EXPECT_EQ(newest(primary, backup), PRIMARY);
  EXPECT_EQ(primary.fields.pageNumber, 21);
}

TEST(ProgressRecordTest, TornSaveFallsBackToThePreviousSave) {
  const Slot primary = versioned(position(1, 1), 4);
  const Slot backup = versioned(position(1, 11), 5);
  // The next save goes to progress.bin (older); power fails mid-write.
  ASSERT_EQ(target(primary, backup), PRIMARY);
  const Slot torn;
  ASSERT_EQ(newest(torn, backup), BACKUP);
  EXPECT_EQ(backup.fields.pageNumber, 11);
  // The retry rewrites the damaged slot with a newer sequence number.
  EXPECT_EQ(target(torn, backup), PRIMARY);
  EXPECT_EQ(nextSeq(torn, backup), 6u);
}

TEST(ProgressRecordTest, UpgradeFromLegacyNeverShadowsNewSaves) {
  // Before upgrade: both files legacy; progress.bin is current.
  Slot primary = legacy(2, 30);
  Slot backup = legacy(2, 20);
  ASSERT_EQ(newest(primary, backup), PRIMARY);
  // The first save replaces progress.bin itself, not the backup.
  ASSERT_EQ(target(primary, backup), PRIMARY);
  primary = versioned(position(2, 40), nextSeq(primary, backup));
  EXPECT_EQ(newest(primary, backup), PRIMARY);
  ASSERT_EQ(target(primary, backup), BACKUP);
  backup = versioned(position(2, 50), nextSeq(primary, backup));
  EXPECT_EQ(newest(primary, backup), BACKUP);
}

TEST(ProgressRecordTest, DowngradeThenUpgradeKeepsOlderFirmwaresSave) {
  // Older firmware rotated a slot record into .bak and wrote a legacy progress.bin.
  Slot primary = legacy(4, 9);
  Slot backup = versioned(position(4, 2), 77);
  EXPECT_EQ(newest(primary, backup), PRIMARY);
  ASSERT_EQ(target(primary, backup), PRIMARY);
  EXPECT_EQ(nextSeq(primary, backup), 78u);
}

TEST(ProgressRecordTest, SequenceComparisonSurvivesWrapAround) {
  const Slot primary = versioned(position(1, 1), 0xFFFFFFFFu);
  const Slot backup = versioned(position(1, 2), 0);
  EXPECT_EQ(newest(primary, backup), BACKUP);
  EXPECT_EQ(target(primary, backup), PRIMARY);
  EXPECT_EQ(nextSeq(primary, backup), 1u);
}

TEST(ProgressRecordTest, CrcMatchesIeeeCheckValue) {
  const char* check = "123456789";
  EXPECT_EQ(crc32(reinterpret_cast<const uint8_t*>(check), strlen(check)), 0xCBF43926u);
}
