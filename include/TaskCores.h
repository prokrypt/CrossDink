#pragma once

#include <freertos/FreeRTOS.h>

// ESP32-S3 core assignment. Core 1 runs the render task. Core 0 runs the
// radio stack and app background workers (library prewarm, OPDS prefetch,
// dictionary lookup). Arduino's loopTask core comes from the SDK config:
// core 0 on the X4 Pro light-sleep builds ([dualpoint_cores] in
// platformio.ini), core 1 on prebuilt-SDK builds.
namespace TaskCores {
constexpr BaseType_t kUi = 1;
constexpr BaseType_t kWorker = 0;
}  // namespace TaskCores
