#include "TxtReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "fontIds.h"

namespace {
constexpr int SKIP_PAGE_MS = 700;
int page=1;
// 新增：100章对应的page偏移量
constexpr int PAGE_OFFSET_100_CHAPTER = 4;
// 新增：顶部特殊选项的索引定义
constexpr int ITEM_SKIP_100_BACK = -2;    // 向前100章选项索引
constexpr int ITEM_SKIP_100_FORWARD = -1; // 向后100章选项索引
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
  // 初始化选中项：默认选中第一个章节（跳过顶部特殊选项）
  if (selectorIndex < 0) selectorIndex = (page - 1) * 25;

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

//章节选择逻辑（新增顶部选项点击处理）
void TxtReaderChapterSelectionActivity::loop() {
  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);

  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;
  const int pageItems = getPageItems();
  const int total = 25;

  // ========== 核心新增：处理顶部特殊选项的确认点击 ==========
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // 点击「向前100章」选项
    if (selectorIndex == ITEM_SKIP_100_BACK) {
      page -= PAGE_OFFSET_100_CHAPTER;
      if (page < 1) page = 1; // 页码保底
      selectorIndex = (page - 1) * total; // 选中当前页第一个章节
      updateRequired = true;
      Serial.printf("[ChapterSkip] ✅ 点击向前100章 | 当前page：%d\n", page);
    }
    // 点击「向后100章」选项
    else if (selectorIndex == ITEM_SKIP_100_FORWARD) {
      page += PAGE_OFFSET_100_CHAPTER;
      selectorIndex = page * total - 1; // 选中当前页最后一个章节
      updateRequired = true;
      Serial.printf("[ChapterSkip] ✅ 点击向后100章 | 当前page：%d\n", page);
    }
    // 原有章节确认逻辑
    else {
      uint32_t chapterOffset = 0;
      chapterOffset = this->txt->getChapterOffsetByIndex(selectorIndex);
      //uint32_t chapterOffset2 = 0;
      //chapterOffset2 = this->txt->getChapterOffsetByIndex(selectorIndex+1);   
      //txt->SectionLayout(chapterOffset,chapterOffset2);  
      onSelectbype(chapterOffset);
    }
  } 
  // 原有返回键逻辑
  else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
  } 
  // ========== 核心修改：上下/左右按键支持选中顶部特殊选项 ==========
  else if (prevReleased) {
    bool isUpKey = mappedInput.wasReleased(MappedInputManager::Button::Up);
    if (skipPage || isUpKey) {
      // 翻页逻辑：如果当前选中的是顶部选项，翻页到上一页
      if (selectorIndex == ITEM_SKIP_100_BACK || selectorIndex == ITEM_SKIP_100_FORWARD) {
        page -= 1;
        if(page < 1) page = 1;
        selectorIndex = (page - 1) * total;
      } else {
        page -= 1;
        if(page < 1) page = 1;
        selectorIndex = (page - 1) * total;
      }
    } else {
      // 单步上选逻辑：支持选中顶部选项
      if (selectorIndex == (page - 1) * total) {
        // 当前选中第一个章节 → 上选到「向后100章」
        selectorIndex = ITEM_SKIP_100_FORWARD;
      } else if (selectorIndex == ITEM_SKIP_100_FORWARD) {
        // 当前选中「向后100章」→ 上选到「向前100章」
        selectorIndex = ITEM_SKIP_100_BACK;
      } else if (selectorIndex == ITEM_SKIP_100_BACK) {
        // 当前选中「向前100章」→ 循环到最后一个章节
        selectorIndex = page * total - 1;
      } else {
        // 正常单步上选
        selectorIndex = (selectorIndex + total - 1) % total + (page - 1) * total;
      }
    }
    updateRequired = true;
  } 
  else if (nextReleased) {
    bool isDownKey = mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (skipPage || isDownKey) {
      // 翻页逻辑：如果当前选中的是顶部选项，翻页到下一页
      if (selectorIndex == ITEM_SKIP_100_BACK || selectorIndex == ITEM_SKIP_100_FORWARD) {
        page += 1;
        selectorIndex = page * total - 1;
      } else {
        page += 1;
        selectorIndex = page * total - 1;
      }
    } else {
      // 单步下选逻辑：支持选中顶部选项
      if (selectorIndex == ITEM_SKIP_100_BACK) {
        // 当前选中「向前100章」→ 下选到「向后100章」
        selectorIndex = ITEM_SKIP_100_FORWARD;
      } else if (selectorIndex == ITEM_SKIP_100_FORWARD) {
        // 当前选中「向后100章」→ 下选到第一个章节
        selectorIndex = (page - 1) * total;
      } else if (selectorIndex == page * total - 1) {
        // 当前选中最后一个章节 → 下选到「向前100章」
        selectorIndex = ITEM_SKIP_100_BACK;
      } else {
        // 正常单步下选
        selectorIndex = (selectorIndex + 1) % total + (page - 1) * total;
      }
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
  static int parsedPage = -1;

  // 同一个页码只解析1次
  if (parsedPage != page) {
    txt->parseChapterIndexAndOffset(pagebegin);
    parsedPage = page;
  }

  const auto pageWidth = renderer.getScreenWidth();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "目  录", true, EpdFontFamily::BOLD);

  uint32_t chapterOffset = 0;
  if (this->txt != nullptr) {
      chapterOffset = this->txt->getChapterOffsetByIndex(pagebegin);
  }

  // ========== 核心新增：顶部特殊选项的绘制参数 ==========
  const int FIX_LINE_HEIGHT = 25;
  // 基准Y值：先绘制顶部两个特殊选项，再绘制章节列表
  const int BASE_Y_SPECIAL = 40;    // 顶部选项起始Y
  const int BASE_Y_CHAPTER = 80;    // 章节列表起始Y（两个选项占2行）

  // ========== 步骤1：绘制顶部「向前100章」选项 ==========
  std::string skipBackText = "【向前100章】";
  int skipBackY = BASE_Y_SPECIAL;
  if (ITEM_SKIP_100_BACK == selectorIndex) {
  renderer.fillRect(10, skipBackY, 150, FIX_LINE_HEIGHT);
  renderer.drawText(UI_10_FONT_ID, 20, skipBackY, skipBackText.c_str(), 0);
  } else {
    //renderer.drawRect(0, drawY, 480, FIX_LINE_HEIGHT);
    renderer.drawText(UI_10_FONT_ID, 20, skipBackY, skipBackText.c_str(), 1);
  }
  //renderer.drawText(UI_10_FONT_ID, 20, skipBackY, skipBackText.c_str(), selectorIndex != ITEM_SKIP_100_BACK);

  // ========== 步骤2：绘制顶部「向后100章」选项 ==========
  std::string skipForwardText = "【向后100章】";
  int skipForwardY = BASE_Y_SPECIAL;
  if (ITEM_SKIP_100_FORWARD == selectorIndex) {
  renderer.fillRect(200, skipBackY, 150, FIX_LINE_HEIGHT);
  renderer.drawText(UI_10_FONT_ID, 200, skipForwardY, skipForwardText.c_str(), 0);
  } else {
    //renderer.drawRect(0, drawY, 480, FIX_LINE_HEIGHT);
    renderer.drawText(UI_10_FONT_ID, 200, skipForwardY, skipForwardText.c_str(), 1);
  }

  //renderer.drawText(UI_10_FONT_ID, 200, skipForwardY, skipForwardText.c_str(), selectorIndex != ITEM_SKIP_100_FORWARD);

  // ========== 步骤3：绘制章节列表（下移到BASE_Y_CHAPTER） ==========
  for (int i = pagebegin; i <= pagebegin + page_chapter - 1; i++) {
      if(this->txt == nullptr || !this->txt->isChapterExist(i)){
          continue;
      }
      
      uint32_t currOffset = this->txt->getChapterOffsetByIndex(i);
      std::string dirTitle = this->txt->getChapterTitleByIndex(i);
      static char title[64];
      strncpy(title, dirTitle.c_str(), sizeof(title)-1);
      title[sizeof(title)-1] = '\0';
      
      int relativeIdx = i - pagebegin;
      int drawY = BASE_Y_CHAPTER + relativeIdx * FIX_LINE_HEIGHT;

      //renderer.drawText(UI_10_FONT_ID, 20, drawY, title, i != selectorIndex);
      if (i == selectorIndex) {
        renderer.fillRect(0, drawY, 480, FIX_LINE_HEIGHT);
        renderer.drawText(UI_10_FONT_ID, 20, drawY, title, 0);
      } else {
        //renderer.drawRect(0, drawY, 480, FIX_LINE_HEIGHT);
        renderer.drawText(UI_10_FONT_ID, 20, drawY, title, 1);
      }
      //Serial.printf("[%lu] [TRC] 查看为啥不匹配：i:%d,selectorIndex: %d \n", millis(),i,selectorIndex);
  }

  renderer.displayBuffer();
}