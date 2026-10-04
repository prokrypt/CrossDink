#include "HttpRange.h"

#include <HalStorage.h>
#include <Logging.h>
#include <WebServer.h>

#include <cinttypes>
#include <cstdio>

void sendFileWithRange(WebServer& server, HalFile& file, const char* contentType) {
  const uint64_t size = file.size();
  uint64_t first = 0;
  uint64_t last = 0;
  const ByteRange range = parseByteRange(server.header("Range").c_str(), size, first, last);
  server.sendHeader("Accept-Ranges", "bytes");
  char contentRange[64];
  if (range == ByteRange::Unsatisfiable) {
    snprintf(contentRange, sizeof(contentRange), "bytes */%" PRIu64, size);
    server.sendHeader("Content-Range", contentRange);
    server.send(416, "text/plain", "Range not satisfiable");
    return;
  }
  uint64_t remaining = size;
  if (range == ByteRange::Ok) {
    if (!file.seek(static_cast<size_t>(first))) {
      LOG_ERR("WEB", "Range seek to %" PRIu64 " failed", first);
      server.send(500, "text/plain", "Seek failed");
      return;
    }
    remaining = last - first + 1;
    snprintf(contentRange, sizeof(contentRange), "bytes %" PRIu64 "-%" PRIu64 "/%" PRIu64, first, last, size);
    server.sendHeader("Content-Range", contentRange);
  }
  server.setContentLength(static_cast<size_t>(remaining));
  server.send(range == ByteRange::Ok ? 206 : 200, contentType, "");

  NetworkClient client = server.client();
  uint8_t buffer[4096];
  bool clientOk = true;
  while (clientOk && remaining > 0) {
    const int result = file.read(buffer, remaining < sizeof(buffer) ? static_cast<size_t>(remaining) : sizeof(buffer));
    if (result <= 0) break;
    const size_t bytesRead = static_cast<size_t>(result);
    size_t totalWritten = 0;
    while (totalWritten < bytesRead) {
      const size_t wrote = client.write(buffer + totalWritten, bytesRead - totalWritten);
      if (wrote == 0) {  // client gone
        clientOk = false;
        break;
      }
      totalWritten += wrote;
    }
    remaining -= totalWritten;
  }
#ifndef SIMULATOR
  client.clear();
#endif
}
