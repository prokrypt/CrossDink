#pragma once

#include <cstddef>

// Strips secrets from one log line before a bug report writes it to SD.
// Pure string code (no Arduino), so the native test covers it.
namespace BugReportSanitize {

// A known secret (saved Wi-Fi SSID/password, OPDS or KOSync login, remote
// token) and the tag that replaces it. Values shorter than MIN_SECRET_LEN are
// skipped: replacing "a" or "ok" everywhere would wreck the log.
struct Secret {
  const char* value;
  const char* tag;
};
constexpr size_t MIN_SECRET_LEN = 4;

// Writes the sanitized form of in[0..len) into out (always NUL-terminated,
// cut at cap - 1) and returns its length. Replaces, in order: known secrets,
// URL userinfo (//user:pass@ -> //[auth]@), values of token/key/pass/auth/sig/
// secret query or key=value pairs, Authorization / X-Token header values,
// MAC addresses, and public IPv4 addresses (LAN, loopback and link-local kept).
size_t sanitizeLine(const char* in, size_t len, char* out, size_t cap, const Secret* secrets, size_t secretCount);

}  // namespace BugReportSanitize
