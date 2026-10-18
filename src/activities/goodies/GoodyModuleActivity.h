#pragma once

#include <Memory.h>

#include <string>

#include "activities/Activity.h"

struct WASMModuleCommon;
struct WASMModuleInstanceCommon;
struct WASMExecEnv;

// Goodies SD module (/debug/goodies/<name>.wasm, ABI in goodies/goody.h), run in
// the WAMR classic interpreter (lib/wamr). All WAMR memory, linear memory
// included, is PSRAM; it lives from onEnter() to onExit(). The module only
// draws into the framebuffer through the host's renderer calls, inside
// render(); the host draws the header and does the refresh, so a module has no
// way to reach panel timing, LUTs or registers. Back always closes it.
// Module calls run under the render lock (render() holds it; loop() takes it),
// so loopTask and the render task never run the module at once.
class GoodyModuleActivity final : public Activity {
 public:
  GoodyModuleActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title, std::string path);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool powerOffPanelWhenIdle() const override { return true; }

  // Host calls (GoodyModuleActivity.cpp) reach the page through these.
  struct Page {
    int x, y, width, height, font, lineHeight;
    bool drawing;  // inside gd_render(): the screen calls only work then
  };
  Page page{};
  GfxRenderer& gfx() { return renderer; }

 private:
  bool load();
  void unload();
  bool call(void* func, int argc, uint32_t* argv);

  std::string title;
  std::string path;
  HeapByteBuffer file;  // the .wasm bytes in PSRAM; WAMR reads from them until unload()
  WASMModuleCommon* module = nullptr;
  WASMModuleInstanceCommon* instance = nullptr;
  WASMExecEnv* execEnv = nullptr;
  void* renderFunc = nullptr;
  void* inputFunc = nullptr;
  bool runtimeUp = false;  // wasm_runtime_full_init() succeeded; unload() destroys it
  std::string error;       // shown instead of the module's page
};
