#pragma once

#include <freertos/FreeRTOS.h>

// ESP32-S3 core assignment. Core 1 runs the render task. Core 0 runs the
// radio stack and app background workers (library prewarm, OPDS prefetch,
// dictionary lookup). Arduino's loopTask is on core 0 only in the X4 Pro envs
// ([x4_pro_loop_core0] in platformio.ini); the board JSON keeps it on core 1
// elsewhere, despite CONFIG_ARDUINO_RUN_CORE0 in [dualpoint_cores].
namespace TaskCores {
constexpr BaseType_t kUi = 1;
constexpr BaseType_t kWorker = 0;
}  // namespace TaskCores
