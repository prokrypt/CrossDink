#include "SdWriteBehind.h"

#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR)

#include <Logging.h>
#include <PerfLog.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstring>

bool SdWriteBehind::begin(const WriteFn fn, void* const ctx, const char* const taskName, const BaseType_t core) {
  writeFn = fn;
  writeCtx = ctx;
  name = taskName;
  for (auto& buf : bufs) {
    buf = static_cast<uint8_t*>(heap_caps_malloc(BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  work = xSemaphoreCreateBinary();
  idle = xSemaphoreCreateBinary();
  failed = false;
  quit = false;
  fillLen = 0;
  fill = 0;
  // 4 KB internal stack: SdFat writes only, never TLS.
  if (!bufs[0] || !bufs[1] || !work || !idle ||
      xTaskCreatePinnedToCore(&taskMain, taskName, 4096, this, 1, &task, core) != pdPASS) {
    LOG_ERR("SDW", "%s: write-behind unavailable; writing directly", taskName);
    task = nullptr;
    release();
    return false;
  }
  xSemaphoreGive(idle);  // writer starts idle
  return true;
}

bool SdWriteBehind::append(const uint8_t* data, size_t len) {
  while (len > 0) {
    const size_t n = std::min(len, BUF_BYTES - fillLen);
    memcpy(bufs[fill] + fillLen, data, n);
    fillLen += n;
    data += n;
    len -= n;
    if (fillLen == BUF_BYTES && !submit()) return false;
  }
  return !failed;
}

bool SdWriteBehind::finish() {
  if (!active()) return true;
  bool ok = fillLen == 0 || submit();
  stopTask();
  ok = ok && !failed;
  release();
  return ok;
}

void SdWriteBehind::abort() {
  if (!active()) return;
  stopTask();
  release();
}

bool SdWriteBehind::submit() {
  xSemaphoreTake(idle, portMAX_DELAY);  // previous buffer written
  if (failed) return false;
  pendingBuf = bufs[fill];
  pendingLen = fillLen;
  fill ^= 1;
  fillLen = 0;
  xSemaphoreGive(work);
  return true;
}

void SdWriteBehind::stopTask() {
  xSemaphoreTake(idle, portMAX_DELAY);
  quit = true;
  xSemaphoreGive(work);
  xSemaphoreTake(idle, portMAX_DELAY);  // the task gives it once more as it exits
  task = nullptr;
}

void SdWriteBehind::release() {
  for (auto& buf : bufs) {
    heap_caps_free(buf);
    buf = nullptr;
  }
  if (work) vSemaphoreDelete(work);
  if (idle) vSemaphoreDelete(idle);
  work = nullptr;
  idle = nullptr;
}

void SdWriteBehind::taskMain(void* param) {
  auto* self = static_cast<SdWriteBehind*>(param);
  for (;;) {
    xSemaphoreTake(self->work, portMAX_DELAY);
    if (self->quit) break;
    if (!self->failed && !self->writeFn(self->writeCtx, self->pendingBuf, self->pendingLen)) {
      self->failed = true;
    }
    xSemaphoreGive(self->idle);
  }
  PerfLog::noteTaskExit(self->name);
  xSemaphoreGive(self->idle);
  vTaskDelete(nullptr);
}

#endif
