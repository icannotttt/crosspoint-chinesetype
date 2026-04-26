#pragma once

#include <BluetoothHIDManager.h>
#include <GfxRenderer.h>
#include <string>

#include "activities/Activity.h"
#include "MappedInputManager.h"

class BluetoothSettingsActivity : public Activity {
 private:
  enum class ViewMode {
    MAIN_MENU,
    DEVICE_LIST
  };

  ViewMode viewMode = ViewMode::MAIN_MENU;
  int selectedIndex = 0;
  BluetoothHIDManager* btMgr = nullptr;
  std::string lastError = "";
  unsigned long lastScanTime = 0;

 public:
  explicit BluetoothSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     const std::function<void()>& onComplete)
      : Activity("BluetoothSettings", renderer, mappedInput), onComplete(onComplete) {}

  void onEnter() override;
  void loop() override;
  void onExit() override;

 private:
   TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;
   static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render();
  void handleMainMenuInput();
  void handleDeviceListInput();
  void renderMainMenu();
  void renderDeviceList();
  std::string getSignalStrengthIndicator(const int32_t rssi) const;
  
  const std::function<void()> onComplete;
};
