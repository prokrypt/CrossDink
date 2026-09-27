#include <gtest/gtest.h>

#include "ReaderProgressSaveDebouncer.h"

TEST(ReaderProgressSaveDebouncer, MetadataChangeQueuesSamePosition) {
  ReaderProgressSaveDebouncer debouncer;
  constexpr uint32_t position = (3U << 16) | 50U;

  EXPECT_FALSE(debouncer.observe(position, 100));
  debouncer.markPersisted(position, 100);
  EXPECT_FALSE(debouncer.hasPending());

  EXPECT_FALSE(debouncer.observe(position, 120));
  EXPECT_TRUE(debouncer.hasPending());
  EXPECT_EQ(debouncer.lastObservedPosition(), position);
  EXPECT_EQ(debouncer.lastObservedMetadata(), 120U);
}

TEST(ReaderProgressSaveDebouncer, StaleMetadataSaveDoesNotClearPendingState) {
  ReaderProgressSaveDebouncer debouncer;
  constexpr uint32_t position = (3U << 16) | 50U;

  debouncer.observe(position, 100);
  debouncer.markPersisted(position, 100);
  debouncer.observe(position, 120);

  debouncer.markPersisted(position, 100);
  EXPECT_TRUE(debouncer.hasPending());

  debouncer.markPersisted(position, 120);
  EXPECT_FALSE(debouncer.hasPending());
}

TEST(ReaderProgressSaveDebouncer, MetadataChangeDoesNotCountAsPageTurn) {
  ReaderProgressSaveDebouncer debouncer;
  constexpr uint32_t initialPosition = (3U << 16) | 50U;

  debouncer.observe(initialPosition, 100);
  debouncer.markPersisted(initialPosition, 100);
  EXPECT_FALSE(debouncer.observe(initialPosition, 120));

  constexpr uint32_t interval = ReaderProgressSaveDebouncer::PAGE_CHANGE_INTERVAL;
  for (uint32_t page = 51; page < 50 + interval; ++page) {
    EXPECT_FALSE(debouncer.observe((3U << 16) | page, 120));
  }
  EXPECT_TRUE(debouncer.observe((3U << 16) | (50U + interval), 120));
}

TEST(ReaderProgressSaveDebouncer, PositionOnlyCallersKeepExistingBehavior) {
  ReaderProgressSaveDebouncer debouncer;

  EXPECT_FALSE(debouncer.observe(7));
  debouncer.markPersisted(7);
  EXPECT_FALSE(debouncer.hasPending());
  EXPECT_FALSE(debouncer.observe(8));
  EXPECT_TRUE(debouncer.hasPending());
  EXPECT_EQ(debouncer.lastObservedMetadata(), 0U);
}

TEST(ReaderProgressSaveDebouncer, ShadowedReaderUsesLongerPageInterval) {
  ReaderProgressSaveDebouncer debouncer;
  debouncer.setShadowed(true);
  debouncer.observe(0);
  debouncer.markPersisted(0);

  constexpr uint32_t interval = ReaderProgressSaveDebouncer::SHADOWED_PAGE_CHANGE_INTERVAL;
  for (uint32_t page = 1; page < interval; ++page) {
    EXPECT_FALSE(debouncer.observe(page));
  }
  EXPECT_TRUE(debouncer.observe(interval));

  debouncer.markPersisted(interval);
  debouncer.setShadowed(false);
  for (uint32_t page = interval + 1; page < interval + ReaderProgressSaveDebouncer::PAGE_CHANGE_INTERVAL; ++page) {
    EXPECT_FALSE(debouncer.observe(page));
  }
  EXPECT_TRUE(debouncer.observe(interval + ReaderProgressSaveDebouncer::PAGE_CHANGE_INTERVAL));
}
