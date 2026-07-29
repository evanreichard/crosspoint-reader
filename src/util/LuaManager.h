#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

extern "C" {
#include <lua.h>
}

class GfxRenderer;
class MappedInputManager;
namespace freeink {
class SecureHttpClient;
}

class LuaManager {
 public:
  LuaManager();
  ~LuaManager();
  LuaManager(const LuaManager&) = delete;
  LuaManager& operator=(const LuaManager&) = delete;

  bool begin(GfxRenderer& renderer, MappedInputManager& input);
  void end();
  bool runPlugin(const std::string& pluginName);
  bool callFunction(const char* functionName);
  bool initializeRuntime();
  bool enqueueButtonEvents();
  bool pollRuntime(uint32_t now);
  bool dispatchPending(GfxRenderer& renderer);
  bool hasDrawCallback() const { return hasDraw; }
  bool preventsAutoSleep();

  void setTickInterval(uint32_t intervalMs);
  bool addTimer(uint32_t intervalMs, const char* id, bool repeating);
  void cancelTimer(const char* id);

  void requestExit() { wantsExit.store(true); }
  bool checkAndClearExit() { return wantsExit.exchange(false); }

  // Latched Input - Lua runs on the render task, but InputManager overwrites its event bits on
  // every poll of the main task, so an event would be gone before Lua could read it. The activity
  // latches events as it polls (main task); beginInputFrame() hands them to the bindings and
  // reopens the latch, so presses arriving mid-draw survive to the next call.
  void latchInputEvents();
  void beginInputFrame();
  void clearInputEvents();
  bool hasPendingInputEvents() const;

  // Coalesce Bursts - a queued event means the frame being drawn is already stale, so hold its
  // panel push and let the final drain iteration display the settled state. Mirrors how native
  // activities collapse several requestUpdate() calls into one refresh.
  void setRefreshSuppressed(bool suppressed) { refreshSuppressed = suppressed; }
  bool isRefreshSuppressed() const { return refreshSuppressed; }

  // Replay The Held Push - the iteration that ends the burst may draw nothing (a queued release,
  // or a press the app ignores), so it issues no refresh of its own and the suppressed frame
  // would sit in the framebuffer forever. Push it once the drain loop settles.
  void holdRefresh(int mode) { heldRefreshMode = mode; }
  void dropHeldRefresh() { heldRefreshMode = -1; }
  void flushHeldRefresh(GfxRenderer& renderer);
  bool wasLatchedPressed(int button) const { return framePressed & (1u << button); }
  bool wasLatchedReleased(int button) const { return frameReleased & (1u << button); }
  bool isLatchedPressed(int button) const { return latchedHeld.load() & (1u << button); }
  bool isAnyLatchedPressed() const { return latchedHeld.load() != 0; }
  const char* getLastError() const { return lastError; }
  freeink::SecureHttpClient* getHttpClient();

 private:
  lua_State* state = nullptr;
  std::atomic_bool wantsExit{false};
  MappedInputManager* input = nullptr;
  static constexpr size_t INPUT_BUTTON_COUNT = 9;  // Back through PageForward
  std::array<std::atomic<uint16_t>, INPUT_BUTTON_COUNT> latchedPressed{};
  std::array<std::atomic<uint16_t>, INPUT_BUTTON_COUNT> latchedReleased{};
  std::atomic<uint16_t> latchedHeld{0};
  uint16_t framePressed = 0;  // render task only
  uint16_t frameReleased = 0;
  bool refreshSuppressed = false;  // render task only
  int heldRefreshMode = -1;        // render task only; -1 when nothing is held
  std::unique_ptr<freeink::SecureHttpClient> httpClient;
  char lastError[192] = {};

  enum class EventType : uint8_t { Button, Timer, Tick };
  struct RuntimeEvent {
    EventType type = EventType::Tick;
    uint8_t button = 0;
    bool down = false;
    char timerId[32] = {};
  };
  struct Timer {
    bool active = false;
    bool repeating = false;
    bool pending = false;
    uint32_t intervalMs = 0;
    uint32_t deadlineMs = 0;
    char id[32] = {};
  };

  static constexpr size_t EVENT_CAPACITY = 16;
  static constexpr size_t TIMER_CAPACITY = 8;
  static constexpr uint32_t DRAW_INTERVAL_MS = 33;
  std::array<RuntimeEvent, EVENT_CAPACITY> events{};
  std::array<Timer, TIMER_CAPACITY> timers{};
  size_t eventHead = 0;
  size_t eventCount = 0;
  uint32_t tickIntervalMs = 0;
  uint32_t tickDeadlineMs = 0;
  uint32_t drawDeadlineMs = 0;
  bool tickPending = false;
  bool drawPending = false;
  bool overflowLogged = false;
  bool hasDraw = false;
  bool hasOnButton = false;

  void registerBindings();
  void stripChunkDebugInfo();
  void setError(const char* message);
  bool hasFunction(const char* functionName);
  bool callFunction(const char* functionName, const char* firstArg, const char* secondArg = nullptr);
  bool enqueueEvent(const RuntimeEvent& event);
  bool popEvent(RuntimeEvent& event);
  void removeQueuedEvents(EventType type, const char* timerId = nullptr);
  void resetRuntime();
};
