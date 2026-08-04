#pragma once

// Owns the shared Lua runtime inside a CrossPoint activity. The runtime drives the Lua
// state, module resolution, timers and app navigation; this class feeds it buttons and
// frame pacing, and adapts its navigation requests onto CrossPoint's ActivityManager
// (an app's "back" past its own root leaves the activity, not the app history).

#include <lua/runtime.h>

#include <GfxRenderer.h>
#include <HalGPIO.h>

#include <atomic>
#include <memory>
#include <string>

#include "activities/Activity.h"
#include "providers.h"

namespace cplua {

// The physical buttons in HalGPIO order, as semantic roles for the runtime.
constexpr struct {
  uint8_t halIndex;
  const char* role;
} kButtons[] = {
    {HalGPIO::BTN_BACK, "back"},   {HalGPIO::BTN_CONFIRM, "confirm"}, {HalGPIO::BTN_LEFT, "left"},
    {HalGPIO::BTN_RIGHT, "right"}, {HalGPIO::BTN_UP, "up"},            {HalGPIO::BTN_DOWN, "down"},
};

class LuaHost final : public Activity {
 public:
  LuaHost(GfxRenderer& renderer, MappedInputManager& input, HalGPIO& gpio, std::string appName);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return true; }
  bool preventAutoSleep() override { return true; }

 private:
  enum class State { Loading, Running, Error };

  HalGPIO& gpio;
  std::string appName;
  std::unique_ptr<Providers> providers;
  std::unique_ptr<esp32lua::Runtime> runtime;
  std::atomic<State> state{State::Loading};
  std::atomic<bool> wantsExit{false};  // poll task asks the main loop to finish() the activity
  std::string loadError;
  bool buttonDown[8] = {};

  void pollButtons();
  void renderError();
  static void pollTask(void* arg);
};

}  // namespace cplua
