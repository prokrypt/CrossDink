#pragma once

struct RenderLock {
  static inline bool held = false;
  static bool peek() { return held; }
};
