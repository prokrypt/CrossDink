// Adapted from FreeInk Opds2Parser; see LICENSE.FreeInk for its MIT notice.
#include "OpdsJsonParser.h"

#include <Logging.h>

#include <cstdlib>
#include <cstring>

namespace {
constexpr size_t MAX_TITLE_CHARS = 160, MAX_AUTHOR_CHARS = 120, MAX_ID_CHARS = 128;
constexpr size_t MAX_HREF_CHARS = 768, MAX_SEARCH_TEMPLATE_CHARS = 768, MAX_PAGE_URL_CHARS = 768;
// Match the SDK acquisition preference without admitting sample/checkout URLs.
int opdsAcquisitionRank(const char* rel) {
  if (strstr(rel, "opds-spec.org/acquisition") != nullptr) {
    if (strstr(rel, "/sample") || strstr(rel, "/preview")) return -1;
    if (strstr(rel, "/open-access")) return 3;
    if (strstr(rel, "/buy")) return 0;
    if (strstr(rel, "/borrow") || strstr(rel, "/subscribe")) return 1;
    return 2;
  }
  if (!strcmp(rel, "download") || !strcmp(rel, "open-access")) return 3;
  if (!strcmp(rel, "acquisition")) return 2;
  if (!strcmp(rel, "borrow") || !strcmp(rel, "subscribe")) return 1;
  return !strcmp(rel, "buy") ? 0 : -1;
}

template <typename String>
void assignBounded(String& target, const char* value, const size_t len, const size_t maxLen) {
  target.assign(value, len < maxLen ? len : maxLen);
}
}  // namespace

// The JSON tokenizer drops any string longer than its buffer (tokenOverflow
// skips the onString callback), so every field the parser stores must fit
// within the buffer's effective capacity or long values would vanish before
// assignBounded() truncates them. The URL limits are the largest fields.
static_assert(MAX_HREF_CHARS < StreamingJsonParser::TOKEN_BUF_SIZE - 1 &&
                  MAX_SEARCH_TEMPLATE_CHARS < StreamingJsonParser::TOKEN_BUF_SIZE - 1 &&
                  MAX_PAGE_URL_CHARS < StreamingJsonParser::TOKEN_BUF_SIZE - 1,
              "OPDS field limits must fit the JSON token buffer or values are dropped by tokenOverflow");

OpdsJsonParser::OpdsJsonParser(OpdsEntry* buffer, size_t capacity, bool publication)
    : parser(JsonCallbacks{this, &sOnKey, &sOnString, &sOnNumber, &sOnBool, nullptr, &sOnObjectStart, &sOnObjectEnd,
                           &sOnArrayStart, &sOnArrayEnd}) {
  entries = buffer;
  entryLimit = capacity;
  publicationDocument = publication;
  if (!entries || entryLimit == 0) errorOccured = true;
}

void OpdsJsonParser::setPreferredLanguage(const char* lang) {
  if (!lang) {
    preferredLang[0] = '\0';
    return;
  }
  // Primary subtag only: "PT_BR" and "pt-BR" both become "pt".
  size_t i = 0;
  for (; lang[i] && lang[i] != '_' && lang[i] != '-' && i < sizeof(preferredLang) - 1; ++i) {
    preferredLang[i] = static_cast<char>((lang[i] >= 'A' && lang[i] <= 'Z') ? lang[i] + 32 : lang[i]);
  }
  preferredLang[i] = '\0';
}

// Case-insensitive match of the current localized-title key against the
// preferred language: "en" matches "en" and "en-US" (but not "eng").
bool OpdsJsonParser::pendingKeyIsPreferred() const {
  if (!preferredLang[0]) return false;
  size_t i = 0;
  for (; preferredLang[i]; ++i) {
    const char k = pendingKey[i];
    const char kl = static_cast<char>((k >= 'A' && k <= 'Z') ? k + 32 : k);
    if (kl != preferredLang[i]) return false;
  }
  return pendingKey[i] == '\0' || pendingKey[i] == '-';
}

size_t OpdsJsonParser::write(const uint8_t c) { return write(&c, 1); }

size_t OpdsJsonParser::write(const uint8_t* data, const size_t length) {
  if (errorOccured || (!data && length)) {
    errorOccured = true;
    return 0;
  }
  parser.feed(reinterpret_cast<const char*>(data), length);
  if (parser.hasError()) {
    errorOccured = true;
    LOG_DBG("OPDS2", "JSON parse error");
  }
  return error() ? 0 : length;
}

void OpdsJsonParser::flush() {
  // End of input: a body that ends before the root object closes is a
  // truncated feed, not a shorter one. Reject it rather than presenting the
  // partial catalog as complete; the entry limit is the one allowed cut-off.
  if (feedTruncated) return;
  if (!sawRoot || depth != 0) {
    LOG_DBG("OPDS2", "Incomplete JSON document");
    errorOccured = true;
  }
}

bool OpdsJsonParser::error() const { return errorOccured || parser.hasError(); }

void OpdsJsonParser::append(OpdsEntry&& entry) {
  if (entryCount >= entryLimit) {
    feedTruncated = true;
    return;
  }
  entries[entryCount++] = std::move(entry);
}

void OpdsJsonParser::resetLink() {
  link.href.clear();
  link.title.clear();
  link.relSearch = link.relNext = link.relPrev = link.relSelf = false;
  link.acqRank = -1;
  link.typeEpub = false;
  link.typeIndirect = false;
  link.typeLcp = false;
  link.templated = false;
  link.numberOfItems = -1;
}

void OpdsJsonParser::applyRel(const char* rel) {
  if (strcmp(rel, "search") == 0) link.relSearch = true;
  if (strcmp(rel, "next") == 0) link.relNext = true;
  if (strcmp(rel, "previous") == 0 || strcmp(rel, "prev") == 0) link.relPrev = true;
  if (strcmp(rel, "self") == 0) link.relSelf = true;
  const int rank = opdsAcquisitionRank(rel);
  if (rank > link.acqRank) link.acqRank = rank;
}

// ---- scope machine ----

OpdsJsonParser::Scope OpdsJsonParser::scopeForChild(const Scope parent, const bool isObject) const {
  switch (parent) {
    case Scope::FEED:
      if (isObject && strcmp(pendingKey, "metadata") == 0) return Scope::FEED_META;
      if (!isObject) {
        if (strcmp(pendingKey, "links") == 0) return Scope::FEED_LINKS;
        if (strcmp(pendingKey, "navigation") == 0) return Scope::NAV;
        if (strcmp(pendingKey, "publications") == 0) return Scope::PUBS;
        if (strcmp(pendingKey, "groups") == 0) return Scope::GROUPS;
        if (strcmp(pendingKey, "facets") == 0) return Scope::FACETS;
      }
      return Scope::SKIP;
    case Scope::GROUP:
      if (isObject && strcmp(pendingKey, "metadata") == 0) return Scope::GROUP_META;
      if (!isObject) {
        if (strcmp(pendingKey, "links") == 0) return Scope::GROUP_LINKS;
        if (strcmp(pendingKey, "navigation") == 0) return Scope::NAV;
        if (strcmp(pendingKey, "publications") == 0) return Scope::PUBS;
      }
      return Scope::SKIP;
    case Scope::FEED_LINKS:
      return isObject ? Scope::FEED_LINK : Scope::SKIP;
    case Scope::GROUP_LINKS:
      return isObject ? Scope::GROUP_LINK : Scope::SKIP;
    case Scope::FEED_LINK:
    case Scope::GROUP_LINK:
    case Scope::PUB_LINK:
      if (!isObject && strcmp(pendingKey, "rel") == 0) return Scope::LINK_REL;
      return Scope::SKIP;  // properties, alternate, children
    case Scope::FACET_LINK:
      if (!isObject && strcmp(pendingKey, "rel") == 0) return Scope::LINK_REL;
      if (isObject && strcmp(pendingKey, "properties") == 0) return Scope::FACET_PROPS;
      return Scope::SKIP;
    case Scope::NAV:
      return isObject ? Scope::NAV_LINK : Scope::SKIP;
    case Scope::PUBS:
      return isObject ? Scope::PUB : Scope::SKIP;
    case Scope::PUB:
      if (isObject && strcmp(pendingKey, "metadata") == 0) return Scope::PUB_META;
      if (!isObject && strcmp(pendingKey, "links") == 0) return Scope::PUB_LINKS;
      return Scope::SKIP;  // images, reading order, resources
    case Scope::PUB_META:
      if (strcmp(pendingKey, "author") == 0) return isObject ? Scope::AUTHOR : Scope::AUTHOR_ARR;
      if (isObject && strcmp(pendingKey, "title") == 0) return Scope::PUB_TITLE;
      return Scope::SKIP;  // belongsTo, subject, other contributors
    case Scope::AUTHOR_ARR:
      return isObject ? Scope::AUTHOR : Scope::SKIP;
    case Scope::PUB_LINKS:
      return isObject ? Scope::PUB_LINK : Scope::SKIP;
    case Scope::GROUPS:
      return isObject ? Scope::GROUP : Scope::SKIP;
    case Scope::FACETS:
      return isObject ? Scope::FACET : Scope::SKIP;
    case Scope::FACET:
      if (!isObject && strcmp(pendingKey, "links") == 0) return Scope::FACET_LINKS;
      return Scope::SKIP;
    case Scope::FACET_LINKS:
      return isObject ? Scope::FACET_LINK : Scope::SKIP;
    default:
      return Scope::SKIP;
  }
}

void OpdsJsonParser::endGroup() {
  // Field order in the group object is not guaranteed, so the "see all" entry
  // is applied only once the whole group has been read.
  if (!groupSelfHref.empty()) {
    OpdsEntry seeAll;
    seeAll.type = OpdsEntryType::NAVIGATION;
    const std::string& label = groupTitle.empty() ? groupSelfHref : groupTitle;
    seeAll.title.assign(label.data(), label.size());
    seeAll.href.assign(groupSelfHref.data(), groupSelfHref.size());
    append(std::move(seeAll));
  }
  groupTitle.clear();
  groupSelfHref.clear();
}

void OpdsJsonParser::onContainerStart(const bool isObject) {
  if (depth >= MAX_DEPTH) {
    errorOccured = true;
    return;
  }
  Scope next;
  if (!sawRoot) {
    sawRoot = true;
    if (!isObject) errorOccured = true;
    next = isObject ? (publicationDocument ? Scope::PUB : Scope::FEED) : Scope::SKIP;
  } else {
    if (depth == 0) {
      errorOccured = true;
      return;
    }
    next = scopeForChild(current(), isObject);
  }
  switch (next) {
    case Scope::FEED_LINK:
    case Scope::NAV_LINK:
    case Scope::PUB_LINK:
    case Scope::GROUP_LINK:
    case Scope::FACET_LINK:
      resetLink();
      break;
    case Scope::PUB:
      currentEntry = OpdsEntry{};
      currentLcp = false;
      pubAcqRank = -1;
      pubHasPlainEpub = false;
      break;
    case Scope::GROUP:
      groupTitle.clear();
      groupSelfHref.clear();
      groupPubCount = 0;
      break;
    case Scope::PUB_TITLE:
      localizedTitleLocked = false;  // new localized title object; re-arm preference
      break;
    default:
      break;
  }
  stack[depth++] = next;
  pendingKey[0] = '\0';
}

void OpdsJsonParser::onContainerEnd() {
  if (depth == 0) return;
  const Scope closed = stack[--depth];
  pendingKey[0] = '\0';
  switch (closed) {
    case Scope::FEED_LINK:
      commitFeedLink();
      break;
    case Scope::NAV_LINK:
      commitNavLink();
      break;
    case Scope::PUB_LINK:
      commitPubLink();
      break;
    case Scope::GROUP_LINK:
      commitGroupLink();
      break;
    case Scope::FACET_LINK:
      commitFacetLink();
      break;
    case Scope::PUB:
      commitPublication();
      break;
    case Scope::GROUP:
      endGroup();
      break;
    default:
      break;
  }
}

void OpdsJsonParser::onStringValue(const char* value, const size_t len) {
  switch (current()) {
    case Scope::FEED_LINK:
    case Scope::NAV_LINK:
    case Scope::PUB_LINK:
    case Scope::GROUP_LINK:
    case Scope::FACET_LINK:
      if (strcmp(pendingKey, "href") == 0) {
        assignBounded(link.href, value, len, MAX_HREF_CHARS);
      } else if (strcmp(pendingKey, "title") == 0) {
        assignBounded(link.title, value, len, MAX_TITLE_CHARS);
      } else if (strcmp(pendingKey, "rel") == 0) {
        applyRel(value);
      } else if (strcmp(pendingKey, "type") == 0) {
        if (strcmp(value, "application/epub+zip") == 0) link.typeEpub = true;
        if (strcmp(value, "application/opds-publication+json") == 0) link.typeIndirect = true;
        if (strcmp(value, "application/vnd.readium.lcp.license.v1.0+json") == 0) link.typeLcp = true;
      }
      break;
    case Scope::LINK_REL:
      applyRel(value);
      break;
    case Scope::GROUP_META:
      if (strcmp(pendingKey, "title") == 0) assignBounded(groupTitle, value, len, MAX_TITLE_CHARS);
      break;
    case Scope::PUB_META:
      if (strcmp(pendingKey, "title") == 0) {
        assignBounded(currentEntry.title, value, len, MAX_TITLE_CHARS);
      } else if (strcmp(pendingKey, "author") == 0) {
        if (currentEntry.author.empty()) assignBounded(currentEntry.author, value, len, MAX_AUTHOR_CHARS);
      } else if (strcmp(pendingKey, "identifier") == 0) {
        assignBounded(currentEntry.id, value, len, MAX_ID_CHARS);
      }
      break;
    case Scope::PUB_TITLE:
      // Localized title object: prefer the UI language's translation, otherwise
      // keep the first one seen.
      if (pendingKeyIsPreferred()) {
        assignBounded(currentEntry.title, value, len, MAX_TITLE_CHARS);
        localizedTitleLocked = true;
      } else if (!localizedTitleLocked && currentEntry.title.empty()) {
        assignBounded(currentEntry.title, value, len, MAX_TITLE_CHARS);
      }
      break;
    case Scope::AUTHOR:
      if (strcmp(pendingKey, "name") == 0 && currentEntry.author.empty()) {
        assignBounded(currentEntry.author, value, len, MAX_AUTHOR_CHARS);
      }
      break;
    case Scope::AUTHOR_ARR:
      // Array of contributor name strings: take the first.
      if (currentEntry.author.empty()) assignBounded(currentEntry.author, value, len, MAX_AUTHOR_CHARS);
      break;
    default:
      break;
  }
}

void OpdsJsonParser::onNumberValue(const char* value) {
  switch (current()) {
    case Scope::FEED_META:
      if (strcmp(pendingKey, "numberOfItems") == 0) {
        feedNumberOfItems = static_cast<int32_t>(strtol(value, nullptr, 10));
      }
      break;
    case Scope::FACET_PROPS:
      if (strcmp(pendingKey, "numberOfItems") == 0) {
        link.numberOfItems = static_cast<int32_t>(strtol(value, nullptr, 10));
      }
      break;
    default:
      break;
  }
}

// ---- commits ----

void OpdsJsonParser::commitFeedLink() {
  if (link.href.empty()) return;
  if (link.relSearch && (link.templated || link.href.find('{') != std::string::npos)) {
    if (searchTemplate.empty() && link.href.size() <= MAX_SEARCH_TEMPLATE_CHARS) searchTemplate = link.href;
    return;
  }
  if (link.relNext && nextPageUrl.empty()) nextPageUrl = link.href;
  if (link.relPrev && prevPageUrl.empty()) prevPageUrl = link.href;
}

void OpdsJsonParser::commitNavLink() {
  if (link.href.empty() || link.title.empty()) return;
  OpdsEntry entry;
  entry.type = OpdsEntryType::NAVIGATION;
  entry.title.assign(link.title.data(), link.title.size());
  entry.href.assign(link.href.data(), link.href.size());
  append(std::move(entry));
}

void OpdsJsonParser::commitGroupLink() {
  if (link.relSelf && groupSelfHref.empty()) groupSelfHref = std::move(link.href);
}

void OpdsJsonParser::commitFacetLink() {
  if (link.href.empty() || link.title.empty()) return;
  OpdsEntry entry;
  entry.type = OpdsEntryType::NAVIGATION;
  entry.title.assign(link.title.data(), link.title.size());
  entry.href.assign(link.href.data(), link.href.size());
  entry.count = link.numberOfItems;
  append(std::move(entry));
}

void OpdsJsonParser::commitPubLink() {
  // A rel="self" publication document describes the entry; it is not an acquisition.
  if (link.relSelf && link.typeIndirect) return;
  if (link.href.empty() || link.acqRank < 0) return;

  if (!(link.typeEpub || link.typeIndirect || link.typeLcp)) return;

  const bool isPlainEpub =
      link.typeEpub && (link.href.find(".epub") != std::string::npos || link.href.find("/epub/") != std::string::npos);

  // Preference order: higher acquisition rank (open-access > acquisition >
  // borrow > buy); at equal rank a direct EPUB beats an indirect link; among
  // direct EPUBs a plain .epub path beats a derived format (kepub etc.).
  bool better = currentEntry.href.empty() || link.acqRank > pubAcqRank;
  if (!better && link.acqRank == pubAcqRank) {
    if (link.typeEpub && (currentEntry.indirect || currentLcp)) {
      better = true;  // a direct EPUB beats both an indirect hop and an LCP license
    } else if (link.typeEpub && !currentEntry.indirect && isPlainEpub && !pubHasPlainEpub) {
      better = true;
    } else if (link.typeLcp && currentEntry.indirect && !currentLcp) {
      better = true;  // a fulfillable LCP license beats an opaque indirect hop
    }
  }
  if (!better) return;

  currentEntry.href.assign(link.href.data(), link.href.size());
  currentLcp = link.typeLcp;
  currentEntry.indirect = !link.typeEpub && !link.typeLcp;
  pubAcqRank = link.acqRank;
  pubHasPlainEpub = isPlainEpub;
  currentEntry.unsupportedAcquisition = currentLcp || link.acqRank == 0;
}

void OpdsJsonParser::commitPublication() {
  if (currentEntry.title.empty() || (currentEntry.href.empty() && !publicationDocument)) return;
  // Grouped publications (stack ... GROUP > PUBS > [PUB just popped]) are a
  // preview: keep only the first MAX_GROUP_PREVIEW so a large group doesn't
  // starve the rest. Top-level (flat-feed) publications are not capped.
  if (depth >= 2 && stack[depth - 2] == Scope::GROUP && ++groupPubCount > MAX_GROUP_PREVIEW) return;
  currentEntry.type = OpdsEntryType::BOOK;
  append(std::move(currentEntry));
  currentEntry = OpdsEntry{};
}

// ---- static callbacks ----

void OpdsJsonParser::sOnKey(void* ctx, const char* key, size_t len) {
  auto* self = static_cast<OpdsJsonParser*>(ctx);
  const size_t max = sizeof(self->pendingKey) - 1;
  if (len > max) len = max;
  memcpy(self->pendingKey, key, len);
  self->pendingKey[len] = '\0';
}

void OpdsJsonParser::sOnString(void* ctx, const char* value, const size_t len) {
  static_cast<OpdsJsonParser*>(ctx)->onStringValue(value, len);
}

void OpdsJsonParser::sOnNumber(void* ctx, const char* value, size_t) {
  static_cast<OpdsJsonParser*>(ctx)->onNumberValue(value);
}

void OpdsJsonParser::sOnBool(void* ctx, const bool value) {
  auto* self = static_cast<OpdsJsonParser*>(ctx);
  const Scope scope = self->current();
  if ((scope == Scope::FEED_LINK || scope == Scope::PUB_LINK) && strcmp(self->pendingKey, "templated") == 0) {
    self->link.templated = value;
  }
}

void OpdsJsonParser::sOnObjectStart(void* ctx) { static_cast<OpdsJsonParser*>(ctx)->onContainerStart(true); }
void OpdsJsonParser::sOnObjectEnd(void* ctx) { static_cast<OpdsJsonParser*>(ctx)->onContainerEnd(); }
void OpdsJsonParser::sOnArrayStart(void* ctx) { static_cast<OpdsJsonParser*>(ctx)->onContainerStart(false); }
void OpdsJsonParser::sOnArrayEnd(void* ctx) { static_cast<OpdsJsonParser*>(ctx)->onContainerEnd(); }
