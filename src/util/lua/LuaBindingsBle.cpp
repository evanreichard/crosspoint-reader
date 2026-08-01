#include <Arduino.h>
#include <BLEDevice.h>

#include <cstdio>

#include "Logging.h"
#include "util/lua/LuaBindings.h"

extern "C" {
#include <lauxlib.h>
}

using luabindings::addFunction;

namespace {
// BLE state
bool ownsBle = false;
bool bleInitialized = false;
BLEClient* bleClient = nullptr;
constexpr size_t MAX_BLE_SCAN_RESULTS = 24;

// The BLE stack heap-allocates one BLEAdvertisedDevice per unique address and only frees them at
// clearResults(), so a crowded room can exhaust the heap mid-scan. Stop on the heap itself rather
// than on a result count, which is the wrong proxy.
constexpr uint32_t BLE_SCAN_HEAP_FLOOR = 16 * 1024;

struct BleScanResult {
  char name[32];
  char address[18];
  int rssi;
};

class BufferedBleScanCallbacks : public BLEAdvertisedDeviceCallbacks {
 public:
  void reset() { count = 0; }
  size_t getCount() const { return count; }
  const BleScanResult& getResult(size_t index) const { return results[index]; }

  void onResult(BLEAdvertisedDevice device) override {
    if (count < MAX_BLE_SCAN_RESULTS) {
      const String name = device.haveName() ? device.getName() : String();
      const String addressText = device.getAddress().toString();
      auto& result = results[count++];
      snprintf(result.name, sizeof(result.name), "%s", name.c_str());
      snprintf(result.address, sizeof(result.address), "%s", addressText.c_str());
      result.rssi = device.getRSSI();
    }
    const uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < BLE_SCAN_HEAP_FLOOR) {
      LOG_ERR("BLE", "Scan stopped early at %u results: free heap %u below floor", static_cast<unsigned>(count),
              freeHeap);
    }
    if (count >= MAX_BLE_SCAN_RESULTS || freeHeap < BLE_SCAN_HEAP_FLOOR) {
      BLEDevice::getScan()->stop();
    }
  }

 private:
  BleScanResult results[MAX_BLE_SCAN_RESULTS]{};
  size_t count = 0;
};

BufferedBleScanCallbacks bleScanCallbacks;
// --- Initializes the BLE stack. Call before any other BLE operation.
// -- @param name[opt] string Local device name (default "CrossPoint")
// -- @return bool ok
// -- @within ble
int bleInit(lua_State* state) {
  if (bleInitialized) {
    lua_pushboolean(state, true);
    return 1;
  }
  const char* name = luaL_optstring(state, 1, "CrossPoint");
  ownsBle = true;
  bleInitialized = BLEDevice::init(name);
  if (!bleInitialized) {
    LOG_ERR("BLE", "Failed to init BLE stack");
    ownsBle = false;
  }
  LOG_INF("BLE", "Init complete: free %u, largest %u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  lua_pushboolean(state, bleInitialized);
  return 1;
}

// --- Scans for BLE devices and returns a table of results.
// -- @param duration[opt] integer Scan duration in milliseconds (default 3000)
// -- @return table results Array of { name: string, address: string, rssi: int }
// -- @within ble
int bleScan(lua_State* state) {
  if (!bleInitialized) {
    lua_pushnil(state);
    lua_pushstring(state, "BLE not initialized");
    return 2;
  }
  const lua_Integer durationMs = luaL_optinteger(state, 1, 3000);
  if (durationMs < 1 || durationMs > 60000) {
    lua_pushnil(state);
    lua_pushstring(state, "Scan duration must be between 1 and 60000 milliseconds");
    return 2;
  }

  LOG_INF("BLE", "Scan start: free %u, largest %u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  BLEScan* scan = BLEDevice::getScan();
  bleScanCallbacks.reset();
  scan->setActiveScan(false);
  scan->setAdvertisedDeviceCallbacks(&bleScanCallbacks);
  scan->start(static_cast<uint32_t>((durationMs + 999) / 1000), false);
  scan->setAdvertisedDeviceCallbacks(nullptr);
  scan->clearResults();

  const size_t count = bleScanCallbacks.getCount();
  LOG_INF("BLE", "Scan complete: %u results, free %u, largest %u", static_cast<unsigned>(count), ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
  lua_createtable(state, static_cast<int>(count), 0);
  for (size_t i = 0; i < count; ++i) {
    const auto& result = bleScanCallbacks.getResult(i);
    lua_createtable(state, 0, 3);
    lua_pushstring(state, result.name);
    lua_setfield(state, -2, "name");
    lua_pushstring(state, result.address);
    lua_setfield(state, -2, "address");
    lua_pushinteger(state, result.rssi);
    lua_setfield(state, -2, "rssi");
    lua_rawseti(state, -2, i + 1);
  }
  return 1;
}

// --- Connects to a BLE device by address.
// -- @param address string BLE address (e.g. "AA:BB:CC:DD:EE:FF")
// -- @return bool ok
// -- @within ble
int bleConnect(lua_State* state) {
  if (!bleInitialized) {
    lua_pushboolean(state, false);
    return 1;
  }
  const char* addr = luaL_checkstring(state, 1);

  if (bleClient) {
    bleClient->disconnect();
    delete bleClient;
    bleClient = nullptr;
  }

  bleClient = BLEDevice::createClient();
  if (!bleClient) {
    LOG_ERR("BLE", "Failed to create BLE client");
    lua_pushboolean(state, false);
    return 1;
  }

  bool ok = bleClient->connect(BLEAddress(addr));
  if (!ok) {
    LOG_ERR("BLE", "Failed to connect to %s", addr);
    delete bleClient;
    bleClient = nullptr;
  }
  lua_pushboolean(state, ok);
  return 1;
}

// --- Disconnects from the current BLE device.
// -- @within ble
int bleDisconnect(lua_State* state) {
  if (bleClient) {
    bleClient->disconnect();
    delete bleClient;
    bleClient = nullptr;
  }
  return 0;
}

// --- Returns whether connected to a BLE device.
// -- @return bool connected
// -- @within ble
int bleIsConnected(lua_State* state) {
  lua_pushboolean(state, bleClient && bleClient->isConnected());
  return 1;
}

// --- Reads a BLE characteristic value.
// -- @param serviceUUID string Service UUID (e.g. "0000180f-0000-1000-8000-00805f9b34fb")
// -- @param charUUID string Characteristic UUID
// -- @return string|nil value Characteristic value as string, nil on failure
// -- @within ble
int bleRead(lua_State* state) {
  if (!bleClient || !bleClient->isConnected()) {
    lua_pushnil(state);
    return 1;
  }
  const char* serviceUuid = luaL_checkstring(state, 1);
  const char* charUuid = luaL_checkstring(state, 2);

  String value = bleClient->getValue(BLEUUID(serviceUuid), BLEUUID(charUuid));
  if (value.isEmpty()) {
    lua_pushnil(state);
  } else {
    lua_pushstring(state, value.c_str());
  }
  return 1;
}

// --- Writes a value to a BLE characteristic.
// -- @param serviceUUID string Service UUID
// -- @param charUUID string Characteristic UUID
// -- @param value string Value to write
// -- @return bool ok
// -- @within ble
int bleWrite(lua_State* state) {
  if (!bleClient || !bleClient->isConnected()) {
    lua_pushboolean(state, false);
    return 1;
  }
  const char* serviceUuid = luaL_checkstring(state, 1);
  const char* charUuid = luaL_checkstring(state, 2);
  size_t valueSize = 0;
  const char* value = luaL_checklstring(state, 3, &valueSize);

  bleClient->setValue(BLEUUID(serviceUuid), BLEUUID(charUuid), String(value));
  lua_pushboolean(state, true);
  return 1;
}

// --- Starts advertising this device as a BLE peripheral.
// -- @param name[opt] string Advertised name (default "CrossPoint")
// -- @return bool ok
// -- @within ble
int bleStartAdvertising(lua_State* state) {
  if (!bleInitialized) {
    lua_pushboolean(state, false);
    return 1;
  }
  const char* name = luaL_optstring(state, 1, "CrossPoint");
  BLEDevice::startAdvertising();
  LOG_DBG("BLE", "Advertising started: %s", name);
  lua_pushboolean(state, true);
  return 1;
}

// --- Stops BLE advertising.
// -- @within ble
int bleStopAdvertising(lua_State* state) {
  BLEDevice::stopAdvertising();
  return 0;
}

// --- Deinitializes the BLE stack, releasing resources.
// -- @within ble
int bleDeinit(lua_State* state) {
  if (bleClient) {
    bleClient->disconnect();
    delete bleClient;
    bleClient = nullptr;
  }
  if (bleInitialized) {
    BLEDevice::deinit(true);
    bleInitialized = false;
  }
  ownsBle = false;
  return 0;
}
}  // namespace

void luabindings::registerBle(lua_State* state) {
  lua_newtable(state);
  addFunction(state, "init", bleInit);
  addFunction(state, "scan", bleScan);
  addFunction(state, "connect", bleConnect);
  addFunction(state, "disconnect", bleDisconnect);
  addFunction(state, "isConnected", bleIsConnected);
  addFunction(state, "read", bleRead);
  addFunction(state, "write", bleWrite);
  addFunction(state, "startAdvertising", bleStartAdvertising);
  addFunction(state, "stopAdvertising", bleStopAdvertising);
  addFunction(state, "deinit", bleDeinit);
  lua_setglobal(state, "ble");
}

void luabindings::shutdownBle() {
  if (ownsBle) bleDeinit(nullptr);
}
