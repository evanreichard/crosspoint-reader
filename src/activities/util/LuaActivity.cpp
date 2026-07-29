#include "LuaActivity.h"

#include <FontCacheManager.h>
#include <I18n.h>

#include <cstdio>

#include "fontIds.h"

void LuaActivity::onEnter() {
  Activity::onEnter();
  state = State::Loading;
  requestUpdateAndWait();

  // Serialize Against The Render Task - every other Lua call happens in render(); holding the lock
  // here keeps VM setup from overlapping a render of the loading screen.
  bool started = false;
  {
    RenderLock lock(*this);
    started = lua.begin(renderer, mappedInput) && lua.runPlugin(pluginName) && lua.callFunction("init");
    if (started) started = lua.initializeRuntime();
  }
  if (!started) {
    state = State::Error;
    requestUpdate();
    return;
  }

  inputReady = false;
  state = State::Running;
  requestUpdate();
}

void LuaActivity::onExit() {
  lua.end();

  // Release The Glyph Cache - app text uses the compressed reading fonts through the
  // non-prewarmed path, which parks a ~10 KB decompressed group in the global FontDecompressor
  // for the rest of the boot. Native screens prewarm and release per page, so nothing else
  // reclaims it.
  // onExit() already runs under the render lock held by ActivityManager::exitActivity().
  if (auto* fcm = renderer.getFontCacheManager()) fcm->clearCache();

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

  if (lua.hasDrawCallback()) lua.latchInputEvents();
  const bool buttonQueued = lua.enqueueButtonEvents();
  const bool runtimeQueued = lua.pollRuntime(millis());
  if (buttonQueued || runtimeQueued) requestUpdate(true);
}

void LuaActivity::render(RenderLock&&) {
  if (state == State::Running) {
    if (!lua.dispatchPending(renderer)) {
      state = State::Error;
      renderError();
    }
    return;
  }

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
