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
  int chapternum = 0;
  int selectorIndex = 0;
  bool updateRequired = false;
  const std::function<void()> onGoBack;
  const std::function<void(int newChapterNum)> onSelectchapter;

  int getPageItems() const;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();

 public:

  explicit TxtReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const std::shared_ptr<Txt> txt, int chapternum,
                                             const std::function<void()>& onGoBack,
                                             const std::function<void(int newChapterNum)>& onSelectchapter)
      : Activity("TxtReaderChapterSelection", renderer, mappedInput),
        txt(txt),
        chapternum(chapternum),
        onGoBack(onGoBack),
        onSelectchapter(onSelectchapter) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
