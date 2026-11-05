#pragma once
#include "FreeRTOS.h"
inline TaskHandle_t xTaskGetCurrentTaskHandle() { return reinterpret_cast<TaskHandle_t>(1); }
inline BaseType_t xPortGetCoreID() { return 0; }
