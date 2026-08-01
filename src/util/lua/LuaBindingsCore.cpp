#include <Arduino.h>

#include <algorithm>
#include <cstring>

#include "Logging.h"
#include "MappedInputManager.h"
#include "util/LuaManager.h"
#include "util/lua/LuaBindings.h"

extern "C" {
#include <lauxlib.h>
}

using luabindings::addFunction;
using luabindings::getManager;

namespace {
constexpr uint32_t MIN_TIMER_INTERVAL_MS = 100;
constexpr uint32_t MIN_TICK_INTERVAL_MS = 33;
constexpr uint32_t MAX_RUNTIME_INTERVAL_MS = 60 * 60 * 1000;

MappedInputManager::Button parseButton(const char* name) {
  if (strcmp(name, "confirm") == 0) return MappedInputManager::Button::Confirm;
  if (strcmp(name, "left") == 0) return MappedInputManager::Button::Left;
  if (strcmp(name, "right") == 0) return MappedInputManager::Button::Right;
  if (strcmp(name, "up") == 0) return MappedInputManager::Button::Up;
  if (strcmp(name, "down") == 0) return MappedInputManager::Button::Down;
  if (strcmp(name, "page_back") == 0) return MappedInputManager::Button::PageBack;
  if (strcmp(name, "page_forward") == 0) return MappedInputManager::Button::PageForward;
  return MappedInputManager::Button::Back;
}
// --- Logs a debug message to serial output.
// -- @param message string
// -- @within log
int luaLogDebug(lua_State* state) {
  LOG_DBG("LUA", "%s", luaL_checkstring(state, 1));
  return 0;
}

// --- Logs an info message to serial output.
// -- @param message string
// -- @within log
int luaLogInfo(lua_State* state) {
  LOG_INF("LUA", "%s", luaL_checkstring(state, 1));
  return 0;
}

// --- Logs an error message to serial output.
// -- @param message string
// -- @within log
int luaLogError(lua_State* state) {
  LOG_ERR("LUA", "%s", luaL_checkstring(state, 1));
  return 0;
}

int luaLogLegacy(lua_State* state) {
  LOG_INF("LUA", "%s", luaL_checkstring(state, 2));
  return 0;
}
// --- Returns true if the button was pressed since the last input frame. Buttons: "back", "confirm", "left", "right",
// "up", "down", "page_back", "page_forward".
// -- @param button string Button name
// -- @return bool
// -- @within input
int inputWasPressed(lua_State* state) {
  auto* manager = getManager(state);
  lua_pushboolean(state,
                  manager && manager->wasLatchedPressed(static_cast<int>(parseButton(luaL_checkstring(state, 1)))));
  return 1;
}

// --- Returns true if the button was released since the last input frame.
// -- @param button string Button name
// -- @return bool
// -- @within input
int inputWasReleased(lua_State* state) {
  auto* manager = getManager(state);
  lua_pushboolean(state,
                  manager && manager->wasLatchedReleased(static_cast<int>(parseButton(luaL_checkstring(state, 1)))));
  return 1;
}

// --- Returns true while the button is currently held down.
// -- @param button string Button name
// -- @return bool
// -- @within input
int inputIsPressed(lua_State* state) {
  auto* manager = getManager(state);
  lua_pushboolean(state,
                  manager && manager->isLatchedPressed(static_cast<int>(parseButton(luaL_checkstring(state, 1)))));
  return 1;
}

// --- Returns true while any button is currently held down.
// -- @return bool
// -- @within input
int inputIsAnyPressed(lua_State* state) {
  auto* manager = getManager(state);
  lua_pushboolean(state, manager && manager->isAnyLatchedPressed());
  return 1;
}
// --- Returns milliseconds since boot.
// -- @return int
// -- @within sys
// -- @alias uptime
int sysMillis(lua_State* state) {
  lua_pushinteger(state, static_cast<lua_Integer>(millis()));
  return 1;
}

// --- Blocks the app for the given number of milliseconds.
// -- @param ms int
// -- @within sys
int sysDelay(lua_State* state) {
  delay(luaL_checkinteger(state, 1));
  return 0;
}

// --- Requests the app to exit back to the launcher.
// -- @within sys
int sysExit(lua_State* state) {
  if (auto* manager = getManager(state)) manager->requestExit();
  return 0;
}
// --- Enables the on_tick() callback at a fixed interval (0 disables, minimum 33ms). Requires on_tick() to be defined.
// -- @param intervalMs int 0-3600000
// -- @within app
int appSetTickInterval(lua_State* state) {
  const lua_Integer requested = luaL_checkinteger(state, 1);
  luaL_argcheck(state, requested >= 0 && requested <= MAX_RUNTIME_INTERVAL_MS, 1, "interval must be 0-3600000ms");
  const uint32_t interval = requested == 0 ? 0 : std::max<uint32_t>(requested, MIN_TICK_INTERVAL_MS);
  if (auto* manager = getManager(state)) manager->setTickInterval(interval);
  return 0;
}
bool isValidTimerId(const char* id, size_t length) {
  if (length == 0 || length >= 32) return false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = id[i];
    const bool alphanumeric = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (!alphanumeric && c != '_' && c != '-') return false;
  }
  return true;
}

int addTimer(lua_State* state, bool repeating) {
  const lua_Integer requested = luaL_checkinteger(state, 1);
  luaL_argcheck(state, requested >= MIN_TIMER_INTERVAL_MS && requested <= MAX_RUNTIME_INTERVAL_MS, 1,
                "interval must be 100-3600000ms");
  size_t idLength = 0;
  const char* id = luaL_checklstring(state, 2, &idLength);
  luaL_argcheck(state, isValidTimerId(id, idLength), 2, "id must be 1-31 letters, digits, underscores, or dashes");
  auto* manager = getManager(state);
  if (!manager || !manager->addTimer(static_cast<uint32_t>(requested), id, repeating)) {
    return luaL_error(state, "timer limit reached");
  }
  return 0;
}

// --- Fires on_timer(id) once after the interval. Apps define on_timer(id) to receive it.
// -- @param intervalMs int 100-3600000
// -- @param id string 1-31 letters, digits, underscores, or dashes
// -- @within timer
int timerAfter(lua_State* state) { return addTimer(state, false); }

// --- Fires on_timer(id) repeatedly at the interval until cancelled.
// -- @param intervalMs int 100-3600000
// -- @param id string 1-31 letters, digits, underscores, or dashes
// -- @within timer
int timerEvery(lua_State* state) { return addTimer(state, true); }

// --- Cancels a timer by id.
// -- @param id string
// -- @within timer
int timerCancel(lua_State* state) {
  size_t idLength = 0;
  const char* id = luaL_checklstring(state, 1, &idLength);
  luaL_argcheck(state, isValidTimerId(id, idLength), 1, "id must be 1-31 letters, digits, underscores, or dashes");
  if (auto* manager = getManager(state)) manager->cancelTimer(id);
  return 0;
}
}  // namespace

void luabindings::registerCore(lua_State* state) {
  lua_newtable(state);
  addFunction(state, "debug", luaLogDebug);
  addFunction(state, "info", luaLogInfo);
  addFunction(state, "error", luaLogError);
  lua_newtable(state);
  lua_pushcfunction(state, luaLogLegacy);
  lua_setfield(state, -2, "__call");
  lua_setmetatable(state, -2);
  lua_setglobal(state, "log");

  lua_newtable(state);
  addFunction(state, "wasPressed", inputWasPressed);
  addFunction(state, "wasReleased", inputWasReleased);
  addFunction(state, "isPressed", inputIsPressed);
  addFunction(state, "isAnyPressed", inputIsAnyPressed);
  lua_setglobal(state, "input");

  lua_newtable(state);
  addFunction(state, "millis", sysMillis);
  addFunction(state, "uptime", sysMillis);
  addFunction(state, "delay", sysDelay);
  addFunction(state, "exit", sysExit);
  lua_setglobal(state, "sys");

  lua_newtable(state);
  addFunction(state, "setTickInterval", appSetTickInterval);
  lua_setglobal(state, "app");

  lua_newtable(state);
  addFunction(state, "after", timerAfter);
  addFunction(state, "every", timerEvery);
  addFunction(state, "cancel", timerCancel);
  lua_setglobal(state, "timer");
}
