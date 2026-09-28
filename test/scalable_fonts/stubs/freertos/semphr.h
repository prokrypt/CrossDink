#pragma once
#include "FreeRTOS.h"
inline void* xSemaphoreCreateMutex() { return reinterpret_cast<void*>(1); }
inline void vSemaphoreDelete(void*) {}
inline void* xSemaphoreGetMutexHolder(void*) { return nullptr; }
inline int xSemaphoreTake(void*, int) { return pdTRUE; }
inline void xSemaphoreGive(void*) {}
