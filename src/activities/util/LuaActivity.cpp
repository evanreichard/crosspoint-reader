#include "LuaActivity.h"

#include <I18n.h>

#include <cstdio>

#include "fontIds.h"

void LuaActivity::onEnter() {
  Activity::onEnter();
  state = State::Loading;
  requestUpdateAndWait();

  if (!lua.begin(renderer, mappedInput) || !lua.runPlugin(pluginName) || !lua.callFunction("init")) {
    state = State::Error;
    requestUpdate();
    return;
  }

  inputReady = false;
  state = State::Running;
}

void LuaActivity::onExit() {
  lua.end();
  Activity::onExit();
}

void LuaActivity::loop() {
  if (state == State::Error) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) finish();
    return;
  }

  if (lua.checkAndClearExit()) {
    finish();
    return;
  }

  if (!inputReady) {
    const bool anyPressed = mappedInput.isPressed(MappedInputManager::Button::Back) ||
                            mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
                            mappedInput.isPressed(MappedInputManager::Button::Left) ||
                            mappedInput.isPressed(MappedInputManager::Button::Right) ||
                            mappedInput.isPressed(MappedInputManager::Button::Up) ||
                            mappedInput.isPressed(MappedInputManager::Button::Down);
    if (!anyPressed && !mappedInput.wasAnyReleased()) inputReady = true;
    return;
  }

  RenderLock lock(*this);
  if (!lua.callFunction("draw")) {
    state = State::Error;
    renderError();
  }
}

void LuaActivity::render(RenderLock&&) {
  if (state == State::Running) return;
  renderer.clearScreen();
  if (state == State::Error) {
    renderError();
    return;
  }

  char message[128];
  snprintf(message, sizeof(message), tr(STR_LOADING_APP), pluginName.c_str());
  renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2, message);
  renderer.displayBuffer();
}

void LuaActivity::renderError() {
  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 3, tr(STR_APP_ERROR), true,
                            EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, lua.getLastError());
  renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() * 2 / 3, tr(STR_PRESS_BACK));
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}
