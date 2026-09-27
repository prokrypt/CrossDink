#pragma once
// Host stand-ins for the FreeRTOS types and calls that lib/ headers pull in.
// Tests run single-threaded, so locks always succeed and nothing blocks.
using BaseType_t = int;
using UBaseType_t = unsigned;
using TickType_t = unsigned;
using SemaphoreHandle_t = void*;
using QueueHandle_t = void*;
using TaskHandle_t = void*;
constexpr TickType_t portMAX_DELAY = 0xFFFFFFFFu;
constexpr BaseType_t pdTRUE = 1;
constexpr BaseType_t pdFALSE = 0;
constexpr BaseType_t pdPASS = 1;
constexpr int portNUM_PROCESSORS = 1;
struct portMUX_TYPE {};
#define portMUX_INITIALIZER_UNLOCKED \
  {                                  \
  }
#define taskENTER_CRITICAL(mux) ((void)(mux))
#define taskEXIT_CRITICAL(mux) ((void)(mux))
