#include "HttpRange.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <WebServer.h>

#include <cinttypes>
#include <cstdio>

#ifndef SIMULATOR
#include <lwip/sockets.h>

#include <algorithm>
#include <cerrno>

namespace {
// Downloads handed to the serving task's loop (pumpFileStreams), so a long
// GET no longer holds every other request until it ends. Serving task only.
struct FileStream {
  NetworkClient client;  // fd() < 0: free slot
  HalFile file;
  uint64_t remaining = 0;          // file bytes not read yet
  std::unique_ptr<uint8_t[]> buf;  // read, not yet sent: [off, len)
  size_t off = 0;
  size_t len = 0;
  unsigned long lastSendMs = 0;
};
constexpr int FILE_STREAMS = 2;  // two clients stream at once; a third waits its turn (blocking send)
constexpr size_t CHUNK = 4096;
constexpr int CHUNKS_PER_PASS = 4;         // bounds one pass, so requests between passes stay quick
constexpr unsigned long STALL_MS = 30000;  // a client that reads nothing this long is dropped
FileStream streams[FILE_STREAMS];

void closeStream(FileStream& s, const char* why) {
  LOG_INF("WEB", "download fd %d closed: %s, %" PRIu64 " bytes unread", s.client.fd(), why,
          s.remaining + (s.len - s.off));
  // Read what the client sent: closing over unread bytes sends a TCP reset.
  while (s.buf && lwip_recv(s.client.fd(), s.buf.get(), CHUNK, MSG_DONTWAIT) > 0) {
  }
  s.client.stop();
  s.file.close();
  s.buf.reset();
  s.off = s.len = 0;
}

// Takes over the client and file once the headers are out. False: no free slot
// or no buffer, and the caller sends the body itself.
bool startStream(const NetworkClient& client, HalFile& file, const uint64_t remaining) {
  for (FileStream& s : streams) {
    if (s.client.fd() >= 0) continue;
    s.buf = makeUniqueNoThrow<uint8_t[]>(CHUNK);  // 4 KB per download; PSRAM on device (above the 1 KB internal limit)
    if (!s.buf) {
      LOG_ERR("WEB", "download stream: no %u-byte buffer, sending inline", static_cast<unsigned>(CHUNK));
      return false;
    }
    s.client = client;  // WebServer drops its copy after the handler; ours keeps the socket
    s.file = std::move(file);
    s.remaining = remaining;
    s.lastSendMs = millis();
    LOG_DBG("WEB", "download fd %d streaming %" PRIu64 " bytes", s.client.fd(), remaining);
    return true;
  }
  return false;
}
}  // namespace

bool pumpFileStreams() {
  bool progressed = false;
  for (FileStream& s : streams) {
    const int fd = s.client.fd();
    if (fd < 0) continue;
    for (int i = 0; i < CHUNKS_PER_PASS; i++) {
      if (s.off == s.len) {
        if (s.remaining == 0) {
          closeStream(s, "done");
          break;
        }
        const int got = s.file.read(s.buf.get(), static_cast<size_t>(std::min<uint64_t>(s.remaining, CHUNK)));
        if (got <= 0) {
          LOG_ERR("WEB", "download fd %d: SD read failed", fd);
          closeStream(s, "read failed");
          break;
        }
        s.off = 0;
        s.len = static_cast<size_t>(got);
        s.remaining -= s.len;
      }
      const int sent = lwip_send(fd, s.buf.get() + s.off, s.len - s.off, MSG_DONTWAIT);
      if (sent < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
          closeStream(s, "client gone");
        } else if (millis() - s.lastSendMs > STALL_MS) {
          closeStream(s, "client stalled");
        }
        break;
      }
      s.off += static_cast<size_t>(sent);
      s.lastSendMs = millis();
      progressed = true;
      if (s.off < s.len) break;  // socket full: the rest next pass
    }
  }
  return progressed;
}

void closeFileStreams() {
  for (FileStream& s : streams) {
    if (s.client.fd() >= 0) closeStream(s, "server stopped");
  }
}
#endif

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
#ifndef SIMULATOR
  if (remaining > 0 && startStream(server.client(), file, remaining)) return;
#endif

  // Heap, not stack: a 4 KB local here left the 8 KB server task 440 B free.
  constexpr size_t BUFFER_SIZE = 4096;
  const auto buffer = makeUniqueNoThrow<uint8_t[]>(BUFFER_SIZE);
  if (!buffer) {
    LOG_ERR("WEB", "download: no %u-byte buffer", static_cast<unsigned>(BUFFER_SIZE));
    return;
  }
  NetworkClient client = server.client();
  bool clientOk = true;
  while (clientOk && remaining > 0) {
    const int result = file.read(buffer.get(), remaining < BUFFER_SIZE ? static_cast<size_t>(remaining) : BUFFER_SIZE);
    if (result <= 0) break;
    const size_t bytesRead = static_cast<size_t>(result);
    size_t totalWritten = 0;
    while (totalWritten < bytesRead) {
      const size_t wrote = client.write(buffer.get() + totalWritten, bytesRead - totalWritten);
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
