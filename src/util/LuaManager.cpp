#include "LuaManager.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Memory.h>
#include <SecureHttpClient.h>
#include <WiFi.h>
#include <esp_random.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Logging.h"
#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

extern "C" {
#include <lauxlib.h>
#include <lualib.h>
}

using freeink::SecureHttpClient;

namespace {
enum class WifiState { Idle, Connecting, Connected, Failed };
WifiState wifiState = WifiState::Idle;
size_t credentialIndex = 0;
bool ownsWifi = false;
unsigned long wifiAttemptStarted = 0;
constexpr unsigned long WIFI_ATTEMPT_TIMEOUT_MS = 15000;
constexpr size_t MAX_FILE_READ_SIZE = 50000;
constexpr size_t MAX_HTTP_RESPONSE_SIZE = 50000;
constexpr size_t MAX_LINE_READ_SIZE = 192;
constexpr size_t MAX_DOWNLOAD_SIZE = 16 * 1024 * 1024;

LuaManager* getManager(lua_State* state) {
  lua_getfield(state, LUA_REGISTRYINDEX, "manager");
  auto* manager = static_cast<LuaManager*>(lua_touserdata(state, -1));
  lua_pop(state, 1);
  return manager;
}

GfxRenderer* getRenderer(lua_State* state) {
  lua_getfield(state, LUA_REGISTRYINDEX, "renderer");
  auto* renderer = static_cast<GfxRenderer*>(lua_touserdata(state, -1));
  lua_pop(state, 1);
  return renderer;
}

MappedInputManager* getInput(lua_State* state) {
  lua_getfield(state, LUA_REGISTRYINDEX, "input");
  auto* input = static_cast<MappedInputManager*>(lua_touserdata(state, -1));
  lua_pop(state, 1);
  return input;
}

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

Color getColor(lua_State* state, int index, Color fallback = Color::Black) {
  if (lua_isnoneornil(state, index)) return fallback;
  if (lua_isboolean(state, index)) return lua_toboolean(state, index) ? Color::Black : Color::White;
  return static_cast<Color>(lua_tointeger(state, index));
}

bool isBlack(Color color) { return color != Color::White && color != Color::Clear; }

int luaLogDebug(lua_State* state) {
  LOG_DBG("LUA", "%s", luaL_checkstring(state, 1));
  return 0;
}

int luaLogInfo(lua_State* state) {
  LOG_INF("LUA", "%s", luaL_checkstring(state, 1));
  return 0;
}

int luaLogError(lua_State* state) {
  LOG_ERR("LUA", "%s", luaL_checkstring(state, 1));
  return 0;
}

int luaLogLegacy(lua_State* state) {
  LOG_INF("LUA", "%s", luaL_checkstring(state, 2));
  return 0;
}

int guiClear(lua_State* state) {
  if (auto* renderer = getRenderer(state)) renderer->clearScreen();
  return 0;
}

int guiRefresh(lua_State* state) {
  const int mode = luaL_optinteger(state, 1, HalDisplay::FAST_REFRESH);
  auto* manager = getManager(state);
  if (manager && manager->isRefreshSuppressed()) {
    manager->holdRefresh(mode);
    return 0;
  }
  if (manager) manager->dropHeldRefresh();
  if (auto* renderer = getRenderer(state)) {
    renderer->displayBuffer(static_cast<HalDisplay::RefreshMode>(mode));
  }
  return 0;
}

int guiDrawRect(lua_State* state) {
  if (auto* renderer = getRenderer(state)) {
    renderer->drawRect(luaL_checkinteger(state, 1), luaL_checkinteger(state, 2), luaL_checkinteger(state, 3),
                       luaL_checkinteger(state, 4), isBlack(getColor(state, 5)));
  }
  return 0;
}

int guiFillRect(lua_State* state) {
  if (auto* renderer = getRenderer(state)) {
    renderer->fillRect(luaL_checkinteger(state, 1), luaL_checkinteger(state, 2), luaL_checkinteger(state, 3),
                       luaL_checkinteger(state, 4), isBlack(getColor(state, 5)));
  }
  return 0;
}

int guiDrawLine(lua_State* state) {
  if (auto* renderer = getRenderer(state)) {
    renderer->drawLine(luaL_checkinteger(state, 1), luaL_checkinteger(state, 2), luaL_checkinteger(state, 3),
                       luaL_checkinteger(state, 4), luaL_optinteger(state, 5, 1), isBlack(getColor(state, 6)));
  }
  return 0;
}

int guiDrawRoundedRect(lua_State* state) {
  if (auto* renderer = getRenderer(state)) {
    renderer->drawRoundedRect(luaL_checkinteger(state, 1), luaL_checkinteger(state, 2), luaL_checkinteger(state, 3),
                              luaL_checkinteger(state, 4), luaL_optinteger(state, 5, 2), luaL_optinteger(state, 6, 10),
                              isBlack(getColor(state, 7)));
  }
  return 0;
}

int guiFillRoundedRect(lua_State* state) {
  if (auto* renderer = getRenderer(state)) {
    renderer->fillRoundedRect(luaL_checkinteger(state, 1), luaL_checkinteger(state, 2), luaL_checkinteger(state, 3),
                              luaL_checkinteger(state, 4), luaL_optinteger(state, 5, 10), getColor(state, 6));
  }
  return 0;
}

int guiDrawPixel(lua_State* state) {
  if (auto* renderer = getRenderer(state)) {
    renderer->drawPixel(luaL_checkinteger(state, 1), luaL_checkinteger(state, 2), isBlack(getColor(state, 3)));
  }
  return 0;
}

void drawCircle(GfxRenderer& renderer, int cx, int cy, int radius, int width, bool black) {
  for (int inset = 0; inset < width && radius - inset >= 0; ++inset) {
    int x = radius - inset;
    int y = 0;
    int error = 1 - x;
    while (x >= y) {
      renderer.drawPixel(cx + x, cy + y, black);
      renderer.drawPixel(cx + y, cy + x, black);
      renderer.drawPixel(cx - y, cy + x, black);
      renderer.drawPixel(cx - x, cy + y, black);
      renderer.drawPixel(cx - x, cy - y, black);
      renderer.drawPixel(cx - y, cy - x, black);
      renderer.drawPixel(cx + y, cy - x, black);
      renderer.drawPixel(cx + x, cy - y, black);
      ++y;
      if (error < 0) {
        error += 2 * y + 1;
      } else {
        --x;
        error += 2 * (y - x) + 1;
      }
    }
  }
}

int guiDrawCircle(lua_State* state) {
  if (auto* renderer = getRenderer(state)) {
    drawCircle(*renderer, luaL_checkinteger(state, 1), luaL_checkinteger(state, 2), luaL_checkinteger(state, 3),
               std::max(1, static_cast<int>(luaL_optinteger(state, 4, 1))), isBlack(getColor(state, 5)));
  }
  return 0;
}

int guiFillCircle(lua_State* state) {
  if (auto* renderer = getRenderer(state)) {
    const int cx = luaL_checkinteger(state, 1);
    const int cy = luaL_checkinteger(state, 2);
    const int radius = luaL_checkinteger(state, 3);
    const bool black = isBlack(getColor(state, 4));
    for (int y = -radius; y <= radius; ++y) {
      const int x = static_cast<int>(sqrtf(static_cast<float>(radius * radius - y * y)));
      renderer->drawLine(cx - x, cy + y, cx + x, cy + y, black);
    }
  }
  return 0;
}

int guiFillPolygon(lua_State* state) {
  auto* renderer = getRenderer(state);
  if (!renderer) return 0;
  luaL_checktype(state, 1, LUA_TTABLE);
  luaL_checktype(state, 2, LUA_TTABLE);
  const int count = std::min(lua_rawlen(state, 1), lua_rawlen(state, 2));
  if (count < 3) return 0;

  auto xPoints = makeUniqueNoThrow<int[]>(count);
  auto yPoints = makeUniqueNoThrow<int[]>(count);
  if (!xPoints || !yPoints) {
    LOG_ERR("LUA", "OOM: polygon points");
    return 0;
  }
  for (int i = 0; i < count; ++i) {
    lua_rawgeti(state, 1, i + 1);
    xPoints[i] = lua_tointeger(state, -1);
    lua_rawgeti(state, 2, i + 1);
    yPoints[i] = lua_tointeger(state, -1);
    lua_pop(state, 2);
  }
  renderer->fillPolygon(xPoints.get(), yPoints.get(), count, isBlack(getColor(state, 3)));
  return 0;
}

int guiDrawText(lua_State* state) {
  if (auto* renderer = getRenderer(state)) {
    renderer->drawText(luaL_checkinteger(state, 1), luaL_checkinteger(state, 2), luaL_checkinteger(state, 3),
                       luaL_checkstring(state, 4), isBlack(getColor(state, 5)),
                       static_cast<EpdFontFamily::Style>(luaL_optinteger(state, 6, EpdFontFamily::REGULAR)));
  }
  return 0;
}

int guiDrawCenteredText(lua_State* state) {
  if (auto* renderer = getRenderer(state)) {
    renderer->drawCenteredText(luaL_checkinteger(state, 1), luaL_checkinteger(state, 2), luaL_checkstring(state, 3),
                               isBlack(getColor(state, 4)),
                               static_cast<EpdFontFamily::Style>(luaL_optinteger(state, 5, EpdFontFamily::REGULAR)));
  }
  return 0;
}

int guiGetTextWidth(lua_State* state) {
  auto* renderer = getRenderer(state);
  lua_pushinteger(state, renderer ? renderer->getTextWidth(luaL_checkinteger(state, 1), luaL_checkstring(state, 2),
                                                           static_cast<EpdFontFamily::Style>(
                                                               luaL_optinteger(state, 3, EpdFontFamily::REGULAR)))
                                  : 0);
  return 1;
}

int guiWidth(lua_State* state) {
  auto* renderer = getRenderer(state);
  lua_pushinteger(state, renderer ? renderer->getScreenWidth() : 0);
  return 1;
}

int guiHeight(lua_State* state) {
  auto* renderer = getRenderer(state);
  lua_pushinteger(state, renderer ? renderer->getScreenHeight() : 0);
  return 1;
}

int guiDrawButtonHints(lua_State* state) {
  auto* renderer = getRenderer(state);
  auto* input = getInput(state);
  if (!renderer || !input) return 0;
  const auto labels = input->mapLabels(luaL_optstring(state, 1, ""), luaL_optstring(state, 2, ""),
                                       luaL_optstring(state, 3, ""), luaL_optstring(state, 4, ""));
  GUI.drawButtonHints(*renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  return 0;
}

int guiSetOrientation(lua_State* state) {
  auto* renderer = getRenderer(state);
  if (!renderer) return 0;
  const char* mode = luaL_checkstring(state, 1);
  auto orientation = GfxRenderer::Portrait;
  if (strcmp(mode, "landscape_cw") == 0) orientation = GfxRenderer::LandscapeClockwise;
  if (strcmp(mode, "landscape_ccw") == 0) orientation = GfxRenderer::LandscapeCounterClockwise;
  if (strcmp(mode, "portrait_inv") == 0) orientation = GfxRenderer::PortraitInverted;
  renderer->setOrientation(orientation);
  return 0;
}

int guiDrawBmp(lua_State* state) {
  auto* renderer = getRenderer(state);
  if (!renderer) {
    lua_pushboolean(state, false);
    return 1;
  }
  HalFile file;
  if (!Storage.openFileForRead("LUA", luaL_checkstring(state, 1), file)) {
    lua_pushboolean(state, false);
    return 1;
  }
  Bitmap bitmap(file, true);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    lua_pushboolean(state, false);
    return 1;
  }
  const int screenWidth = renderer->getScreenWidth();
  const int screenHeight = renderer->getScreenHeight();
  const int x = luaL_optinteger(state, 2, std::max(0, (screenWidth - bitmap.getWidth()) / 2));
  const int y = luaL_optinteger(state, 3, std::max(0, (screenHeight - bitmap.getHeight()) / 2));
  renderer->drawBitmap(bitmap, x, y, luaL_optinteger(state, 4, screenWidth), luaL_optinteger(state, 5, screenHeight));
  lua_pushboolean(state, true);
  return 1;
}

int inputWasPressed(lua_State* state) {
  auto* manager = getManager(state);
  lua_pushboolean(state,
                  manager && manager->wasLatchedPressed(static_cast<int>(parseButton(luaL_checkstring(state, 1)))));
  return 1;
}

int inputWasReleased(lua_State* state) {
  auto* manager = getManager(state);
  lua_pushboolean(state,
                  manager && manager->wasLatchedReleased(static_cast<int>(parseButton(luaL_checkstring(state, 1)))));
  return 1;
}

int inputIsPressed(lua_State* state) {
  auto* manager = getManager(state);
  lua_pushboolean(state,
                  manager && manager->isLatchedPressed(static_cast<int>(parseButton(luaL_checkstring(state, 1)))));
  return 1;
}

int inputIsAnyPressed(lua_State* state) {
  auto* manager = getManager(state);
  lua_pushboolean(state, manager && manager->isAnyLatchedPressed());
  return 1;
}

int sysMillis(lua_State* state) {
  lua_pushinteger(state, static_cast<lua_Integer>(millis()));
  return 1;
}

int sysDelay(lua_State* state) {
  delay(luaL_checkinteger(state, 1));
  return 0;
}

int sysExit(lua_State* state) {
  if (auto* manager = getManager(state)) manager->requestExit();
  return 0;
}

std::vector<std::string> listEntries(const char* path, bool directories) {
  std::vector<std::string> names;
  names.reserve(16);
  auto root = Storage.open(path);
  if (!root || !root.isDirectory()) return names;
  root.rewindDirectory();
  char name[192];
  for (auto entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    if (entry.isDirectory() == directories) {
      entry.getName(name, sizeof(name));
      if (name[0] != '.') names.emplace_back(name);
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

int pushEntries(lua_State* state, bool directories) {
  const auto names = listEntries(luaL_checkstring(state, 1), directories);
  lua_createtable(state, names.size(), 0);
  for (size_t i = 0; i < names.size(); ++i) {
    lua_pushlstring(state, names[i].data(), names[i].size());
    lua_rawseti(state, -2, i + 1);
  }
  return 1;
}

int fsListDirs(lua_State* state) { return pushEntries(state, true); }
int fsListFiles(lua_State* state) { return pushEntries(state, false); }

int fsExists(lua_State* state) {
  lua_pushboolean(state, Storage.exists(luaL_checkstring(state, 1)));
  return 1;
}

int fsReadFile(lua_State* state) {
  HalFile file;
  if (!Storage.openFileForRead("LUA", luaL_checkstring(state, 1), file) || file.isDirectory()) {
    lua_pushnil(state);
    return 1;
  }
  const size_t size = std::min(file.size(), MAX_FILE_READ_SIZE);
  if (size == 0) {
    file.close();
    lua_pushliteral(state, "");
    return 1;
  }
  auto buffer = makeUniqueNoThrow<char[]>(size);
  if (!buffer) {
    LOG_ERR("LUA", "OOM: %u byte file read", static_cast<unsigned>(size));
    file.close();
    lua_pushnil(state);
    return 1;
  }
  size_t total = 0;
  while (total < size) {
    const int read = file.read(buffer.get() + total, size - total);
    if (read <= 0) break;
    total += read;
  }
  file.close();
  lua_pushlstring(state, buffer.get(), total);
  return 1;
}

int fsFileSize(lua_State* state) {
  HalFile file;
  if (!Storage.openFileForRead("LUA", luaL_checkstring(state, 1), file) || file.isDirectory()) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushinteger(state, static_cast<lua_Integer>(file.size()));
  return 1;
}

int fsReadLineAt(lua_State* state) {
  const char* path = luaL_checkstring(state, 1);
  const lua_Integer requestedOffset = luaL_checkinteger(state, 2);
  luaL_argcheck(state, requestedOffset >= 0, 2, "offset must be non-negative");

  HalFile file;
  if (!Storage.openFileForRead("LUA", path, file) || file.isDirectory()) {
    lua_pushnil(state);
    lua_pushnil(state);
    return 2;
  }

  const size_t fileSize = file.size();
  const size_t offset = static_cast<size_t>(requestedOffset);
  if (offset >= fileSize) {
    lua_pushnil(state);
    lua_pushnil(state);
    return 2;
  }

  if (offset > 0) {
    if (!file.seek(offset - 1)) {
      lua_pushnil(state);
      lua_pushnil(state);
      return 2;
    }
    if (file.read() != '\n') {
      file.seek(offset);
      int value;
      do {
        value = file.read();
      } while (value >= 0 && value != '\n');
      if (value < 0 || file.position() >= fileSize) {
        lua_pushnil(state);
        lua_pushnil(state);
        return 2;
      }
    }
  }

  char line[MAX_LINE_READ_SIZE];
  size_t length = 0;
  bool overflow = false;
  for (int value = file.read(); value >= 0 && value != '\n'; value = file.read()) {
    if (value == '\r') continue;
    if (length < sizeof(line)) {
      line[length++] = static_cast<char>(value);
    } else {
      overflow = true;
    }
  }

  if (overflow) {
    lua_pushnil(state);
  } else {
    lua_pushlstring(state, line, length);
  }
  lua_pushinteger(state, static_cast<lua_Integer>(file.position()));
  return 2;
}

bool isSafeMutationPath(const char* path, bool allowProtectedRoot = false) {
  if (!path || path[0] != '/' || path[1] == '\0') return false;

  const char* component = path + 1;
  for (const char* cursor = component;; ++cursor) {
    if (*cursor == '\\') return false;
    if (*cursor != '/' && *cursor != '\0') continue;
    const size_t length = cursor - component;
    if ((length == 1 && component[0] == '.') || (length == 2 && component[0] == '.' && component[1] == '.')) {
      return false;
    }
    if (*cursor == '\0') break;
    component = cursor + 1;
  }

  size_t length = strlen(path);
  while (length > 1 && path[length - 1] == '/') --length;
  const bool protectedRoot = (length == 6 && strncmp(path, "/.apps", length) == 0) ||
                             (length == 12 && strncmp(path, "/.crosspoint", length) == 0);
  return allowProtectedRoot || !protectedRoot;
}

int pushFsResult(lua_State* state, bool ok, const char* error) {
  if (ok) {
    lua_pushboolean(state, true);
    return 1;
  }
  LOG_ERR("LUA", "%s", error);
  lua_pushnil(state);
  lua_pushstring(state, error);
  return 2;
}

int fsMkdir(lua_State* state) {
  const char* path = luaL_checkstring(state, 1);
  if (!isSafeMutationPath(path, true)) return pushFsResult(state, false, "Unsafe directory path");
  if (Storage.exists(path)) {
    auto existing = Storage.open(path);
    return pushFsResult(state, existing && existing.isDirectory(), "Path exists and is not a directory");
  }
  return pushFsResult(state, Storage.mkdir(path), "Failed to create directory");
}

int fsRename(lua_State* state) {
  const char* source = luaL_checkstring(state, 1);
  const char* destination = luaL_checkstring(state, 2);
  if (!isSafeMutationPath(source) || !isSafeMutationPath(destination)) {
    return pushFsResult(state, false, "Unsafe rename path");
  }
  if (Storage.exists(destination)) return pushFsResult(state, false, "Rename destination exists");
  return pushFsResult(state, Storage.rename(source, destination), "Failed to rename path");
}

int fsRemove(lua_State* state) {
  const char* path = luaL_checkstring(state, 1);
  if (!isSafeMutationPath(path)) return pushFsResult(state, false, "Unsafe remove path");
  return pushFsResult(state, Storage.remove(path), "Failed to remove file");
}

int fsRemoveTree(lua_State* state) {
  const char* path = luaL_checkstring(state, 1);
  if (!isSafeMutationPath(path)) return pushFsResult(state, false, "Unsafe remove tree path");
  return pushFsResult(state, Storage.removeDir(path), "Failed to remove directory tree");
}

int fsWriteFile(lua_State* state) {
  const char* path = luaL_checkstring(state, 1);
  size_t size = 0;
  const char* content = luaL_checklstring(state, 2, &size);
  HalFile file;
  if (!Storage.openFileForWrite("LUA", path, file)) {
    lua_pushboolean(state, false);
    return 1;
  }
  lua_pushboolean(state, file.write(content, size) == size);
  return 1;
}

void startWifiCredential(size_t index) {
  const auto& credentials = WIFI_STORE.getCredentials();
  if (index >= credentials.size()) {
    wifiState = WifiState::Failed;
    return;
  }
  const auto& credential = credentials[index];
  WiFi.begin(credential.ssid.c_str(), credential.password.empty() ? nullptr : credential.password.c_str());
  wifiAttemptStarted = millis();
  LOG_INF("LUA", "Connecting WiFi: %s", credential.ssid.c_str());
}

int netWifiConnect(lua_State*) {
  WIFI_STORE.loadFromFile();
  const auto& credentials = WIFI_STORE.getCredentials();
  if (credentials.empty()) {
    wifiState = WifiState::Failed;
    return 0;
  }
  WiFi.mode(WIFI_STA);
  credentialIndex = 0;
  ownsWifi = true;
  const auto& lastSsid = WIFI_STORE.getLastConnectedSsid();
  for (size_t i = 0; i < credentials.size(); ++i) {
    if (credentials[i].ssid == lastSsid) {
      credentialIndex = i;
      break;
    }
  }
  wifiState = WifiState::Connecting;
  startWifiCredential(credentialIndex);
  return 0;
}

int netWifiStatus(lua_State* state) {
  if (wifiState == WifiState::Connecting) {
    const wl_status_t status = WiFi.status();
    if (status == WL_CONNECTED) {
      wifiState = WifiState::Connected;
      WIFI_STORE.setLastConnectedSsid(WiFi.SSID().c_str());
    } else if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL ||
               millis() - wifiAttemptStarted >= WIFI_ATTEMPT_TIMEOUT_MS) {
      startWifiCredential(++credentialIndex);
    }
  }
  const char* value = "idle";
  if (wifiState == WifiState::Connecting) value = "connecting";
  if (wifiState == WifiState::Connected) value = "connected";
  if (wifiState == WifiState::Failed) value = "failed";
  lua_pushstring(state, value);
  return 1;
}

int netWifiDisconnect(lua_State*) {
  if (ownsWifi) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
    ownsWifi = false;
  }
  wifiState = WifiState::Idle;
  return 0;
}

int netRequest(lua_State* state, const char* method, bool bodyExpected) {
  auto* manager = getManager(state);
  auto* client = manager ? manager->getHttpClient() : nullptr;
  if (!client) {
    lua_pushnil(state);
    lua_pushinteger(state, -1);
    return 2;
  }

  const char* url = luaL_checkstring(state, 1);
  const char* body = "";
  size_t bodySize = 0;
  int headersIndex = 2;
  if (bodyExpected || lua_isstring(state, 2)) {
    body = luaL_optlstring(state, 2, "", &bodySize);
    headersIndex = 3;
  }

  if (!client->begin(url)) {
    lua_pushnil(state);
    lua_pushinteger(state, -1);
    return 2;
  }
  client->setFollowRedirects(5);
  client->setUserAgent("CrossPoint-ESP32-" CROSSPOINT_VERSION);
  if (lua_istable(state, headersIndex)) {
    lua_pushnil(state);
    while (lua_next(state, headersIndex)) {
      const char* name = lua_tostring(state, -2);
      const char* value = lua_tostring(state, -1);
      if (name && value) client->addHeader(name, value);
      lua_pop(state, 1);
    }
  }

  std::string response;
  bool responseTooLarge = false;
  const int status = client->sendRequest(method, reinterpret_cast<const uint8_t*>(body), bodySize,
                                         [&response, &responseTooLarge](const uint8_t* data, size_t size) {
                                           if (response.size() + size > MAX_HTTP_RESPONSE_SIZE) {
                                             responseTooLarge = true;
                                             return false;
                                           }
                                           response.append(reinterpret_cast<const char*>(data), size);
                                           return true;
                                         });
  if (status >= 200 && status < 300 && !responseTooLarge && client->responseComplete()) {
    lua_pushlstring(state, response.data(), response.size());
  } else {
    LOG_ERR("LUA", "%s %s failed: %d%s", method, url, status, responseTooLarge ? " (response too large)" : "");
    lua_pushnil(state);
  }
  lua_pushinteger(state, status);
  return 2;
}

int netGet(lua_State* state) { return netRequest(state, "GET", false); }
int netHead(lua_State* state) { return netRequest(state, "HEAD", false); }
int netDelete(lua_State* state) { return netRequest(state, "DELETE", false); }
int netPost(lua_State* state) { return netRequest(state, "POST", true); }
int netPatch(lua_State* state) { return netRequest(state, "PATCH", true); }

int pushLuaError(lua_State* state, const char* error) {
  LOG_ERR("LUA", "%s", error);
  lua_pushnil(state);
  lua_pushstring(state, error);
  return 2;
}

int hexValue(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
  return value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
}

bool parseSha256(const char* value, size_t length, uint8_t* output) {
  if (!value || length != 64) return false;
  for (size_t i = 0; i < 32; ++i) {
    const int high = hexValue(value[i * 2]);
    const int low = hexValue(value[i * 2 + 1]);
    if (high < 0 || low < 0) return false;
    output[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

const char* downloadErrorMessage(HttpDownloader::DownloadError error) {
  switch (error) {
    case HttpDownloader::OK:
      return "";
    case HttpDownloader::HTTP_ERROR:
      return "HTTPS download failed";
    case HttpDownloader::FILE_ERROR:
      return "Download destination failed";
    case HttpDownloader::ABORTED:
      return "Download aborted";
    case HttpDownloader::INVALID_URL:
      return "Download requires a valid HTTPS URL";
    case HttpDownloader::LIMIT_EXCEEDED:
      return "Download exceeded maxBytes";
    case HttpDownloader::SIZE_MISMATCH:
      return "Downloaded size did not match expectedSize";
    case HttpDownloader::HASH_MISMATCH:
      return "Downloaded SHA-256 did not match";
  }
  return "Download failed";
}

// Empirical floor, not a measured peak: downloads are observed working at 31 KB free, and the
// panic seen at 36 KB free was a 5 KB request failing against a fragmented heap. This lowers the
// failure rate; it cannot rule one out, since fragmentation gates the handshake as much as free
// bytes do. wolfSSL panics on a failed allocation rather than returning nullptr, so the check has
// to happen here while a Lua error is still possible.
constexpr uint32_t TLS_HEAP_FLOOR = 24 * 1024;

int netDownload(lua_State* state) {
  const char* url = luaL_checkstring(state, 1);
  const char* destination = luaL_checkstring(state, 2);
  if (!lua_istable(state, 3)) return pushLuaError(state, "net.download options table is required");

  lua_getfield(state, 3, "maxBytes");
  if (!lua_isinteger(state, -1)) return pushLuaError(state, "maxBytes must be an integer");
  const lua_Integer requestedMax = lua_tointeger(state, -1);
  lua_pop(state, 1);
  if (requestedMax <= 0 || static_cast<uint64_t>(requestedMax) > MAX_DOWNLOAD_SIZE) {
    return pushLuaError(state, "maxBytes is outside the allowed range");
  }
  const size_t maxBytes = static_cast<size_t>(requestedMax);

  size_t expectedSize = 0;
  lua_getfield(state, 3, "expectedSize");
  if (!lua_isnil(state, -1)) {
    if (!lua_isinteger(state, -1) || lua_tointeger(state, -1) <= 0) {
      return pushLuaError(state, "expectedSize must be a positive integer");
    }
    expectedSize = static_cast<size_t>(lua_tointeger(state, -1));
    if (expectedSize > maxBytes) return pushLuaError(state, "expectedSize exceeds maxBytes");
  }
  lua_pop(state, 1);

  uint8_t expectedHash[32];
  const uint8_t* expectedHashPtr = nullptr;
  lua_getfield(state, 3, "sha256");
  if (!lua_isnil(state, -1)) {
    size_t hashLength = 0;
    const char* hash = lua_tolstring(state, -1, &hashLength);
    if (!parseSha256(hash, hashLength, expectedHash)) return pushLuaError(state, "sha256 must be 64 hex characters");
    expectedHashPtr = expectedHash;
  }
  lua_pop(state, 1);

  lua_pushnil(state);
  while (lua_next(state, 3)) {
    const char* key = lua_tostring(state, -2);
    if (!key || (strcmp(key, "maxBytes") != 0 && strcmp(key, "expectedSize") != 0 && strcmp(key, "sha256") != 0)) {
      return pushLuaError(state, "Unknown net.download option");
    }
    lua_pop(state, 1);
  }

  lua_gc(state, LUA_GCCOLLECT, 0);
  auto* manager = getManager(state);
  auto* client = manager ? manager->getHttpClient() : nullptr;
  if (!client) return pushLuaError(state, "Not enough memory for HTTPS client");

  // Pre-flight Heap Check - A failed allocation inside wolfSSL panics the device
  // (CONFIG_HEAP_ABORT_WHEN_ALLOCATION_FAILS), so refuse the handshake here while a Lua error
  // is still possible.
  const uint32_t freeHeap = ESP.getFreeHeap();
  LOG_INF("LUA", "Heap before download: %u", freeHeap);
  if (freeHeap < TLS_HEAP_FLOOR) return pushLuaError(state, "Not enough memory to start a secure download");

  size_t bytesWritten = 0;
  const auto result = HttpDownloader::downloadBoundedToFile(url, destination, maxBytes, expectedSize, expectedHashPtr,
                                                            bytesWritten, client);
  if (result != HttpDownloader::OK) return pushLuaError(state, downloadErrorMessage(result));

  lua_pushinteger(state, static_cast<lua_Integer>(bytesWritten));
  return 1;
}

int netUrlEncode(lua_State* state) {
  size_t size = 0;
  const auto* input = reinterpret_cast<const unsigned char*>(luaL_checklstring(state, 1, &size));
  luaL_Buffer output;
  luaL_buffinit(state, &output);
  char encoded[4];
  for (size_t i = 0; i < size; ++i) {
    const unsigned char c = input[i];
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      luaL_addchar(&output, c);
    } else {
      snprintf(encoded, sizeof(encoded), "%%%02X", c);
      luaL_addlstring(&output, encoded, 3);
    }
  }
  luaL_pushresult(&output);
  return 1;
}

void addFunction(lua_State* state, const char* name, lua_CFunction function) {
  lua_pushcfunction(state, function);
  lua_setfield(state, -2, name);
}

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
  lastError[0] = '\0';
  LOG_INF("LUA", "Heap before VM: %u", ESP.getFreeHeap());
  state = luaL_newstate();
  if (!state) {
    setError("Not enough memory to create Lua VM");
    LOG_ERR("LUA", "%s", lastError);
    return false;
  }
  luaL_openlibs(state);

  lua_pushlightuserdata(state, this);
  lua_setfield(state, LUA_REGISTRYINDEX, "manager");
  lua_pushlightuserdata(state, &renderer);
  lua_setfield(state, LUA_REGISTRYINDEX, "renderer");
  lua_pushlightuserdata(state, &input);
  lua_setfield(state, LUA_REGISTRYINDEX, "input");

  registerBindings();
  lua_getglobal(state, "math");
  lua_getfield(state, -1, "randomseed");
  lua_pushinteger(state, static_cast<lua_Integer>(esp_random()));
  lua_call(state, 1, 0);
  lua_pop(state, 1);
  LOG_INF("LUA", "VM ready, heap: %u", ESP.getFreeHeap());
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

void LuaManager::end() {
  input = nullptr;
  clearInputEvents();
  httpClient.reset();
  if (state) {
    lua_close(state);
    state = nullptr;
  }
  if (ownsWifi) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
    ownsWifi = false;
  }
  wifiState = WifiState::Idle;
  wantsExit.store(false);
}

void LuaManager::registerBindings() {
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
  addFunction(state, "clear", guiClear);
  addFunction(state, "refresh", guiRefresh);
  addFunction(state, "drawRect", guiDrawRect);
  addFunction(state, "fillRect", guiFillRect);
  addFunction(state, "drawLine", guiDrawLine);
  addFunction(state, "drawRoundedRect", guiDrawRoundedRect);
  addFunction(state, "fillRoundedRect", guiFillRoundedRect);
  addFunction(state, "drawPixel", guiDrawPixel);
  addFunction(state, "drawCircle", guiDrawCircle);
  addFunction(state, "fillCircle", guiFillCircle);
  addFunction(state, "fillPolygon", guiFillPolygon);
  addFunction(state, "drawText", guiDrawText);
  addFunction(state, "drawCenteredText", guiDrawCenteredText);
  addFunction(state, "getTextWidth", guiGetTextWidth);
  addFunction(state, "width", guiWidth);
  addFunction(state, "height", guiHeight);
  addFunction(state, "drawButtonHints", guiDrawButtonHints);
  addFunction(state, "setOrientation", guiSetOrientation);
  addFunction(state, "drawBmp", guiDrawBmp);
  lua_pushliteral(state, "<<");
  lua_setfield(state, -2, "HINT_BACK");
  lua_pushliteral(state, "o");
  lua_setfield(state, -2, "HINT_OK");
  lua_pushliteral(state, "<");
  lua_setfield(state, -2, "HINT_PREV");
  lua_pushliteral(state, ">");
  lua_setfield(state, -2, "HINT_NEXT");
  lua_setglobal(state, "gui");

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
  addFunction(state, "listDirs", fsListDirs);
  addFunction(state, "listFiles", fsListFiles);
  addFunction(state, "exists", fsExists);
  addFunction(state, "readFile", fsReadFile);
  addFunction(state, "fileSize", fsFileSize);
  addFunction(state, "readLineAt", fsReadLineAt);
  addFunction(state, "mkdir", fsMkdir);
  addFunction(state, "rename", fsRename);
  addFunction(state, "remove", fsRemove);
  addFunction(state, "removeTree", fsRemoveTree);
  addFunction(state, "writeFile", fsWriteFile);
  lua_setglobal(state, "fs");

  lua_newtable(state);
  addFunction(state, "wifiConnect", netWifiConnect);
  addFunction(state, "wifiStatus", netWifiStatus);
  addFunction(state, "wifiDisconnect", netWifiDisconnect);
  addFunction(state, "get", netGet);
  addFunction(state, "head", netHead);
  addFunction(state, "delete", netDelete);
  addFunction(state, "post", netPost);
  addFunction(state, "patch", netPatch);
  addFunction(state, "download", netDownload);
  addFunction(state, "urlencode", netUrlEncode);
  lua_setglobal(state, "net");

  lua_pushinteger(state, HalDisplay::FULL_REFRESH);
  lua_setglobal(state, "REFRESH_FULL");
  lua_pushinteger(state, HalDisplay::HALF_REFRESH);
  lua_setglobal(state, "REFRESH_HALF");
  lua_pushinteger(state, HalDisplay::FAST_REFRESH);
  lua_setglobal(state, "REFRESH_FAST");

  lua_pushinteger(state, static_cast<int>(Color::Clear));
  lua_setglobal(state, "COLOR_CLEAR");
  lua_pushinteger(state, static_cast<int>(Color::White));
  lua_setglobal(state, "COLOR_WHITE");
  lua_pushinteger(state, static_cast<int>(Color::LightGray));
  lua_setglobal(state, "COLOR_LIGHT_GRAY");
  lua_pushinteger(state, static_cast<int>(Color::DarkGray));
  lua_setglobal(state, "COLOR_DARK_GRAY");
  lua_pushinteger(state, static_cast<int>(Color::Black));
  lua_setglobal(state, "COLOR_BLACK");

  lua_pushinteger(state, NOTOSERIF_14_FONT_ID);
  lua_setglobal(state, "FONT_BOOKERLY_14");
  lua_pushinteger(state, NOTOSERIF_12_FONT_ID);
  lua_setglobal(state, "FONT_BOOKERLY_12");
  lua_pushinteger(state, NOTOSERIF_16_FONT_ID);
  lua_setglobal(state, "FONT_BOOKERLY_16");
  lua_pushinteger(state, NOTOSANS_12_FONT_ID);
  lua_setglobal(state, "FONT_NOTOSANS_12");
  lua_pushinteger(state, NOTOSANS_14_FONT_ID);
  lua_setglobal(state, "FONT_NOTOSANS_14");
  lua_pushinteger(state, NOTOSANS_16_FONT_ID);
  lua_setglobal(state, "FONT_NOTOSANS_16");
  lua_pushinteger(state, UI_10_FONT_ID);
  lua_setglobal(state, "FONT_UI_10");
  lua_pushinteger(state, UI_12_FONT_ID);
  lua_setglobal(state, "FONT_UI_12");
  lua_pushinteger(state, SMALL_FONT_ID);
  lua_setglobal(state, "FONT_SMALL");

  lua_pushinteger(state, EpdFontFamily::REGULAR);
  lua_setglobal(state, "STYLE_REGULAR");
  lua_pushinteger(state, EpdFontFamily::REGULAR);
  lua_setglobal(state, "STYLE_NORMAL");
  lua_pushinteger(state, EpdFontFamily::BOLD);
  lua_setglobal(state, "STYLE_BOLD");
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
  if (result == LUA_OK) result = lua_pcall(state, 0, 0, 0);
  if (result != LUA_OK) {
    setError(lua_tostring(state, -1));
    LOG_ERR("LUA", "%s", lastError);
    lua_pop(state, 1);
    return false;
  }
  return true;
}

bool LuaManager::callFunction(const char* functionName) {
  if (!state) return false;
  lua_getglobal(state, functionName);
  if (!lua_isfunction(state, -1)) {
    lua_pop(state, 1);
    snprintf(lastError, sizeof(lastError), "Missing %s()", functionName);
    return false;
  }
  if (lua_pcall(state, 0, 0, 0) != LUA_OK) {
    setError(lua_tostring(state, -1));
    LOG_ERR("LUA", "%s", lastError);
    lua_pop(state, 1);
    return false;
  }
  return true;
}
