#include "LuaManager.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Memory.h>
#include <SecureHttpClient.h>
#include <esp_random.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "Logging.h"
#include "MappedInputManager.h"
#include "util/lua/LuaBindings.h"

extern "C" {
#include <lauxlib.h>
#include <lualib.h>
}

using freeink::SecureHttpClient;

namespace {
portMUX_TYPE luaRuntimeSpinlock = portMUX_INITIALIZER_UNLOCKED;

bool deadlineReached(uint32_t now, uint32_t deadline) { return static_cast<int32_t>(now - deadline) >= 0; }

struct LuaFileReader {
  HalFile file;
  uint8_t buffer[512];
};

const char* readLuaChunk(lua_State*, void* context, size_t* size) {
  auto* reader = static_cast<LuaFileReader*>(context);
  const int read = reader->file.read(reader->buffer, sizeof(reader->buffer));
  *size = read > 0 ? static_cast<size_t>(read) : 0;
  return read > 0 ? reinterpret_cast<const char*>(reader->buffer) : nullptr;
}
}  // namespace

LuaManager::LuaManager() = default;

LuaManager::~LuaManager() { end(); }

void LuaManager::setError(const char* message) {
  snprintf(lastError, sizeof(lastError), "%s", message ? message : "Unknown Lua error");
}

SecureHttpClient* LuaManager::getHttpClient() {
  if (!httpClient) {
    httpClient = makeUniqueNoThrow<SecureHttpClient>();
    if (!httpClient) {
      LOG_ERR("LUA", "OOM: HTTP client");
      return nullptr;
    }
    httpClient->setInsecure();
    httpClient->setTimeout(60000);
  }
  return httpClient.get();
}

bool LuaManager::begin(GfxRenderer& renderer, MappedInputManager& input) {
  if (state) return true;
  wantsExit.store(false);
  this->input = &input;
  clearInputEvents();
  resetRuntime();
  lastError[0] = '\0';
  LOG_INF("LUA", "Heap before VM: %u", ESP.getFreeHeap());
  state = luaL_newstate();
  if (!state) {
    setError("Not enough memory to create Lua VM");
    LOG_ERR("LUA", "%s", lastError);
    return false;
  }
  lua_pushlightuserdata(state, this);
  lua_setfield(state, LUA_REGISTRYINDEX, "manager");
  lua_pushlightuserdata(state, &renderer);
  lua_setfield(state, LUA_REGISTRYINDEX, "renderer");
  lua_pushlightuserdata(state, &input);
  lua_setfield(state, LUA_REGISTRYINDEX, "input");

  LOG_INF("LUA", "VM created, heap: %u", ESP.getFreeHeap());
  return true;
}

void LuaManager::latchInputEvents() {
  if (!input) return;
  static constexpr MappedInputManager::Button LATCHED[] = {
      MappedInputManager::Button::Back,     MappedInputManager::Button::Confirm,     MappedInputManager::Button::Left,
      MappedInputManager::Button::Right,    MappedInputManager::Button::Up,          MappedInputManager::Button::Down,
      MappedInputManager::Button::PageBack, MappedInputManager::Button::PageForward,
  };
  uint16_t held = 0;
  for (const auto button : LATCHED) {
    const auto index = static_cast<size_t>(button);
    const auto bit = static_cast<uint16_t>(1u << index);
    if (input->wasPressed(button)) latchedPressed[index].fetch_add(1);
    if (input->wasReleased(button)) latchedReleased[index].fetch_add(1);
    if (input->isPressed(button)) held |= bit;
  }
  latchedHeld.store(held);
}

bool LuaManager::enqueueButtonEvents() {
  if (!input || !hasOnButton) return false;
  static constexpr MappedInputManager::Button BUTTONS[] = {
      MappedInputManager::Button::Back,  MappedInputManager::Button::Confirm,  MappedInputManager::Button::Left,
      MappedInputManager::Button::Right, MappedInputManager::Button::PageBack, MappedInputManager::Button::PageForward,
  };
  const auto edges = input->getButtonEdges();
  bool queued = false;
  for (uint8_t i = 0; i < std::size(BUTTONS); ++i) {
    const uint16_t bit = 1u << static_cast<uint8_t>(BUTTONS[i]);
    if (edges.pressed & bit) queued = enqueueEvent({EventType::Button, i, true, {}}) || queued;
    if (edges.released & bit) queued = enqueueEvent({EventType::Button, i, false, {}}) || queued;
  }
  return queued;
}

void LuaManager::beginInputFrame() {
  framePressed = 0;
  frameReleased = 0;
  const auto consumeOne = [](std::atomic<uint16_t>& count) {
    uint16_t value = count.load();
    while (value > 0 && !count.compare_exchange_weak(value, value - 1)) {
    }
    return value > 0;
  };
  for (size_t i = 0; i < INPUT_BUTTON_COUNT; ++i) {
    if (consumeOne(latchedPressed[i])) framePressed |= 1u << i;
    if (consumeOne(latchedReleased[i])) frameReleased |= 1u << i;
  }
}

bool LuaManager::hasPendingInputEvents() const {
  for (size_t i = 0; i < INPUT_BUTTON_COUNT; ++i) {
    if (latchedPressed[i].load() || latchedReleased[i].load()) return true;
  }
  return false;
}

void LuaManager::flushHeldRefresh(GfxRenderer& renderer) {
  if (heldRefreshMode < 0) return;
  renderer.displayBuffer(static_cast<HalDisplay::RefreshMode>(heldRefreshMode));
  heldRefreshMode = -1;
}

void LuaManager::clearInputEvents() {
  for (auto& count : latchedPressed) count.store(0);
  for (auto& count : latchedReleased) count.store(0);
  latchedHeld.store(0);
  framePressed = 0;
  frameReleased = 0;
  refreshSuppressed = false;
  heldRefreshMode = -1;
}

void LuaManager::resetRuntime() {
  taskENTER_CRITICAL(&luaRuntimeSpinlock);
  eventHead = 0;
  eventCount = 0;
  tickIntervalMs = 0;
  tickDeadlineMs = 0;
  drawDeadlineMs = 0;
  tickPending = false;
  drawPending = false;
  overflowLogged = false;
  hasDraw = false;
  hasOnButton = false;
  for (auto& timer : timers) timer = Timer{};
  taskEXIT_CRITICAL(&luaRuntimeSpinlock);
}

bool LuaManager::initializeRuntime() {
  const bool draw = hasFunction("draw");
  const bool onTick = hasFunction("on_tick");
  const bool onButton = hasFunction("on_button");
  const bool onTimer = hasFunction("on_timer");
  const uint32_t now = millis();
  bool timersActive = false;
  bool tickActive = false;
  taskENTER_CRITICAL(&luaRuntimeSpinlock);
  hasDraw = draw;
  hasOnButton = onButton;
  drawPending = draw;
  drawDeadlineMs = now + DRAW_INTERVAL_MS;
  if (tickIntervalMs > 0) tickDeadlineMs = now + tickIntervalMs;
  tickActive = tickIntervalMs > 0;
  for (const auto& timer : timers) timersActive = timersActive || timer.active;
  taskEXIT_CRITICAL(&luaRuntimeSpinlock);
  if (tickActive && !onTick) {
    setError("Missing on_tick()");
    LOG_ERR("LUA", "%s", lastError);
    return false;
  }
  if (timersActive && !onTimer) {
    setError("Missing on_timer()");
    LOG_ERR("LUA", "%s", lastError);
    return false;
  }
  return true;
}

bool LuaManager::enqueueEvent(const RuntimeEvent& event) {
  bool queued = false;
  bool logOverflow = false;
  taskENTER_CRITICAL(&luaRuntimeSpinlock);
  if (eventCount < EVENT_CAPACITY) {
    events[(eventHead + eventCount) % EVENT_CAPACITY] = event;
    ++eventCount;
    queued = true;
  } else if (!overflowLogged) {
    overflowLogged = true;
    logOverflow = true;
  }
  taskEXIT_CRITICAL(&luaRuntimeSpinlock);
  if (logOverflow) LOG_ERR("LUA", "Event queue full; rejecting incoming event");
  return queued;
}

bool LuaManager::popEvent(RuntimeEvent& event) {
  taskENTER_CRITICAL(&luaRuntimeSpinlock);
  if (eventCount == 0) {
    taskEXIT_CRITICAL(&luaRuntimeSpinlock);
    return false;
  }
  event = events[eventHead];
  eventHead = (eventHead + 1) % EVENT_CAPACITY;
  --eventCount;
  overflowLogged = false;
  taskEXIT_CRITICAL(&luaRuntimeSpinlock);
  return true;
}

void LuaManager::removeQueuedEvents(EventType type, const char* timerId) {
  size_t kept = 0;
  for (size_t i = 0; i < eventCount; ++i) {
    const size_t read = (eventHead + i) % EVENT_CAPACITY;
    const bool matches = events[read].type == type &&
                         (type != EventType::Timer || (timerId && strcmp(events[read].timerId, timerId) == 0));
    if (!matches) {
      events[(eventHead + kept) % EVENT_CAPACITY] = events[read];
      ++kept;
    }
  }
  eventCount = kept;
}

bool LuaManager::preventsAutoSleep() {
  taskENTER_CRITICAL(&luaRuntimeSpinlock);
  const bool prevent = hasDraw || tickIntervalMs > 0;
  taskEXIT_CRITICAL(&luaRuntimeSpinlock);
  return prevent;
}

void LuaManager::setTickInterval(uint32_t intervalMs) {
  const uint32_t now = millis();
  taskENTER_CRITICAL(&luaRuntimeSpinlock);
  removeQueuedEvents(EventType::Tick);
  tickPending = false;
  tickIntervalMs = intervalMs;
  tickDeadlineMs = intervalMs > 0 ? now + intervalMs : 0;
  taskEXIT_CRITICAL(&luaRuntimeSpinlock);
}

bool LuaManager::addTimer(uint32_t intervalMs, const char* id, bool repeating) {
  const uint32_t now = millis();
  taskENTER_CRITICAL(&luaRuntimeSpinlock);
  Timer* slot = nullptr;
  for (auto& timer : timers) {
    if (timer.active && strcmp(timer.id, id) == 0) {
      slot = &timer;
      break;
    }
    if (!timer.active && !slot) slot = &timer;
  }
  if (!slot) {
    taskEXIT_CRITICAL(&luaRuntimeSpinlock);
    return false;
  }
  removeQueuedEvents(EventType::Timer, id);
  *slot = Timer{};
  slot->active = true;
  slot->repeating = repeating;
  slot->intervalMs = intervalMs;
  slot->deadlineMs = now + intervalMs;
  snprintf(slot->id, sizeof(slot->id), "%s", id);
  taskEXIT_CRITICAL(&luaRuntimeSpinlock);
  return true;
}

void LuaManager::cancelTimer(const char* id) {
  taskENTER_CRITICAL(&luaRuntimeSpinlock);
  for (auto& timer : timers) {
    if (timer.active && strcmp(timer.id, id) == 0) timer = Timer{};
  }
  removeQueuedEvents(EventType::Timer, id);
  taskEXIT_CRITICAL(&luaRuntimeSpinlock);
}

bool LuaManager::pollRuntime(uint32_t now) {
  bool queued = false;
  bool logOverflow = false;
  taskENTER_CRITICAL(&luaRuntimeSpinlock);
  const auto pushEvent = [&](const RuntimeEvent& event) {
    if (eventCount >= EVENT_CAPACITY) {
      if (!overflowLogged) {
        overflowLogged = true;
        logOverflow = true;
      }
      return false;
    }
    events[(eventHead + eventCount) % EVENT_CAPACITY] = event;
    ++eventCount;
    queued = true;
    return true;
  };

  if (hasDraw && !drawPending && deadlineReached(now, drawDeadlineMs)) {
    drawPending = true;
    drawDeadlineMs = now + DRAW_INTERVAL_MS;
    queued = true;
  }
  if (tickIntervalMs > 0 && !tickPending && deadlineReached(now, tickDeadlineMs)) {
    if (pushEvent({EventType::Tick, 0, false, {}})) tickPending = true;
    if (!tickPending) tickDeadlineMs = now + tickIntervalMs;
  }
  for (auto& timer : timers) {
    if (!timer.active || timer.pending || !deadlineReached(now, timer.deadlineMs)) continue;
    RuntimeEvent event;
    event.type = EventType::Timer;
    snprintf(event.timerId, sizeof(event.timerId), "%s", timer.id);
    if (pushEvent(event)) {
      timer.pending = true;
    } else if (timer.repeating) {
      timer.deadlineMs = now + timer.intervalMs;
    } else {
      timer = Timer{};
    }
  }
  taskEXIT_CRITICAL(&luaRuntimeSpinlock);
  if (logOverflow) LOG_ERR("LUA", "Event queue full; rejecting incoming event");
  return queued;
}

bool LuaManager::dispatchPending(GfxRenderer& renderer) {
  static constexpr const char* BUTTON_NAMES[] = {"back", "confirm", "left", "right", "page_back", "page_forward"};
  size_t eventsToDispatch = 0;
  bool runDraw = false;
  taskENTER_CRITICAL(&luaRuntimeSpinlock);
  eventsToDispatch = eventCount;
  runDraw = drawPending;
  drawPending = false;
  taskEXIT_CRITICAL(&luaRuntimeSpinlock);

  bool ok = true;
  const bool batchRefresh = eventsToDispatch + (runDraw ? 1 : 0) > 1;
  setRefreshSuppressed(batchRefresh);

  RuntimeEvent event;
  for (size_t dispatched = 0; dispatched < eventsToDispatch && popEvent(event); ++dispatched) {
    if (event.type == EventType::Button) {
      ok = callFunction("on_button", BUTTON_NAMES[event.button], event.down ? "down" : "up");
    } else if (event.type == EventType::Timer) {
      ok = callFunction("on_timer", event.timerId);
      const uint32_t now = millis();
      taskENTER_CRITICAL(&luaRuntimeSpinlock);
      for (auto& timer : timers) {
        if (!timer.active || !timer.pending || strcmp(timer.id, event.timerId) != 0) continue;
        if (timer.repeating) {
          timer.pending = false;
          timer.deadlineMs = now + timer.intervalMs;
        } else {
          timer = Timer{};
        }
        break;
      }
      taskEXIT_CRITICAL(&luaRuntimeSpinlock);
    } else {
      ok = callFunction("on_tick");
      const uint32_t now = millis();
      taskENTER_CRITICAL(&luaRuntimeSpinlock);
      if (tickPending) {
        tickPending = false;
        tickDeadlineMs = tickIntervalMs > 0 ? now + tickIntervalMs : 0;
      }
      taskEXIT_CRITICAL(&luaRuntimeSpinlock);
    }
    if (!ok) break;
  }

  if (ok && runDraw) {
    do {
      beginInputFrame();
      if (!batchRefresh) setRefreshSuppressed(hasPendingInputEvents());
      ok = callFunction("draw");
    } while (ok && hasPendingInputEvents());
  }

  setRefreshSuppressed(false);
  if (ok) {
    flushHeldRefresh(renderer);
  } else {
    dropHeldRefresh();
  }
  return ok;
}

void LuaManager::end() {
  input = nullptr;
  clearInputEvents();
  resetRuntime();
  httpClient.reset();
  if (state) {
    lua_close(state);
    state = nullptr;
  }
  luabindings::shutdownNet();
  luabindings::shutdownBle();
  wantsExit.store(false);
}

void LuaManager::registerBindings() {
  luabindings::registerCore(state);
  luabindings::registerGui(state);
  luabindings::registerFs(state);
  luabindings::registerNet(state);
  luabindings::registerBle(state);
}

// Strip Debug Info - Lua keeps per-instruction line tables, local names, and upvalue names for
// every prototype, which for a ~18 KB script costs more RAM than the bytecode itself. Dumping
// with strip and reloading discards them, at the cost of line numbers in runtime error messages.
// Leaves the stack unchanged on any failure: the original chunk stays loaded.
void LuaManager::stripChunkDebugInfo() {
  std::string stripped;
  const auto writer = [](lua_State*, const void* chunk, size_t size, void* out) {
    static_cast<std::string*>(out)->append(static_cast<const char*>(chunk), size);
    return 0;
  };
  if (lua_dump(state, writer, &stripped, 1) != 0 || stripped.empty()) return;

  if (luaL_loadbuffer(state, stripped.data(), stripped.size(), "=app") != LUA_OK) {
    LOG_ERR("LUA", "Stripped reload failed: %s", lua_tostring(state, -1));
    lua_pop(state, 1);
    return;
  }
  lua_remove(state, -2);  // drop the original chunk, keeping the stripped one
  lua_gc(state, LUA_GCCOLLECT, 0);
}

bool LuaManager::runPlugin(const std::string& pluginName) {
  if (!state) return false;
  const std::string path = "/.apps/" + pluginName + "/main.lua";
  auto reader = makeUniqueNoThrow<LuaFileReader>();
  if (!reader) {
    setError("Not enough memory to load plugin");
    LOG_ERR("LUA", "%s", lastError);
    return false;
  }
  if (!Storage.openFileForRead("LUA", path, reader->file)) {
    setError("Plugin main.lua was not found");
    return false;
  }

  const std::string chunkName = "@" + pluginName;
  int result = lua_load(state, readLuaChunk, reader.get(), chunkName.c_str(), nullptr);
  reader->file.close();
  if (result == LUA_OK) stripChunkDebugInfo();
  if (result == LUA_OK) {
    // Delay Globals Until After Compilation - Bytecode stripping temporarily holds both compiled chunks.
    luaL_openlibs(state);
    registerBindings();

    lua_getglobal(state, "math");
    lua_getfield(state, -1, "randomseed");
    lua_remove(state, -2);
    lua_pushinteger(state, static_cast<lua_Integer>(esp_random()));
    result = lua_pcall(state, 1, 0, 0);
    if (result == LUA_OK) {
      LOG_INF("LUA", "Runtime ready, heap: %u", ESP.getFreeHeap());
      result = lua_pcall(state, 0, 0, 0);
    }
  }
  if (result != LUA_OK) {
    setError(lua_tostring(state, -1));
    LOG_ERR("LUA", "%s", lastError);
    lua_pop(state, 1);
    return false;
  }
  return true;
}

bool LuaManager::hasFunction(const char* functionName) {
  if (!state) return false;
  const int stackTop = lua_gettop(state);
  lua_getglobal(state, functionName);
  const bool found = lua_isfunction(state, -1);
  lua_settop(state, stackTop);
  return found;
}

bool LuaManager::callFunction(const char* functionName) { return callFunction(functionName, nullptr, nullptr); }

bool LuaManager::callFunction(const char* functionName, const char* firstArg, const char* secondArg) {
  if (!state) return false;
  const int stackTop = lua_gettop(state);
  lua_getglobal(state, functionName);
  if (!lua_isfunction(state, -1)) {
    lua_settop(state, stackTop);
    snprintf(lastError, sizeof(lastError), "Missing %s()", functionName);
    return false;
  }
  int argumentCount = 0;
  if (firstArg) {
    lua_pushstring(state, firstArg);
    ++argumentCount;
  }
  if (secondArg) {
    lua_pushstring(state, secondArg);
    ++argumentCount;
  }
  if (lua_pcall(state, argumentCount, 0, 0) != LUA_OK) {
    setError(lua_tostring(state, -1));
    LOG_ERR("LUA", "%s", lastError);
    lua_settop(state, stackTop);
    return false;
  }
  lua_settop(state, stackTop);
  return true;
}
