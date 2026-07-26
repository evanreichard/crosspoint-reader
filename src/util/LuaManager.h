#pragma once

#include <array>
#include <atomic>
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

  void registerBindings();
  void stripChunkDebugInfo();
  void setError(const char* message);
};
