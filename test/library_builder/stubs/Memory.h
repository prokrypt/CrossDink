#pragma once

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <type_traits>
#include <utility>

#include "HalStorage.h"

template <typename T, typename... Args>
  requires(!std::is_array_v<T>)
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  if (fake::fail(fake::failAlloc)) return nullptr;
  return std::make_unique<T>(std::forward<Args>(args)...);
}

template <typename T>
  requires std::is_unbounded_array_v<T>
std::unique_ptr<T> makeUniqueNoThrow(size_t count) {
  if (fake::fail(fake::failAlloc)) return nullptr;
  return std::make_unique<T>(count);
}

struct HeapByteBufferDeleter {
  void operator()(uint8_t* ptr) const { std::free(ptr); }
};
using HeapByteBuffer = std::unique_ptr<uint8_t[], HeapByteBufferDeleter>;

// fake::psram stands in for a board with PSRAM; off by default, as on a C3.
inline HeapByteBuffer makePsramByteBufferNoThrow(const size_t count) {
  if (!fake::psram || count == 0) return {};
  return HeapByteBuffer(static_cast<uint8_t*>(std::malloc(count)));
}
