#pragma once

#include <cstddef>
#include <cstdint>

// Registry for the framebuffer bytes lent out during a build phase
// (GfxRenderer::FrameBufferLoan). The lender (GfxRenderer) deposits the block
// with lend()/reclaim(); a memory-hungry consumer (e.g. InflateStream's ~43KB
// tinfl state + window) may claim() it instead of allocating from the heap.
//
// Exactly one claimant at a time; claim() returns nullptr when the block is
// absent, already claimed, or the caller is neither the lending task nor its
// adopted helper, and consumers must fall back to the heap. Background
// workers therefore never touch the framebuffer while the lender can redraw. The
// underlying storage is the framebuffer allocation itself, which is never
// freed -- so even the pathological case (reclaim() while still claimed, which
// logs an error) reads garbage, never freed memory.
namespace buildscratch {

// Lender side (GfxRenderer only).
void lend(uint8_t* buf, size_t len);
void reclaim();

// Advisory only: does not reserve the block; claim() must still succeed.
bool available(size_t minLen);

// Consumer side: exclusive claim of the whole block if it is at least minLen
// bytes; nullptr means "use the heap". Release with the same pointer.
uint8_t* claim(size_t minLen, size_t* lenOut = nullptr);
void release(const uint8_t* p);

// Opaque id of the calling task.
const void* self();

// A helper task the lender spawns AND joins before reclaim() (the overlapped
// HTML inflate worker) may claim too. Construct on the helper with the
// lender's self(); inert if that task is not the current lender or another
// helper is adopted.
class HelperScope {
 public:
  explicit HelperScope(const void* lendingTask);
  ~HelperScope();
  HelperScope(const HelperScope&) = delete;
  HelperScope& operator=(const HelperScope&) = delete;

 private:
  bool adopted_ = false;
};

}  // namespace buildscratch
