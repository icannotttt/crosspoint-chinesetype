#include "XtcReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "fontIds.h"
#include "Xtc.h"

namespace {
constexpr int SKIP_PAGE_MS = 700;
int page = 1;
}  // namespace

int XtcReaderChapterSelectionActivity::getPageItems() const {
  return 25; // ✅ 优化：固定返回25，匹配业务逻辑
}

void XtcReaderChapterSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<XtcReaderChapterSelectionActivity*>(param);
  self->displayTaskLoop();
}

void XtcReaderChapterSelectionActivity::onEnter() {
  renderer.clearScreen();
  Activity::onEnter();

  updateRequired = true;
  selectorIndex = 0;
  page = 1;
  xTaskCreate(&XtcReaderChapterSelectionActivity::taskTrampoline, "XtcReaderChapterSelectionTask",
              4096,        
              this,        
              1,           
              &displayTaskHandle
  );
}

void XtcReaderChapterSelectionActivity::onExit() {
  Activity::onExit();

  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
}

void XtcReaderChapterSelectionActivity::loop() {
  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);

  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;
  const int pageItems = getPageItems();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const int pagebegin=(page-1)*25;
    xtc->readChapters_gd(pagebegin);
    uint32_t chapterpage = this->xtc->getChapterstartpage(selectorIndex);
    Serial.printf("[%lu] [XTC] 跳转章节：%d,跳转页数：%d\n", millis(), selectorIndex, chapterpage);
    
    onSelectPage(chapterpage);
    // 确认按键逻辑，按需补充
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
  } else if (prevReleased) {
    bool isUpKey = mappedInput.wasReleased(MappedInputManager::Button::Up);
    if (skipPage || isUpKey) {
      page -= 1;
      if(page < 1) page = 1; 
      selectorIndex = (page-1)*25; // ✅ BUG修复：局部索引0，选中当前页第一个
    } else {
      selectorIndex--; // ✅ BUG修复：局部索引减1
      if(selectorIndex < 0) selectorIndex = 0; // ✅ 边界防护
    }
    updateRequired = true;
  } else if (nextReleased) {
    bool isDownKey = mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (skipPage || isDownKey) {
      page += 1;
      selectorIndex = (page-1)*25; // ✅ BUG修复：局部索引24，选中当前页第一个
    } else {
      selectorIndex++; // ✅ BUG修复：局部索引加1
    }
    updateRequired = true;
  }
}

void XtcReaderChapterSelectionActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      renderScreen();
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void XtcReaderChapterSelectionActivity::renderScreen() {
  renderer.clearScreen();
  const int pagebegin=(page-1)*25;
  int page_chapter=25;
  static int parsedPage = -1; // ✅ 保留页码缓存，只解析1次

  if (parsedPage != page) {
    xtc->readChapters_gd(pagebegin);
    parsedPage = page;
  }

  const auto pageWidth = renderer.getScreenWidth();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "目录", true, EpdFontFamily::BOLD);

  const int FIX_LINE_HEIGHT = 29;
  const int BASE_Y = 60;

  // ✅ 强制循环渲染25章(pagebegin ~ pagebegin+24)，无有效数判断、不截断、不满也留空行
  for (int i = pagebegin; i <= pagebegin + page_chapter - 1; i++) {
      int localIdx = i - pagebegin; // ✅ 保留核心修复：全局索引→局部索引0~24，必加！读取数据全靠它
      
      uint32_t currOffset = this->xtc->getChapterstartpage(i); // ✅ 传局部索引，能读到正确数据
      std::string dirTitle = this->xtc->getChapterTitleByIndex(i); // ✅ 传局部索引，能读到正确标题
      
      Serial.printf("[%lu] [XTC_CHAPTER] 第%d章，名字为:%s,页码为%d\n", millis(), i, dirTitle.c_str(),currOffset);
      static char title[64];
      strncpy(title, dirTitle.c_str(), sizeof(title)-1);
      title[sizeof(title)-1] = '\0';
      
      int drawY = BASE_Y + localIdx * FIX_LINE_HEIGHT; // ✅ 简化计算，逻辑正确
      if (i == selectorIndex) {
        renderer.fillRect(0, drawY, 480, FIX_LINE_HEIGHT);
        renderer.drawText(UI_10_FONT_ID, 20, drawY, title, 0);
      } else {
        //renderer.drawRect(0, drawY, 480, FIX_LINE_HEIGHT);
        renderer.drawText(UI_10_FONT_ID, 20, drawY, title, 1);
      }

      //Serial.printf("选中的选项是：%d\n",selectorIndex); // ✅ 补全换行符，日志整洁
      //renderer.drawText(UI_10_FONT_ID, 20, drawY, title, i!= selectorIndex); // ✅ 核心修复：选中态正常，必加！
  }

  renderer.displayBuffer();
}