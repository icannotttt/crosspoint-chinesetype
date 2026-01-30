#pragma once
#include <Txt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <memory>

#include "../Activity.h"

class TxtReaderChapterSelectionActivity final : public Activity {
  std::shared_ptr<Txt> txt;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  uint32_t beginbype = 0;
  int selectorIndex = 0;
  bool updateRequired = false;
  const std::function<void()> onGoBack;
  const std::function<void(uint32_t newbype)> onSelectbype;

  int getPageItems() const;
  int findChapterIndexForPage(uint32_t bype) const;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();

 public:

  explicit TxtReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const std::shared_ptr<Txt> txt, uint32_t beginbype,
                                             const std::function<void()>& onGoBack,
                                             const std::function<void(uint32_t newbype)>& onSelectbype)
      : Activity("TxtReaderChapterSelection", renderer, mappedInput),
        txt(txt),
        beginbype(beginbype),
        onGoBack(onGoBack),
        onSelectbype(onSelectbype) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
