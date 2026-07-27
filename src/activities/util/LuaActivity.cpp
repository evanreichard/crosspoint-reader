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

  // Poll On The Main Task - the render task can be inside a panel refresh or a download for a
  // second or more, and input events survive only one poll, so latch them here and let the Lua
  // call in render() drain them. Notifications coalesce, so a burst of presses costs one repaint.
  lua.latchInputEvents();

  // Apps poll state inside draw() (waiting on WiFi, timers), so keep ticking when idle rather
  // than only on input. Notifications are cheap; the app decides whether to repaint the panel.
  const unsigned long now = millis();
  if (now - lastTickMs < TICK_INTERVAL_MS) return;
  lastTickMs = now;
  requestUpdate(true);
}

void LuaActivity::render(RenderLock&&) {
  if (state == State::Running) {
    // Drain One Event Per draw() - apps read at most one press per call, so a burst needs one
    // pass each to advance state. Only the final pass is allowed to touch the panel.
    do {
      lua.beginInputFrame();
      lua.setRefreshSuppressed(lua.hasPendingInputEvents());
      if (!lua.callFunction("draw")) {
        lua.setRefreshSuppressed(false);
        lua.dropHeldRefresh();
        state = State::Error;
        renderError();
        return;
      }
    } while (lua.hasPendingInputEvents());
    lua.setRefreshSuppressed(false);
    lua.flushHeldRefresh(renderer);
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
