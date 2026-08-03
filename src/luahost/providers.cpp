#include "providers.h"

#include <Arduino.h>
#include <Bitmap.h>

#include <algorithm>
#include <utility>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <WiFi.h>

#include "fontIds.h"

namespace cplua {

void Log::write(esp32lua::LogLevel level, const std::string& message) {
  switch (level) {
    case esp32lua::LogLevel::Debug:
      LOG_DBG("LUA", "%s", message.c_str());
      break;
    case esp32lua::LogLevel::Info:
      LOG_INF("LUA", "%s", message.c_str());
      break;
    case esp32lua::LogLevel::Error:
      LOG_ERR("LUA", "%s", message.c_str());
      break;
  }
}

// ---------------------------------------------------------------------------

esp32lua::FileReader* Fs::openRead(const std::string& path) {
  HalFile file;
  if (!Storage.openFileForRead("LUA", path, file) || file.isDirectory()) return nullptr;
  return new Reader(std::move(file));
}

esp32lua::Status Fs::fileSize(const std::string& path, int32_t& size) const {
  HalFile file;
  if (!Storage.openFileForRead("LUA", path, file) || file.isDirectory()) {
    return esp32lua::Status::failure("cannot open file");
  }
  size = static_cast<int32_t>(file.fileSize());
  file.close();
  return esp32lua::Status::success();
}

esp32lua::Status Fs::list(const std::string& path, bool dirs, std::vector<std::string>& names) {
  auto root = Storage.open(path.c_str());
  if (!root || !root.isDirectory()) return esp32lua::Status::failure("not a directory");
  char name[256];
  for (auto entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    if (entry.isDirectory() == dirs) {
      entry.getName(name, sizeof(name));
      names.emplace_back(name);
    }
    entry.close();
  }
  root.close();
  return esp32lua::Status::success();
}

esp32lua::Status Fs::mkdir(const std::string& path) {
  return Storage.ensureDirectoryExists(path.c_str()) ? esp32lua::Status::success()
                                                     : esp32lua::Status::failure("could not create directory");
}

esp32lua::Status Fs::readFile(const std::string& path, int32_t maxBytes, std::string& content) const {
  HalFile file;
  if (!Storage.openFileForRead("LUA", path, file) || file.isDirectory()) {
    return esp32lua::Status::failure("cannot open file");
  }
  const int32_t size = std::min<int32_t>(static_cast<int32_t>(file.fileSize()), maxBytes);
  content.resize(size);
  const int got = file.read(reinterpret_cast<uint8_t*>(content.data()), size);
  content.resize(got > 0 ? got : 0);
  file.close();
  return esp32lua::Status::success();
}

esp32lua::Status Fs::readLineAt(const std::string& path, int32_t offset, int32_t maxBytes, bool& found,
                                std::string& line, int32_t& nextOffset) const {
  HalFile file;
  if (!Storage.openFileForRead("LUA", path, file) || file.isDirectory()) {
    return esp32lua::Status::failure("cannot open file");
  }
  if (offset > 0 && !file.seek(offset)) {
    file.close();
    return esp32lua::Status::failure("seek failed");
  }
  line.clear();
  found = false;
  while (static_cast<int32_t>(line.size()) < maxBytes) {
    uint8_t c;
    if (file.read(&c, 1) != 1) break;
    if (c == '\n') {
      found = true;
      break;
    }
    line.push_back(static_cast<char>(c));
  }
  if (!line.empty() && line.back() == '\r') line.pop_back();
  if (!found && !line.empty()) found = true;  // final line without a terminator
  nextOffset = offset + static_cast<int32_t>(line.size()) + (line.empty() ? 0 : 1);
  file.close();
  return esp32lua::Status::success();
}

// ---------------------------------------------------------------------------

// E-ink is 1bpp here: quantize any colour to black or white, the same way the old binding did.
int32_t Gui::color(int32_t r, int32_t g, int32_t b) const { return (r * 3 + g * 4 + b) / 8 < 128 ? 0 : 1; }

esp32lua::FontIds Gui::fonts() const {
  return {SMALL_FONT_ID, UI_10_FONT_ID, UI_12_FONT_ID, NOTOSANS_18_FONT_ID,
          EpdFontFamily::REGULAR, EpdFontFamily::BOLD};
}

void Gui::drawLine(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t color, int32_t width) {
  if (width <= 1) {
    renderer.drawLine(x1, y1, x2, y2, color != 0);
  } else {
    renderer.drawLine(x1, y1, x2, y2, width, color != 0);
  }
}

// GfxRenderer has no circle primitive; walk the outline with the midpoint algorithm and
// stamp a square brush, which is how the thicker widths read on e-ink anyway.
void Gui::drawCircle(int32_t x, int32_t y, int32_t radius, int32_t color, int32_t width) {
  for (int32_t t = 0; t < width; t++) {
    const int32_t r = radius - t;
    int32_t px = r, py = 0, err = 1 - r;
    while (px >= py) {
      for (auto [dx, dy] : {std::pair{px, py}, {py, px}, {-px, py}, {-py, px},
                            {-px, -py}, {-py, -px}, {px, -py}, {py, -px}}) {
        renderer.drawPixel(x + dx, y + dy, color != 0);
      }
      py++;
      if (err < 0) err += 2 * py + 1;
      else { px--; err += 2 * (py - px) + 1; }
    }
  }
}

void Gui::fillCircle(int32_t x, int32_t y, int32_t radius, int32_t color, const int32_t*) {
  for (int32_t dy = -radius; dy <= radius; dy++) {
    int32_t dx = radius;
    while (dx * dx + dy * dy > radius * radius) dx--;
    renderer.drawLine(x - dx, y + dy, x + dx, y + dy, color != 0);
  }
}

void Gui::roundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t, int32_t, const int32_t* top,
                    const int32_t* bottom, const int32_t* border) {
  // GfxRenderer has no rounded primitive; the corners stay square, which on e-ink reads as
  // intentional rather than broken.
  if (top && bottom) {
    const int32_t split = h / 2;
    fillRect(x, y, w, split, *top);
    fillRect(x, y + split, w, h - split, *bottom);
  } else if (top) {
    fillRect(x, y, w, h, *top);
  }
  if (border) drawRect(x, y, w, h, *border);
}

void Gui::fillPolygon(const int32_t* xs, const int32_t* ys, size_t count, int32_t color) {
  // Scanline fill, small enough for the few-point polys the node painter draws.
  if (count < 3) return;
  int32_t minY = ys[0], maxY = ys[0];
  for (size_t i = 1; i < count; i++) {
    minY = std::min(minY, ys[i]);
    maxY = std::max(maxY, ys[i]);
  }
  for (int32_t y = minY; y <= maxY; y++) {
    int32_t nodes[16];
    size_t hits = 0;
    for (size_t i = 0, j = count - 1; i < count && hits < 16; j = i++) {
      if ((ys[i] < y) != (ys[j] < y)) {
        nodes[hits++] = xs[i] + (y - ys[i]) * (xs[j] - xs[i]) / (ys[j] - ys[i]);
      }
    }
    std::sort(nodes, nodes + hits);
    for (size_t i = 0; i + 1 < hits; i += 2) {
      renderer.drawLine(nodes[i], y, nodes[i + 1], y, color != 0);
    }
  }
}

esp32lua::Status Gui::drawBmp(const std::string& path, const int32_t* x, const int32_t* y, const int32_t* maxWidth,
                              const int32_t* maxHeight) {
  HalFile file;
  if (!Storage.openFileForRead("LUA", path, file)) return esp32lua::Status::failure("cannot open file");
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    file.close();
    return esp32lua::Status::failure(Bitmap::errorToString(BmpReaderError::UnsupportedBpp));
  }
  const int32_t px = x ? *x : (width() - bitmap.getWidth()) / 2;
  const int32_t py = y ? *y : (height() - bitmap.getHeight()) / 2;
  renderer.drawBitmap1Bit(bitmap, px, py, maxWidth ? *maxWidth : width(), maxHeight ? *maxHeight : height());
  file.close();
  return esp32lua::Status::success();
}

int32_t Gui::textWidth(int32_t font, const std::string& text, int32_t style) const {
  return renderer.getTextWidth(font, text.c_str(), static_cast<EpdFontFamily::Style>(style));
}

int32_t Gui::fontHeight(int32_t font, int32_t) const { return renderer.getLineHeight(font); }

void Gui::drawText(int32_t font, int32_t x, int32_t y, const std::string& text, int32_t color, int32_t style,
                   const int32_t*) {
  renderer.drawText(font, x, y, text.c_str(), color == 0, static_cast<EpdFontFamily::Style>(style));
}

void Gui::commit() { renderer.displayBuffer(); }

// ---------------------------------------------------------------------------

int32_t Sys::millis() const { return ::millis(); }

esp32lua::MemoryInfo Sys::memory() const {
  return {static_cast<int32_t>(ESP.getFreeHeap()), static_cast<int32_t>(ESP.getHeapSize()),
          static_cast<int32_t>(ESP.getMaxAllocHeap())};
}

bool Sys::isClockSynced() const { return false; }

// ---------------------------------------------------------------------------

esp32lua::Status Http::request(const std::string& method, const std::string& url, const std::string& body,
                               const std::vector<esp32lua::HttpHeader>& headers, int32_t maxBytes,
                               esp32lua::HttpResponse& response) {
  if (WiFi.status() != WL_CONNECTED) return esp32lua::Status::failure("WiFi is not connected");
  freeink::SecureHttpClient client;
  if (!client.begin(url)) return esp32lua::Status::failure("malformed URL");
  for (const auto& h : headers) client.addHeader(h.name, h.value);
  client.setTimeout(15000);

  size_t received = 0;
  const int code = client.sendRequest(
      method.c_str(), reinterpret_cast<const uint8_t*>(body.data()), body.size(),
      [&](const uint8_t* data, size_t len) {
        if (static_cast<int32_t>(received + len) > maxBytes) return false;  // stop mid-stream over the cap
        received += len;
        return true;
      });
  client.end();
  if (code < 0) return esp32lua::Status::failure("request failed");
  response.status = code;
  response.body = client.getString();
  return esp32lua::Status::success();
}

esp32lua::Status Http::download(const std::string& url, const std::string& destination,
                                const esp32lua::HttpDownload& options, int32_t& bytesWritten) {
  if (WiFi.status() != WL_CONNECTED) return esp32lua::Status::failure("WiFi is not connected");
  freeink::SecureHttpClient client;
  if (!client.begin(url)) return esp32lua::Status::failure("malformed URL");
  client.setTimeout(30000);

  HalFile file;
  if (!Storage.openFileForWrite("LUA", destination, file)) {
    client.end();
    return esp32lua::Status::failure("cannot open destination");
  }
  size_t received = 0;
  bool failed = false;
  const int code = client.GET([&](const uint8_t* data, size_t len) {
    if (static_cast<int32_t>(received + len) > options.maxBytes) {
      failed = true;
      return false;
    }
    failed = file.write(data, len) != len;
    received += len;
    return !failed;
  });
  file.close();
  client.end();
  if (failed || code < 0 || code >= 400) {
    Storage.remove(destination.c_str());
    return esp32lua::Status::failure(code < 0 || failed ? "download failed" : "server returned an error");
  }
  bytesWritten = static_cast<int32_t>(received);
  return esp32lua::Status::success();
}

// ---------------------------------------------------------------------------

esp32lua::Status Timer::schedule(esp32lua::TimerId id, int32_t intervalMs, bool repeating) {
  if (static_cast<size_t>(id) >= slots_.size()) slots_.resize(id + 1);
  slots_[id] = {intervalMs, ::millis() + intervalMs, repeating, true};
  return esp32lua::Status::success();
}

void Timer::cancel(esp32lua::TimerId id) {
  if (static_cast<size_t>(id) < slots_.size()) slots_[id].active = false;
}

void Timer::pump(esp32lua::Runtime& runtime) {
  const uint32_t now = ::millis();
  for (size_t id = 0; id < slots_.size(); id++) {
    auto& slot = slots_[id];
    if (!slot.active || static_cast<int32_t>(now - slot.dueAt) < 0) continue;
    if (slot.repeating) {
      slot.dueAt = now + slot.intervalMs;
    } else {
      slot.active = false;
    }
    runtime.callTimer(id);
  }
}

// ---------------------------------------------------------------------------

esp32lua::Status Wifi::scan(std::vector<esp32lua::WifiNetwork>& networks) {
  const int16_t found = WiFi.scanNetworks();
  if (found < 0) return esp32lua::Status::failure("scan failed");
  for (int16_t i = 0; i < found; i++) {
    networks.push_back({WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.encryptionType(i) != WIFI_AUTH_OPEN});
  }
  WiFi.scanDelete();
  return esp32lua::Status::success();
}

esp32lua::Status Wifi::connect(const std::string* ssid, const std::string* password) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid ? ssid->c_str() : nullptr, password ? password->c_str() : nullptr);
  return esp32lua::Status::success();
}

esp32lua::WifiStatus Wifi::status() const {
  const wl_status_t state = WiFi.status();
  if (state == WL_CONNECTED) {
    return {"connected", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI()};
  }
  if (state == WL_IDLE_STATUS) return {"connecting", "", "", 0};
  if (state == WL_CONNECT_FAILED || state == WL_NO_SSID_AVAIL) return {"failed", "", "", 0};
  return {"disconnected", "", "", 0};
}

void Wifi::disconnect() { WiFi.disconnect(true); }

esp32lua::Status Wifi::forget() {
  WiFi.disconnect(true, true);
  return esp32lua::Status::success();
}

// ---------------------------------------------------------------------------

int Buttons::halIndex(const std::string& button) {
  if (button == "back") return HalGPIO::BTN_BACK;
  if (button == "confirm") return HalGPIO::BTN_CONFIRM;
  if (button == "left") return HalGPIO::BTN_LEFT;
  if (button == "right") return HalGPIO::BTN_RIGHT;
  if (button == "up") return HalGPIO::BTN_UP;
  if (button == "down") return HalGPIO::BTN_DOWN;
  return -1;
}

bool Buttons::isPressed(const std::string& button) const {
  const int index = halIndex(button);
  return index >= 0 && gpio.isPressed(index);
}

bool Buttons::wasPressed(const std::string& button) const {
  const int index = halIndex(button);
  return index >= 0 && gpio.wasPressed(index);
}

bool Buttons::wasReleased(const std::string& button) const {
  const int index = halIndex(button);
  return index >= 0 && !gpio.isPressed(index) && gpio.wasPressed(index);  // HalGPIO tracks edges as presses
}

bool Buttons::isAnyPressed() const {
  for (uint8_t i = 0; i <= HalGPIO::BTN_DOWN; i++) {
    if (gpio.isPressed(i)) return true;
  }
  return false;
}

}  // namespace cplua
