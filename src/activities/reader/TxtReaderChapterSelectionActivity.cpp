#include "TxtReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "fontIds.h"

namespace {
constexpr int SKIP_PAGE_MS = 700;
int page=1;
}  // namespace

int TxtReaderChapterSelectionActivity::getPageItems() const {
  constexpr int startY = 60;
  constexpr int lineHeight = 30;

  const int screenHeight = renderer.getScreenHeight();
  const int availableHeight = screenHeight - startY;
  int items = availableHeight / lineHeight;
  if (items < 1) {
    items = 1;
  }
  return items;
}

int TxtReaderChapterSelectionActivity::findChapterIndexForPage(uint32_t bype) const {
  if (!txt) {
    return 0;
  }

  //const auto& chapters = xtc->getChapters();
  //for (size_t i = 0; i < chapters.size(); i++) {
  //  if (page >= chapters[i].startPage && page <= chapters[i].endPage) {
  //    return static_cast<int>(i);
  //  }
  //}
  return 1;
}

void TxtReaderChapterSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<TxtReaderChapterSelectionActivity*>(param);
  self->displayTaskLoop();
}

void TxtReaderChapterSelectionActivity::onEnter() {
  Activity::onEnter();

  if (!txt) {
    return;
  }

  renderingMutex = xSemaphoreCreateMutex();
  selectorIndex = findChapterIndexForPage(beginbype);

  updateRequired = true;
  xTaskCreate(&TxtReaderChapterSelectionActivity::taskTrampoline, "TxtReaderChapterSelectionActivityTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void TxtReaderChapterSelectionActivity::onExit() {
  Activity::onExit();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}
//章节选择逻辑
void TxtReaderChapterSelectionActivity::loop() {
  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);

  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;
  const int pageItems = getPageItems();
  const int total = 25;

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    uint32_t chapterOffset = 0;
    chapterOffset = this->txt->getChapterOffsetByIndex(selectorIndex);
    onSelectbype(chapterOffset);
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
  } else if (prevReleased) {
    // ========== 核心修改 ==========
    // Left → 短按=单步上选，长按=翻页 【逻辑不变】
    // Up → 【无条件翻页】，不管长按短按 都是 skipPage 翻页
    bool isUpKey = mappedInput.wasReleased(MappedInputManager::Button::Up);
    if (skipPage || isUpKey) {
      page -= 1;
      if(page < 1) page = 1; // 页码保底，防止越界
      selectorIndex = (page - 1) * total;
    } else {
      // 只有Left短按 走单步上选逻辑
      selectorIndex = (selectorIndex + total - 1) % total + (page - 1) * total;
    }
    updateRequired = true;
  } else if (nextReleased) {
    // ========== 核心修改 ==========
    // Right → 短按=单步下选，长按=翻页 【逻辑不变】
    // Down → 【无条件翻页】，不管长按短按 都是 skipPage 翻页
    bool isDownKey = mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (skipPage || isDownKey) {
      page += 1;
      selectorIndex = page * total - 1;
    } else {
      // 只有Right短按 走单步下选逻辑
      selectorIndex = (selectorIndex + 1) % total + (page - 1) * total;
    }
    updateRequired = true;
  }
}

//章节加载放在后台，按confirm随时加载
void TxtReaderChapterSelectionActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      renderScreen();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void TxtReaderChapterSelectionActivity::renderScreen() {
  renderer.clearScreen();
  const int pagebegin=(page-1)*25;
  int page_chapter=25;
  static int parsedPage = -1; // ✅ 页码缓存：同一个page只解析1次，解决耗时问题，无需改头文件

  // ✅ 核心优化：同一个页码只解析1次，彻底杜绝重复调用耗时解析
  if (parsedPage != page) {
    txt->parseChapterIndexAndOffset(pagebegin);
    parsedPage = page;
  }

  const auto pageWidth = renderer.getScreenWidth();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Select Chapter", true, BOLD);

  uint32_t chapterOffset = 0;
  if (this->txt != nullptr) {
      chapterOffset = this->txt->getChapterOffsetByIndex(pagebegin);
  }

  // ✅ 修复核心1：【修改行高公式】，解决最后一行超屏被截断的问题 ✔️ 重中之重
  // 方案：固定行高 29px ，适配800高度屏幕+25行，完美显示无裁剪，是最优值
  const int FIX_LINE_HEIGHT = 29;
  // 基准Y值微调为 35 ，上下留白均匀，不会顶头/露底
  const int BASE_Y = 35;

  // ✅ 修复核心2：【循环条件改为 i <= pagebegin + page_chapter - 1】，确保循环25次
  // pagebegin ~ pagebegin+24  刚好25个数字，不多不少
  for (int i = pagebegin; i <= pagebegin + page_chapter - 1; i++) {
      // ✅ 保留：章节为空则跳过，避免空行
      if(this->txt == nullptr || !this->txt->isChapterExist(i)){
          continue;
      }
      
      uint32_t currOffset = this->txt->getChapterOffsetByIndex(i);
      std::string dirTitle = this->txt->getChapterTitleByIndex(i);
      static char title[64];
      strncpy(title, dirTitle.c_str(), sizeof(title)-1);
      title[sizeof(title)-1] = '\0';
      
      // ✅ 保留：相对索引，彻底杜绝重叠，逻辑正确
      int relativeIdx = i - pagebegin;
      int drawY = BASE_Y + relativeIdx * FIX_LINE_HEIGHT;

      Serial.printf("选中的选项是：%d",selectorIndex);
      renderer.drawText(UI_10_FONT_ID, 20, drawY, title, i != selectorIndex);
  }

  renderer.displayBuffer();
}