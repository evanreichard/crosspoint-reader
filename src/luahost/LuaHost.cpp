#include "luahost/LuaHost.h"

#include <Logging.h>

#include "fontIds.h"

namespace cplua {

LuaHost::LuaHost(GfxRenderer& renderer, MappedInputManager& input, HalGPIO& gpio, std::string appName)
    : Activity("LuaApp:" + appName, renderer, input), gpio(gpio), appName(std::move(appName)) {}

void LuaHost::onEnter() {
  Activity::onEnter();

  // The Lua state and the renderer's font cache together run past what loop() can afford
  // next to the watchdog, so app loading and the event pump live on their own task, as
  // they did in LuaManager.
  providers = std::make_unique<Providers>(renderer, gpio);
  esp32lua::Providers shared;
  shared.fs = &providers->fs;
  shared.gui = &providers->gui;
  shared.settings = &providers->settings;
  shared.sys = &providers->sys;
  shared.buttons = &providers->buttons;
  shared.http = &providers->http;
  shared.timer = &providers->timer;
  shared.wifi = &providers->wifi;
  shared.ble = &providers->ble;
  shared.log = &providers->log;

  runtime = std::make_unique<esp32lua::Runtime>(shared);

  xTaskCreate(&LuaHost::pollTask, "LuaApp", 8192, this, 5, nullptr);
}

void LuaHost::onExit() {
  Activity::onExit();
  runtime.reset();
  providers.reset();
}

void LuaHost::pollTask(void* arg) {
  auto* self = static_cast<LuaHost*>(arg);
  if (!self->runtime->open()) {
    self->loadError = "could not open runtime";
    self->state = State::Error;
    self->requestUpdate();
    vTaskDelete(nullptr);
    return;
  }
  if (!self->runtime->startApp(self->appName)) {
    self->loadError = "could not start " + self->appName;
    self->state = State::Error;
    self->requestUpdate();
    vTaskDelete(nullptr);
    return;
  }
  self->state = State::Running;
  self->requestUpdate();

  for (;;) {
    if (!self->runtime) break;  // activity exited under us
    self->providers->timer.pump(*self->runtime);
    self->pollButtons();
    if (self->runtime->hasPendingNavigation()) {
      if (!self->runtime->canGoBack()) break;  // back past the app root: leave the activity
      if (!self->runtime->applyPendingNavigation()) self->state = State::Error;
      self->requestUpdate();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  self->wantsExit = true;
  self->requestUpdate();
  vTaskDelete(nullptr);
}

void LuaHost::pollButtons() {
  for (const auto& b : kButtons) {
    const bool down = gpio.isPressed(b.halIndex);
    if (down == buttonDown[b.halIndex]) continue;
    buttonDown[b.halIndex] = down;
    runtime->callButton(b.role, down);
    requestUpdate();
  }
}

void LuaHost::loop() {
  if (state == State::Error && mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  // The poll task broke out -- back past the app's own root, or a navigation that failed to
  // start. finish() pops us back to Apps; it must run on this thread, not the Lua task.
  if (wantsExit) finish();
}

void LuaHost::render(RenderLock&&) {
  switch (state) {
    case State::Loading:
      renderer.clearScreen();
      renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2 - 10, "Loading...", true);
      renderer.displayBuffer();
      break;
    case State::Running:
      // The app's own painting went through the Gui provider and the batch committed the
      // frame; there is nothing chrome-side to add.
      break;
    case State::Error:
      renderError();
      break;
  }
}

void LuaHost::renderError() {
  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2 - 20, "App failed to load", true);
  renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2 + 10, loadError.c_str(), true);
  renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() - 40, "Press Back to return", true);
  renderer.displayBuffer();
}

}  // namespace cplua
