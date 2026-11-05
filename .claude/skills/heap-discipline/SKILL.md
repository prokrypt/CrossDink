---
name: heap-discipline
description: Memory allocation discipline for CrossDink's ESP32-S3 readers (scarce internal RAM, PSRAM for large CPU-only buffers, one 48KB framebuffer in PSRAM). Use whenever writing or reviewing code that allocates: new / malloc / std::vector / std::string, buffers, caches, or anything held across a loop or an activity lifecycle. Covers makeUniqueNoThrow vs raw new/malloc, internal RAM vs PSRAM placement, fragmentation avoidance, reserve-before-push_back, alloc-once-reuse, stack vs heap sizing, and the chunked grayscale buffer pattern.
---

# Heap Discipline (ESP32-S3)

CLAUDE.md states the allocation rules. This is the procedure you run while
writing the code and the gate you run before handing it back.

The constraint that makes every call matter: internal RAM stays scarce even
with PSRAM. Task stacks, DMA buffers (EPD SPI, SDMMC) and ISR data need
internal RAM, and plain `malloc`/`new` of 1KB or less prefers it; larger plain
allocations try PSRAM first and fall back to internal RAM. The one 48KB
framebuffer lives in PSRAM (`FREEINK_FB_PSRAM`). **Fragmentation, not total
usage, is what kills this device.** Internal free-heap (`ESP.getFreeHeap()`)
can read fine while the largest free block (`ESP.getMaxAllocHeap()`) is too
small for the next allocation. Optimize for not leaving holes, not just for
using fewer bytes.

## Allocation decision procedure

Ask in order; stop at the first yes.

1. **Stack?** Local, bounded, under ~256 bytes total: plain array/struct. No
   heap, no fragmentation. Keep frames lean; the task stack is small.
2. **Compile-time constant?** `static constexpr` lives in flash, costs zero DRAM.
3. **Allocated once and reused for an activity's lifetime?** Allocate in
   `onEnter`, hold in a member, release in `onExit`. Never per-frame, never
   per-iteration.
4. **Dynamic and fallible?** `makeUniqueNoThrow<T>(...)` /
   `makeUniqueNoThrow<T[]>(n)` from `lib/Memory/Memory.h`. Null-check, `LOG_ERR`
   with the size, return false. It frees on every exit path. To pick the pool,
   use the `Memory.h` byte buffers: `makePsramByteBufferNoThrow` for large
   CPU-only data, `makeInternalByteBufferNoThrow` for anything DMA, an ISR or a
   latency-sensitive path touches. Never PSRAM for stacks, DMA or ISR data.
5. **A C/SDK API takes ownership and frees it itself?** Only then raw
   `new (std::nothrow)` / `malloc`, with a comment naming who frees it.

Bare `new` / `new[]` is never correct here: under `-fno-exceptions` it calls
`abort()` on OOM instead of returning null.

## Fragmentation rules

- `std::vector`: `reserve(n)` before any `push_back` loop. Each growth is
  alloc-copy-free (three heap ops) and leaves a hole. Unknown n: estimate high.
- No repeated `new`/`delete` or growing containers inside a loop or render path.
  Hoist the allocation out of the loop.
- Large contiguous blocks fragment worst. Full-screen-class buffers use the
  chunked `storeBwBuffer` / `restoreBwBuffer` path in `GfxRenderer` (8000-byte
  chunks, PSRAM-first like any malloc above 1KB) so they never demand one
  contiguous 48KB block, even when they fall back to internal RAM. Reuse that
  path. Do not malloc a second full-screen buffer.
- `std::string` / Arduino `String`: acceptable on cold paths (file I/O, one-shot
  setup). Banned on hot/render paths. Build text with a stack `char[]` +
  `snprintf`; if a `std::string` is unavoidable, `reserve` it first.

## Justify every allocation

Per CLAUDE.md's evidence rule: when you add a heap allocation, state in one line
why stack/static/reuse was rejected and the worst-case size. If you cannot name
the size, you cannot budget it, and you should not allocate it.

## Self-review before handoff

- [ ] No bare `new`/`new[]`. Every fallible alloc is `makeUniqueNoThrow`, or a
      raw alloc with an explicit owner comment.
- [ ] Every allocation is null-checked with `LOG_ERR` before the error return.
- [ ] No allocation inside a loop or render path that could be hoisted.
- [ ] Every `push_back` loop has a preceding `reserve`.
- [ ] Anything allocated in `onEnter` is released in `onExit`; member `HalFile`
      closed there too.
- [ ] No second full-screen buffer; grayscale uses store/restoreBwBuffer.
- [ ] Nothing a DMA transfer, an ISR or a task stack uses is placed in PSRAM.
- [ ] Each new allocation carries a one-line size + why-not-stack/static note.
