#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Memory.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Logging.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/LuaManager.h"
#include "util/lua/LuaBindings.h"

extern "C" {
#include <lauxlib.h>
}

using luabindings::addFunction;
using luabindings::getInput;
using luabindings::getManager;
using luabindings::getRenderer;

namespace {
Color getColor(lua_State* state, int index, Color fallback = Color::Black) {
  if (lua_isnoneornil(state, index)) return fallback;
  if (lua_isboolean(state, index)) return lua_toboolean(state, index) ? Color::Black : Color::White;
  return static_cast<Color>(lua_tointeger(state, index));
}

bool isBlack(Color color) { return color != Color::White && color != Color::Clear; }
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
}  // namespace

void luabindings::registerGui(lua_State* state) {
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
