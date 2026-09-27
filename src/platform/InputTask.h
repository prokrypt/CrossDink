#pragma once

#include <cstdint>

// Samples keys and touch on a task of its own, woken by the InputWake lines,
// so presses, taps and gestures made while the loop is busy (a long render, a
// section build) are queued in order instead of dropped. The loop reads them
// through gpio.update() as before.
namespace InputTask {
// Starts sampling. Call once at the end of setup(), after boot-time input has
// been absorbed.
void begin();

// Loop idle wait: returns as soon as an input event is queued, or after
// timeoutMs.
void waitForInput(uint32_t timeoutMs);
}  // namespace InputTask
