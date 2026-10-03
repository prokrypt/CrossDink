#pragma once

#include <cstdint>

// Idle wait for the main loop that ends early when a digital key or the touch
// controller's INT line changes level, including from automatic light sleep.
// Without it, a press made while the loop sleeps waits for the next poll tick.
namespace InputWake {
// Registers the wake lines. Call once after the input pins are configured.
void begin();

// Waits up to timeoutMs, returning as soon as a wake line changes level.
void wait(uint32_t timeoutMs);

// Ends a running wait() early (another task has work for the loop).
void wake();

// True when every input this board has is on a wake line, so a long idle wait
// cannot delay or drop a press. ADC-ladder keys and touch controllers other
// than the GT911 still depend on the poll tick.
bool coversAllInputs();

// True once after the charger STAT line changed level during a wait (cleared).
bool takeChargeWake();

// Debug: button and touch line interrupts since the previous call (cleared).
void takeWakeCounts(uint32_t& buttons, uint32_t& touch);
// Debug: touch line interrupts since boot (not cleared); 0 without the perf log.
uint32_t touchWakeTotal();
// "pin armed/now" per line, e.g. "0 L/1 21 int:H/0" (int: ends the wait only,
// not a light-sleep wake), for [PM]'s all-rejected line.
void describePins(char* out, uint32_t size);
}  // namespace InputWake
