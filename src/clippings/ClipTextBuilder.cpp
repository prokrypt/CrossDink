#include "ClipTextBuilder.h"

#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace ClipTextBuilder {
namespace {

bool hasEmSpace(const char* word) { return word[0] == '\xe2' && word[1] == '\x80' && word[2] == '\x83'; }

bool isUtf8SpaceAt(const std::string& text, const size_t index, size_t& advance) {
  const auto c = static_cast<unsigned char>(text[index]);
  if (c == 0xC2 && index + 1 < text.size() && static_cast<unsigned char>(text[index + 1]) == 0xA0) {
    advance = 2;
    return true;
  }
  if (c == 0xE2 && index + 2 < text.size() && static_cast<unsigned char>(text[index + 1]) == 0x80) {
    const auto c2 = static_cast<unsigned char>(text[index + 2]);
    if (c2 == 0x83 || c2 == 0xAF) {
      advance = 3;
      return true;
    }
  }
  return false;
}

std::string cleanWordText(const std::string& word) {
  std::string out;
  out.reserve(word.size());
  for (size_t i = 0; i < word.size();) {
    size_t advance = 0;
    if (isUtf8SpaceAt(word, i, advance)) {
      if (!out.empty() && out.back() != ' ') {
        out += ' ';
      }
      i += advance;
      continue;
    }
    const char c = word[i++];
    if (c == '\r' || c == '\n' || c == '\t') {
      if (!out.empty() && out.back() != ' ') {
        out += ' ';
      }
      continue;
    }
    out += c;
  }

  while (!out.empty() && out.front() == ' ') {
    out.erase(out.begin());
  }
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  return out;
}

std::string stripTrailingInsertedHyphen(std::string word, const bool insertedHyphen) {
  if (insertedHyphen && !word.empty() && word.back() == '-') {
    word.pop_back();
  }
  return word;
}

bool areWordsVisuallyAttached(const WordRef& previousWord, const WordRef& word) {
  if (word.pageIdx != previousWord.pageIdx || word.y != previousWord.y) return false;

  if (word.x >= previousWord.x) {
    return word.x <= previousWord.x + previousWord.w + 2;
  }
  return previousWord.x <= word.x + word.w + 2;
}

std::string selectedWordText(const ClipWordStore& wordStore, const WordRef& word,
                             const SelectionBounds* selectionBounds) {
  const bool isFirstBound = selectionBounds && word.pageIdx == selectionBounds->firstPageIdx &&
                            word.pageWordIndex == selectionBounds->firstPageWordOrdinal;
  const bool isLastBound = selectionBounds && word.pageIdx == selectionBounds->lastPageIdx &&
                           word.pageWordIndex == selectionBounds->lastPageWordOrdinal;
  if (!selectionBounds || (!isFirstBound && !isLastBound)) {
    return cleanWordText(wordStore.text(word));
  }

  const size_t textLength = word.textLength;
  const size_t begin = isFirstBound ? std::min<size_t>(selectionBounds->firstWordByteOffset, textLength) : 0;
  const size_t end = isLastBound ? std::min<size_t>(selectionBounds->lastWordByteEndOffset, textLength) : textLength;
  if (end < begin) return {};
  return cleanWordText(std::string(wordStore.text(word) + begin, end - begin));
}

bool isSelectedTableWord(const WordRef& word, const uint16_t tableSelection) {
  return tableSelection == UINT16_MAX || word.tableSelection == tableSelection;
}

bool isQuoteAt(const char* text) {
  return text[0] == '\xE2' && text[1] == '\x80' && (text[2] == '\x9C' || text[2] == '\x9D');
}

// Byte length of a double quote (" or a curly one) ending at text[end - 1], else 0.
size_t quoteLengthBefore(const char* text, const size_t end) {
  if (end >= 1 && text[end - 1] == '"') return 1;
  return end >= 3 && isQuoteAt(text + end - 3) ? 3 : 0;
}

size_t quoteLengthAt(const char* text, const size_t begin, const size_t end) {
  if (begin < end && text[begin] == '"') return 1;
  return begin + 3 <= end && isQuoteAt(text + begin) ? 3 : 0;
}

// A sentence starts with an uppercase letter (after any opening quotes) at a word start, right after
// .?! or the start of the loaded text. shortcut: ASCII capitals only, upgrade for non-Latin scripts.
bool isSentenceStart(const ClipWordStore& wordStore, const uint16_t* wordOrder, const int fromOrder, const int toOrder,
                     const size_t firstOffset) {
  const auto& words = wordStore.words;
  if (firstOffset != 0) return false;
  if (fromOrder > 0) {
    const WordRef& previous = words[wordOrder[fromOrder - 1]];
    const char* text = wordStore.text(previous);
    size_t end = previous.textLength;
    while (const size_t q = quoteLengthBefore(text, end)) end -= q;
    if (end == 0 || !strchr(".?!", text[end - 1])) return false;
  }
  for (int order = fromOrder; order <= toOrder; ++order) {
    const WordRef& word = words[wordOrder[order]];
    const char* text = wordStore.text(word);
    size_t i = 0;
    while (const size_t q = quoteLengthAt(text, i, word.textLength)) i += q;
    if (i < word.textLength) return text[i] >= 'A' && text[i] <= 'Z';
  }
  return false;
}

}  // namespace

void fitBoundsToPunctuation(const ClipWordStore& wordStore, const uint16_t* wordOrder, const int orderCount,
                            int& fromOrder, int& toOrder, SelectionBounds& bounds) {
  const auto& words = wordStore.words;
  const auto isAttachedPunctuation = [&](const WordRef& anchor, const WordRef& word) {
    return areWordsVisuallyAttached(anchor, word) && !utf8ContainsLookupCharacter(wordStore.text(word));
  };
  if (bounds.lastWordByteEndOffset >= words[wordOrder[toOrder]].textLength) {
    while (toOrder + 1 < orderCount &&
           isAttachedPunctuation(words[wordOrder[toOrder]], words[wordOrder[toOrder + 1]])) {
      const WordRef& word = words[wordOrder[++toOrder]];
      bounds.lastPageIdx = static_cast<uint8_t>(word.pageIdx);
      bounds.lastPageWordOrdinal = word.pageWordIndex;
      bounds.lastWordByteEndOffset = word.textLength;
    }
  }
  if (bounds.firstWordByteOffset == 0) {
    while (fromOrder > 0 && isAttachedPunctuation(words[wordOrder[fromOrder]], words[wordOrder[fromOrder - 1]])) {
      const WordRef& word = words[wordOrder[--fromOrder]];
      bounds.firstPageIdx = static_cast<uint8_t>(word.pageIdx);
      bounds.firstPageWordOrdinal = word.pageWordIndex;
    }
  }

  // Text after `?` stays only when the selection is a sentence; a double quote at
  // either end stays only when the selection holds another one.
  if (bounds.lastWordByteEndOffset > words[wordOrder[toOrder]].textLength) {
    bounds.lastWordByteEndOffset = words[wordOrder[toOrder]].textLength;
  }
  const auto setLast = [&](const int order, const size_t end) {
    const WordRef& word = words[wordOrder[order]];
    toOrder = order;
    bounds.lastPageIdx = static_cast<uint8_t>(word.pageIdx);
    bounds.lastPageWordOrdinal = word.pageWordIndex;
    bounds.lastWordByteEndOffset = static_cast<uint16_t>(end);
  };
  // Widened words are punctuation-only, so a trim that empties one steps back to the full previous word.
  const auto trimLastTo = [&](const size_t end) {
    if (end == 0 && toOrder > fromOrder) {
      setLast(toOrder - 1, words[wordOrder[toOrder - 1]].textLength);
    } else {
      bounds.lastWordByteEndOffset = static_cast<uint16_t>(end);
    }
  };

  if (!isSentenceStart(wordStore, wordOrder, fromOrder, toOrder, bounds.firstWordByteOffset)) {
    // Drop the first `?` of the trailing ?/quote run and everything after it.
    for (bool trimmed = true; trimmed;) {
      trimmed = false;
      const char* text = wordStore.text(words[wordOrder[toOrder]]);
      const size_t end = bounds.lastWordByteEndOffset;
      size_t i = end;
      size_t questionAt = SIZE_MAX;
      while (i > 0) {
        if (text[i - 1] == '?') {
          questionAt = --i;
        } else if (const size_t q = quoteLengthBefore(text, i)) {
          i -= q;
        } else {
          break;
        }
      }
      if (questionAt != SIZE_MAX) {
        const int before = toOrder;
        trimLastTo(questionAt);
        trimmed = toOrder != before;  // stepped back: the previous word may end in ? too
      }
    }
  }

  int quotes = 0;
  for (int order = fromOrder; order <= toOrder; ++order) {
    const WordRef& word = words[wordOrder[order]];
    const char* text = wordStore.text(word);
    const size_t begin = order == fromOrder ? std::min<size_t>(bounds.firstWordByteOffset, word.textLength) : 0;
    const size_t end = order == toOrder ? bounds.lastWordByteEndOffset : word.textLength;
    for (size_t i = end; i > begin;) {
      if (const size_t q = quoteLengthBefore(text, i); q && i - q >= begin) {
        ++quotes;
        i -= q;
      } else {
        --i;
      }
    }
  }
  if (quotes >= 2) return;

  if (const size_t q = quoteLengthBefore(wordStore.text(words[wordOrder[toOrder]]), bounds.lastWordByteEndOffset)) {
    if (fromOrder < toOrder || bounds.lastWordByteEndOffset - q >= bounds.firstWordByteOffset) {
      trimLastTo(bounds.lastWordByteEndOffset - q);
      return;
    }
  }
  const WordRef& first = words[wordOrder[fromOrder]];
  if (const size_t q = quoteLengthAt(wordStore.text(first), bounds.firstWordByteOffset, first.textLength)) {
    if (bounds.firstWordByteOffset + q >= first.textLength && fromOrder < toOrder) {
      const WordRef& next = words[wordOrder[++fromOrder]];
      bounds.firstPageIdx = static_cast<uint8_t>(next.pageIdx);
      bounds.firstPageWordOrdinal = next.pageWordIndex;
      bounds.firstWordByteOffset = 0;
    } else {
      bounds.firstWordByteOffset = static_cast<uint16_t>(bounds.firstWordByteOffset + q);
    }
  }
}

ClippingResult build(const ClipWordStore& wordStore, const uint16_t* wordOrder, const int fromOrder, const int toOrder,
                     const int startPageInSection, const int sectionPageCount, const SelectionBounds* selectionBounds,
                     const uint16_t tableSelection) {
  const auto& words = wordStore.words;
  std::string text;
  text.reserve(256);

  const WordRef& firstWord = words[wordOrder[fromOrder]];
  const WordRef& lastWord = words[wordOrder[toOrder]];
  uint16_t startPageWordIndex = firstWord.pageWordIndex;
  uint16_t endPageWordIndex = lastWord.pageWordIndex;
  uint16_t selectedWordCount = 0;
  for (int orderIdx = fromOrder; orderIdx <= toOrder; ++orderIdx) {
    const WordRef& word = words[wordOrder[orderIdx]];
    if (!isSelectedTableWord(word, tableSelection)) continue;
    selectedWordCount++;
    if (word.pageIdx == firstWord.pageIdx) {
      startPageWordIndex = std::min(startPageWordIndex, word.pageWordIndex);
    }
    if (word.pageIdx == lastWord.pageIdx) {
      endPageWordIndex = std::max(endPageWordIndex, word.pageWordIndex);
    }
  }

  const WordRef* previousWord = nullptr;
  std::string previousClean;

  for (int orderIdx = fromOrder; orderIdx <= toOrder; ++orderIdx) {
    const WordRef& word = words[wordOrder[orderIdx]];
    if (!isSelectedTableWord(word, tableSelection)) continue;
    const auto cleanText = selectedWordText(wordStore, word, selectionBounds);
    const auto wordText = stripTrailingInsertedHyphen(cleanText, word.endsWithInsertedHyphen);
    if (wordText.empty()) {
      previousWord = &word;
      previousClean = cleanText;
      continue;
    }
    const bool joinsInsertedHyphen =
        previousWord && previousWord->endsWithInsertedHyphen && !previousClean.empty() && previousClean.back() == '-';
    if (joinsInsertedHyphen) {
      text += wordText;
      previousWord = &word;
      previousClean = cleanText;
      continue;
    }

    const bool yGap =
        previousWord && word.pageIdx == previousWord->pageIdx && word.y > previousWord->y + previousWord->h;
    const bool paragraphStart = previousWord && (hasEmSpace(wordStore.text(word)) || word.paragraphStart || yGap);

    if (previousWord && !text.empty() && !paragraphStart) {
      if (!previousClean.empty() && previousClean.back() == '-' &&
          !std::isspace(static_cast<unsigned char>(wordText[0])) &&
          !std::ispunct(static_cast<unsigned char>(wordText[0]))) {
        text += wordText;
        previousWord = &word;
        previousClean = cleanText;
        continue;
      }
    }

    if (paragraphStart) {
      text += '\n';
    } else if (!text.empty()) {
      const bool attached = previousWord && areWordsVisuallyAttached(*previousWord, word);
      if (!attached) {
        text += ' ';
      }
    }
    text += wordText;

    previousWord = &word;
    previousClean = cleanText;
  }

  return ClippingResult{std::move(text),
                        static_cast<uint16_t>(startPageInSection + firstWord.pageIdx),
                        static_cast<uint16_t>(startPageInSection + lastWord.pageIdx),
                        static_cast<uint16_t>(std::max(1, sectionPageCount)),
                        startPageWordIndex,
                        endPageWordIndex,
                        UINT16_MAX,
                        tableSelection,
                        selectedWordCount};
}

}  // namespace ClipTextBuilder
