#pragma once

#include <cstddef>
#include <string>

namespace StringUtils {

constexpr size_t kDefaultMaxFilenameBytes = 150;

/**
 * Sanitize a string for use as a filename.
 * Replaces invalid characters with underscores, trims spaces/dots,
 * and limits length to maxBytes bytes.
 */
std::string sanitizeFilename(const std::string& name, size_t maxBytes = kDefaultMaxFilenameBytes);
// "1.5 MB", "640 KB" or "12 B" for download progress; buffers of 16 bytes fit any size_t.
void formatByteSize(size_t bytes, char* out, size_t capacity);

}  // namespace StringUtils
