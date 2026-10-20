#include "GoodyModuleActivity.h"

#if CROSSDINK_GOODIES && !defined(SIMULATOR)

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_heap_caps.h>
#include <wasm_export.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "platform/PinMon.h"

namespace {

constexpr size_t MAX_MODULE_BYTES = 64 * 1024;
constexpr uint32_t WASM_STACK_BYTES = 8 * 1024;  // interpreter frames, in PSRAM
constexpr uint32_t ERROR_BYTES = 128;
enum : int32_t { GD_UP = 1, GD_DOWN, GD_LEFT, GD_RIGHT, GD_CONFIRM, GD_TAP };  // goodies/goody.h

// Every WAMR allocation, linear memory included, goes to PSRAM: its esp-idf
// os_mmap would take 64 KB pages from internal RAM (MALLOC_CAP_8BIT).
void* wamrMalloc(mem_alloc_usage_t, unsigned size) {
  return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
void* wamrRealloc(mem_alloc_usage_t, bool, void* ptr, unsigned size) {
  return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
void wamrFree(mem_alloc_usage_t, void* ptr) { heap_caps_free(ptr); }

GoodyModuleActivity::Page* pageOf(wasm_exec_env_t env) {
  return &static_cast<GoodyModuleActivity*>(wasm_runtime_get_user_data(env))->page;
}
GfxRenderer& gfxOf(wasm_exec_env_t env) {
  return static_cast<GoodyModuleActivity*>(wasm_runtime_get_user_data(env))->gfx();
}

// Host API (goodies/goody.h). WAMR checks "$" strings lie in linear memory.
int32_t gdWidth(wasm_exec_env_t env) { return pageOf(env)->width; }
int32_t gdHeight(wasm_exec_env_t env) { return pageOf(env)->height; }
int32_t gdLineHeight(wasm_exec_env_t env) { return pageOf(env)->lineHeight; }

void gdText(wasm_exec_env_t env, int32_t x, int32_t y, const char* text, int32_t bold) {
  const auto* p = pageOf(env);
  if (!p->drawing || x < 0 || y < 0 || x >= p->width || y + p->lineHeight > p->height) return;
  const auto style = bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  GfxRenderer& r = gfxOf(env);
  r.drawText(p->font, p->x + x, p->y + y, r.truncatedText(p->font, text, p->width - x, style).c_str(), true, style);
}

void gdRect(wasm_exec_env_t env, int32_t x, int32_t y, int32_t w, int32_t h, int32_t fill) {
  const auto* p = pageOf(env);
  if (!p->drawing) return;
  const int x0 = std::max<int32_t>(x, 0), y0 = std::max<int32_t>(y, 0);
  const int x1 = std::min<int64_t>(int64_t{x} + w, p->width), y1 = std::min<int64_t>(int64_t{y} + h, p->height);
  if (x1 <= x0 || y1 <= y0) return;
  if (fill) {
    gfxOf(env).fillRect(p->x + x0, p->y + y0, x1 - x0, y1 - y0);
  } else {
    gfxOf(env).drawRect(p->x + x0, p->y + y0, x1 - x0, y1 - y0);
  }
}

void gdLog(wasm_exec_env_t, const char* text) { LOG_INF("GDY", "%s", text); }

int32_t gdPinCount(wasm_exec_env_t) { return PinMon::PIN_COUNT; }
int32_t gdPinStat(wasm_exec_env_t, int32_t pin, int32_t field) {
  if (pin < 0 || pin >= static_cast<int32_t>(PinMon::PIN_COUNT)) return 0;
  const PinMon::PinStat s = PinMon::stat(pin);
  switch (field) {
    case 0:
      return s.gpio;
    case 1:
      return s.level;
    case 2:
      return static_cast<int32_t>(s.changes);
    case 3:
      return static_cast<int32_t>(s.wakes);
    case 4:
      return s.chatter;
    default:
      return 0;
  }
}
int32_t gdPinmonEnabled(wasm_exec_env_t) { return PinMon::enabled(); }
void gdPinmonSet(wasm_exec_env_t, int32_t on) { PinMon::setEnabled(on != 0); }

// Writable: WAMR sorts it in place on registration.
NativeSymbol HOST_API[] = {
    {"gd_width", reinterpret_cast<void*>(gdWidth), "()i", nullptr},
    {"gd_height", reinterpret_cast<void*>(gdHeight), "()i", nullptr},
    {"gd_line_height", reinterpret_cast<void*>(gdLineHeight), "()i", nullptr},
    {"gd_text", reinterpret_cast<void*>(gdText), "(ii$i)", nullptr},
    {"gd_rect", reinterpret_cast<void*>(gdRect), "(iiiii)", nullptr},
    {"gd_log", reinterpret_cast<void*>(gdLog), "($)", nullptr},
    {"gd_pin_count", reinterpret_cast<void*>(gdPinCount), "()i", nullptr},
    {"gd_pin_stat", reinterpret_cast<void*>(gdPinStat), "(ii)i", nullptr},
    {"gd_pinmon_enabled", reinterpret_cast<void*>(gdPinmonEnabled), "()i", nullptr},
    {"gd_pinmon_set", reinterpret_cast<void*>(gdPinmonSet), "(i)", nullptr},
};

size_t internalFree() { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

}  // namespace

GoodyModuleActivity::GoodyModuleActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                                         std::string path)
    : Activity("GoodyModule", renderer, mappedInput), title(std::move(title)), path(std::move(path)) {}

void GoodyModuleActivity::onEnter() {
  Activity::onEnter();
  const size_t freeBefore = internalFree();
  const uint32_t t0 = millis();
  if (!load()) {
    LOG_ERR("GDY", "%s: %s", path.c_str(), error.c_str());
    unload();
  }
  LOG_INF("GDY", "%s: load %lu ms, internal free %u -> %u (min %u), psram free %u", path.c_str(),
          static_cast<unsigned long>(millis() - t0), static_cast<unsigned>(freeBefore),
          static_cast<unsigned>(internalFree()),
          static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  requestUpdate();
}

bool GoodyModuleActivity::load() {
  HalFile f = Storage.open(path.c_str(), O_RDONLY);
  if (!f) {
    error = "Cannot open the module file";
    return false;
  }
  const size_t size = f.fileSize();
  if (size == 0 || size > MAX_MODULE_BYTES) {
    f.close();
    error = "Module is empty or over 64 KB";
    return false;
  }
  file = makePsramByteBufferNoThrow(size);
  const bool read = file && f.read(file.get(), size) == static_cast<int>(size);
  f.close();
  if (!read) {
    error = file ? "Cannot read the module file" : "Out of PSRAM";
    return false;
  }

  RuntimeInitArgs args{};
  args.mem_alloc_type = Alloc_With_Allocator;
  args.mem_alloc_option.allocator.malloc_func = reinterpret_cast<void*>(wamrMalloc);
  args.mem_alloc_option.allocator.realloc_func = reinterpret_cast<void*>(wamrRealloc);
  args.mem_alloc_option.allocator.free_func = reinterpret_cast<void*>(wamrFree);
  args.native_module_name = "env";
  args.native_symbols = HOST_API;
  args.n_native_symbols = sizeof(HOST_API) / sizeof(HOST_API[0]);
  runtimeUp = wasm_runtime_full_init(&args);
  if (!runtimeUp) {
    error = "WAMR init failed";
    return false;
  }
  char err[ERROR_BYTES];
  module = wasm_runtime_load(file.get(), size, err, sizeof(err));
  if (module) instance = wasm_runtime_instantiate(module, WASM_STACK_BYTES, 0, err, sizeof(err));
  if (!instance) {
    error = err;
    return false;
  }
  execEnv = wasm_runtime_create_exec_env(instance, WASM_STACK_BYTES);
  renderFunc = wasm_runtime_lookup_function(instance, "gd_render");
  inputFunc = wasm_runtime_lookup_function(instance, "gd_input");
  if (!execEnv || !renderFunc || !inputFunc) {
    error = execEnv ? "Module lacks gd_render or gd_input" : "Out of PSRAM";
    return false;
  }
  wasm_runtime_set_user_data(execEnv, this);
  return true;
}

void GoodyModuleActivity::unload() {
  renderFunc = inputFunc = nullptr;
  if (execEnv) wasm_runtime_destroy_exec_env(execEnv);
  if (instance) wasm_runtime_deinstantiate(instance);
  if (module) wasm_runtime_unload(module);
  execEnv = nullptr;
  instance = nullptr;
  module = nullptr;
  if (runtimeUp) wasm_runtime_destroy();
  runtimeUp = false;
  file.reset();
}

void GoodyModuleActivity::onExit() {
  unload();
  LOG_INF("GDY", "%s closed, internal free %u", path.c_str(), static_cast<unsigned>(internalFree()));
  Activity::onExit();
}

// A trap (bad memory access, unreachable, stack overflow) stops the module, not the device.
bool GoodyModuleActivity::call(void* func, const int argc, uint32_t* argv) {
  if (!func) return false;
  if (wasm_runtime_call_wasm(execEnv, func, argc, argv)) return true;
  const char* exception = wasm_runtime_get_exception(instance);
  error = exception ? exception : "Module stopped";
  LOG_ERR("GDY", "%s trapped: %s", path.c_str(), error.c_str());
  renderFunc = inputFunc = nullptr;
  return false;
}

void GoodyModuleActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    finish();
    return;
  }
  using B = MappedInputManager::Button;
  int32_t code = mappedInput.wasReleased(B::Up)        ? GD_UP
                 : mappedInput.wasReleased(B::Down)    ? GD_DOWN
                 : mappedInput.wasReleased(B::Left)    ? GD_LEFT
                 : mappedInput.wasReleased(B::Right)   ? GD_RIGHT
                 : mappedInput.wasReleased(B::Confirm) ? GD_CONFIRM
                                                       : 0;
  int x = 0, y = 0;
  if (!code && mappedInput.wasScreenTapped(x, y)) {
    RenderLock lock(*this);  // page is set by render()
    x -= page.x;
    y -= page.y;
    if (x >= 0 && y >= 0 && x < page.width && y < page.height) code = GD_TAP;
  }
  if (!code) return;
  bool redraw;
  {
    RenderLock lock(*this);  // render() runs the module too
    uint32_t argv[3] = {static_cast<uint32_t>(code), static_cast<uint32_t>(x), static_cast<uint32_t>(y)};
    redraw = !inputFunc || !call(inputFunc, 3, argv) || argv[0] != 0;
  }
  if (redraw) requestUpdate();
}

void GoodyModuleActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.clearScreen();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  TouchHeaderBackButton::draw(renderer, header, title.c_str(), false);
  page.font = uiScaleSpec().bodyFontId;
  page.lineHeight = renderer.getLineHeight(page.font);
  page.x = metrics.listInset + metrics.listSidePadding;
  page.y = header.y + header.height + metrics.verticalSpacing;
  page.width = renderer.getScreenWidth() - 2 * page.x;
  page.height = renderer.getScreenHeight() - metrics.buttonHintsHeight - page.y;
  if (renderFunc) {
    const uint32_t t0 = micros();
    uint32_t none[1] = {};
    page.drawing = true;
    call(renderFunc, 0, none);
    page.drawing = false;
    LOG_INF("GDY", "%s render %lu us", title.c_str(), static_cast<unsigned long>(micros() - t0));
  }
  if (!renderFunc) {
    renderer.fillRect(page.x, page.y, page.width, page.height, false);  // whatever a trapped gd_render() drew
    for (const auto& line : renderer.wrappedText(page.font, error.c_str(), page.width, 4)) {
      renderer.drawText(page.font, page.x, page.y, line.c_str());
      page.y += page.lineHeight + 6;
    }
  }
  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

#endif  // CROSSDINK_GOODIES && !SIMULATOR
