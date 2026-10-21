#pragma once

#include <cstdint>
#include <vector>

#include "activities/ActivityResult.h"
#include "activities/reader/WordRef.h"

namespace ClipTextBuilder {

struct AllocationBudget {
  size_t textCapacity = 0;
  size_t temporaryBytes = 0;

  bool fits(size_t freeBytes, size_t largestBlockBytes) const {
    return largestBlockBytes > textCapacity && freeBytes > textCapacity + temporaryBytes + 1024;
  }
};

AllocationBudget allocationBudget(const ClipWordStore& wordStore, const uint16_t* wordOrder, int fromOrder, int toOrder,
                                  uint16_t tableSelection = UINT16_MAX);

struct SelectionBounds {
  uint8_t firstPageIdx = 0;
  uint16_t firstPageWordOrdinal = 0;
  uint16_t firstWordByteOffset = 0;
  uint8_t lastPageIdx = 0;
  uint16_t lastPageWordOrdinal = 0;
  uint16_t lastWordByteEndOffset = 0;
};

// A punctuation-only run glued to a word (a closing ?" in its own text node) is a
// separate page word that dictionary selection cannot select; widen the range over it.
// Then drop a trailing ? (and what follows it) unless the selection is a sentence, and a
// double quote at either end unless the selection holds another one.
void fitBoundsToPunctuation(const ClipWordStore& wordStore, const uint16_t* wordOrder, int orderCount, int& fromOrder,
                            int& toOrder, SelectionBounds& bounds);

ClippingResult build(const ClipWordStore& wordStore, const uint16_t* wordOrder, int fromOrder, int toOrder,
                     int startPageInSection, int sectionPageCount, const SelectionBounds* selectionBounds = nullptr,
                     uint16_t tableSelection = UINT16_MAX);

}  // namespace ClipTextBuilder
