#pragma once
#include <Xtc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <memory>

#include "../Activity.h"

class XtcReaderChapterSelectionActivity final : public Activity {
  std::shared_ptr<Xtc> xtc;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  uint32_t currentPage = 0;
  int selectorIndex = 0;
  bool updateRequired = false;
  const std::function<void()> onGoBack;
  const std::function<void(uint32_t newPage)> onSelectPage;

  int getPageItems() const;
  int findChapterIndexForPage(uint32_t page) const;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();
    // ========== ✅ 新增3个核心变量 ==========
  // ========== 章节选择页 核心成员变量 ==========
  #define TOTAL_SHOW_CHAPTERS 50  // 固定一页显示25章
  uint16_t m_currentChapterStart = 0;  // 当前读取的章节起始索引 (0开始，翻页+=1/-=1)
  uint8_t  realShowCount = 0;           // 实际解析到的章节数量 (0~25)
  


 public:
  explicit XtcReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const std::shared_ptr<Xtc>& xtc, uint32_t currentPage,
                                             const std::function<void()>& onGoBack,
                                             const std::function<void(uint32_t newPage)>& onSelectPage)
      : Activity("XtcReaderChapterSelection", renderer, mappedInput),
        xtc(xtc),
        currentPage(currentPage),
        onGoBack(onGoBack),
        onSelectPage(onSelectPage) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
