#pragma once

extern "C" {
#include <lua.h>
}

class GfxRenderer;
class LuaManager;
class MappedInputManager;

// Bindings Split By Module - each register*() owns one Lua namespace and lives in its own
// translation unit, so module state (Wi-Fi, BLE) stays private to the module that mutates it.
// scripts/gen_lua_stubs.py parses the annotations and these bodies to generate the LuaLS stub.
namespace luabindings {

inline LuaManager* getManager(lua_State* state) {
  lua_getfield(state, LUA_REGISTRYINDEX, "manager");
  auto* manager = static_cast<LuaManager*>(lua_touserdata(state, -1));
  lua_pop(state, 1);
  return manager;
}

inline GfxRenderer* getRenderer(lua_State* state) {
  lua_getfield(state, LUA_REGISTRYINDEX, "renderer");
  auto* renderer = static_cast<GfxRenderer*>(lua_touserdata(state, -1));
  lua_pop(state, 1);
  return renderer;
}

inline MappedInputManager* getInput(lua_State* state) {
  lua_getfield(state, LUA_REGISTRYINDEX, "input");
  auto* input = static_cast<MappedInputManager*>(lua_touserdata(state, -1));
  lua_pop(state, 1);
  return input;
}

inline void addFunction(lua_State* state, const char* name, lua_CFunction function) {
  lua_pushcfunction(state, function);
  lua_setfield(state, -2, name);
}

void registerCore(lua_State* state);  // log, input, sys, app, timer
void registerGui(lua_State* state);   // gui plus the REFRESH_/COLOR_/FONT_/STYLE_ constants
void registerFs(lua_State* state);
void registerNet(lua_State* state);  // wifi, http
void registerBle(lua_State* state);

// Called from LuaManager::end(); the owning module releases what it started.
void shutdownNet();
void shutdownBle();

}  // namespace luabindings
