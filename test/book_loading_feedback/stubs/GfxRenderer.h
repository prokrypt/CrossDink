#pragma once
class GfxRenderer {
 public:
  bool available = true;
  int clears = 0;
  bool hasFrameBuffer() const { return available; }
  void clearScreen() { ++clears; }
};
