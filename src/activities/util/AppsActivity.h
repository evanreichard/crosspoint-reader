#pragma once

#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class AppsActivity final : public Activity {
 public:
  AppsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput) : Activity("Apps", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct AppInfo {
    std::string name;
    std::string description;
  };

  ButtonNavigator buttonNavigator;
  std::vector<AppInfo> apps;
  std::unique_ptr<char[]> scanBuffer;
  int selectedIndex = 0;

  void loadApps();
  void launchSelected();
};
