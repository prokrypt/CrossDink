#pragma once
#include <string>

namespace UrlUtils {

/**
 * Check if URL uses HTTPS protocol
 */
bool isHttpsUrl(const std::string& url);

/**
 * Prepend http:// if no protocol specified (server will redirect to https if needed)
 */
std::string ensureProtocol(const std::string& url);

/**
 * Extract host with protocol from URL (e.g., "http://example.com" from "http://example.com/path")
 */
std::string extractHost(const std::string& url);

/**
 * Percent-encode raw characters that esp_http_client rejects in a URL.
 */
std::string encodeUnsafeUrlChars(const std::string& url);

/**
 * Build full URL from server URL and path.
 * If path starts with /, it's an absolute path from the host root.
 * Otherwise, it's relative to the server URL.
 */
std::string buildUrl(const std::string& serverUrl, const std::string& path);

/**
 * Resolve a feed link against the document it came from (RFC 3986: a relative
 * link replaces the base's last path segment; "?q" and "../" work). Unlike
 * buildUrl, which joins a path under a directory.
 */
std::string resolveReference(const std::string& baseUrl, const std::string& reference);

// Captive-portal checks use a foreign DNS host; IP, .local and single-label hosts stay local.
bool isForeignHost(const char* hostHeader);

/**
 * For logs: the URL (or bare host) with its userinfo masked, "user:pass@" ->
 * "****:****@" ("****@" without a password), so it shows credentials were sent.
 */
std::string maskUserInfo(const std::string& url);

}  // namespace UrlUtils
