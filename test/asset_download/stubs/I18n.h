#pragma once
// Only the source contract validation is substituted; that parser has its own tests.
#include "../../../lib/I18n/I18n.h"
inline bool sourceValid = true;
struct I18nFake {
  language_cache::Result validateSource(const char*, const char*, const char*, const char*, void* context,
                                        bool (*proceed)(void*)) const {
    return !proceed(context) ? language_cache::Result::Cancelled
           : sourceValid     ? language_cache::Result::Ok
                             : language_cache::Result::Invalid;
  }
};
inline I18nFake assetI18n;
#undef I18N
#define I18N assetI18n
