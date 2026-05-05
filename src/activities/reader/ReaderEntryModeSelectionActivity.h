#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <vector>

#include "activities/ActivityWithSubactivity.h"

class ReaderEntryModeSelectionActivity final : public ActivityWithSubactivity {
 public:
  enum class Mode { CHAPTER, BOOKMARK, AUTO_PAGE_TURN, AUTO_PAGE_TURN_TIME, BLUETOOTH };

  explicit ReaderEntryModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                            const std::function<void()>& onGoBack,
                                            const std::function<void(Mode)>& onSelect)
      : ActivityWithSubactivity("ReaderEntryModeSelection", renderer, mappedInput),
        onGoBack(onGoBack),
        onSelect(onSelect) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  struct Item {
    Mode mode;
    const char* label;
  };

  const std::vector<Item> items = {{Mode::CHAPTER, "目录"},
                                   {Mode::BOOKMARK, "书签"},
                                   {Mode::AUTO_PAGE_TURN, "自动翻页"},
                                   {Mode::AUTO_PAGE_TURN_TIME, "自动翻页时间"},
                                   {Mode::BLUETOOTH, "蓝牙设置"}};
  int selectedIndex = 0;
  bool updateRequired = false;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;

  const std::function<void()> onGoBack;
  const std::function<void(Mode)> onSelect;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();
};
