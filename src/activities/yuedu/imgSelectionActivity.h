#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"

class Bitmap;

class imgSelectionActivity final : public Activity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  std::string basepath = "/";
  std::vector<std::string> files;
  int selectorIndex = 0;
  bool updateRequired = false;
  const std::function<void(const std::string&)> onSelect;
  const std::function<void()> onGoHome;
  //加图片选择支持
  void renderBitmapScreen(const Bitmap& bitmap) const;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  void loadFiles();

 public:
  explicit imgSelectionActivity(GfxRenderer& renderer, InputManager& inputManager,
                                 const std::function<void(const std::string&)>& onSelect,
                                 const std::function<void()>& onGoHome)
      : Activity("imgSelection", renderer, inputManager), onSelect(onSelect), onGoHome(onGoHome) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
