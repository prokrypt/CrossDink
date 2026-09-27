#pragma once

#include <freertos/FreeRTOS.h>

// ESP32-S3 core assignment. Core 1 runs the UI: the render task and the
// latency-sensitive work that feeds it. Core 0 runs the radio stack and app
// background workers (library prewarm, OPDS prefetch, dictionary lookup).
namespace TaskCores {
constexpr BaseType_t kUi = 1;
constexpr BaseType_t kWorker = 0;
}  // namespace TaskCores
