#pragma once

#include <freertos/FreeRTOS.h>

// ESP32-S3 core assignment. Core 1 runs the render task. Core 0 runs the
// radio stack and app background workers (library prewarm, OPDS prefetch,
// dictionary lookup) and Arduino's loopTask (SDK config, [dualpoint_cores] in
// platformio.ini, applied to every firmware env).
namespace TaskCores {
constexpr BaseType_t kUi = 1;
constexpr BaseType_t kWorker = 0;
}  // namespace TaskCores
