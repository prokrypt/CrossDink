#include <Epub.h>
#include <GfxRenderer.h>
#include <Serialization.h>

#include "Epub/HtmlInflateStream.h"
#include "Epub/Page.h"
#include "Epub/hyphenation/Hyphenator.h"
#include "Epub/parsers/ChapterHtmlSlimParser.h"

Epub::Epub(std::string path, const std::string& cacheDir) : filepath(std::move(path)), cachePath(cacheDir) {}

const std::string& Epub::getCachePath() const { return cachePath; }
bool Epub::getItemSize(const std::string&, size_t*) const { return false; }

BookMetadataCache::~BookMetadataCache() = default;

// Host tests have one core, so sections always build on the single-core path.
HtmlInflateStream::~HtmlInflateStream() = default;
bool HtmlInflateStream::worthSplitting() { return false; }
bool HtmlInflateStream::start(const Epub&, const std::string&, size_t, size_t, HalFile&) { return false; }
size_t HtmlInflateStream::read(void*, size_t) { return 0; }
bool HtmlInflateStream::drained() const { return true; }
bool HtmlInflateStream::finish(bool) { return true; }
const std::string& Epub::getLanguage() const {
  static const std::string language = "en";
  return language;
}
BookMetadataCache::SpineEntry Epub::getSpineItem(int) const { return {}; }
BookMetadataCache::TocEntry Epub::getTocItem(int) const { return {}; }
int Epub::getTocItemsCount() const { return 0; }
int Epub::getTocIndexForSpineIndex(int) const { return -1; }
bool Epub::readItemContentsToStream(const std::string&, Print&, size_t, bool) const { return false; }

bool CssParser::loadFromCache() { return false; }
size_t CssParser::SvHash::operator()(std::string_view sv) const noexcept { return std::hash<std::string_view>{}(sv); }
size_t CssParser::SvHash::operator()(const std::string& s) const noexcept { return operator()(std::string_view(s)); }

void Hyphenator::setPreferredLanguage(const std::string&) {}

ChapterHtmlSlimParser::~ChapterHtmlSlimParser() = default;
bool ChapterHtmlSlimParser::beginParse() { return false; }
ChapterHtmlSlimParser::ParseStatus ChapterHtmlSlimParser::parseStep() { return ParseStatus::Error; }
bool ChapterHtmlSlimParser::finishParse() { return true; }
void ChapterHtmlSlimParser::abortParse() {}
void ChapterHtmlSlimParser::releaseInputFile() {}

bool Page::serialize(FsFile& file) const {
  constexpr uint32_t marker = 0x50414745;
  return serialization::tryWritePod(file, marker);
}

std::unique_ptr<Page> Page::deserialize(FsFile& file) {
  uint32_t marker = 0;
  if (!serialization::tryReadPod(file, marker) || marker != 0x50414745) return nullptr;
  return std::make_unique<Page>();
}

uint16_t Page::imageEstimateUnits(uint16_t) const { return 0; }
