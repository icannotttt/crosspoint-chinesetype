#pragma once

#include <Xtc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <memory>

#include "BookmarkStore.h"
#include "activities/ActivityWithSubactivity.h"

class XtcBookmarkSelectionActivity final : public ActivityWithSubactivity {
 public:
  explicit XtcBookmarkSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        const std::shared_ptr<Xtc>& xtc, const std::function<void()>& onGoBack,
                                        const std::function<void(uint32_t)>& onSelect)
      : ActivityWithSubactivity("XtcBookmarkSelection", renderer, mappedInput),
        xtc(xtc),
        onGoBack(onGoBack),
        onSelect(onSelect) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  std::shared_ptr<Xtc> xtc;
  std::vector<BookmarkStore::BookmarkRecord> bookmarks;
  int selectorIndex = 0;
  bool updateRequired = false;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  const std::function<void()> onGoBack;
  const std::function<void(uint32_t)> onSelect;

  int getTotalItems() const { return static_cast<int>(bookmarks.size()); }
  const BookmarkStore::BookmarkRecord& getRecordByUiIndex(int uiIndex) const;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();
};
