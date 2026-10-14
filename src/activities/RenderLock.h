#pragma once

#include <atomic>

class Activity;  // forward declaration

// RAII helper to lock rendering mutex for the duration of a scope.
class RenderLock {
  bool isLocked = false;

 public:
  enum class Mode { Blocking, Try };
  explicit RenderLock(Mode mode = Mode::Blocking);
  explicit RenderLock(Activity&, Mode mode = Mode::Blocking);  // Compatibility overload used by activity call sites.
  explicit RenderLock(unsigned long timeoutMs);                // bounded take; check ownsLock()
  RenderLock(const RenderLock&) = delete;
  RenderLock& operator=(const RenderLock&) = delete;
  ~RenderLock();
  void unlock();
  bool ownsLock() const { return isLocked; }
  // True while a task other than backgroundHolder owns the lock.
  static bool peek();
  // A task that holds the lock one short step at a time and stops for input
  // (the reader's background section build). peek() callers don't wait it out.
  static inline std::atomic<void*> backgroundHolder{nullptr};
  static bool heldByCaller();
  // Called every ~10 ms while a blocking take or requestUpdateAndWait() waits.
  static void (*waitTick)();
  static constexpr unsigned long WAIT_TICK_MS = 10;
};
