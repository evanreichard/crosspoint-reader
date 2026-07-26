#pragma once

#include <atomic>
#include <string>

#include "activities/Activity.h"
#include "util/LuaManager.h"

class LuaActivity final : public Activity {
 public:
  LuaActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string pluginName)
      : Activity("LuaApp", renderer, mappedInput), pluginName(std::move(pluginName)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }

 private:
  enum class State { Loading, Running, Error };

  // Matches the old per-loop draw() cadence closely enough for app-side polling without spinning
  // the render task.
  static constexpr unsigned long TICK_INTERVAL_MS = 33;

  LuaManager lua;
  std::string pluginName;
  std::atomic<State> state{State::Loading};
  bool inputReady = false;
  unsigned long lastTickMs = 0;

  void renderError();
};
