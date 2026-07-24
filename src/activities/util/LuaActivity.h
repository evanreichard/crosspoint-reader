#pragma once

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

  LuaManager lua;
  std::string pluginName;
  State state = State::Loading;
  bool inputReady = false;

  void renderError();
};
