#include "FirmwareFlasher.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <TaskCores.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>
#include <spi_flash_mmap.h>

#include <algorithm>
#include <atomic>
#include <cstring>

#include "FirmwareBoardTag.h"
#include "OtaBootSwitch.h"

namespace firmware_flash {

namespace {
constexpr uint8_t ESP_IMAGE_MAGIC = 0xE9;
constexpr size_t MIN_FIRMWARE_SIZE = 64 * 1024;
constexpr size_t SEC = SPI_FLASH_SEC_SIZE;  // 4 KiB
constexpr size_t BLK = 64 * 1024;           // 64 KiB block-erase granularity
constexpr size_t CHUNK = 16 * 1024;
constexpr size_t SHA_TRAILER = 32;
constexpr uint8_t CHECKSUM_SEED = 0xEF;
constexpr size_t HEADER_SIZE = 24;
constexpr size_t SEG_HEADER_SIZE = 8;
}  // namespace

const char* resultName(Result r) {
  switch (r) {
    case Result::OK:
      return "OK";
    case Result::OPEN_FAIL:
      return "OPEN_FAIL";
    case Result::TOO_SMALL:
      return "TOO_SMALL";
    case Result::TOO_LARGE:
      return "TOO_LARGE";
    case Result::BAD_MAGIC:
      return "BAD_MAGIC";
    case Result::BAD_SEGMENTS:
      return "BAD_SEGMENTS";
    case Result::BAD_CHECKSUM:
      return "BAD_CHECKSUM";
    case Result::BAD_SHA:
      return "BAD_SHA";
    case Result::BAD_CHIP:
      return "BAD_CHIP";
    case Result::WRONG_BOARD:
      return "WRONG_BOARD";
    case Result::BAD_SIZE:
      return "BAD_SIZE";
    case Result::NO_PARTITION:
      return "NO_PARTITION";
    case Result::OOM:
      return "OOM";
    case Result::READ_FAIL:
      return "READ_FAIL";
    case Result::ERASE_FAIL:
      return "ERASE_FAIL";
    case Result::WRITE_FAIL:
      return "WRITE_FAIL";
    case Result::OTADATA_FAIL:
      return "OTADATA_FAIL";
  }
  return "?";
}

namespace {
// Image I/O buffer. A large DMA-capable one lets an SDMMC card fill it in one
// multi-block transfer and cuts per-read overhead on SPI cards; fall back to one
// flash sector when the heap is too tight for it.
class IoBuffer {
 public:
  IoBuffer() {
    data_ = static_cast<uint8_t*>(heap_caps_malloc(CHUNK, MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    if (data_) {
      size_ = CHUNK;
      return;
    }
    data_ = static_cast<uint8_t*>(heap_caps_malloc(SEC, MALLOC_CAP_8BIT));
    size_ = data_ ? SEC : 0;
  }
  ~IoBuffer() { heap_caps_free(data_); }
  IoBuffer(const IoBuffer&) = delete;
  IoBuffer& operator=(const IoBuffer&) = delete;

  explicit operator bool() const { return data_ != nullptr; }
  uint8_t* get() const { return data_; }
  size_t size() const { return size_; }

 private:
  uint8_t* data_ = nullptr;
  size_t size_ = 0;
};
}  // namespace

uint16_t runningPartitionChipId() {
  // Reading SPI flash is relatively expensive; the running image is immutable,
  // so cache its chip ID once per boot.
  static const uint16_t cached = [] {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running == nullptr) return static_cast<uint16_t>(0xFFFF);

    uint16_t chipId = 0xFFFF;
    if (esp_partition_read(running, 12, &chipId, sizeof(chipId)) != ESP_OK) {
      return static_cast<uint16_t>(0xFFFF);
    }
    return chipId;
  }();
  return cached;
}

namespace {
// Stream `length` bytes from `file` starting at the current read offset, feeding them through
// both the XOR-checksum and SHA256 accumulators. Used by validateImageFile so the whole image
// is verified end-to-end without holding it in RAM (ESP32-C3 only has ~380 KB).
Result feedHashAndChecksum(HalFile& file, size_t length, uint8_t* xorAccum, mbedtls_sha256_context* sha,
                           const IoBuffer& ioBuf, board_tag::Scanner* tagScanner) {
  uint8_t* const buf = ioBuf.get();
  size_t remaining = length;
  while (remaining > 0) {
    const size_t want = std::min<size_t>(ioBuf.size(), remaining);
    const int got = file.read(buf, want);
    if (got <= 0 || static_cast<size_t>(got) != want) return Result::READ_FAIL;
    if (sha) mbedtls_sha256_update(sha, buf, want);
    if (tagScanner) tagScanner->feed(buf, want);
    if (xorAccum) {
      uint8_t acc = *xorAccum;
      for (size_t i = 0; i < want; i++) acc ^= buf[i];
      *xorAccum = acc;
    }
    remaining -= want;
  }
  return Result::OK;
}
}  // namespace

Result validateOpenImageFile(HalFile& file, size_t partitionSize) {
  if (!file.seek(0)) {
    LOG_ERR("FLASH", "validate: seek failed");
    return Result::READ_FAIL;
  }
  const size_t fileSize = file.fileSize();
  if (fileSize < MIN_FIRMWARE_SIZE) {
    LOG_ERR("FLASH", "validate: too small: %u", static_cast<unsigned>(fileSize));
    return Result::TOO_SMALL;
  }
  if (partitionSize > 0 && fileSize > partitionSize) {
    LOG_ERR("FLASH", "validate: too large: %u > %u", static_cast<unsigned>(fileSize),
            static_cast<unsigned>(partitionSize));
    return Result::TOO_LARGE;
  }

  uint8_t header[HEADER_SIZE];
  if (file.read(header, HEADER_SIZE) != static_cast<int>(HEADER_SIZE)) {
    LOG_ERR("FLASH", "validate: header read failed");
    return Result::READ_FAIL;
  }
  if (header[0] != ESP_IMAGE_MAGIC) {
    LOG_ERR("FLASH", "validate: bad magic 0x%02X", header[0]);
    return Result::BAD_MAGIC;
  }
  uint16_t imageChipId;
  std::memcpy(&imageChipId, header + 12, sizeof(imageChipId));
  const uint16_t runningChipId = runningPartitionChipId();
  if (runningChipId != 0xFFFF && imageChipId != runningChipId) {
    LOG_ERR("FLASH", "validate: wrong chip: image=0x%04X device=0x%04X", imageChipId, runningChipId);
    return Result::BAD_CHIP;
  }
  const uint8_t segCount = header[1];
  const bool hashAppended = header[23] != 0;

  const IoBuffer buf;
  if (!buf) {
    return Result::OOM;
  }

  mbedtls_sha256_context shaCtx;
  mbedtls_sha256_init(&shaCtx);
  mbedtls_sha256_starts(&shaCtx, /*is224=*/0);
  mbedtls_sha256_update(&shaCtx, header, HEADER_SIZE);

  uint8_t xorAccum = CHECKSUM_SEED;
  size_t pos = HEADER_SIZE;
  // Scan the segment stream already read for integrity validation, avoiding
  // additional SD I/O or a whole-image buffer on memory-constrained X3/X4.
  board_tag::Scanner tagScanner;

  for (uint8_t i = 0; i < segCount; i++) {
    if (pos + SEG_HEADER_SIZE > fileSize) {
      LOG_ERR("FLASH", "validate: seg %u header overruns EOF at %u", i, static_cast<unsigned>(pos));
      mbedtls_sha256_free(&shaCtx);
      return Result::BAD_SEGMENTS;
    }
    uint8_t segHdr[SEG_HEADER_SIZE];
    if (file.read(segHdr, SEG_HEADER_SIZE) != static_cast<int>(SEG_HEADER_SIZE)) {
      mbedtls_sha256_free(&shaCtx);
      return Result::READ_FAIL;
    }
    mbedtls_sha256_update(&shaCtx, segHdr, SEG_HEADER_SIZE);
    pos += SEG_HEADER_SIZE;

    uint32_t dataLen;
    std::memcpy(&dataLen, segHdr + 4, sizeof(dataLen));
    if (pos + dataLen > fileSize) {
      LOG_ERR("FLASH", "validate: seg %u data overruns EOF (%u + %u > %u)", i, static_cast<unsigned>(pos),
              static_cast<unsigned>(dataLen), static_cast<unsigned>(fileSize));
      mbedtls_sha256_free(&shaCtx);
      return Result::BAD_SEGMENTS;
    }

    const Result feedRes = feedHashAndChecksum(file, dataLen, &xorAccum, &shaCtx, buf, &tagScanner);
    if (feedRes != Result::OK) {
      mbedtls_sha256_free(&shaCtx);
      return feedRes;
    }
    pos += dataLen;
  }

  if (tagScanner.mismatch()) {
    LOG_ERR("FLASH", "validate: wrong board: image=%s device=%.*s", tagScanner.foundName(),
            static_cast<int>(board_tag::boardNameLen()), board_tag::boardName());
    mbedtls_sha256_free(&shaCtx);
    return Result::WRONG_BOARD;
  }

  // pad_end is the 16-byte aligned offset at which the checksum byte sits at pad_end - 1.
  const size_t padEnd = (pos + 16) & ~static_cast<size_t>(15);
  const size_t expectedTotal = padEnd + (hashAppended ? SHA_TRAILER : 0);
  if (expectedTotal != fileSize) {
    LOG_ERR("FLASH", "validate: size mismatch body+pad=%u sha=%u expected=%u actual=%u", static_cast<unsigned>(padEnd),
            static_cast<unsigned>(hashAppended ? SHA_TRAILER : 0), static_cast<unsigned>(expectedTotal),
            static_cast<unsigned>(fileSize));
    mbedtls_sha256_free(&shaCtx);
    return Result::BAD_SIZE;
  }

  // Read the padding bytes (which include the stored checksum at the last byte) into the SHA stream.
  const size_t padLen = padEnd - pos;
  uint8_t padBuf[16];
  if (padLen > sizeof(padBuf)) {
    mbedtls_sha256_free(&shaCtx);
    return Result::BAD_SIZE;
  }
  if (padLen > 0 && file.read(padBuf, padLen) != static_cast<int>(padLen)) {
    mbedtls_sha256_free(&shaCtx);
    return Result::READ_FAIL;
  }
  mbedtls_sha256_update(&shaCtx, padBuf, padLen);

  const uint8_t storedChecksum = padBuf[padLen - 1];
  if ((xorAccum & 0xFF) != storedChecksum) {
    LOG_ERR("FLASH", "validate: checksum mismatch computed=0x%02X stored=0x%02X", xorAccum, storedChecksum);
    mbedtls_sha256_free(&shaCtx);
    return Result::BAD_CHECKSUM;
  }

  if (hashAppended) {
    uint8_t computed[SHA_TRAILER];
    mbedtls_sha256_finish(&shaCtx, computed);
    uint8_t stored[SHA_TRAILER];
    if (file.read(stored, SHA_TRAILER) != static_cast<int>(SHA_TRAILER)) {
      mbedtls_sha256_free(&shaCtx);
      return Result::READ_FAIL;
    }
    if (std::memcmp(computed, stored, SHA_TRAILER) != 0) {
      LOG_ERR("FLASH", "validate: SHA256 mismatch");
      mbedtls_sha256_free(&shaCtx);
      return Result::BAD_SHA;
    }
  }

  mbedtls_sha256_free(&shaCtx);
  if (!file.seek(0)) {
    LOG_ERR("FLASH", "validate: rewind failed");
    return Result::READ_FAIL;
  }
  return Result::OK;
}

Result validateImageFile(const char* sdPath, size_t partitionSize) {
  HalFile file;
  if (!Storage.openFileForRead("FLASH", sdPath, file) || !file) {
    LOG_ERR("FLASH", "validate: open failed: %s", sdPath);
    return Result::OPEN_FAIL;
  }
  const Result result = validateOpenImageFile(file, partitionSize);
  file.close();
  return result;
}

namespace {
// Double-buffered read-ahead: a reader task fills one I/O buffer from the SD
// card while the caller erases and programs the other. Flash operations stall
// the cache on both cores, so the overlap is partial, but the card's DMA and
// command turnaround no longer sit between every write. Falls back to plain
// synchronous reads when the second buffer or the task can't be created.
class ChunkSource {
 public:
  ChunkSource(HalFile& file, const size_t total) : file_(file), total_(total) {}
  ~ChunkSource() { stop(); }
  ChunkSource(const ChunkSource&) = delete;
  ChunkSource& operator=(const ChunkSource&) = delete;

  bool begin() {
    if (!bufs_[0]) return false;
    chunk_ = bufs_[0].size();
    if (!bufs_[1] || bufs_[1].size() != chunk_) return true;
    for (int i = 0; i < 2; ++i) {
      free_[i] = xSemaphoreCreateBinary();
      full_[i] = xSemaphoreCreateBinary();
    }
    done_ = xSemaphoreCreateBinary();
    const bool semsOk = free_[0] && free_[1] && full_[0] && full_[1] && done_;
    if (semsOk) {
      xSemaphoreGive(free_[0]);
      xSemaphoreGive(free_[1]);
      async_ = xTaskCreatePinnedToCore(readTask, "FwRead", 3072, this, 1, nullptr, TaskCores::kUi) == pdPASS;
    }
    if (async_) {
      LOG_INF("FLASH", "read-ahead on (2 x %u bytes)", static_cast<unsigned>(chunk_));
    } else {
      LOG_ERR("FLASH", "read-ahead unavailable; reading synchronously");
      deleteSems();
    }
    return true;
  }

  // Next chunk (chunk_ bytes, less at the end), or nullptr on a read failure.
  const uint8_t* next(size_t& len) {
    if (!async_) {
      len = std::min(chunk_, total_ - pos_);
      const int read = file_.read(bufs_[0].get(), len);
      if (read <= 0 || static_cast<size_t>(read) != len) {
        LOG_ERR("FLASH", "read @%u: got=%d want=%u", static_cast<unsigned>(pos_), read, static_cast<unsigned>(len));
        return nullptr;
      }
      pos_ += len;
      return bufs_[0].get();
    }
    xSemaphoreTake(full_[idx_], portMAX_DELAY);
    if (!ok_[idx_]) return nullptr;
    len = len_[idx_];
    return bufs_[idx_].get();
  }

  // The chunk from next() has been written; its buffer may be refilled.
  void release() {
    if (!async_) return;
    xSemaphoreGive(free_[idx_]);
    idx_ ^= 1;
  }

  // Stops the reader task (if any) and waits for it to let go of the file.
  void stop() {
    if (!async_) return;
    stopping_.store(true, std::memory_order_release);
    xSemaphoreGive(free_[0]);
    xSemaphoreGive(free_[1]);
    xSemaphoreTake(done_, portMAX_DELAY);
    async_ = false;
    deleteSems();
  }

 private:
  static void readTask(void* arg) {
    static_cast<ChunkSource*>(arg)->readLoop();
    vTaskDelete(nullptr);
  }

  void readLoop() {
    size_t pos = 0;
    int i = 0;
    while (pos < total_) {
      xSemaphoreTake(free_[i], portMAX_DELAY);
      if (stopping_.load(std::memory_order_acquire)) break;
      const size_t want = std::min(chunk_, total_ - pos);
      const int read = file_.read(bufs_[i].get(), want);
      ok_[i] = read > 0 && static_cast<size_t>(read) == want;
      len_[i] = want;
      if (!ok_[i]) {
        LOG_ERR("FLASH", "read @%u: got=%d want=%u", static_cast<unsigned>(pos), read, static_cast<unsigned>(want));
      }
      xSemaphoreGive(full_[i]);
      if (!ok_[i]) break;
      pos += want;
      i ^= 1;
    }
    // Last touch of `this`: stop() may destroy the object once this is given.
    xSemaphoreGive(done_);
  }

  void deleteSems() {
    for (int i = 0; i < 2; ++i) {
      if (free_[i]) vSemaphoreDelete(free_[i]);
      if (full_[i]) vSemaphoreDelete(full_[i]);
      free_[i] = full_[i] = nullptr;
    }
    if (done_) vSemaphoreDelete(done_);
    done_ = nullptr;
  }

  HalFile& file_;
  const size_t total_;
  IoBuffer bufs_[2];
  size_t chunk_ = 0;
  size_t pos_ = 0;  // synchronous mode only
  int idx_ = 0;
  bool async_ = false;
  std::atomic<bool> stopping_{false};
  SemaphoreHandle_t free_[2] = {};
  SemaphoreHandle_t full_[2] = {};
  SemaphoreHandle_t done_ = nullptr;
  size_t len_[2] = {};
  bool ok_[2] = {};
};

bool allErased(const uint8_t* data, const size_t len) {
  for (size_t i = 0; i < len; ++i) {
    if (data[i] != 0xFF) return false;
  }
  return true;
}

// True when flash reads back all 0xFF over [offset, offset + len): NOR erase
// only sets bits to 1 and programming only clears them, so such a range is
// already in the erased state. Stops at the first programmed byte, so a used
// block costs one 4 KiB read.
bool rangeErased(const esp_partition_t* dest, const size_t offset, const size_t len, uint8_t* scratch) {
  for (size_t done = 0; done < len; done += SEC) {
    const size_t n = std::min(SEC, len - done);
    if (esp_partition_read(dest, offset + done, scratch, n) != ESP_OK) return false;
    if (!allErased(scratch, n)) return false;
  }
  return true;
}

// Erase + write `file` into `dest`, interleaved. With `sha`, also hashes every
// byte before the final 32 (the appended SHA-256 trailer) as it streams.
Result writeImage(HalFile& file, const esp_partition_t* dest, ProgressCb onProgress, void* ctx,
                  mbedtls_sha256_context* sha) {
  const size_t firmwareSize = file.fileSize();
  LOG_INF("FLASH", "open image size=%u dest=%s @0x%x partsize=%u", static_cast<unsigned>(firmwareSize), dest->label,
          static_cast<unsigned>(dest->address), static_cast<unsigned>(dest->size));
  if (!file.seek(0)) {
    LOG_ERR("FLASH", "seek before flash failed");
    return Result::READ_FAIL;
  }

  ChunkSource source(file, firmwareSize);
  if (!source.begin()) {
    LOG_ERR("FLASH", "OOM");
    return Result::OOM;
  }
  // Blank skips compare plaintext 0xFF against raw flash, which only holds on
  // an unencrypted partition. Without the scratch buffer every block is erased.
  const bool blankSkips = !dest->encrypted;
  auto scratch = blankSkips ? makeUniqueNoThrow<uint8_t[]>(SEC) : nullptr;
  unsigned skippedErases = 0;
  unsigned skippedChunks = 0;

  const size_t hashedSize = firmwareSize > SHA_TRAILER_BYTES ? firmwareSize - SHA_TRAILER_BYTES : 0;
  // Interleave erase + write so the progress bar advances 0→100% smoothly
  // rather than stalling for several seconds during a single up-front erase.
  size_t streamPos = 0;
  size_t erasedUpto = 0;
  while (streamPos < firmwareSize) {
    size_t len = 0;
    const uint8_t* data = source.next(len);
    if (!data) return Result::READ_FAIL;

    // Chunks are a fixed power-of-two size from offset 0, so one never spans
    // an erase boundary; the loop still covers the whole write if it did.
    while (erasedUpto < streamPos + len) {
      size_t eraseLen = std::min<size_t>(BLK, dest->size - erasedUpto);
      eraseLen = (eraseLen + SEC - 1) & ~(SEC - 1);
      eraseLen = std::min<size_t>(eraseLen, dest->size - erasedUpto);
      if (scratch && rangeErased(dest, erasedUpto, eraseLen, scratch.get())) {
        ++skippedErases;
      } else if (esp_partition_erase_range(dest, erasedUpto, eraseLen) != ESP_OK) {
        LOG_ERR("FLASH", "erase @%u (len=%u) failed", static_cast<unsigned>(erasedUpto),
                static_cast<unsigned>(eraseLen));
        return Result::ERASE_FAIL;
      }
      erasedUpto += eraseLen;
      // Once per 64 KiB block: lets the idle task and render task run without
      // paying a tick per chunk.
      delay(1);
    }

    if (sha && streamPos < hashedSize) {
      mbedtls_sha256_update(sha, data, std::min(len, hashedSize - streamPos));
    }
    // The range is erased (all 0xFF), so programming 0xFF would change nothing.
    if (blankSkips && allErased(data, len)) {
      ++skippedChunks;
    } else if (esp_partition_write(dest, streamPos, data, len) != ESP_OK) {
      LOG_ERR("FLASH", "write @%u failed", static_cast<unsigned>(streamPos));
      return Result::WRITE_FAIL;
    }
    source.release();
    streamPos += len;
    if (onProgress) onProgress(streamPos, firmwareSize, ctx);
  }
  source.stop();
  LOG_INF("FLASH", "written: %u blank blocks not erased, %u blank chunks not programmed", skippedErases, skippedChunks);
  return Result::OK;
}
}  // namespace

Result flashValidatedFile(HalFile& file, ProgressCb onProgress, void* ctx) {
  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  if (!dest) {
    LOG_ERR("FLASH", "no next-update partition");
    return Result::NO_PARTITION;
  }
  const Result written = writeImage(file, dest, onProgress, ctx, nullptr);
  if (written != Result::OK) return written;
  if (!ota_boot::switchTo(dest)) {
    LOG_ERR("FLASH", "otadata switch failed");
    return Result::OTADATA_FAIL;
  }
  return Result::OK;
}

bool readShaTrailer(const char* sdPath, size_t& size, uint8_t sha[SHA_TRAILER_BYTES]) {
  HalFile file;
  if (!Storage.openFileForRead("FLASH", sdPath, file) || !file) {
    LOG_ERR("FLASH", "open failed: %s", sdPath);
    return false;
  }
  size = file.fileSize();
  // esp_image_header_t.hash_appended (byte 23): only such images end in a SHA-256.
  constexpr size_t HASH_APPENDED_OFFSET = 23;
  uint8_t header[HASH_APPENDED_OFFSET + 1];
  const bool hashed =
      file.read(header, sizeof(header)) == static_cast<int>(sizeof(header)) && header[HASH_APPENDED_OFFSET] == 1;
  const bool ok = hashed && size > SHA_TRAILER_BYTES && file.seek(size - SHA_TRAILER_BYTES) &&
                  file.read(sha, SHA_TRAILER_BYTES) == static_cast<int>(SHA_TRAILER_BYTES);
  file.close();
  if (!ok) LOG_INF("FLASH", "no SHA-256 trailer to pin (hash_appended=%d): %s", hashed ? 1 : 0, sdPath);
  return ok;
}

Result flashConfirmedFile(const char* sdPath, const size_t expectedSize, const uint8_t expectedSha[SHA_TRAILER_BYTES],
                          ProgressCb onProgress, void* ctx) {
  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  if (!dest) {
    LOG_ERR("FLASH", "no next-update partition");
    return Result::NO_PARTITION;
  }
  HalFile file;
  if (!Storage.openFileForRead("FLASH", sdPath, file) || !file) {
    LOG_ERR("FLASH", "open failed: %s", sdPath);
    return Result::OPEN_FAIL;
  }
  const size_t size = file.fileSize();
  uint8_t trailer[SHA_TRAILER_BYTES];
  if (size != expectedSize || size > dest->size || !file.seek(size - SHA_TRAILER_BYTES) ||
      file.read(trailer, SHA_TRAILER_BYTES) != static_cast<int>(SHA_TRAILER_BYTES) ||
      memcmp(trailer, expectedSha, SHA_TRAILER_BYTES) != 0) {
    LOG_ERR("FLASH", "file changed since validation: %s (size %u, expected %u)", sdPath, static_cast<unsigned>(size),
            static_cast<unsigned>(expectedSize));
    file.close();
    return Result::BAD_SHA;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, /*is224=*/0);
  const Result written = writeImage(file, dest, onProgress, ctx, &sha);
  file.close();
  uint8_t digest[SHA_TRAILER_BYTES];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);
  if (written != Result::OK) return written;
  // The bytes just written hash to the trailer validateImageFile() accepted,
  // so the partition holds exactly the image that passed the full check.
  if (memcmp(digest, expectedSha, SHA_TRAILER_BYTES) != 0) {
    LOG_ERR("FLASH", "written image SHA-256 mismatch; otadata left unchanged");
    return Result::BAD_SHA;
  }
  if (!ota_boot::switchTo(dest)) {
    LOG_ERR("FLASH", "otadata switch failed");
    return Result::OTADATA_FAIL;
  }
  return Result::OK;
}

Result flashFromSdPath(const char* sdPath, ProgressCb onProgress, void* ctx) {
  // Resolve destination first so validation can enforce the OTA partition
  // limit before the same open file is used for the write pass.
  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  if (!dest) {
    LOG_ERR("FLASH", "no next-update partition");
    return Result::NO_PARTITION;
  }

  HalFile file;
  if (!Storage.openFileForRead("FLASH", sdPath, file) || !file) {
    LOG_ERR("FLASH", "open failed: %s", sdPath);
    return Result::OPEN_FAIL;
  }

  const Result validateRes = validateOpenImageFile(file, dest->size);
  if (validateRes != Result::OK) {
    LOG_ERR("FLASH", "image validation failed: %s", resultName(validateRes));
    file.close();
    return validateRes;
  }
  const Result result = flashValidatedFile(file, onProgress, ctx);
  file.close();
  return result;
}

}  // namespace firmware_flash
