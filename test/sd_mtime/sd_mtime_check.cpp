// Host check: does replacing a file on FAT32/exFAT stamp the current time?
// Build: see run.sh. Uses SdFat over a RAM block device and a fake clock.
#include <cstdio>
#include <cstring>
#include <vector>
#include "FsLib/FsFile.h"
#include "FsLib/FsVolume.h"
#include "FatLib/FatFormatter.h"
#include "ExFatLib/ExFatFormatter.h"

static uint16_t gDate, gTime;
static void cb(uint16_t* d, uint16_t* t, uint8_t* ms) { *d = gDate; *t = gTime; *ms = 0; }

struct Out : print_t {
  size_t write(uint8_t c) override { putchar(c); return 1; }
  size_t write(const uint8_t* b, size_t n) override { fwrite(b, 1, n, stdout); return n; }
};

struct Ram : FsBlockDeviceInterface {
  std::vector<uint8_t> m;
  explicit Ram(size_t sectors) : m(sectors * 512) {}
  bool isBusy() override { return false; }
  bool readSector(uint32_t s, uint8_t* d) override { memcpy(d, &m[s * 512], 512); return true; }
  bool readSectors(uint32_t s, uint8_t* d, size_t n) override { memcpy(d, &m[s * 512], n * 512); return true; }
  uint32_t sectorCount() override { return m.size() / 512; }
  bool syncDevice() override { return true; }
  bool writeSector(uint32_t s, const uint8_t* d) override { memcpy(&m[s * 512], d, 512); return true; }
  bool writeSectors(uint32_t s, const uint8_t* d, size_t n) override { memcpy(&m[s * 512], d, n * 512); return true; }
};

static void put(FsVolume& v, const char* path, int flags) {
  FsFile f = v.open(path, flags);
  f.write("data", 4);
  f.close();
}

static int mtimeOf(FsVolume& v, const char* path) {
  FsFile f = v.open(path, O_RDONLY);
  uint16_t d = 0, t = 0;
  f.getModifyDateTime(&d, &t);
  f.close();
  return (d << 16) | t;
}

static int run(bool exfat) {
  static Out out;
  static uint8_t buf[512];
  Ram dev(exfat ? 2097152 : 262144);  // exFAT formatter needs >= 1 GB
  bool fmt = exfat ? ExFatFormatter().format(&dev, buf, &out) : FatFormatter().format(&dev, buf);
  FsVolume v;
  if (!fmt || !(v.begin(&dev, true, 1) || v.begin(&dev, true, 0))) { printf("%s format=%d mount failed\n", exfat ? "exFAT" : "FAT32", fmt); return 1; }
  FsDateTime::setCallback(cb);
  const int t1 = (FS_DATE(2026, 10, 1) << 16) | FS_TIME(8, 0, 0);
  const int t2 = (FS_DATE(2026, 10, 2) << 16) | FS_TIME(21, 0, 0);
  gDate = t1 >> 16; gTime = t1 & 0xFFFF;
  put(v, "/a.txt", O_RDWR | O_CREAT | O_TRUNC);
  put(v, "/b.txt", O_RDWR | O_CREAT | O_TRUNC);
  gDate = t2 >> 16; gTime = t2 & 0xFFFF;
  // WebDAV PUT path: temp file, remove old, rename temp into place.
  put(v, "/a.davtmp", O_RDWR | O_CREAT | O_TRUNC);
  v.remove("/a.txt");
  { FsFile t = v.open("/a.davtmp", O_RDONLY); t.rename("/a.txt"); t.close(); }
  // Direct truncating open of existing file.
  put(v, "/b.txt", O_RDWR | O_CREAT | O_TRUNC);
  int ra = mtimeOf(v, "/a.txt"), rb = mtimeOf(v, "/b.txt");
  printf("%s rename-replace %s (got %08x want %08x), trunc-replace %s (got %08x)\n", exfat ? "exFAT" : "FAT32",
         ra == t2 ? "OK" : "STALE", ra, t2, rb == t2 ? "OK" : "STALE", rb);
  return ra == t2 && rb == t2 ? 0 : 1;
}

int main() { return run(false) | run(true); }
