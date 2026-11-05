#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class HalSpiBus {
 public:
  class Lock {
   public:
    Lock();
    ~Lock();
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

   private:
    bool acquired = false;
  };

  static HalSpiBus& getInstance();

  // Lend the bus to other tasks while the calling task idles on a long wait
  // (an EPD BUSY wait) that needs no bus traffic. Only releases when the caller
  // holds exactly one Lock, so a task that nested another Lock around the wait
  // keeps the bus. Returns true when released; pair with reacquireAfterIdle().
  bool releaseForIdle();
  void reacquireAfterIdle();

 private:
  HalSpiBus();

  SemaphoreHandle_t mutex = nullptr;
  int depth = 0;  // Lock nesting of the holder; only touched while holding mutex

  friend class Lock;
};
