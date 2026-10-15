#include "BugReportSanitize.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>

namespace BugReportSanitize {
namespace {

struct Out {
  char* buf;
  size_t cap;
  size_t n = 0;
  void put(const char c) {
    if (n + 1 < cap) buf[n++] = c;
  }
  void put(const char* s, size_t len) {
    for (size_t i = 0; i < len; ++i) put(s[i]);
  }
  void put(const char* s) { put(s, strlen(s)); }
};

bool isWordChar(const char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'; }
bool isHex(const char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }
bool isDigit(const char c) { return c >= '0' && c <= '9'; }

bool startsWithNoCase(const char* s, const size_t len, const char* prefix) {
  const size_t n = strlen(prefix);
  if (len < n) return false;
  for (size_t i = 0; i < n; ++i) {
    if (std::tolower(static_cast<unsigned char>(s[i])) != prefix[i]) return false;
  }
  return true;
}

bool equalsNoCase(const char* s, const size_t len, const char* word) {
  return strlen(word) == len && startsWithNoCase(s, len, word);
}

// key=value names whose value is masked. Exact names: "spine=3" or "keys=2" must survive.
// No "pin": GPIO dumps log pin=<register>; the remote PIN is a known secret.
constexpr const char* SECRET_KEYS[] = {"token", "key",  "apikey",    "api_key", "pass",   "passwd",  "password",
                                       "pwd",   "auth", "signature", "sig",     "secret", "userkey", "psk"};
// Header names whose value (to end of line) is masked.
constexpr const char* SECRET_HEADERS[] = {"authorization:", "x-token:", "x-auth-key:", "x-auth-user:", "cookie:"};

bool isSecretKey(const char* s, const size_t len) {
  return std::any_of(std::begin(SECRET_KEYS), std::end(SECRET_KEYS),
                     [&](const char* k) { return equalsNoCase(s, len, k); });
}

// MAC "aa:bb:cc:dd:ee:ff" at in[i]; length 17 when it matches.
bool macAt(const char* in, const size_t len, const size_t i) {
  if (i + 17 > len) return false;
  if (i > 0 && (isHex(in[i - 1]) || in[i - 1] == ':')) return false;
  for (size_t k = 0; k < 17; ++k) {
    const bool sep = k % 3 == 2;
    if (sep ? in[i + k] != ':' : !isHex(in[i + k])) return false;
  }
  return i + 17 == len || !(isHex(in[i + 17]) || in[i + 17] == ':');
}

// Dotted IPv4 at in[i]; returns its length (0 = none) and the four octets.
size_t ipv4At(const char* in, const size_t len, const size_t i, int (&oct)[4]) {
  if (i > 0 && (isDigit(in[i - 1]) || in[i - 1] == '.')) return 0;
  size_t j = i;
  for (int part = 0; part < 4; ++part) {
    if (part > 0) {
      if (j >= len || in[j] != '.') return 0;
      ++j;
    }
    int value = 0;
    size_t digits = 0;
    while (j < len && isDigit(in[j]) && digits < 4) {
      value = value * 10 + (in[j] - '0');
      ++j;
      ++digits;
    }
    if (digits == 0 || digits > 3 || value > 255) return 0;
    oct[part] = value;
  }
  // "1.2.3.4.5" is not an address; a sentence-ending "." is fine.
  if (j < len && (isDigit(in[j]) || (in[j] == '.' && j + 1 < len && isDigit(in[j + 1])))) return 0;
  return j - i;
}

bool isPrivateIp(const int (&o)[4]) {
  return o[0] == 10 || o[0] == 127 || o[0] == 0 || (o[0] == 192 && o[1] == 168) ||
         (o[0] == 172 && o[1] >= 16 && o[1] <= 31) || (o[0] == 169 && o[1] == 254) ||
         (o[0] == 255 && o[1] == 255 && o[2] == 255 && o[3] == 255);
}

bool isValueEnd(const char c) {
  return c == '&' || c == ' ' || c == '\t' || c == '"' || c == '\'' || c == ',' || c == ';' || c == ')' || c == '\r' ||
         c == '\n';
}

}  // namespace

size_t sanitizeLine(const char* in, const size_t len, char* out, const size_t cap, const Secret* secrets,
                    const size_t secretCount) {
  if (cap == 0) return 0;
  Out o{out, cap};
  size_t i = 0;
  while (i < len) {
    // 1. Known secrets.
    bool replaced = false;
    for (size_t s = 0; s < secretCount && !replaced; ++s) {
      const size_t vlen = secrets[s].value ? strlen(secrets[s].value) : 0;
      if (vlen >= MIN_SECRET_LEN && vlen <= len - i && memcmp(in + i, secrets[s].value, vlen) == 0) {
        o.put(secrets[s].tag);
        i += vlen;
        replaced = true;
      }
    }
    if (replaced) continue;

    // 2. URL userinfo: scheme://user:pass@host -> scheme://[auth]@host.
    if (len - i >= 3 && memcmp(in + i, "://", 3) == 0) {
      size_t end = i + 3;
      size_t at = 0;
      while (end < len && in[end] != '/' && in[end] != ' ' && in[end] != '"' && in[end] != '\'' && in[end] != '\n') {
        if (in[end] == '@') at = end;
        ++end;
      }
      if (at) {
        o.put("://[auth]@");
        i = at + 1;
        continue;
      }
    }

    const bool wordStart = i == 0 || !isWordChar(in[i - 1]);
    if (wordStart) {
      // 3. Secret header values, to the end of the line.
      const char* const* header = std::find_if(std::begin(SECRET_HEADERS), std::end(SECRET_HEADERS),
                                               [&](const char* h) { return startsWithNoCase(in + i, len - i, h); });
      if (header != std::end(SECRET_HEADERS)) {
        o.put(in + i, strlen(*header));
        o.put(" [x]");
        i += strlen(*header);
        while (i < len && in[i] != '\r' && in[i] != '\n') ++i;
        continue;
      }

      // 4. key=value and JSON "key":"value" with a secret key name.
      size_t j = i;
      while (j < len && isWordChar(in[j])) ++j;
      if (j > i && isSecretKey(in + i, j - i)) {
        size_t value = 0;
        if (j < len && in[j] == '=') {
          value = j + 1;
        } else if (j + 2 < len && in[j] == '"' && in[j + 1] == ':') {
          value = j + 2;
          while (value < len && in[value] == ' ') ++value;
          if (value < len && in[value] == '"') ++value;
        }
        if (value) {
          o.put(in + i, value - i);
          o.put("[x]");
          i = value;
          while (i < len && !isValueEnd(in[i])) ++i;
          continue;
        }
      }
    }

    // 5. MAC / BSSID.
    if (isHex(in[i]) && macAt(in, len, i)) {
      o.put("[mac]");
      i += 17;
      continue;
    }

    // 6. Public IPv4 (LAN addresses help debugging and stay).
    if (isDigit(in[i])) {
      int oct[4];
      const size_t n = ipv4At(in, len, i, oct);
      if (n) {
        if (isPrivateIp(oct)) {
          o.put(in + i, n);
        } else {
          o.put("[ip]");
        }
        i += n;
        continue;
      }
    }

    o.put(in[i]);
    ++i;
  }
  out[o.n] = '\0';
  return o.n;
}

}  // namespace BugReportSanitize
