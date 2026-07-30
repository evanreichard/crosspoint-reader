#include "LuaManager.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <BuildScratch.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Memory.h>
#include <ScratchHeap.h>
#include <SecureHttpClient.h>
#include <WiFi.h>
#include <esp_random.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "Logging.h"
#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "network/TlsScratch.h"

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
constexpr uint32_t MIN_TIMER_INTERVAL_MS = 100;
constexpr uint32_t MIN_TICK_INTERVAL_MS = 33;
constexpr uint32_t MAX_RUNTIME_INTERVAL_MS = 60 * 60 * 1000;
portMUX_TYPE luaRuntimeSpinlock = portMUX_INITIALIZER_UNLOCKED;

bool deadlineReached(uint32_t now, uint32_t deadline) { return static_cast<int32_t>(now - deadline) >= 0; }

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

// --- Logs a debug message to serial output.
// -- @param message string
// -- @within log
int luaLogDebug(lua_State* state) {
  LOG_DBG("LUA", "%s", luaL_checkstring(state, 1));
  return 0;
}

// --- Logs an info message to serial output.
// -- @param message string
// -- @within log
int luaLogInfo(lua_State* state) {
  LOG_INF("LUA", "%s", luaL_checkstring(state, 1));
  return 0;
}

// --- Logs an error message to serial output.
// -- @param message string
// -- @within log
int luaLogError(lua_State* state) {
  LOG_ERR("LUA", "%s", luaL_checkstring(state, 1));
  return 0;
}

int luaLogLegacy(lua_State* state) {
  LOG_INF("LUA", "%s", luaL_checkstring(state, 2));
  return 0;
}

// --- Clears the framebuffer to white.
// -- @within gui
int guiClear(lua_State* state) {
  if (auto* renderer = getRenderer(state)) renderer->clearScreen();
  return 0;
}

// --- Pushes the framebuffer to the panel.
// -- @param mode[opt=REFRESH_FAST] int REFRESH_* constant
// -- @within gui
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

// --- Draws an unfilled rectangle.
// -- @param x int Left edge
// -- @param y int Top edge
// -- @param w int Width
// -- @param h int Height
// -- @param color[opt=COLOR_BLACK] int COLOR_* constant
// -- @within gui
int guiDrawRect(lua_State* state) {
  if (auto* renderer = getRenderer(state)) {
    renderer->drawRect(luaL_checkinteger(state, 1), luaL_checkinteger(state, 2), luaL_checkinteger(state, 3),
                       luaL_checkinteger(state, 4), isBlack(getColor(state, 5)));
  }
  return 0;
}

// --- Draws a filled rectangle.
// -- @param x int Left edge
// -- @param y int Top edge
// -- @param w int Width
// -- @param h int Height
// -- @param color[opt=COLOR_BLACK] int COLOR_* constant
// -- @within gui
int guiFillRect(lua_State* state) {
  if (auto* renderer = getRenderer(state)) {
    renderer->fillRect(luaL_checkinteger(state, 1), luaL_checkinteger(state, 2), luaL_checkinteger(state, 3),
                       luaL_checkinteger(state, 4), isBlack(getColor(state, 5)));
  }
  return 0;
}

// --- Draws a line.
// -- @param x1 int Start X
// -- @param y1 int Start Y
// -- @param x2 int End X
// -- @param y2 int End Y
// -- @param width[opt=1] int Stroke width
// -- @param color[opt=COLOR_BLACK] int COLOR_* constant
// -- @within gui
int guiDrawLine(lua_State* state) {
  if (auto* renderer = getRenderer(state)) {
    renderer->drawLine(luaL_checkinteger(state, 1), luaL_checkinteger(state, 2), luaL_checkinteger(state, 3),
                       luaL_checkinteger(state, 4), luaL_optinteger(state, 5, 1), isBlack(getColor(state, 6)));
  }
  return 0;
}

// --- Draws an unfilled rounded rectangle.
// -- @param x int Left edge
// -- @param y int Top edge
// -- @param w int Width
// -- @param h int Height
// -- @param width[opt=2] int Stroke width
// -- @param radius[opt=10] int Corner radius
// -- @param color[opt=COLOR_BLACK] int COLOR_* constant
// -- @within gui
int guiDrawRoundedRect(lua_State* state) {
  if (auto* renderer = getRenderer(state)) {
    renderer->drawRoundedRect(luaL_checkinteger(state, 1), luaL_checkinteger(state, 2), luaL_checkinteger(state, 3),
                              luaL_checkinteger(state, 4), luaL_optinteger(state, 5, 2), luaL_optinteger(state, 6, 10),
                              isBlack(getColor(state, 7)));
  }
  return 0;
}

// --- Draws a filled rounded rectangle.
// -- @param x int Left edge
// -- @param y int Top edge
// -- @param w int Width
// -- @param h int Height
// -- @param radius[opt=10] int Corner radius
// -- @param color[opt=COLOR_BLACK] int COLOR_* constant (supports gray values)
// -- @within gui
int guiFillRoundedRect(lua_State* state) {
  if (auto* renderer = getRenderer(state)) {
    renderer->fillRoundedRect(luaL_checkinteger(state, 1), luaL_checkinteger(state, 2), luaL_checkinteger(state, 3),
                              luaL_checkinteger(state, 4), luaL_optinteger(state, 5, 10), getColor(state, 6));
  }
  return 0;
}

// --- Draws a single pixel.
// -- @param x int
// -- @param y int
// -- @param color[opt=COLOR_BLACK] int COLOR_* constant
// -- @within gui
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

// --- Draws an unfilled circle.
// -- @param cx int Center X
// -- @param cy int Center Y
// -- @param radius int
// -- @param width[opt=1] int Stroke width
// -- @param color[opt=COLOR_BLACK] int COLOR_* constant
// -- @within gui
int guiDrawCircle(lua_State* state) {
  if (auto* renderer = getRenderer(state)) {
    drawCircle(*renderer, luaL_checkinteger(state, 1), luaL_checkinteger(state, 2), luaL_checkinteger(state, 3),
               std::max(1, static_cast<int>(luaL_optinteger(state, 4, 1))), isBlack(getColor(state, 5)));
  }
  return 0;
}

// --- Draws a filled circle.
// -- @param cx int Center X
// -- @param cy int Center Y
// -- @param radius int
// -- @param color[opt=COLOR_BLACK] int COLOR_* constant
// -- @within gui
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

// --- Draws a filled polygon from parallel vertex arrays (needs 3+ points).
// -- @param xs int[] X coordinates of each vertex
// -- @param ys int[] Y coordinates of each vertex
// -- @param color[opt=COLOR_BLACK] int COLOR_* constant
// -- @within gui
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

// --- Draws text at a position.
// -- @param font int FONT_* constant
// -- @param x int Left edge
// -- @param y int Baseline Y
// -- @param text string
// -- @param color[opt=COLOR_BLACK] int COLOR_* constant
// -- @param style[opt=STYLE_REGULAR] int STYLE_* constant
// -- @within gui
int guiDrawText(lua_State* state) {
  if (auto* renderer = getRenderer(state)) {
    renderer->drawText(luaL_checkinteger(state, 1), luaL_checkinteger(state, 2), luaL_checkinteger(state, 3),
                       luaL_checkstring(state, 4), isBlack(getColor(state, 5)),
                       static_cast<EpdFontFamily::Style>(luaL_optinteger(state, 6, EpdFontFamily::REGULAR)));
  }
  return 0;
}

// --- Draws text centered horizontally on the screen.
// -- @param font int FONT_* constant
// -- @param y int Baseline Y
// -- @param text string
// -- @param color[opt=COLOR_BLACK] int COLOR_* constant
// -- @param style[opt=STYLE_REGULAR] int STYLE_* constant
// -- @within gui
int guiDrawCenteredText(lua_State* state) {
  if (auto* renderer = getRenderer(state)) {
    renderer->drawCenteredText(luaL_checkinteger(state, 1), luaL_checkinteger(state, 2), luaL_checkstring(state, 3),
                               isBlack(getColor(state, 4)),
                               static_cast<EpdFontFamily::Style>(luaL_optinteger(state, 5, EpdFontFamily::REGULAR)));
  }
  return 0;
}

// --- Returns the rendered width of a string in pixels.
// -- @param font int FONT_* constant
// -- @param text string
// -- @param style[opt=STYLE_REGULAR] int STYLE_* constant
// -- @return int width
// -- @within gui
int guiGetTextWidth(lua_State* state) {
  auto* renderer = getRenderer(state);
  lua_pushinteger(state, renderer ? renderer->getTextWidth(luaL_checkinteger(state, 1), luaL_checkstring(state, 2),
                                                           static_cast<EpdFontFamily::Style>(
                                                               luaL_optinteger(state, 3, EpdFontFamily::REGULAR)))
                                  : 0);
  return 1;
}

// --- Returns the screen width in pixels for the current orientation.
// -- @return int
// -- @within gui
int guiWidth(lua_State* state) {
  auto* renderer = getRenderer(state);
  lua_pushinteger(state, renderer ? renderer->getScreenWidth() : 0);
  return 1;
}

// --- Returns the screen height in pixels for the current orientation.
// -- @return int
// -- @within gui
int guiHeight(lua_State* state) {
  auto* renderer = getRenderer(state);
  lua_pushinteger(state, renderer ? renderer->getScreenHeight() : 0);
  return 1;
}

// --- Draws the standard four-button hint bar (use gui.HINT_* for arrow glyphs).
// -- @param btn1[opt] string Back button label (default "")
// -- @param btn2[opt] string Confirm button label (default "")
// -- @param btn3[opt] string Left button label (default "")
// -- @param btn4[opt] string Right button label (default "")
// -- @within gui
int guiDrawButtonHints(lua_State* state) {
  auto* renderer = getRenderer(state);
  auto* input = getInput(state);
  if (!renderer || !input) return 0;
  const auto labels = input->mapLabels(luaL_optstring(state, 1, ""), luaL_optstring(state, 2, ""),
                                       luaL_optstring(state, 3, ""), luaL_optstring(state, 4, ""));
  GUI.drawButtonHints(*renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  return 0;
}

// --- Changes the screen orientation (invalidates any layout assumptions).
// -- @param mode string "portrait" | "portrait_inv" | "landscape_cw" | "landscape_ccw"
// -- @within gui
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

// --- Draws a BMP file from the SD card, centered by default.
// -- @param path string Absolute path on the SD card
// -- @param x[opt=centered] int Left edge
// -- @param y[opt=centered] int Top edge
// -- @param maxWidth[opt=screen] int Bounds the image is scaled into
// -- @param maxHeight[opt=screen] int Bounds the image is scaled into
// -- @return bool ok False if the file is missing or not a valid BMP
// -- @within gui
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

// --- Returns true if the button was pressed since the last input frame. Buttons: "back", "confirm", "left", "right", "up", "down", "page_back", "page_forward".
// -- @param button string Button name
// -- @return bool
// -- @within input
int inputWasPressed(lua_State* state) {
  auto* manager = getManager(state);
  lua_pushboolean(state,
                  manager && manager->wasLatchedPressed(static_cast<int>(parseButton(luaL_checkstring(state, 1)))));
  return 1;
}

// --- Returns true if the button was released since the last input frame.
// -- @param button string Button name
// -- @return bool
// -- @within input
int inputWasReleased(lua_State* state) {
  auto* manager = getManager(state);
  lua_pushboolean(state,
                  manager && manager->wasLatchedReleased(static_cast<int>(parseButton(luaL_checkstring(state, 1)))));
  return 1;
}

// --- Returns true while the button is currently held down.
// -- @param button string Button name
// -- @return bool
// -- @within input
int inputIsPressed(lua_State* state) {
  auto* manager = getManager(state);
  lua_pushboolean(state,
                  manager && manager->isLatchedPressed(static_cast<int>(parseButton(luaL_checkstring(state, 1)))));
  return 1;
}

// --- Returns true while any button is currently held down.
// -- @return bool
// -- @within input
int inputIsAnyPressed(lua_State* state) {
  auto* manager = getManager(state);
  lua_pushboolean(state, manager && manager->isAnyLatchedPressed());
  return 1;
}

// --- Returns milliseconds since boot.
// -- @return int
// -- @within sys
// -- @alias uptime
int sysMillis(lua_State* state) {
  lua_pushinteger(state, static_cast<lua_Integer>(millis()));
  return 1;
}

// --- Blocks the app for the given number of milliseconds.
// -- @param ms int
// -- @within sys
int sysDelay(lua_State* state) {
  delay(luaL_checkinteger(state, 1));
  return 0;
}

// --- Requests the app to exit back to the launcher.
// -- @within sys
int sysExit(lua_State* state) {
  if (auto* manager = getManager(state)) manager->requestExit();
  return 0;
}

// --- Enables the on_tick() callback at a fixed interval (0 disables, minimum 33ms). Requires on_tick() to be defined.
// -- @param intervalMs int 0-3600000
// -- @within app
int appSetTickInterval(lua_State* state) {
  const lua_Integer requested = luaL_checkinteger(state, 1);
  luaL_argcheck(state, requested >= 0 && requested <= MAX_RUNTIME_INTERVAL_MS, 1, "interval must be 0-3600000ms");
  const uint32_t interval = requested == 0 ? 0 : std::max<uint32_t>(requested, MIN_TICK_INTERVAL_MS);
  if (auto* manager = getManager(state)) manager->setTickInterval(interval);
  return 0;
}

bool isValidTimerId(const char* id, size_t length) {
  if (length == 0 || length >= 32) return false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = id[i];
    const bool alphanumeric = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (!alphanumeric && c != '_' && c != '-') return false;
  }
  return true;
}

int addTimer(lua_State* state, bool repeating) {
  const lua_Integer requested = luaL_checkinteger(state, 1);
  luaL_argcheck(state, requested >= MIN_TIMER_INTERVAL_MS && requested <= MAX_RUNTIME_INTERVAL_MS, 1,
                "interval must be 100-3600000ms");
  size_t idLength = 0;
  const char* id = luaL_checklstring(state, 2, &idLength);
  luaL_argcheck(state, isValidTimerId(id, idLength), 2, "id must be 1-31 letters, digits, underscores, or dashes");
  auto* manager = getManager(state);
  if (!manager || !manager->addTimer(static_cast<uint32_t>(requested), id, repeating)) {
    return luaL_error(state, "timer limit reached");
  }
  return 0;
}

// --- Fires on_timer(id) once after the interval. Apps define on_timer(id) to receive it.
// -- @param intervalMs int 100-3600000
// -- @param id string 1-31 letters, digits, underscores, or dashes
// -- @within timer
int timerAfter(lua_State* state) { return addTimer(state, false); }

// --- Fires on_timer(id) repeatedly at the interval until cancelled.
// -- @param intervalMs int 100-3600000
// -- @param id string 1-31 letters, digits, underscores, or dashes
// -- @within timer
int timerEvery(lua_State* state) { return addTimer(state, true); }

// --- Cancels a timer by id.
// -- @param id string
// -- @within timer
int timerCancel(lua_State* state) {
  size_t idLength = 0;
  const char* id = luaL_checklstring(state, 1, &idLength);
  luaL_argcheck(state, isValidTimerId(id, idLength), 1, "id must be 1-31 letters, digits, underscores, or dashes");
  if (auto* manager = getManager(state)) manager->cancelTimer(id);
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

// --- Lists sorted sub-directory names under a path (hidden entries excluded).
// -- @param path string Absolute path on the SD card
// -- @return string[] names
// -- @within fs
int fsListDirs(lua_State* state) { return pushEntries(state, true); }
// --- Lists sorted file names under a path (hidden entries excluded).
// -- @param path string Absolute path on the SD card
// -- @return string[] names
// -- @within fs
int fsListFiles(lua_State* state) { return pushEntries(state, false); }

// --- Returns true if a file or directory exists.
// -- @param path string Absolute path on the SD card
// -- @return bool
// -- @within fs
int fsExists(lua_State* state) {
  lua_pushboolean(state, Storage.exists(luaL_checkstring(state, 1)));
  return 1;
}

// --- Reads an entire file (capped at ~50KB).
// -- @param path string Absolute path on the SD card
// -- @return string|nil content Nil if the file is missing or unreadable
// -- @within fs
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

// --- Returns the size of a file in bytes.
// -- @param path string Absolute path on the SD card
// -- @return int|nil size Nil if the file is missing or a directory
// -- @within fs
int fsFileSize(lua_State* state) {
  HalFile file;
  if (!Storage.openFileForRead("LUA", luaL_checkstring(state, 1), file) || file.isDirectory()) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushinteger(state, static_cast<lua_Integer>(file.size()));
  return 1;
}

// --- Reads the first full line beginning at or after a byte offset, for paging large files.
// -- @param path string Absolute path on the SD card
// -- @param offset int Byte offset (0-based); snaps forward to the next line boundary if mid-line
// -- @return string|nil line Nil past end of file, or if the line exceeds 192 bytes
// -- @return int|nil nextOffset Byte offset of the following line
// -- @within fs
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

// --- Creates a directory.
// -- @param path string Absolute path on the SD card
// -- @return bool|nil ok
// -- @return string|nil error Present when ok is nil
// -- @within fs
int fsMkdir(lua_State* state) {
  const char* path = luaL_checkstring(state, 1);
  if (!isSafeMutationPath(path, true)) return pushFsResult(state, false, "Unsafe directory path");
  if (Storage.exists(path)) {
    auto existing = Storage.open(path);
    return pushFsResult(state, existing && existing.isDirectory(), "Path exists and is not a directory");
  }
  return pushFsResult(state, Storage.mkdir(path), "Failed to create directory");
}

// --- Renames (moves) a file or directory.
// -- @param source string
// -- @param destination string Must not already exist
// -- @return bool|nil ok
// -- @return string|nil error Present when ok is nil
// -- @within fs
int fsRename(lua_State* state) {
  const char* source = luaL_checkstring(state, 1);
  const char* destination = luaL_checkstring(state, 2);
  if (!isSafeMutationPath(source) || !isSafeMutationPath(destination)) {
    return pushFsResult(state, false, "Unsafe rename path");
  }
  if (Storage.exists(destination)) return pushFsResult(state, false, "Rename destination exists");
  return pushFsResult(state, Storage.rename(source, destination), "Failed to rename path");
}

// --- Deletes a file. /.apps and /.crosspoint are protected.
// -- @param path string Absolute path on the SD card
// -- @return bool|nil ok
// -- @return string|nil error Present when ok is nil
// -- @within fs
int fsRemove(lua_State* state) {
  const char* path = luaL_checkstring(state, 1);
  if (!isSafeMutationPath(path)) return pushFsResult(state, false, "Unsafe remove path");
  return pushFsResult(state, Storage.remove(path), "Failed to remove file");
}

// --- Recursively deletes a directory tree. /.apps and /.crosspoint are protected.
// -- @param path string Absolute path on the SD card
// -- @return bool|nil ok
// -- @return string|nil error Present when ok is nil
// -- @within fs
int fsRemoveTree(lua_State* state) {
  const char* path = luaL_checkstring(state, 1);
  if (!isSafeMutationPath(path)) return pushFsResult(state, false, "Unsafe remove tree path");
  return pushFsResult(state, Storage.removeDir(path), "Failed to remove directory tree");
}

// --- Writes a file (creating or overwriting).
// -- @param path string Absolute path on the SD card
// -- @param content string
// -- @return bool ok
// -- @within fs
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

// --- Starts connecting to a stored Wi-Fi network (credentials from the reader settings). Poll wifi.status().
// -- @within wifi
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

// --- Returns the Wi-Fi connection state, advancing any in-progress attempt.
// -- @return string status "idle" | "connecting" | "connected" | "failed"
// -- @within wifi
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

// --- Disconnects Wi-Fi if the app started it.
// -- @within wifi
int netWifiDisconnect(lua_State*) {
  if (ownsWifi) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
    ownsWifi = false;
  }
  wifiState = WifiState::Idle;
  return 0;
}

// --- Returns whether Wi-Fi is currently connected to an access point.
// -- @return bool connected
// -- @within wifi
int netWifiIsConnected(lua_State* state) {
  lua_pushboolean(state, WiFi.status() == WL_CONNECTED);
  return 1;
}

// --- Returns the local IP address of the Wi-Fi interface.
// -- @return string ip "0.0.0.0" if not connected
// -- @within wifi
int netWifiLocalIp(lua_State* state) {
  lua_pushstring(state, WiFi.localIP().toString().c_str());
  return 1;
}

// Empirical floors, not measured peaks: downloads are observed working at 31 KB free, and the
// panic seen at 36 KB free was a 5 KB request failing against a fragmented heap, so a contiguous
// block is required alongside the total. wolfSSL panics on a failed allocation and the Arduino
// HTTP stack allocates with throwing new, so the check has to happen here while a Lua error is
// still possible.
constexpr uint32_t TLS_HEAP_FLOOR = 24 * 1024;
constexpr uint32_t TLS_BLOCK_FLOOR = 8 * 1024;

// Only wolfSSL's small allocations still land on the heap once the framebuffer is lent.
constexpr uint32_t TLS_SCRATCH_HEAP_FLOOR = 10 * 1024;

// Lend The Framebuffer To wolfSSL - by the time an app transfers, Wi-Fi and the Lua VM have left
// the heap's largest block (~22 KB) barely above wolfSSL's ~17 KB record buffer. The framebuffer's
// bytes are lent in place and never freed, so the panel keeps showing its last frame and the block
// cannot fail to come back. Lua is blocked inside the transfer call, so nothing can draw meanwhile.
class TlsScratchLoan {
 public:
  TlsScratchLoan(lua_State* state, freeink::SecureHttpClient* client) : client_(client) {
    auto* renderer = getRenderer(state);
    if (!renderer || !client_) return;

    loan_.emplace(*renderer);
    size_t length = 0;
    scratch_ = buildscratch::claim(MIN_TLS_SCRATCH, &length);
    if (!scratch_ || !tlsscratch::activate(scratch_, length)) {
      if (scratch_) buildscratch::release(scratch_);
      scratch_ = nullptr;
      loan_.reset();  // hand the framebuffer straight back; the transfer falls back to the heap
      return;
    }
    LOG_DBG("LUA", "TLS scratch: %u bytes lent from the framebuffer", (unsigned)length);
  }

  ~TlsScratchLoan() {
    if (scratch_) {
      // Close The Session First - a reusable client keeps the connection (and wolfSSL's buffers)
      // alive after a transfer, and those buffers live in the block we are about to hand back.
      client_->end();
      tlsscratch::deactivate();
      buildscratch::release(scratch_);
    }
    loan_.reset();
  }

  TlsScratchLoan(const TlsScratchLoan&) = delete;
  TlsScratchLoan& operator=(const TlsScratchLoan&) = delete;

 private:
  static constexpr size_t MIN_TLS_SCRATCH = 24 * 1024;

  freeink::SecureHttpClient* client_;
  std::optional<GfxRenderer::FrameBufferLoan> loan_;
  uint8_t* scratch_ = nullptr;
};

bool hasTlsHeadroom() {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t largestBlock = ESP.getMaxAllocHeap();
  const bool scratchActive = scratchheap::isActive();
  LOG_INF("LUA", "Heap before TLS: free %u, largest %u%s", freeHeap, largestBlock,
          scratchActive ? ", framebuffer lent" : "");

  // With the framebuffer lent, wolfSSL's big buffers no longer come from the heap, so the floors
  // that exist to keep the handshake off a fragmented heap would refuse transfers that now fit.
  if (scratchActive) return freeHeap >= TLS_SCRATCH_HEAP_FLOOR;
  return freeHeap >= TLS_HEAP_FLOOR && largestBlock >= TLS_BLOCK_FLOOR;
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

  const TlsScratchLoan scratch(state, client);

  // Same Pre-flight As net.download - the Arduino HTTP stack allocates with throwing new, so an
  // OOM here terminates the firmware instead of returning an error to the app.
  if (!hasTlsHeadroom() || !client->begin(url)) {
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

// --- Performs an HTTP GET request.
// -- @param url string
// -- @param headers[opt] table<string,string> Optional request headers
// -- @return string|nil body Nil on failure (capped at ~50KB)
// -- @return int status HTTP status code, or -1 if the request never happened
// -- @within http
int netGet(lua_State* state) { return netRequest(state, "GET", false); }
// --- Performs an HTTP HEAD request.
// -- @param url string
// -- @param headers[opt] table<string,string> Optional request headers
// -- @return string|nil body Nil on failure
// -- @return int status HTTP status code, or -1 if the request never happened
// -- @within http
int netHead(lua_State* state) { return netRequest(state, "HEAD", false); }
// --- Performs an HTTP DELETE request.
// -- @param url string
// -- @param headers[opt] table<string,string> Optional request headers
// -- @return string|nil body Nil on failure
// -- @return int status HTTP status code, or -1 if the request never happened
// -- @within http
int netDelete(lua_State* state) { return netRequest(state, "DELETE", false); }
// --- Performs an HTTP POST request.
// -- @param url string
// -- @param body[opt] string Request body (default "")
// -- @param headers[opt] table<string,string> Optional request headers
// -- @return string|nil body Nil on failure (capped at ~50KB)
// -- @return int status HTTP status code, or -1 if the request never happened
// -- @within http
int netPost(lua_State* state) { return netRequest(state, "POST", true); }
// --- Performs an HTTP PATCH request.
// -- @param url string
// -- @param body[opt] string Request body (default "")
// -- @param headers[opt] table<string,string> Optional request headers
// -- @return string|nil body Nil on failure (capped at ~50KB)
// -- @return int status HTTP status code, or -1 if the request never happened
// -- @within http
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

// --- Downloads an HTTPS URL to a file with optional integrity checks.
// -- @param url string HTTPS URL
// -- @param destination string Absolute destination path on the SD card
// -- @param options table { maxBytes: int (required, up to 16MB), expectedSize?: int, sha256?: string (64 hex chars) }
// -- @return int|nil bytesWritten Nil on failure
// -- @return string|nil error Present when the download fails
// -- @within http
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
  // Lend Before Measuring - the loan is what makes the transfer affordable, so the pre-flight has
  // to see the heap as it will be during the handshake, not before.
  const TlsScratchLoan scratch(state, client);

  if (!hasTlsHeadroom()) return pushLuaError(state, "Not enough memory to start a secure download");

  size_t bytesWritten = 0;
  const auto result = HttpDownloader::downloadBoundedToFile(url, destination, maxBytes, expectedSize, expectedHashPtr,
                                                            bytesWritten, client);
  if (result != HttpDownloader::OK) return pushLuaError(state, downloadErrorMessage(result));

  lua_pushinteger(state, static_cast<lua_Integer>(bytesWritten));
  return 1;
}

// --- Percent-encodes a string for use in a URL query.
// -- @param input string
// -- @return string encoded
// -- @within http
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
  addFunction(state, "setTickInterval", appSetTickInterval);
  lua_setglobal(state, "app");

  lua_newtable(state);
  addFunction(state, "after", timerAfter);
  addFunction(state, "every", timerEvery);
  addFunction(state, "cancel", timerCancel);
  lua_setglobal(state, "timer");

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
  addFunction(state, "connect", netWifiConnect);
  addFunction(state, "status", netWifiStatus);
  addFunction(state, "disconnect", netWifiDisconnect);
  addFunction(state, "isConnected", netWifiIsConnected);
  addFunction(state, "localIP", netWifiLocalIp);
  lua_setglobal(state, "wifi");

  lua_newtable(state);
  addFunction(state, "get", netGet);
  addFunction(state, "head", netHead);
  addFunction(state, "delete", netDelete);
  addFunction(state, "post", netPost);
  addFunction(state, "patch", netPatch);
  addFunction(state, "download", netDownload);
  addFunction(state, "urlencode", netUrlEncode);
  lua_setglobal(state, "http");

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
