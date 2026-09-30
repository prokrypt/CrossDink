#include "BuildScratch.h"

#include <Logging.h>

#include <atomic>

#if defined(ESP_PLATFORM)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace buildscratch {
namespace {
uint8_t* block = nullptr;
size_t blockLen = 0;
// atomic exchange so an opportunistic claim from another task can never
// double-hand-out the block (single core, but FreeRTOS preempts).
std::atomic<bool> claimed{false};
// Only the lending task and the one helper it adopted may claim: any other
// task (a background cover/image worker) would decode into the framebuffer
// while the lender redraws it after reclaim().
std::atomic<const void*> lender{nullptr};
std::atomic<const void*> helper{nullptr};

bool callerMayClaim() {
  const void* me = self();
  return me && (me == lender.load() || me == helper.load());
}
}  // namespace

const void* self() {
#if defined(ESP_PLATFORM)
  return xTaskGetCurrentTaskHandle();
#else
  thread_local char tag;
  return &tag;
#endif
}

void lend(uint8_t* buf, const size_t len) {
  if (block) {
    LOG_ERR("SCR", "Build scratch lent twice; ignoring second lend");
    return;
  }
  block = buf;
  blockLen = len;
  claimed.store(false);
  helper.store(nullptr);
  lender.store(self());
}

void reclaim() {
  lender.store(nullptr);
  if (claimed.load()) {
    // A consumer still holds the block. The storage stays valid (it is the
    // framebuffer allocation, never freed) but its contents are about to be
    // clobbered; the consumer's output will be garbage. Loud log so a
    // lifetime bug is visible instead of a silent corrupt decode.
    LOG_ERR("SCR", "Build scratch reclaimed while still claimed");
  }
  block = nullptr;
  blockLen = 0;
  helper.store(nullptr);
  claimed.store(false);
}

bool available(const size_t minLen) {
  return block && blockLen >= minLen && !claimed.load() && callerMayClaim();
}

uint8_t* claim(const size_t minLen, size_t* lenOut) {
  if (!block || blockLen < minLen || !callerMayClaim()) return nullptr;
  bool expected = false;
  if (!claimed.compare_exchange_strong(expected, true)) return nullptr;
  if (lenOut) *lenOut = blockLen;
  return block;
}

void release(const uint8_t* p) {
  if (p && p == block) claimed.store(false);
}

HelperScope::HelperScope(const void* lendingTask) {
  const void* expected = nullptr;
  adopted_ = lendingTask && lendingTask == lender.load() && helper.compare_exchange_strong(expected, self());
}

HelperScope::~HelperScope() {
  if (!adopted_) return;
  const void* me = self();
  helper.compare_exchange_strong(me, nullptr);
}

}  // namespace buildscratch
