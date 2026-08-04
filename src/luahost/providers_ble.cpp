#include "providers.h"

#include <Arduino.h>
#include <BLEDevice.h>
#include <Logging.h>

namespace cplua {

namespace {

// The BLE stack heap-allocates one BLEAdvertisedDevice per unique address and only frees them at
// clearResults(), so a crowded room can exhaust the heap mid-scan. Stop on the heap itself rather
// than on a result count, which is the wrong proxy.
constexpr uint32_t BLE_SCAN_HEAP_FLOOR = 16 * 1024;
constexpr size_t MAX_BLE_SCAN_RESULTS = 24;

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
    if (count >= MAX_BLE_SCAN_RESULTS || freeHeap < BLE_SCAN_HEAP_FLOOR) {
      BLEDevice::getScan()->stop();
    }
  }

 private:
  BleScanResult results[MAX_BLE_SCAN_RESULTS]{};
  size_t count = 0;
};

BufferedBleScanCallbacks scanCallbacks;

// One provider instance owns the stack for its lifetime; onExit drops the whole Providers
// struct, so deinit always fires.
bool bleInitialized = false;
BLEClient* bleClient = nullptr;

}  // namespace

esp32lua::Status Ble::init(const std::string* name) {
  if (bleInitialized) return esp32lua::Status::success();
  bleInitialized = BLEDevice::init(name ? name->c_str() : "CrossPoint");
  if (!bleInitialized) return esp32lua::Status::failure("BLE stack init failed");
  LOG_INF("BLE", "Init complete: free %u, largest %u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  return esp32lua::Status::success();
}

esp32lua::Status Ble::scan(int32_t durationMs, std::vector<esp32lua::BleDevice>& devices) {
  if (!bleInitialized) return esp32lua::Status::failure("BLE not initialized");

  LOG_INF("BLE", "Scan start: free %u, largest %u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  BLEScan* scan = BLEDevice::getScan();
  scanCallbacks.reset();
  scan->setActiveScan(false);
  scan->setAdvertisedDeviceCallbacks(&scanCallbacks);
  scan->start(static_cast<uint32_t>((durationMs + 999) / 1000), false);
  scan->setAdvertisedDeviceCallbacks(nullptr);
  scan->clearResults();

  const size_t count = scanCallbacks.getCount();
  LOG_INF("BLE", "Scan complete: %u results, free %u", static_cast<unsigned>(count), ESP.getFreeHeap());
  devices.clear();
  for (size_t i = 0; i < count; ++i) {
    const auto& r = scanCallbacks.getResult(i);
    devices.push_back({r.name, r.address, r.rssi});
  }
  return esp32lua::Status::success();
}

esp32lua::Status Ble::connect(const std::string& address) {
  if (!bleInitialized) return esp32lua::Status::failure("BLE not initialized");
  if (bleClient) {
    bleClient->disconnect();
    delete bleClient;
    bleClient = nullptr;
  }
  bleClient = BLEDevice::createClient();
  if (!bleClient) return esp32lua::Status::failure("could not create BLE client");
  if (!bleClient->connect(BLEAddress(address.c_str()))) {
    delete bleClient;
    bleClient = nullptr;
    return esp32lua::Status::failure("connect failed");
  }
  return esp32lua::Status::success();
}

void Ble::disconnect() {
  if (bleClient) {
    bleClient->disconnect();
    delete bleClient;
    bleClient = nullptr;
  }
}

bool Ble::isConnected() const { return bleClient && bleClient->isConnected(); }

esp32lua::Status Ble::read(const std::string& service, const std::string& characteristic, std::string& value) {
  if (!bleClient || !bleClient->isConnected()) return esp32lua::Status::failure("not connected");
  String result = bleClient->getValue(BLEUUID(service.c_str()), BLEUUID(characteristic.c_str()));
  if (result.isEmpty()) return esp32lua::Status::failure("read returned empty");
  value = result.c_str();
  return esp32lua::Status::success();
}

esp32lua::Status Ble::write(const std::string& service, const std::string& characteristic,
                            const std::string& value) {
  if (!bleClient || !bleClient->isConnected()) return esp32lua::Status::failure("not connected");
  bleClient->setValue(BLEUUID(service.c_str()), BLEUUID(characteristic.c_str()), String(value.c_str()));
  return esp32lua::Status::success();
}

esp32lua::Status Ble::startAdvertising(const std::string*) {
  if (!bleInitialized) return esp32lua::Status::failure("BLE not initialized");
  BLEDevice::startAdvertising();
  return esp32lua::Status::success();
}

void Ble::stopAdvertising() { BLEDevice::stopAdvertising(); }

void Ble::deinit() {
  if (bleClient) {
    bleClient->disconnect();
    delete bleClient;
    bleClient = nullptr;
  }
  if (bleInitialized) {
    BLEDevice::deinit(true);
    bleInitialized = false;
  }
}

}  // namespace cplua
