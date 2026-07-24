#pragma once

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
  const char* getLastError() const { return lastError; }
  freeink::SecureHttpClient* getHttpClient();

 private:
  lua_State* state = nullptr;
  std::atomic_bool wantsExit{false};
  std::unique_ptr<freeink::SecureHttpClient> httpClient;
  char lastError[192] = {};

  void registerBindings();
  void setError(const char* message);
};
