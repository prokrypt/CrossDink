#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <type_traits>
#include <utility>

template <typename T, typename... Args>
  requires(!std::is_array_v<T>)
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  return std::make_unique<T>(std::forward<Args>(args)...);
}

template <typename T>
  requires std::is_unbounded_array_v<T>
std::unique_ptr<T> makeUniqueNoThrow(size_t count) {
  return std::make_unique<T>(count);
}

struct HeapByteBufferDeleter {
  void operator()(uint8_t* ptr) const { std::free(ptr); }
};
using HeapByteBuffer = std::unique_ptr<uint8_t[], HeapByteBufferDeleter>;

// Stands in for a board with PSRAM; off by default, as on a C3.
inline bool fakePsram = false;
inline HeapByteBuffer makePsramByteBufferNoThrow(const size_t count) {
  if (!fakePsram || count == 0) return {};
  return HeapByteBuffer(static_cast<uint8_t*>(std::malloc(count)));
}
