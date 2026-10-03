#include <ZipFile.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) {                                              \
      fprintf(stderr, "Failed line %d: %s\n", __LINE__, #x); \
      exit(1);                                               \
    }                                                        \
  } while (0)
struct Output : Print {
  size_t count = 0, limit = 99999;
  size_t write(uint8_t) override {
    if (count == limit) return 0;
    ++count;
    return 1;
  }
};
int main() {
  std::ifstream f(GOLDEN_DIR "/stored-vs-deflated.zip", std::ios::binary);
  CHECK(f.good());
  Storage.put("test.epub", {std::istreambuf_iterator<char>(f), {}});
  Output stored;
  CHECK(ZipFile("test.epub").readStoredFileToStream("stored.pxc2", stored));
  CHECK(stored.count > 32);
  Output deflated;
  CHECK(!ZipFile("test.epub").readStoredFileToStream("deflated.pxc2", deflated));
  CHECK(deflated.count == 0);
  CHECK(InflateStream::initCalls == 0);
  Output failed;
  failed.limit = 19;
  CHECK(!ZipFile("test.epub").readStoredFileToStream("stored.pxc2", failed));
  CHECK(!ZipFile("test.epub").readStoredFileToStream("absent.pxc2", failed));
  puts("Stored-only ZIP rejects deflated PXC2 without initializing an inflater");
  // Central-directory table: binary search answers both entries and misses,
  // and a changed file (size differs) rebuilds it instead of reusing offsets.
  size_t size = 0;
  CHECK(ZipFile("test.epub").getInflatedFileSize("stored.pxc2", &size) && size == 2139);
  CHECK(ZipFile("test.epub").getInflatedFileSize("deflated.pxc2", &size) && size == 2139);
  CHECK(!ZipFile("test.epub").getInflatedFileSize("stored.pxc", &size));
  std::vector<uint8_t> grown = Storage.bytes("test.epub");
  grown.resize(grown.size() + 64, 0);  // a size change rebuilds; EOCD is still found in the last 1 KB
  Storage.put("test.epub", grown);
  CHECK(ZipFile("test.epub").getInflatedFileSize("stored.pxc2", &size) && size == 2139);
  Storage.put("other.epub", grown);
  CHECK(ZipFile("other.epub").getInflatedFileSize("deflated.pxc2", &size) && size == 2139);
  Output again;
  CHECK(ZipFile("test.epub").readStoredFileToStream("stored.pxc2", again) && again.count == stored.count);
  puts("Central-directory table finds entries, rejects misses, and rebuilds per file");
}
