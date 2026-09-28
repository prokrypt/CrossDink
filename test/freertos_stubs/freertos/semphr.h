#pragma once
#include "FreeRTOS.h"
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return reinterpret_cast<SemaphoreHandle_t>(1); }
inline SemaphoreHandle_t xSemaphoreCreateBinary() { return reinterpret_cast<SemaphoreHandle_t>(1); }
inline void vSemaphoreDelete(SemaphoreHandle_t) {}
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return pdTRUE; }
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t) { return pdTRUE; }
inline TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t) { return nullptr; }
