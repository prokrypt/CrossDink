#include "HalSystem.h"

#include <algorithm>
#include <string>

#include "AppVersion.h"
#include "Arduino.h"
#include "HalStorage.h"
#include "Logging.h"
#include "PsramLog.h"
#include "esp_app_desc.h"
#include "esp_debug_helpers.h"
#include "esp_private/esp_cpu_internal.h"
#include "esp_private/esp_system_attr.h"
#include "esp_private/panic_internal.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_IDF_TARGET_ARCH_XTENSA
#include "esp_cpu_utils.h"
#include "esp_memory_utils.h"
#include "freertos/task.h"
#include "xtensa/corebits.h"
#endif

#define MAX_PANIC_STACK_DEPTH 32
#define MAX_PANIC_BACKTRACE_DEPTH 32
#define PANIC_CAPTURE_MAGIC 0x50414E49u

RTC_NOINIT_ATTR char panicMessage[256];
RTC_NOINIT_ATTR HalSystem::StackFrame panicStack[MAX_PANIC_STACK_DEPTH];
#if CONFIG_IDF_TARGET_ARCH_RISCV
RTC_NOINIT_ATTR uint32_t panicBacktrace[MAX_PANIC_BACKTRACE_DEPTH];
RTC_NOINIT_ATTR volatile size_t panicBacktraceDepth;
#endif

#if CONFIG_IDF_TARGET_ARCH_RISCV
// Preserve the exception-frame values that identify a C3 panic. Unlike the raw
// stack words below, MEPC is the actual faulting instruction and MTVAL is the
// fault address/value reported by the CPU.
struct RiscvPanicRegisters {
  uint32_t mepc;
  uint32_t ra;
  uint32_t sp;
  uint32_t fp;
  uint32_t mcause;
  uint32_t mtval;
  uint32_t mstatus;
  uint32_t captured;
};
RTC_NOINIT_ATTR RiscvPanicRegisters panicRiscvRegisters;
#endif
#if CONFIG_IDF_TARGET_ARCH_XTENSA
constexpr size_t MAX_PANIC_CORES = 2;
constexpr size_t PANIC_TASK_NAME_BYTES = 16;
// Keep each S3 core's register dump and backtrace separate. ESP-IDF prints the
// primary panic core first, then any other core that has an exception frame.
struct XtensaPanicRegisters {
  uint32_t pc;
  uint32_t a0;
  uint32_t a1;
  uint32_t ps;
  uint32_t exccause;
  uint32_t excvaddr;
  uint32_t captured;
  char taskName[PANIC_TASK_NAME_BYTES];
};
RTC_NOINIT_ATTR XtensaPanicRegisters panicXtensaRegisters[MAX_PANIC_CORES];
RTC_NOINIT_ATTR uint32_t panicXtensaBacktrace[MAX_PANIC_CORES][MAX_PANIC_BACKTRACE_DEPTH];
RTC_NOINIT_ATTR size_t panicXtensaBacktraceDepth[MAX_PANIC_CORES];
RTC_NOINIT_ATTR int32_t panicPrimaryCore;
RTC_NOINIT_ATTR uint32_t panicCoreCaptureCount;
#endif
// RTC_NOINIT is uninitialized on cold boot, so only this exact marker proves a
// panic diagnostic was captured before the reset.
RTC_NOINIT_ATTR volatile uint32_t panicCaptureMarker;

extern "C" {

void __real_panic_abort(const char* message);
void __real_panic_print_backtrace(const void* frame, int core);

static DRAM_ATTR const char PANIC_REASON_UNKNOWN[] = "(unknown panic reason)";

#if CONFIG_IDF_TARGET_ARCH_XTENSA
void IRAM_ATTR resetXtensaPanicCapture() {
  for (size_t core = 0; core < MAX_PANIC_CORES; ++core) {
    panicXtensaRegisters[core].captured = 0;
    panicXtensaBacktraceDepth[core] = 0;
  }
  panicPrimaryCore = -1;
}

void IRAM_ATTR captureXtensaPanicBacktrace(const void* frame, const int core) {
  if (core < 0 || static_cast<size_t>(core) >= MAX_PANIC_CORES) return;
  // RTC_NOINIT may contain arbitrary bytes if this is the first boot's panic.
  if (panicCaptureMarker != PANIC_CAPTURE_MAGIC) {
    panicCoreCaptureCount = 0;
    panicMessage[0] = '\0';
  }
  if (panicCoreCaptureCount == 0) {
    resetXtensaPanicCapture();
    panicPrimaryCore = core;
  }
  ++panicCoreCaptureCount;

  const auto* exceptionFrame = static_cast<const XtExcFrame*>(frame);
  auto& registers = panicXtensaRegisters[core];
  registers.pc = exceptionFrame->pc;
  registers.a0 = exceptionFrame->a0;
  registers.a1 = exceptionFrame->a1;
  registers.ps = exceptionFrame->ps;
  registers.exccause = exceptionFrame->exccause;
  registers.excvaddr = exceptionFrame->excvaddr;
  registers.taskName[0] = '\0';
  const TaskHandle_t task = xTaskGetCurrentTaskHandleForCore(core);
  if (task && esp_ptr_in_dram(task)) {
    const char* name = pcTaskGetName(task);
    if (name && esp_ptr_byte_accessible(name)) {
      size_t i = 0;
      for (; i < sizeof(registers.taskName) - 1 && name[i]; ++i) registers.taskName[i] = name[i];
      registers.taskName[i] = '\0';
    }
  }
  registers.captured = PANIC_CAPTURE_MAGIC;

  esp_backtrace_frame_t backtraceFrame = {
      .pc = static_cast<uint32_t>(exceptionFrame->pc),
      .sp = static_cast<uint32_t>(exceptionFrame->a1),
      .next_pc = static_cast<uint32_t>(exceptionFrame->a0),
      .exc_frame = exceptionFrame,
  };

  size_t depth = 0;
  uint32_t pc = esp_cpu_process_stack_pc(backtraceFrame.pc);
  panicXtensaBacktrace[core][depth++] = pc;

  bool corrupted =
      !esp_stack_ptr_is_sane(backtraceFrame.sp) ||
      (!esp_ptr_executable(reinterpret_cast<const void*>(pc)) && exceptionFrame->exccause != EXCCAUSE_INSTR_PROHIBITED);
  while (depth < MAX_PANIC_BACKTRACE_DEPTH && backtraceFrame.next_pc != 0 && !corrupted) {
    if (!esp_backtrace_get_next_frame(&backtraceFrame)) {
      break;
    }

    pc = esp_cpu_process_stack_pc(backtraceFrame.pc);
    if (esp_ptr_executable(reinterpret_cast<const void*>(pc))) {
      panicXtensaBacktrace[core][depth++] = pc;
    }
  }

  panicXtensaBacktraceDepth[core] = depth;
  panicCaptureMarker = PANIC_CAPTURE_MAGIC;
}
#endif

#if CONFIG_IDF_TARGET_ARCH_RISCV
void IRAM_ATTR captureRiscvPanicRegisters(const void* frame) {
  const auto* exceptionFrame = static_cast<const esp_cpu_frame_t*>(frame);
  panicRiscvRegisters.mepc = exceptionFrame->mepc;
  panicRiscvRegisters.ra = exceptionFrame->ra;
  panicRiscvRegisters.sp = exceptionFrame->sp;
  panicRiscvRegisters.fp = exceptionFrame->s0;
  panicRiscvRegisters.mcause = exceptionFrame->mcause;
  panicRiscvRegisters.mtval = exceptionFrame->mtval;
  panicRiscvRegisters.mstatus = exceptionFrame->mstatus;
  panicRiscvRegisters.captured = PANIC_CAPTURE_MAGIC;
}
#endif

// IRAM-safe bounded append (strncat is not IRAM-safe in panic context).
static size_t IRAM_ATTR appendPanicMessage(size_t at, const char* text) {
  for (; at < sizeof(panicMessage) - 1 && *text; ++at, ++text) panicMessage[at] = *text;
  panicMessage[at] = '\0';
  return at;
}

static void IRAM_ATTR beginPanicMessage() {
#if CONFIG_IDF_TARGET_ARCH_XTENSA
  if (panicCaptureMarker != PANIC_CAPTURE_MAGIC) {
    panicCoreCaptureCount = 0;
    resetXtensaPanicCapture();
  }
#endif
  panicMessage[0] = '\0';
}

void IRAM_ATTR __wrap_panic_abort(const char* message) {
  beginPanicMessage();
  appendPanicMessage(0, message ? message : PANIC_REASON_UNKNOWN);
  panicCaptureMarker = PANIC_CAPTURE_MAGIC;

  __real_panic_abort(message);
}

// Task watchdog timeout (ISR, before the abort). The abort path records no
// message and the backtrace covers only the aborting core, so name the task
// each core was running, as IDF prints to the console.
void IRAM_ATTR esp_task_wdt_isr_user_handler(void) {
  static DRAM_ATTR const char PREFIX[] = "task_wdt: running";
  static DRAM_ATTR const char CPU_LABEL[] = " CPU";
  static DRAM_ATTR const char UNKNOWN[] = "?";
  beginPanicMessage();
  size_t at = appendPanicMessage(0, PREFIX);
  for (int core = 0; core < portNUM_PROCESSORS; ++core) {
    const char digit[3] = {static_cast<char>('0' + core), '=', '\0'};
    at = appendPanicMessage(at, CPU_LABEL);
    at = appendPanicMessage(at, digit);
    const TaskHandle_t task = xTaskGetCurrentTaskHandleForCore(core);
    const char* name = task ? pcTaskGetName(task) : nullptr;
    at = appendPanicMessage(at, name ? name : UNKNOWN);
  }
  panicCaptureMarker = PANIC_CAPTURE_MAGIC;
}

void IRAM_ATTR __wrap_panic_print_backtrace(const void* frame, int core) {
  if (!frame) {
    __real_panic_print_backtrace(frame, core);
    return;
  }

#if CONFIG_IDF_TARGET_ARCH_XTENSA
  captureXtensaPanicBacktrace(frame, core);
  __real_panic_print_backtrace(frame, core);
  return;
#elif !__riscv
  __real_panic_print_backtrace(frame, core);
  return;
#else
  for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
    panicStack[i].sp = 0;
  }
  panicBacktraceDepth = 0;

  captureRiscvPanicRegisters(frame);

  // Copied from components/esp_system/port/arch/riscv/panic_arch.c
  uint32_t sp = (uint32_t)((RvExcFrame*)frame)->sp;
  const int per_line = 8;
  int depth = 0;
  for (int x = 0; x < 1024; x += per_line * sizeof(uint32_t)) {
    uint32_t* spp = (uint32_t*)(sp + x);
    // panic_print_hex(sp + x);
    // panic_print_str(": ");
    panicStack[depth].sp = sp + x;
    for (int y = 0; y < per_line; y++) {
      // panic_print_str("0x");
      // panic_print_hex(spp[y]);
      // panic_print_str(y == per_line - 1 ? "\r\n" : " ");
      panicStack[depth].spp[y] = spp[y];
    }

    depth++;
    if (depth >= MAX_PANIC_STACK_DEPTH) {
      break;
    }
  }
  panicCaptureMarker = PANIC_CAPTURE_MAGIC;

  __real_panic_print_backtrace(frame, core);
#endif
}
}

namespace HalSystem {

void begin() {
  // On a panic reboot, preserve diagnostics until checkPanic() has tried to write them to the SD card.
  // Ordinary boots clear any stale retained diagnostics.
  if (!isRebootFromPanic()) {
    clearPanic();
  } else {
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    // The saved per-core data remains available for the report below. A second
    // panic during this boot must start a fresh capture, even if SD writing fails.
    panicCoreCaptureCount = 0;
#endif
    // Panic reboot: preserve logs and panic info, but clamp logHead in case the
    // panic occurred before begin() ever ran (e.g. in a static constructor).
    // If logHead was out of range, logMessages is also garbage — clear it so
    // getLastLogs() does not dump corrupt data into the crash report.
    if (sanitizeLogHead()) {
      clearLastLogs();
    }
  }
}

void checkPanic() {
  if (isRebootFromPanic()) {
    auto panicInfo = getPanicInfo(true);
    // Under /debug with the other logs; an older /crash_report.txt stays as it was.
    Storage.ensureDirectoryExists("/debug");
    auto file = Storage.open("/debug/crash_report.txt", O_WRITE | O_CREAT | O_TRUNC);
    if (file) {
      const size_t written = file.write(panicInfo.c_str(), panicInfo.size());
#if CROSSDINK_PSRAM_LOG
      // The PSRAM ring survives a panic reset: add its last 16 KB, which holds
      // far more of the run-up to the crash than the RTC lines above.
      static constexpr char header[] = "\n\nPSRAM log (last 16 KB):\n";
      file.write(header, sizeof(header) - 1);
      const uint32_t end = PsramLog::end();
      uint32_t cursor = std::max<uint32_t>(PsramLog::oldest(), end > 16384 ? end - 16384 : 0);
      char chunk[512];  // stack, not heap: runs once in setup() on the loop task
      size_t n;
      while (cursor < end && (n = PsramLog::read(cursor, chunk, std::min<size_t>(sizeof(chunk), end - cursor))) > 0) {
        if (file.write(chunk, n) != n) {
          LOG_ERR("SYS", "Crash report: PSRAM log write failed");
          break;
        }
      }
#endif
      file.close();
      if (written == panicInfo.size()) {
        // Keep the crash data for CrashActivity, but mark it consumed so a
        // later watchdog reset cannot be mistaken for this panic.
        panicCaptureMarker = 0;
        LOG_INF("SYS", "Dumped panic info to SD card");
      } else {
        LOG_ERR("SYS", "Failed to write complete crash report (%zu of %zu bytes)", written, panicInfo.size());
      }
    } else {
      LOG_ERR("SYS", "Failed to open crash_report.txt for writing");
    }
  }
}

void clearPanic() {
  panicCaptureMarker = 0;
  panicMessage[0] = '\0';
  for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
    panicStack[i].sp = 0;
  }
#if CONFIG_IDF_TARGET_ARCH_RISCV
  panicBacktraceDepth = 0;
  panicRiscvRegisters.captured = 0;
#endif
#if CONFIG_IDF_TARGET_ARCH_XTENSA
  resetXtensaPanicCapture();
  panicCoreCaptureCount = 0;
#endif
  clearLastLogs();
}

std::string getPanicInfo(bool full) {
  if (!full) {
    return panicMessage;
  } else {
    std::string info;

    info += "CrossDink version: ";
    info += AppVersion::version();
    info += "\nCrossDink device type: " CROSSDINK_FIRMWARE_DEVICE_TYPE;
    char elfSha[65] = {};
    esp_app_get_elf_sha256(elfSha, sizeof(elfSha));
    info += "\nFirmware ELF SHA256: " + std::string(elfSha);
    info += "\n\nPanic reason: ";
    info += panicMessage[0] ? panicMessage : "(not captured; see exception registers)";
    info += "\n\nLast logs:\n" + getLastLogs();
    auto toHex = [](uint32_t value) {
      char buffer[9];
      snprintf(buffer, sizeof(buffer), "%08" PRIX32, value);
      return std::string(buffer);
    };
#if CONFIG_IDF_TARGET_ARCH_RISCV
    if (panicRiscvRegisters.captured == PANIC_CAPTURE_MAGIC) {
      info += "\n\nRISC-V exception registers:\n";
      info += "MEPC (faulting instruction): 0x" + toHex(panicRiscvRegisters.mepc);
      info += "\nRA (caller): 0x" + toHex(panicRiscvRegisters.ra);
      info += "\nSP (stack pointer): 0x" + toHex(panicRiscvRegisters.sp);
      info += "\nS0/FP (frame pointer): 0x" + toHex(panicRiscvRegisters.fp);
      info += "\nMCAUSE: 0x" + toHex(panicRiscvRegisters.mcause);
      info += "\nMTVAL (fault address/value): 0x" + toHex(panicRiscvRegisters.mtval);
      info += "\nMSTATUS: 0x" + toHex(panicRiscvRegisters.mstatus);
    }
#endif
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    if (panicPrimaryCore >= 0 && static_cast<size_t>(panicPrimaryCore) < MAX_PANIC_CORES) {
      info += "\nPrimary panic core: " + std::to_string(panicPrimaryCore);
    }
    for (size_t core = 0; core < MAX_PANIC_CORES; ++core) {
      const auto& registers = panicXtensaRegisters[core];
      if (registers.captured != PANIC_CAPTURE_MAGIC) continue;
      info += "\n\nCore " + std::to_string(core);
      if (static_cast<int32_t>(core) == panicPrimaryCore) info += " (primary)";
      info += " task: ";
      info += registers.taskName[0] ? registers.taskName : "(unavailable)";
      info += "\nPC (instruction): 0x" + toHex(registers.pc);
      info += "\nA0 (return address): 0x" + toHex(registers.a0);
      info += "\nA1 (stack pointer): 0x" + toHex(registers.a1);
      info += "\nPS (processor state): 0x" + toHex(registers.ps);
      info += "\nEXCCAUSE: 0x" + toHex(registers.exccause);
      info += "\nEXCVADDR (fault address): 0x" + toHex(registers.excvaddr);
      const size_t depth =
          panicXtensaBacktraceDepth[core] <= MAX_PANIC_BACKTRACE_DEPTH ? panicXtensaBacktraceDepth[core] : 0;
      if (depth > 0) {
        info += "\nBacktrace:\n";
        for (size_t i = 0; i < depth; ++i) info += "0x" + toHex(panicXtensaBacktrace[core][i]) + "\n";
      }
    }
#endif
    if (panicStack[0].sp != 0) {
      info += "\n\nStack memory:\n";
      for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
        if (panicStack[i].sp == 0) {
          break;
        }
        info += "0x" + toHex(panicStack[i].sp) + ": ";
        for (size_t j = 0; j < 8; j++) {
          info += "0x" + toHex(panicStack[i].spp[j]) + " ";
        }
        info += "\n";
      }
    }

#if CONFIG_IDF_TARGET_ARCH_RISCV
    const size_t backtraceDepth = panicBacktraceDepth <= MAX_PANIC_BACKTRACE_DEPTH ? panicBacktraceDepth : 0;
    if (backtraceDepth > 0) {
      info += "\nStack trace:\n";
      for (size_t i = 0; i < backtraceDepth; i++) {
        info += "0x" + toHex(panicBacktrace[i]) + "\n";
      }
    }
#endif

    return info;
  }
}

bool isRebootFromPanic() {
  const auto resetReason = esp_reset_reason();
  if (resetReason == ESP_RST_PANIC || resetReason == ESP_RST_CPU_LOCKUP) {
    return true;
  }

  const bool watchdogReset =
      resetReason == ESP_RST_INT_WDT || resetReason == ESP_RST_TASK_WDT || resetReason == ESP_RST_WDT;
  return watchdogReset && panicCaptureMarker == PANIC_CAPTURE_MAGIC;
}

}  // namespace HalSystem
