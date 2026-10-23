#pragma once

#include <EpdFontFamily.h>

#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace BidiUtils {
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}

class GfxRenderer {
 public:
  inline static bool loanActive = false;
  inline static bool fileProbeHadLoan = false;
  uint32_t loans = 0;
  bool hasFrameBuffer() const { return !loanActive; }
  uint32_t frameBufferLoanCount() const { return loans; }
  class FrameBufferLoan {
   public:
    explicit FrameBufferLoan(GfxRenderer& renderer) : active_(!loanActive) {
      if (active_) {
        loanActive = true;
        ++renderer.loans;
      }
    }
    ~FrameBufferLoan() { end(); }
    void end() {
      if (active_) {
        loanActive = false;
        active_ = false;
      }
    }

   private:
    bool active_;
  };
  int textAdvancePerChar = 0;
  int getFontAscenderSize(int) const { return 12; }
  int getLineHeight(int) const { return 16; }
  int getTextWidth(int, const char*, EpdFontFamily::Style = EpdFontFamily::REGULAR) const { return 0; }
  int getTextAdvanceX(int, const char* text, EpdFontFamily::Style, uint32_t = 0, int8_t = 0) const {
    return static_cast<int>(std::strlen(text)) * textAdvancePerChar;
  }
  int getSpaceWidth(int, EpdFontFamily::Style) const { return 0; }
  int getKerning(int, uint32_t, uint32_t, EpdFontFamily::Style, int8_t = 0) const { return 0; }
  int getSpaceAdvance(int, uint32_t, uint32_t, EpdFontFamily::Style) const { return 0; }
  bool isSdCardFont(int) const { return false; }
  bool releaseSdCardFontForLowMemory(int, bool = false) { return false; }
  // Codepoint batches handed to the font, for the section advance prewarm tests.
  mutable std::vector<std::vector<uint32_t>> fedCodepoints;
  bool ensureSdCardFontReady(int, const uint32_t* codepoints, size_t count, bool, bool, uint8_t) const {
    fedCodepoints.emplace_back(codepoints, codepoints + count);
    return true;
  }
  bool ensureSdCardFontReady(int, const std::deque<std::string>&, bool, uint8_t) const { return true; }
  bool ensureSdCardFontReady(int, const char*, uint8_t) const { return true; }
  std::vector<std::string> wrappedText(int, const char*, int, int,
                                       EpdFontFamily::Style = EpdFontFamily::REGULAR) const {
    return {};
  }
};
