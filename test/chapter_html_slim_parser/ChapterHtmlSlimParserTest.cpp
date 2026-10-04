#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

#define class struct
#define private public
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#undef private
#undef class

#include <Epub.h>
#include <MemoryBudget.h>

namespace {

TEST(ParagraphIndentTest, DoesNotInventIndentWithoutSourceCss) {
  GfxRenderer renderer;

  for (const bool extraParagraphSpacing : {false, true}) {
    ParsedText paragraph(extraParagraphSpacing);
    EXPECT_EQ(paragraph.resolveFirstLineIndent(true, renderer, 0), 0);
  }
}

class ChapterHtmlSlimParserTest : public ::testing::TestWithParam<const char*> {
 protected:
  std::string filepath = "unused.xhtml";
  Epub epub;
  GfxRenderer renderer;
  CssParser cssParser{"/tmp"};
  ChapterHtmlSlimParser parser{epub,  filepath, renderer, 0,  1.0f, false, false, 0, 480, 800,     false,
                               false, false,    0,        {}, true, "",    "",    0, {},  nullptr, &cssParser};
  // Size the stacks like the parse arena does; the parser only bounds-checks against these maxima.
  std::array<ChapterHtmlSlimParser::StyleStackEntry, ChapterHtmlSlimParser::MAX_INLINE_STYLE_DEPTH> inlineStyles{};
  std::array<BlockStyle, ChapterHtmlSlimParser::MAX_BLOCK_STYLE_DEPTH> blockStyles{};

  void SetUp() override {
    GfxRenderer::loanActive = false;
    GfxRenderer::fileProbeHadLoan = false;
    MemoryBudget::imageAllowed = true;
    parser.currentTextBlock = std::make_unique<ParsedText>(false);
    parser.inlineStyleBuf_ = inlineStyles.data();
    parser.blockStyleBuf_ = blockStyles.data();
    parser.blockStyleCount_ = 1;
  }

  void expectChapterFigureLayout(CssTextAlign alignment, uint16_t imageHeight = 6, bool blockDisplay = false) {
    parser.paragraphAlignment = static_cast<uint8_t>(alignment);
    cssParser.rulesBySelector_["figure.inline_group"] =
        CssParser::parseInlineStyle("margin-top: 0.5em; margin-bottom: 3em; text-align: center");
    epub.optimizerImageAvailable = true;
    epub.optimizerImageWidth = 115;
    epub.optimizerImageHeight = imageHeight;
    const XML_Char* figureAttributes[] = {"class", "inline_group", nullptr};
    const XML_Char* imageAttributes[] = {"src", "ornament.jpg", "style", blockDisplay ? "display: block" : "", nullptr};

    ChapterHtmlSlimParser::startElement(&parser, "h1", nullptr);
    ChapterHtmlSlimParser::characterData(&parser, "1", 1);
    ChapterHtmlSlimParser::endElement(&parser, "h1");
    ChapterHtmlSlimParser::startElement(&parser, "figure", figureAttributes);
    const auto figureStyle = parser.currentTextBlock->getBlockStyle();
    EXPECT_EQ(figureStyle.alignment, CssTextAlign::Center);
    EXPECT_EQ(figureStyle.marginTop, 6);
    EXPECT_EQ(figureStyle.marginBottom, 0);  // Applied when the figure closes.
    ChapterHtmlSlimParser::startElement(&parser, "img", imageAttributes);
    ChapterHtmlSlimParser::endElement(&parser, "img");
    ChapterHtmlSlimParser::endElement(&parser, "figure");
    ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
    EXPECT_EQ(parser.currentTextBlock->getBlockStyle().alignment,
              alignment == CssTextAlign::None ? CssTextAlign::Justify : alignment);
    ChapterHtmlSlimParser::characterData(&parser, "Date", 4);
    parser.flushPartWordBuffer();
    parser.makePages();

    ASSERT_NE(parser.currentPage, nullptr);
    ASSERT_EQ(parser.currentPage->elements.size(), 3u);
    ASSERT_EQ(parser.currentPage->elements[0]->getTag(), TAG_PageLine);
    ASSERT_EQ(parser.currentPage->elements[1]->getTag(), TAG_PageImage);
    ASSERT_EQ(parser.currentPage->elements[2]->getTag(), TAG_PageLine);
    const auto& heading = static_cast<const PageLine&>(*parser.currentPage->elements[0]);
    const auto& image = static_cast<const PageImage&>(*parser.currentPage->elements[1]);
    const auto& following = static_cast<const PageLine&>(*parser.currentPage->elements[2]);
    EXPECT_EQ(heading.getBlock()->wordCount(), 1u);
    EXPECT_EQ(image.xPos, (parser.viewportWidth - 115) / 2);
    EXPECT_GE(image.yPos, heading.yPos + renderer.getLineHeight(0) + 6);
    EXPECT_EQ(following.yPos, image.yPos + imageHeight + 36);
    EXPECT_EQ(parser.currentPageNextY, following.yPos + renderer.getLineHeight(0));
    EXPECT_EQ(following.getBlock()->wordCount(), 1u);
    EXPECT_EQ(parser.blockStyleCount_, 2u);  // Root and following paragraph only.
  }

  void expectNestedFigureClosingSpacing() {
    cssParser.rulesBySelector_["div"] = CssParser::parseInlineStyle("margin-top: 10px; margin-bottom: 12px");
    cssParser.rulesBySelector_["figure"] = CssParser::parseInlineStyle("margin-top: 6px; margin-bottom: 36px");
    epub.optimizerImageAvailable = true;
    epub.optimizerImageWidth = 115;
    epub.optimizerImageHeight = 60;
    const XML_Char* attributes[] = {"src", "art.jpg", nullptr};
    ChapterHtmlSlimParser::startElement(&parser, "div", nullptr);
    ChapterHtmlSlimParser::startElement(&parser, "figure", nullptr);
    ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
    ChapterHtmlSlimParser::endElement(&parser, "img");
    ChapterHtmlSlimParser::endElement(&parser, "figure");
    ChapterHtmlSlimParser::endElement(&parser, "div");
    ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
    ChapterHtmlSlimParser::characterData(&parser, "After", 5);
    parser.flushPartWordBuffer();
    parser.makePages();
    ASSERT_NE(parser.currentPage, nullptr);
    ASSERT_EQ(parser.currentPage->elements.size(), 2u);
    EXPECT_EQ(parser.currentPage->elements[0]->yPos, 10);
    EXPECT_EQ(parser.currentPage->elements[1]->yPos, 118);
    EXPECT_EQ(parser.currentPageNextY, 118 + renderer.getLineHeight(0));
  }

  BlockStyle parseParagraph(const char* className) {
    const XML_Char* attributes[] = {"class", className, nullptr};
    ChapterHtmlSlimParser::startElement(&parser, "p", attributes);
    const BlockStyle style = parser.currentTextBlock->getBlockStyle();
    ChapterHtmlSlimParser::characterData(&parser, "Text", 4);
    ChapterHtmlSlimParser::endElement(&parser, "p");
    return style;
  }
};

TEST_F(ChapterHtmlSlimParserTest, InheritsBodyTextIndentAndPreservesExplicitParagraphZero) {
  parser.cssParser->rulesBySelector_[".class-0"] =
      CssParser::parseInlineStyle("text-indent: 1.5em; text-align: justify");
  parser.cssParser->rulesBySelector_[".class_s4K-0"] = CssParser::parseInlineStyle("text-indent: 0");
  parser.cssParser->rulesBySelector_[".class_s4P-0"] = CssParser::parseInlineStyle("margin-top: 0; margin-bottom: 0");

  const XML_Char* bodyAttributes[] = {"class", "class-0", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "body", bodyAttributes);

  const BlockStyle openingParagraph = parseParagraph("class_s4K-0");
  EXPECT_TRUE(openingParagraph.textIndentDefined);
  EXPECT_EQ(openingParagraph.textIndent, 0);
  EXPECT_EQ(parser.currentTextBlock->resolveFirstLineIndent(true, renderer, 0), 0);

  const BlockStyle followingParagraph = parseParagraph("class_s4P-0");
  EXPECT_TRUE(followingParagraph.textIndentDefined);
  EXPECT_EQ(followingParagraph.textIndent, 18);
  EXPECT_EQ(parser.currentTextBlock->resolveFirstLineIndent(true, renderer, 0), 18);

  ChapterHtmlSlimParser::endElement(&parser, "body");
}

TEST_F(ChapterHtmlSlimParserTest, HtmlIndentFlowsThroughBodyAndBodyIndentOverridesIt) {
  parser.cssParser->rulesBySelector_[".html-indent"] = CssParser::parseInlineStyle("text-indent: 1em");
  parser.cssParser->rulesBySelector_[".body-indent"] = CssParser::parseInlineStyle("text-indent: 1.5em");
  parser.cssParser->rulesBySelector_[".plain"] = CssParser::parseInlineStyle("margin: 0");

  const XML_Char* htmlAttributes[] = {"class", "html-indent", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "html", htmlAttributes);
  EXPECT_EQ(parser.blockStyleBuf_[0].textIndent, 12);

  const XML_Char* bodyAttributes[] = {"class", "body-indent", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "body", bodyAttributes);

  const BlockStyle inheritedParagraph = parseParagraph("plain");
  EXPECT_TRUE(inheritedParagraph.textIndentDefined);
  EXPECT_EQ(inheritedParagraph.textIndent, 18);
  EXPECT_EQ(parser.currentTextBlock->resolveFirstLineIndent(true, renderer, 0), 18);

  ChapterHtmlSlimParser::endElement(&parser, "body");
  ChapterHtmlSlimParser::endElement(&parser, "html");
}

TEST_F(ChapterHtmlSlimParserTest, InheritsTextIndentFromDivAndKeepsParagraphOverride) {
  parser.cssParser->rulesBySelector_[".ancestor-indent"] = CssParser::parseInlineStyle("text-indent: 1.5em");
  parser.cssParser->rulesBySelector_[".plain"] = CssParser::parseInlineStyle("margin: 0");
  parser.cssParser->rulesBySelector_[".zero"] = CssParser::parseInlineStyle("text-indent: 0");

  const XML_Char* divAttributes[] = {"class", "ancestor-indent", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "div", divAttributes);

  const BlockStyle inheritedParagraph = parseParagraph("plain");
  EXPECT_TRUE(inheritedParagraph.textIndentDefined);
  EXPECT_EQ(inheritedParagraph.textIndent, 18);
  EXPECT_EQ(parser.currentTextBlock->resolveFirstLineIndent(true, renderer, 0), 18);

  const BlockStyle zeroParagraph = parseParagraph("zero");
  EXPECT_TRUE(zeroParagraph.textIndentDefined);
  EXPECT_EQ(zeroParagraph.textIndent, 0);
  EXPECT_EQ(parser.currentTextBlock->resolveFirstLineIndent(true, renderer, 0), 0);

  ChapterHtmlSlimParser::endElement(&parser, "div");
}

TEST_F(ChapterHtmlSlimParserTest, BodyIndentRespectsSpacingAndForcedIndentSettings) {
  parser.cssParser->rulesBySelector_[".body-indent"] = CssParser::parseInlineStyle("text-indent: 1.5em");
  parser.cssParser->rulesBySelector_[".plain"] = CssParser::parseInlineStyle("margin: 0");
  parser.cssParser->rulesBySelector_[".zero"] = CssParser::parseInlineStyle("text-indent: 0");
  const XML_Char* bodyAttributes[] = {"class", "body-indent", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "body", bodyAttributes);

  parser.extraParagraphSpacing = true;
  parser.currentTextBlock = std::make_unique<ParsedText>(true, false);
  const BlockStyle spacedParagraph = parseParagraph("plain");
  EXPECT_EQ(spacedParagraph.textIndent, 18);
  EXPECT_EQ(parser.currentTextBlock->resolveFirstLineIndent(true, renderer, 0), 0);

  parser.extraParagraphSpacing = false;
  const BlockStyle unspacedParagraph = parseParagraph("plain");
  EXPECT_EQ(unspacedParagraph.textIndent, 18);
  EXPECT_EQ(parser.currentTextBlock->resolveFirstLineIndent(true, renderer, 0), 18);

  parser.forceParagraphIndents = true;
  const BlockStyle forcedZeroParagraph = parseParagraph("zero");
  EXPECT_EQ(forcedZeroParagraph.textIndent, renderer.getFontAscenderSize(0));
  EXPECT_EQ(parser.currentTextBlock->resolveFirstLineIndent(true, renderer, 0), renderer.getFontAscenderSize(0));

  ChapterHtmlSlimParser::endElement(&parser, "body");
}

TEST_F(ChapterHtmlSlimParserTest, RootIndentIsIgnoredWhenEmbeddedStyleIsOff) {
  parser.embeddedStyle = false;
  parser.cssParser->rulesBySelector_[".body-indent"] = CssParser::parseInlineStyle("text-indent: 1.5em");
  parser.cssParser->rulesBySelector_[".plain"] = CssParser::parseInlineStyle("margin: 0");
  const XML_Char* bodyAttributes[] = {"class", "body-indent", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "body", bodyAttributes);

  const BlockStyle plainParagraph = parseParagraph("plain");
  EXPECT_FALSE(plainParagraph.textIndentDefined);
  EXPECT_EQ(parser.currentTextBlock->resolveFirstLineIndent(true, renderer, 0), 0);

  ChapterHtmlSlimParser::endElement(&parser, "body");
}

TEST_P(ChapterHtmlSlimParserTest, KeepsCssVerticalAlignAndInternalLinkMetadata) {
  const char* verticalAlign = GetParam();
  const char* expectedHref = "#note-target";
  const XML_Char* attributes[] = {"href", expectedHref, "style", verticalAlign, nullptr};

  ChapterHtmlSlimParser::startElement(&parser, "a", attributes);
  ChapterHtmlSlimParser::characterData(&parser, "1", 1);
  ChapterHtmlSlimParser::endElement(&parser, "a");

  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  const auto style = parser.currentTextBlock->getWordStyleAt(0);
  const auto expectedStyle =
      std::string(verticalAlign).find("super") != std::string::npos ? EpdFontFamily::SUP : EpdFontFamily::SUB;
  EXPECT_NE(static_cast<uint8_t>(style) & static_cast<uint8_t>(expectedStyle), 0u);

  ASSERT_EQ(parser.pendingFootnotes.size(), 1u);
  const FootnoteEntry& footnote = parser.pendingFootnotes.front().second;
  EXPECT_STREQ(footnote.href, expectedHref);
  ASSERT_NE(footnote.linkId, 0u);
  ASSERT_EQ(parser.currentTextBlock->wordBackgroundBlack.size(), 1u);
  const uint8_t wordLinkId =
      static_cast<uint8_t>((parser.currentTextBlock->wordBackgroundBlack.front() & TextBlock::WORD_FLAG_LINK_ID_MASK) >>
                           TextBlock::WORD_FLAG_LINK_ID_SHIFT);
  EXPECT_EQ(wordLinkId, footnote.linkId);
}

INSTANTIATE_TEST_SUITE_P(CssVerticalAlign, ChapterHtmlSlimParserTest,
                         ::testing::Values("vertical-align: super", "vertical-align: sub"));

TEST_F(ChapterHtmlSlimParserTest, PreservesEmptyInlinePaddingBeforeDialogueText) {
  parser.cssParser->rulesBySelector_[".spacey"] = CssParser::parseInlineStyle("padding-left: 2em");
  ChapterHtmlSlimParser::characterData(&parser, "EERO:", 5);
  const XML_Char* attributes[] = {"class", "spacey", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "span", attributes);
  ChapterHtmlSlimParser::endElement(&parser, "span");
  ChapterHtmlSlimParser::characterData(&parser, "Kappusiwai!", 11);
  parser.flushPartWordBuffer();

  ASSERT_EQ(parser.currentTextBlock->words.size(), 2u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "EERO:");
  EXPECT_EQ(parser.currentTextBlock->words[1], "Kappusiwai!");
  ASSERT_EQ(parser.currentTextBlock->inlinePaddings.size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->inlinePaddings[0].wordIndex, 1u);
  EXPECT_EQ(parser.currentTextBlock->inlinePaddings[0].pixels, 24);
  EXPECT_TRUE(parser.currentTextBlock->wordContinues[1]);

  std::shared_ptr<TextBlock> renderedLine;
  ASSERT_TRUE(parser.currentTextBlock->layoutAndExtractLines(
      renderer, 0, 480,
      [&renderedLine](std::shared_ptr<TextBlock> line, uint32_t, uint32_t) { renderedLine = std::move(line); }));
  ASSERT_NE(renderedLine, nullptr);
  ASSERT_EQ(renderedLine->wordCount(), 2u);
  EXPECT_EQ(renderedLine->wordXpos(0), 0);
  EXPECT_EQ(renderedLine->wordXpos(1), 24);
}

// SOF header of a 800 x 200 JPEG. The real streaming dimension parser is linked.
static const std::vector<uint8_t> jpegHeader = {0xff, 0xd8, 0xff, 0xc0, 0, 7, 8, 0, 200, 3, 32};

TEST_F(ChapterHtmlSlimParserTest, OrdinaryImageHeaderDoesNotLoanWhenHeapProbeSucceeds) {
  epub.probeBytes = jpegHeader;
  const XML_Char* attributes[] = {"src", "wide.jpg", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  EXPECT_EQ(epub.streamReadCount, 1u);
  EXPECT_EQ(epub.extractCount, 0u);
  EXPECT_EQ(renderer.loans, 0u);
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 1u);
}

TEST_F(ChapterHtmlSlimParserTest, RetriesImageHeaderWithLoanAndKeepsLazySource) {
  epub.probeBytes = jpegHeader;
  epub.requireLoan = true;
  const XML_Char* attributes[] = {"src", "wide.jpg", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  EXPECT_EQ(epub.streamReadCount, 2u);
  EXPECT_EQ(epub.extractCount, 0u);
  EXPECT_EQ(renderer.loans, 1u);
  EXPECT_TRUE(renderer.hasFrameBuffer());
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 1u);
  const auto& image = static_cast<const PageImage&>(*parser.currentPage->elements.front()).getImageBlock();
  EXPECT_EQ(image.getWidth(), 480);
  EXPECT_EQ(image.getHeight(), 120);
}

TEST_F(ChapterHtmlSlimParserTest, FailedProbeLoansOnlyAfterHeapAttemptsFail) {
  // streamFails: the read itself failed, so the probe retries under a loan.
  // Otherwise the header was read but not understood: no probe retry.
  for (const bool streamFails : {false, true}) {
    epub.probeBytes = streamFails ? jpegHeader : std::vector<uint8_t>{0xff, 0xd8, 0x00};
    epub.streamFails = streamFails;
    epub.streamReadCount = epub.extractCount = 0;
    renderer.loans = 0;
    const XML_Char* attributes[] = {"src", "broken.jpg", nullptr};
    ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
    EXPECT_EQ(epub.streamReadCount, streamFails ? 2u : 1u);
    EXPECT_EQ(epub.extractCount, 2u);
    EXPECT_TRUE(epub.extractHadLoan);
    EXPECT_EQ(renderer.loans, streamFails ? 2u : 1u);
    EXPECT_TRUE(renderer.hasFrameBuffer());
    EXPECT_TRUE(!parser.currentPage || parser.currentPage->elements.empty());
    ChapterHtmlSlimParser::endElement(&parser, "img");
  }
}

TEST_F(ChapterHtmlSlimParserTest, FullFileFallbackExtractsWithoutLoanWhenHeapSuffices) {
  epub.probeBytes = {0xff, 0xd8, 0};
  epub.extractSucceeds = true;
  const XML_Char* attributes[] = {"src", "unusual.jpg", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  EXPECT_EQ(epub.extractCount, 1u);
  EXPECT_FALSE(epub.extractHadLoan);
  EXPECT_EQ(renderer.loans, 0u);
  EXPECT_FALSE(GfxRenderer::fileProbeHadLoan);
  EXPECT_TRUE(renderer.hasFrameBuffer());
  ASSERT_NE(parser.currentPage, nullptr);
  EXPECT_EQ(parser.currentPage->elements.size(), 1u);
}

TEST_F(ChapterHtmlSlimParserTest, ImageAdmissionStillRejectsBeforeAnyProbeOrLoan) {
  MemoryBudget::imageAllowed = false;
  epub.probeBytes = jpegHeader;
  epub.requireLoan = true;
  const XML_Char* attributes[] = {"src", "wide.jpg", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  EXPECT_EQ(epub.streamReadCount, 0u);
  EXPECT_EQ(epub.extractCount, 0u);
  EXPECT_EQ(renderer.loans, 0u);
  EXPECT_TRUE(parser.lowMemoryImageFallback);
}

TEST_F(ChapterHtmlSlimParserTest, NestedProbeLoanDoesNotReturnOuterStorage) {
  epub.probeBytes = {0xff, 0xd8, 0};
  const XML_Char* attributes[] = {"src", "broken.jpg", nullptr};
  {
    GfxRenderer::FrameBufferLoan outer(renderer);
    ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
    EXPECT_FALSE(renderer.hasFrameBuffer());
    EXPECT_EQ(renderer.loans, 1u);
  }
  EXPECT_TRUE(renderer.hasFrameBuffer());
}

TEST_F(ChapterHtmlSlimParserTest, UsesOptimizerImageDimensionsWithoutReadingTheCompressedImage) {
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 800;
  epub.optimizerImageHeight = 7;
  const XML_Char* attributes[] = {"src", "wide.jpg", nullptr};

  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);

  EXPECT_EQ(epub.streamReadCount, 0u);
  EXPECT_EQ(renderer.loans, 0u);
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 1u);
  ASSERT_EQ(parser.currentPage->elements.front()->getTag(), TAG_PageImage);
  const auto& image = static_cast<const PageImage&>(*parser.currentPage->elements.front()).getImageBlock();
  EXPECT_EQ(image.getWidth(), 480);
  EXPECT_EQ(image.getHeight(), 4);
  EXPECT_FALSE(static_cast<const PageImage&>(*parser.currentPage->elements.front()).isInlineImage());
}

TEST_F(ChapterHtmlSlimParserTest, PlacesSmallImageInsideTextLine) {
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 16;
  epub.optimizerImageHeight = 16;
  const XML_Char* attributes[] = {"src", "icon.jpg", nullptr};

  ChapterHtmlSlimParser::characterData(&parser, "Before", 6);
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  ChapterHtmlSlimParser::endElement(&parser, "img");
  ChapterHtmlSlimParser::characterData(&parser, "After", 5);
  parser.flushPartWordBuffer();
  parser.makePages();

  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 2u);
  EXPECT_EQ(parser.currentPage->elements[0]->getTag(), TAG_PageLine);
  EXPECT_EQ(parser.currentPage->elements[1]->getTag(), TAG_PageImage);
  const auto& line = static_cast<const PageLine&>(*parser.currentPage->elements[0]);
  EXPECT_EQ(line.getBlock()->wordCount(), 2u);
  const auto& image = static_cast<const PageImage&>(*parser.currentPage->elements[1]);
  EXPECT_TRUE(image.isInlineImage());
  EXPECT_EQ(image.yPos, parser.currentPage->elements[0]->yPos);
  EXPECT_EQ(image.getImageBlock().getWidth(), 16);
  EXPECT_TRUE(parser.pendingInlineImages.empty());
}

TEST_F(ChapterHtmlSlimParserTest, WrapsTextAfterInlineImageWithoutSplittingTheImage) {
  renderer.textAdvancePerChar = 4;
  parser.viewportWidth = 22;
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 16;
  epub.optimizerImageHeight = 16;
  const XML_Char* attributes[] = {"src", "icon.jpg", nullptr};

  ChapterHtmlSlimParser::characterData(&parser, "A", 1);
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  ChapterHtmlSlimParser::endElement(&parser, "img");
  ChapterHtmlSlimParser::characterData(&parser, "B", 1);
  parser.flushPartWordBuffer();
  parser.makePages();

  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 3u);
  EXPECT_EQ(parser.currentPage->elements[0]->getTag(), TAG_PageLine);
  EXPECT_EQ(parser.currentPage->elements[1]->getTag(), TAG_PageImage);
  EXPECT_EQ(parser.currentPage->elements[2]->getTag(), TAG_PageLine);
  EXPECT_EQ(parser.currentPage->elements[1]->xPos, 4);
  EXPECT_EQ(parser.currentPage->elements[0]->yPos, parser.currentPage->elements[1]->yPos);
  EXPECT_EQ(parser.currentPage->elements[2]->yPos, 16);
}

TEST_F(ChapterHtmlSlimParserTest, AlignsTextWithTallerInlineImage) {
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 20;
  epub.optimizerImageHeight = 32;
  const XML_Char* attributes[] = {"src", "icon.jpg", nullptr};

  ChapterHtmlSlimParser::characterData(&parser, "A", 1);
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  ChapterHtmlSlimParser::endElement(&parser, "img");
  parser.makePages();

  ASSERT_EQ(parser.currentPage->elements.size(), 2u);
  EXPECT_EQ(parser.currentPage->elements[0]->getTag(), TAG_PageLine);
  EXPECT_EQ(parser.currentPage->elements[1]->getTag(), TAG_PageImage);
  EXPECT_EQ(parser.currentPage->elements[0]->yPos, 16);
  EXPECT_EQ(parser.currentPage->elements[1]->yPos, 0);
  EXPECT_EQ(parser.currentPageNextY, 32);
}

TEST_F(ChapterHtmlSlimParserTest, HonorsExplicitBlockDisplayForSmallImage) {
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 16;
  epub.optimizerImageHeight = 16;
  const XML_Char* attributes[] = {"src", "icon.jpg", "style", "display: block", nullptr};

  ChapterHtmlSlimParser::characterData(&parser, "A", 1);
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);

  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 2u);
  EXPECT_EQ(parser.currentPage->elements[0]->getTag(), TAG_PageLine);
  EXPECT_EQ(parser.currentPage->elements[1]->getTag(), TAG_PageImage);
  EXPECT_GE(parser.currentPage->elements[1]->yPos, 16);
}

TEST_F(ChapterHtmlSlimParserTest, HonorsExplicitInlineDisplayForTallerImage) {
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 20;
  epub.optimizerImageHeight = 48;
  const XML_Char* attributes[] = {"src", "icon.jpg", "style", "display: inline", nullptr};

  ChapterHtmlSlimParser::characterData(&parser, "A", 1);
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  ChapterHtmlSlimParser::endElement(&parser, "img");
  parser.makePages();

  ASSERT_EQ(parser.currentPage->elements.size(), 2u);
  EXPECT_EQ(parser.currentPage->elements[0]->getTag(), TAG_PageLine);
  EXPECT_EQ(parser.currentPage->elements[1]->getTag(), TAG_PageImage);
  EXPECT_EQ(parser.currentPage->elements[0]->yPos, 32);
  EXPECT_EQ(parser.currentPage->elements[1]->yPos, 0);
  EXPECT_EQ(parser.currentPageNextY, 48);
}

TEST_F(ChapterHtmlSlimParserTest, BoundsPendingImagesInAnIconOnlyParagraph) {
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 1;
  epub.optimizerImageHeight = 1;
  const XML_Char* attributes[] = {"src", "icon.jpg", nullptr};

  for (int i = 0; i < 40; ++i) {
    ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
    ChapterHtmlSlimParser::endElement(&parser, "img");
    EXPECT_LT(parser.pendingInlineImages.size(), parser.MAX_PENDING_INLINE_IMAGES);
  }
  parser.makePages();

  ASSERT_NE(parser.currentPage, nullptr);
  EXPECT_TRUE(parser.pendingInlineImages.empty());
  EXPECT_EQ(parser.currentPage->elements.size(), 40u);
}

TEST_F(ChapterHtmlSlimParserTest, CentersChapterFigureWithJustifiedParagraphs) {
  expectChapterFigureLayout(CssTextAlign::Justify);
}

TEST_F(ChapterHtmlSlimParserTest, CentersChapterFigureWithLeftAlignedParagraphs) {
  expectChapterFigureLayout(CssTextAlign::Left);
}

TEST_F(ChapterHtmlSlimParserTest, BlockSizedFigureImageConsumesMarginsOnce) {
  expectChapterFigureLayout(CssTextAlign::Justify, 60);
}

TEST_F(ChapterHtmlSlimParserTest, ExplicitBlockFigureImageConsumesMarginsOnce) {
  expectChapterFigureLayout(CssTextAlign::Justify, 6, true);
}

TEST_F(ChapterHtmlSlimParserTest, BlockFigureImagesKeepBottomSpacingAfterTheLastImage) {
  cssParser.rulesBySelector_["figure"] = CssParser::parseInlineStyle("margin-top: 6px; margin-bottom: 36px");
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 115;
  epub.optimizerImageHeight = 60;
  const XML_Char* attributes[] = {"src", "art.jpg", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "figure", nullptr);
  for (int i = 0; i < 2; ++i) {
    ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
    ChapterHtmlSlimParser::endElement(&parser, "img");
  }
  ChapterHtmlSlimParser::endElement(&parser, "figure");
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "After", 5);
  parser.flushPartWordBuffer();
  parser.makePages();
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 3u);
  EXPECT_EQ(parser.currentPage->elements[0]->yPos, 6);
  EXPECT_EQ(parser.currentPage->elements[1]->yPos, 66);
  EXPECT_EQ(parser.currentPage->elements[2]->yPos, 162);
  EXPECT_EQ(parser.currentPageNextY, 178);
}

TEST_F(ChapterHtmlSlimParserTest, NestedFigureImageConsumesEachContainersClosingSpacingOnce) {
  expectNestedFigureClosingSpacing();
}

TEST_F(ChapterHtmlSlimParserTest, BlockFigureImageKeepsCaptionBeforeClosingSpacing) {
  cssParser.rulesBySelector_["figure"] = CssParser::parseInlineStyle("margin-top: 6px; margin-bottom: 36px");
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 115;
  epub.optimizerImageHeight = 60;
  const XML_Char* attributes[] = {"src", "art.jpg", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "figure", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  ChapterHtmlSlimParser::endElement(&parser, "img");
  ChapterHtmlSlimParser::startElement(&parser, "figcaption", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Caption", 7);
  ChapterHtmlSlimParser::endElement(&parser, "figcaption");
  ChapterHtmlSlimParser::endElement(&parser, "figure");
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "After", 5);
  parser.flushPartWordBuffer();
  parser.makePages();
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 3u);
  EXPECT_EQ(parser.currentPage->elements[0]->yPos, 6);
  EXPECT_EQ(parser.currentPage->elements[1]->yPos, 66);
  EXPECT_EQ(parser.currentPage->elements[2]->yPos, 118);
  EXPECT_EQ(parser.currentPageNextY, 134);
}

TEST_F(ChapterHtmlSlimParserTest, FigureWithoutEmbeddedStylesKeepsBlockBoundary) {
  parser.embeddedStyle = false;
  cssParser.rulesBySelector_["figure.inline_group"] =
      CssParser::parseInlineStyle("margin-top: 0.5em; margin-bottom: 3em; text-align: center");
  const XML_Char* attributes[] = {"class", "inline_group", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Before", 6);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  ChapterHtmlSlimParser::startElement(&parser, "figure", attributes);
  const auto& style = parser.currentTextBlock->getBlockStyle();
  EXPECT_EQ(style.alignment, CssTextAlign::Justify);
  EXPECT_EQ(style.topInset(), 0);
  ChapterHtmlSlimParser::characterData(&parser, "Figure", 6);
  ChapterHtmlSlimParser::endElement(&parser, "figure");
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "After", 5);
  parser.flushPartWordBuffer();
  parser.makePages();

  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 3u);
  for (const auto& element : parser.currentPage->elements) {
    ASSERT_EQ(element->getTag(), TAG_PageLine);
    EXPECT_EQ(static_cast<const PageLine&>(*element).getBlock()->wordCount(), 1u);
  }
}

TEST_F(ChapterHtmlSlimParserTest, HiddenElementsSuppressContentAndResumeVisibleText) {
  for (const char* tag : {"p", "h1", "span", "div", "a", "table"}) {
    for (const char* value : {"hidden", "", "false"}) {
      const XML_Char* attributes[] = {"hidden", value, "style", "display: block", nullptr};
      ChapterHtmlSlimParser::startElement(&parser, tag, attributes);
      ChapterHtmlSlimParser::startElement(&parser, "span", nullptr);
      ChapterHtmlSlimParser::characterData(&parser, "HIDDEN ", 7);
      ChapterHtmlSlimParser::endElement(&parser, "span");
      ChapterHtmlSlimParser::endElement(&parser, tag);
      EXPECT_EQ(parser.currentTextBlock->size(), 0u);
      EXPECT_EQ(parser.partWordBufferIndex, 0);
    }
  }
  ChapterHtmlSlimParser::characterData(&parser, "Visible ", 8);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "Visible");
}

TEST_F(ChapterHtmlSlimParserTest, StablePageOffsetsCollapseClusteredWhitespace) {
  parser.trackReferenceCharacters = true;
  parser.currentTextBlock = std::make_unique<ParsedText>(false, false, false, false, false, 0, BlockStyle{}, true);

  constexpr char text[] = "  Alpha     Beta ";
  ChapterHtmlSlimParser::characterData(&parser, text, sizeof(text) - 1);
  parser.flushPartWordBuffer();

  ASSERT_EQ(parser.currentTextBlock->wordReferenceOffsets.size(), 2u);
  EXPECT_EQ(parser.currentTextBlock->wordReferenceOffsets[0], 0u);
  EXPECT_EQ(parser.currentTextBlock->wordReferenceOffsets[1], 6u);
  EXPECT_EQ(parser.referenceTextOffset, 10u);
  EXPECT_TRUE(parser.referenceWhitespacePending);
}

TEST_F(ChapterHtmlSlimParserTest, StablePageOffsetsResumeAfterNestedExcludedMarkup) {
  parser.trackReferenceCharacters = true;
  parser.currentTextBlock = std::make_unique<ParsedText>(false, false, false, false, false, 0, BlockStyle{}, true);

  ChapterHtmlSlimParser::startElement(&parser, "html", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "head", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "style", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "p { display: block; }", 21);
  ChapterHtmlSlimParser::endElement(&parser, "style");
  ChapterHtmlSlimParser::endElement(&parser, "head");
  ChapterHtmlSlimParser::startElement(&parser, "body", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "svg", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "metadata", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "not book text", 13);
  ChapterHtmlSlimParser::endElement(&parser, "metadata");
  ChapterHtmlSlimParser::endElement(&parser, "svg");
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Visible text ", 13);
  parser.flushPartWordBuffer();

  EXPECT_EQ(parser.referenceExcludedUntilDepth, INT_MAX);
  EXPECT_EQ(parser.referenceTextOffset, 12u);
  ASSERT_EQ(parser.currentTextBlock->wordReferenceOffsets.size(), 2u);
  EXPECT_EQ(parser.currentTextBlock->wordReferenceOffsets[0], 0u);
  EXPECT_EQ(parser.currentTextBlock->wordReferenceOffsets[1], 8u);
}

TEST_F(ChapterHtmlSlimParserTest, HiddenImageDoesNotReadImageDataWithoutCss) {
  parser.cssParser = nullptr;
  const XML_Char* attributes[] = {"hidden", "", "src", "missing.jpg", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  ChapterHtmlSlimParser::endElement(&parser, "img");
  EXPECT_EQ(epub.streamReadCount, 0u);
  EXPECT_EQ(parser.currentPage, nullptr);
}

TEST_F(ChapterHtmlSlimParserTest, HiddenIdsDoNotBecomeAnchorsOrTocPageBreaks) {
  parser.tocAnchors.push_back("hidden-chapter");
  const XML_Char* idFirst[] = {"id", "hidden-chapter", "hidden", "hidden", nullptr};
  const XML_Char* hiddenFirst[] = {"hidden", "", "id", "hidden-chapter", nullptr};
  for (auto* attributes : {idFirst, hiddenFirst}) {
    ChapterHtmlSlimParser::startElement(&parser, "h1", attributes);
    ChapterHtmlSlimParser::characterData(&parser, "Hidden", 6);
    ChapterHtmlSlimParser::endElement(&parser, "h1");
    EXPECT_TRUE(parser.pendingAnchorId.empty());
    ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
    EXPECT_TRUE(parser.anchorData.empty());
    EXPECT_EQ(parser.completedPageCount, 0);
    ChapterHtmlSlimParser::endElement(&parser, "p");
  }
}

TEST_F(ChapterHtmlSlimParserTest, NumbersOrderedListsAndRestartsNestedCounters) {
  ChapterHtmlSlimParser::startElement(&parser, "ol", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "1.");

  ChapterHtmlSlimParser::startElement(&parser, "ol", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "1.");
  ChapterHtmlSlimParser::endElement(&parser, "li");
  ChapterHtmlSlimParser::endElement(&parser, "ol");

  ChapterHtmlSlimParser::endElement(&parser, "li");
  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "2.");
}

TEST_F(ChapterHtmlSlimParserTest, HonorsOrderedListStartAndItemValue) {
  const XML_Char* listAttributes[] = {"start", "5", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "ol", listAttributes);
  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "5.");
  ChapterHtmlSlimParser::endElement(&parser, "li");

  const XML_Char* itemAttributes[] = {"value", "9", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "li", itemAttributes);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "9.");
  ChapterHtmlSlimParser::endElement(&parser, "li");

  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "10.");
}

TEST_F(ChapterHtmlSlimParserTest, SupportsNegativeOrderedListValues) {
  const XML_Char* listAttributes[] = {"start", "-2", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "ol", listAttributes);
  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "-2.");
  ChapterHtmlSlimParser::endElement(&parser, "li");

  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "-1.");
}

TEST_F(ChapterHtmlSlimParserTest, SupportsMarkerFreeListsAndContainerInsets) {
  const XML_Char* listAttributes[] = {"style", "list-style-type: none; margin-left: 10px; padding-left: 5px", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "ul", listAttributes);
  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);

  EXPECT_TRUE(parser.currentTextBlock->isEmpty());
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().leftInset(), 15);
}

TEST_F(ChapterHtmlSlimParserTest, HiddenNestedListDoesNotResetOuterCounter) {
  ChapterHtmlSlimParser::startElement(&parser, "ol", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  EXPECT_EQ(parser.currentTextBlock->words[0], "1.");
  ChapterHtmlSlimParser::endElement(&parser, "li");

  const XML_Char* hidden[] = {"hidden", "", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "ul", hidden);
  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ChapterHtmlSlimParser::endElement(&parser, "li");
  ChapterHtmlSlimParser::endElement(&parser, "ul");

  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "2.");
}

TEST_F(ChapterHtmlSlimParserTest, AdvancePrewarmScanResumesAcrossSlicesAndFeedsEveryCodepoint) {
  // Longer than two 512-byte reads, with a 2-byte UTF-8 character straddling a read boundary.
  std::string html = "<p class=\"x\">ab&amp;c</p>";
  html.append(510 - html.size(), 'd');
  html += "\xC3\xA9";  // U+00E9 split across bytes 510/511 and 512
  html += "<b>zz</b> \xE2\x80\x94 end";
  html.append(600, 'q');

  const auto scanAll = [&](const bool reseekEachChunk) {
    ChapterHtmlSlimParser::AdvancePrewarm state;
    state.codepoints = std::make_unique<uint32_t[]>(4096);
    state.buffer = std::make_unique<uint8_t[]>(512);
    HalFile file;
    file.data = html;
    while (!state.scanned) {
      if (reseekEachChunk) {
        file.seek(0);  // the parser moved the file; the scan resumes from its own offset
        EXPECT_TRUE(file.seek(state.offset));
      }
      parser.scanAdvancePrewarmChunk(state, file);
    }
    EXPECT_EQ(state.offset, html.size());
    return std::vector<uint32_t>(state.codepoints.get(), state.codepoints.get() + state.cpCount);
  };

  const auto oneGo = scanAll(false);
  EXPECT_EQ(scanAll(true), oneGo);
  for (const uint32_t cp : {uint32_t{'a'}, uint32_t{' '}, uint32_t{'z'}, uint32_t{0xE9}, uint32_t{0x2014}}) {
    EXPECT_NE(std::find(oneGo.begin(), oneGo.end(), cp), oneGo.end()) << cp;
  }
  EXPECT_EQ(std::find(oneGo.begin(), oneGo.end(), uint32_t{'<'}), oneGo.end());
  EXPECT_EQ(std::find(oneGo.begin(), oneGo.end(), uint32_t{'m'}), oneGo.end());  // only inside &amp;

  ChapterHtmlSlimParser::AdvancePrewarm state;
  state.codepoints = std::make_unique<uint32_t[]>(oneGo.size());
  std::copy(oneGo.begin(), oneGo.end(), state.codepoints.get());
  state.cpCount = static_cast<uint32_t>(oneGo.size());
  while (state.fedCount < state.cpCount) parser.feedAdvancePrewarm(state, 3);
  std::vector<uint32_t> fed;
  for (const auto& batch : renderer.fedCodepoints) {
    EXPECT_LE(batch.size(), 3u);
    fed.insert(fed.end(), batch.begin(), batch.end());
  }
  EXPECT_EQ(fed, oneGo);
}

}  // namespace

TEST(CssBorderTest, SuppressionAndDeclarationOrder) {
  for (const char* declarations :
       {"border: none", "border: HIDDEN !important", "border: 0", "border: solid 0px black", "border-style: none",
        "border-width: 0rem", "border-width: 0 0px 0em 0pt", "border-style: none hidden",
        "border: solid; border-style: none", "border-width: 0; border-style: solid",
        "border: none; border-style: solid; border-width: 0",
        "border-top: none; border-right: 0; border-bottom: hidden; border-left-width: 0"}) {
    SCOPED_TRACE(declarations);
    const auto style = CssParser::parseInlineStyle(declarations);
    EXPECT_TRUE(style.defined.border);
    EXPECT_TRUE(style.suppressesHorizontalRule());
  }
  for (const char* declarations :
       {"", "border: solid", "border-style: dashed", "border-width: thin", "border-top: none",
        "border: none; border: 1px solid black", "border: none; border-top-style: solid", "border-width: 0 1px",
        "border: 0; border-width: 2px; border-style: solid", "border-style: none; border-style: solid"}) {
    EXPECT_FALSE(CssParser::parseInlineStyle(declarations).suppressesHorizontalRule()) << declarations;
  }
}

TEST(CssBorderTest, InvalidLonghandsDoNotOverrideSuppression) {
  for (const char* value : {"-1px", "1badpx", "nanpx", "2%", "2", "nonsense", "1px 1px 1px 1px 1px"}) {
    const auto style = CssParser::parseInlineStyle(std::string("border-width: 0; border-width: ") + value);
    EXPECT_TRUE(style.suppressesHorizontalRule()) << value;
  }
  EXPECT_TRUE(CssParser::parseInlineStyle("border-style: none; border-style: nonsense").suppressesHorizontalRule());
}

TEST(CssBorderTest, CascadeRestoresEdgesWithoutLosingZeroWidths) {
  auto style = CssParser::parseInlineStyle("border: none");
  style.applyOver(CssParser::parseInlineStyle("border-top-style: solid"));
  EXPECT_FALSE(style.suppressesHorizontalRule());
  style.applyOver(CssParser::parseInlineStyle("border-top-width: 0"));
  EXPECT_TRUE(style.suppressesHorizontalRule());
  style.applyOver(CssParser::parseInlineStyle("border-top-width: medium"));
  EXPECT_FALSE(style.suppressesHorizontalRule());
  style.reset();
  EXPECT_FALSE(style.defined.border);
  EXPECT_FALSE(style.suppressesHorizontalRule());
}

TEST_F(ChapterHtmlSlimParserTest, CackleTransitionKeepsOnlyPublisherOrnament) {
  cssParser.rulesBySelector_["hr.transition"] = CssParser::parseInlineStyle("display: block; border: none; margin: 0");
  cssParser.rulesBySelector_["div.ornament"] = CssParser::parseInlineStyle("text-align: center; margin: 0");
  const XML_Char* hrAttrs[] = {"class", "transition", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "hr", hrAttrs);
  ChapterHtmlSlimParser::endElement(&parser, "hr");
  EXPECT_EQ(parser.currentPageNextY, 0);
  const XML_Char* ornamentAttrs[] = {"class", "ornament", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "div", ornamentAttrs);
  ChapterHtmlSlimParser::characterData(&parser, "—", 3);
  ChapterHtmlSlimParser::endElement(&parser, "div");
  // Starting the following paragraph seals the ornament's text block.
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 1u);
  EXPECT_EQ(parser.currentPage->elements.front()->getTag(), TAG_PageLine);
  EXPECT_EQ(parser.depth, 1);
}

TEST_F(ChapterHtmlSlimParserTest, SuppressedRuleRetainsExplicitSpacing) {
  const XML_Char* attrs[] = {"style", "border: none; margin: 7px 0 9px; padding: 2px 0 3px", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "hr", attrs);
  ChapterHtmlSlimParser::endElement(&parser, "hr");
  ASSERT_NE(parser.currentPage, nullptr);
  EXPECT_TRUE(parser.currentPage->elements.empty());
  EXPECT_EQ(parser.currentPageNextY, 21);
}

TEST_F(ChapterHtmlSlimParserTest, PlainAndExplicitlyVisibleRulesStillRender) {
  ChapterHtmlSlimParser::startElement(&parser, "hr", nullptr);
  ChapterHtmlSlimParser::endElement(&parser, "hr");
  const XML_Char* attrs[] = {"style", "border: none; border-top: 1px solid black", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "hr", attrs);
  ChapterHtmlSlimParser::endElement(&parser, "hr");
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 2u);
  for (const auto& element : parser.currentPage->elements) EXPECT_EQ(element->getTag(), TAG_PageHorizontalRule);
}

TEST_F(ChapterHtmlSlimParserTest, ParentBorderDoesNotHideChildRule) {
  const XML_Char* attrs[] = {"style", "border: none", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "div", attrs);
  ChapterHtmlSlimParser::startElement(&parser, "hr", nullptr);
  ChapterHtmlSlimParser::endElement(&parser, "hr");
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 1u);
  EXPECT_EQ(parser.currentPage->elements.front()->getTag(), TAG_PageHorizontalRule);
}
