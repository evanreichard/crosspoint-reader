#include "AppsActivity.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

#include "activities/ActivityManager.h"
#include <HalGPIO.h>

#include "luahost/LuaHost.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t SCAN_BUFFER_SIZE = 256;
constexpr size_t MAX_APPS = 64;
constexpr const char* DESCRIPTION_TAG = "-- DESCRIPTION:";
}  // namespace

void AppsActivity::loadApps() {
  apps.clear();
  apps.reserve(8);

  auto root = Storage.open("/.lua/apps");
  if (!root || !root.isDirectory() || !scanBuffer) return;
  root.rewindDirectory();

  for (auto entry = root.openNextFile(); entry && apps.size() < MAX_APPS; entry = root.openNextFile()) {
    if (!entry.isDirectory()) continue;
    entry.getName(scanBuffer.get(), SCAN_BUFFER_SIZE);
    if (scanBuffer[0] != '.') apps.push_back({scanBuffer.get(), tr(STR_LUA_APP)});
  }
  root.close();

  for (size_t i = 0; i < apps.size();) {
    HalFile script;
    if (!Storage.openFileForRead("APPS", "/.lua/apps/" + apps[i].name + "/main.lua", script)) {
      apps.erase(apps.begin() + i);
      continue;
    }

    const int bytesRead = script.read(scanBuffer.get(), SCAN_BUFFER_SIZE - 1);
    script.close();
    scanBuffer[bytesRead > 0 ? bytesRead : 0] = '\0';
    const char* tag = strstr(scanBuffer.get(), DESCRIPTION_TAG);
    if (tag) {
      tag += strlen(DESCRIPTION_TAG);
      while (*tag == ' ' || *tag == '\t') ++tag;
      const char* end = strpbrk(tag, "\r\n");
      if (end && end > tag) apps[i].description.assign(tag, end - tag);
    }
    ++i;
  }

  std::sort(apps.begin(), apps.end(), [](const AppInfo& left, const AppInfo& right) { return left.name < right.name; });
}

void AppsActivity::onEnter() {
  Activity::onEnter();
  scanBuffer = makeUniqueNoThrow<char[]>(SCAN_BUFFER_SIZE);
  if (!scanBuffer) LOG_ERR("APPS", "OOM: scan buffer");
  selectedIndex = 0;
  loadApps();
  requestUpdate();
}

void AppsActivity::onExit() {
  apps.clear();
  scanBuffer.reset();
  Activity::onExit();
}

void AppsActivity::launchSelected() {
  if (apps.empty()) return;
  auto activity = makeUniqueNoThrow<cplua::LuaHost>(renderer, mappedInput, gpio, apps[selectedIndex].name);
  if (!activity) {
    LOG_ERR("APPS", "OOM: LuaHost");
    return;
  }
  // Rescan on Return - An app can install or remove other apps (the AppStore does), so the list
  // built in onEnter() is stale by the time control comes back.
  startActivityForResult(std::move(activity), [this](const ActivityResult&) {
    const std::string previous = apps.empty() ? std::string() : apps[selectedIndex].name;
    loadApps();
    selectedIndex = 0;
    for (size_t i = 0; i < apps.size(); ++i) {
      if (apps[i].name == previous) {
        selectedIndex = static_cast<int>(i);
        break;
      }
    }
    requestUpdate();
  });
}

void AppsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    activityManager.goHome(HomeMenuItem::APPS);
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    launchSelected();
    return;
  }
  if (apps.empty()) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  switch (handleListTouch(selectedIndex, apps.size(), contentTop, contentHeight, true)) {
    case ListTouchResult::Activated:
      launchSelected();
      return;
    case ListTouchResult::Consumed:
      return;
    case ListTouchResult::None:
      break;
  }

  const int pageItems = GUI.getListPageItems(contentHeight, true);
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectedIndex = ButtonNavigator::nextPageIndex(selectedIndex, apps.size(), pageItems);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectedIndex = ButtonNavigator::previousPageIndex(selectedIndex, apps.size(), pageItems);
    requestUpdate();
    return;
  }

  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, apps.size());
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, apps.size());
    requestUpdate();
  });
}

void AppsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_APPS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = height - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  if (apps.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, height / 2, tr(STR_NO_APPS));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, width, contentHeight}, apps.size(), selectedIndex,
        [this](int index) { return apps[index].name; }, [this](int index) { return apps[index].description; });
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
