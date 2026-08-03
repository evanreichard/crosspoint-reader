#pragma once

// CrossPoint's implementations of the shared platform's provider interfaces.
//
// These are the firmware seam: the runtime and bindings in lib/esp32-lua-api are shared
// with slate32, while everything here talks to CrossPoint's HAL (HalStorage,
// GfxRenderer, HalGPIO) and its own settings and network stacks. A provider never
// decides what an app sees; it only carries out what the runtime asked.

#include <lua/providers.h>
#include <lua/runtime.h>  // Timer::pump drives Runtime::callTimer

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>

#include <memory>
#include <string>

namespace cplua {

class Log final : public esp32lua::LogProvider {
 public:
  void write(esp32lua::LogLevel level, const std::string& message) override;
};

class Fs final : public esp32lua::FsProvider {
  class Reader final : public esp32lua::FileReader {
   public:
    explicit Reader(HalFile&& file) : file(std::move(file)) {}
    int32_t read(char* out, int32_t maxBytes) override { return file.read(reinterpret_cast<uint8_t*>(out), maxBytes); }

   private:
    HalFile file;
  };

 public:
  esp32lua::FileReader* openRead(const std::string& path) override;
  bool exists(const std::string& path) const override { return Storage.exists(path.c_str()); }
  esp32lua::Status fileSize(const std::string& path, int32_t& size) const override;
  esp32lua::Status listDirs(const std::string& path, std::vector<std::string>& names) const override {
    return list(path, true, names);
  }
  esp32lua::Status listFiles(const std::string& path, std::vector<std::string>& names) const override {
    return list(path, false, names);
  }
  esp32lua::Status mkdir(const std::string& path) override;
  esp32lua::Status readFile(const std::string& path, int32_t maxBytes, std::string& content) const override;
  esp32lua::Status readLineAt(const std::string& path, int32_t offset, int32_t maxBytes, bool& found,
                              std::string& line, int32_t& nextOffset) const override;
  esp32lua::Status remove(const std::string& path) override {
    return Storage.remove(path.c_str()) ? esp32lua::Status::success() : esp32lua::Status::failure("remove failed");
  }
  esp32lua::Status removeTree(const std::string& path) override {
    return Storage.removeDir(path.c_str()) ? esp32lua::Status::success() : esp32lua::Status::failure("remove failed");
  }
  esp32lua::Status rename(const std::string& source, const std::string& destination) override {
    return Storage.rename(source.c_str(), destination.c_str()) ? esp32lua::Status::success()
                                                               : esp32lua::Status::failure("rename failed");
  }
  esp32lua::Status writeFile(const std::string& path, const std::string& content) override {
    return Storage.writeFile(path.c_str(), content.c_str()) ? esp32lua::Status::success()
                                                            : esp32lua::Status::failure("write failed");
  }

 private:
  static esp32lua::Status list(const std::string& path, bool dirs, std::vector<std::string>& names);
};

// Draws through GfxRenderer. E-ink frame delivery is deferred until the runtime's batch
// unwinds, when commit() hands the framebuffer to the panel.
class Gui final : public esp32lua::GuiProvider {
 public:
  explicit Gui(GfxRenderer& renderer) : renderer(renderer) {}

  esp32lua::FontIds fonts() const override;
  int32_t width() const override { return renderer.getScreenWidth(); }
  int32_t height() const override { return renderer.getScreenHeight(); }
  int32_t rotation() const override { return 0; }
  void setRotation(int32_t) override {}
  int32_t color(int32_t r, int32_t g, int32_t b) const override;
  void clear(int32_t color) override { renderer.clearScreen(color != 0 ? 0xFF : 0x00); }
  void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t color) override {
    renderer.fillRect(x, y, w, h, color != 0);
  }
  void drawRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t color) override {
    renderer.drawRect(x, y, w, h, color != 0);
  }
  void drawLine(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t color, int32_t width) override;
  void drawPixel(int32_t x, int32_t y, int32_t color) override { renderer.drawPixel(x, y, color != 0); }
  void drawCircle(int32_t x, int32_t y, int32_t radius, int32_t color, int32_t width) override;
  void fillCircle(int32_t x, int32_t y, int32_t radius, int32_t color, const int32_t*) override;
  void roundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, int32_t background, const int32_t* top,
                 const int32_t* bottom, const int32_t* border) override;
  void setFullscreen(bool) override {}
  void commit() override;
  void fillPolygon(const int32_t* xs, const int32_t* ys, size_t count, int32_t color) override;
  esp32lua::Status drawBmp(const std::string& path, const int32_t* x, const int32_t* y, const int32_t* maxWidth,
                           const int32_t* maxHeight) override;
  int32_t textWidth(int32_t font, const std::string& text, int32_t style) const override;
  int32_t fontHeight(int32_t font, int32_t style) const override;
  void drawText(int32_t font, int32_t x, int32_t y, const std::string& text, int32_t color, int32_t style,
                const int32_t* background) override;

 private:
  GfxRenderer& renderer;
};

// CrossPoint has no screen rotation or timezone setting yet; the runtime keeps asking, so
// the answers are fixed until the reader grows them.
class Settings final : public esp32lua::SettingsProvider {
 public:
  int32_t rotation() const override { return 0; }
  esp32lua::Status setRotation(int32_t) override { return esp32lua::Status::failure("this device does not rotate"); }
  std::string timezone() const override { return "UTC0"; }
  esp32lua::Status setTimezone(const std::string&) override { return esp32lua::Status::success(); }
};

class Sys final : public esp32lua::SysProvider {
 public:
  int32_t millis() const override;
  esp32lua::MemoryInfo memory() const override;
  bool isClockSynced() const override;
};

class Http final : public esp32lua::HttpProvider {
 public:
  esp32lua::Status request(const std::string& method, const std::string& url, const std::string& body,
                           const std::vector<esp32lua::HttpHeader>& headers, int32_t maxBytes,
                           esp32lua::HttpResponse& response) override;
  esp32lua::Status download(const std::string& url, const std::string& destination,
                            const esp32lua::HttpDownload& options, int32_t& bytesWritten) override;
};

// The runtime owns timer identity and callbacks; this only carries deadlines on the poll
// task's clock. The host pumps runDueTimers-equivalents by calling callTimer itself.
class Timer final : public esp32lua::TimerProvider {
 public:
  esp32lua::Status schedule(esp32lua::TimerId id, int32_t intervalMs, bool repeating) override;
  void cancel(esp32lua::TimerId id) override;

  // Fires every elapsed deadline through fire(). Called on the Lua thread.
  void pump(esp32lua::Runtime& runtime);

 private:
  struct Slot {
    int32_t intervalMs;
    uint32_t dueAt;
    bool repeating;
    bool active = false;
  };
  std::vector<Slot> slots_;
};

class Wifi final : public esp32lua::WifiProvider {
 public:
  esp32lua::Status scan(std::vector<esp32lua::WifiNetwork>& networks) override;
  esp32lua::Status connect(const std::string* ssid, const std::string* password) override;
  esp32lua::WifiStatus status() const override;
  void disconnect() override;
  esp32lua::Status forget() override;
};

// CrossPoint's BLE is a HID keyboard, which the runtime contract models as a client.
// Refuse cleanly rather than pretend: init reports what this device actually has.
class Ble final : public esp32lua::BleProvider {
 public:
  esp32lua::Status init(const std::string*) override {
    return esp32lua::Status::failure("this device's Bluetooth is a keyboard, not a client");
  }
  void deinit() override {}
  esp32lua::Status scan(int32_t, std::vector<esp32lua::BleDevice>&) override {
    return esp32lua::Status::failure("this device's Bluetooth is a keyboard, not a client");
  }
  esp32lua::Status connect(const std::string&) override { return esp32lua::Status::failure("no BLE client"); }
  void disconnect() override {}
  bool isConnected() const override { return false; }
  esp32lua::Status read(const std::string&, const std::string&, std::string&) override {
    return esp32lua::Status::failure("no BLE client");
  }
  esp32lua::Status write(const std::string&, const std::string&, const std::string&) override {
    return esp32lua::Status::failure("no BLE client");
  }
  esp32lua::Status startAdvertising(const std::string*) override { return esp32lua::Status::failure("no BLE server"); }
  void stopAdvertising() override {}
};

class Buttons final : public esp32lua::ButtonsProvider {
 public:
  explicit Buttons(HalGPIO& gpio) : gpio(gpio) {}
  std::vector<std::string> buttons() const override { return {"up", "down", "left", "right", "confirm", "back"}; }
  bool isAnyPressed() const override;
  bool isPressed(const std::string& button) const override;
  bool wasPressed(const std::string& button) const override;
  bool wasReleased(const std::string& button) const override;

 private:
  HalGPIO& gpio;
  static int halIndex(const std::string& button);
};

struct Providers {
  Log log;
  Fs fs;
  Gui gui;
  Settings settings;
  Sys sys;
  Http http;
  Timer timer;
  Wifi wifi;
  Ble ble;
  Buttons buttons;
  explicit Providers(GfxRenderer& renderer, HalGPIO& gpio) : gui(renderer), buttons(gpio) {}
};

}  // namespace cplua
