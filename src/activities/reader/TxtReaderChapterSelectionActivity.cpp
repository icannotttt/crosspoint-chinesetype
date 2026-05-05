#include "TxtReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "fontIds.h"

namespace {
int page=1;
constexpr int CHAPTER_PAGE_SIZE = 15;
}  // namespace

int TxtReaderChapterSelectionActivity::getPageItems() const {
  return CHAPTER_PAGE_SIZE;
}


void TxtReaderChapterSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<TxtReaderChapterSelectionActivity*>(param);
  self->displayTaskLoop();
}

void TxtReaderChapterSelectionActivity::onEnter() {
  Activity::onEnter();


  renderingMutex = xSemaphoreCreateMutex();
  //进入当前章节
  page=chapternum / getPageItems()+1;
  selectorIndex = chapternum; // 计算当前章节在页内的索引
  // 初始化选中项：默认选中第一个章节（跳过顶部特殊选项）
  if (selectorIndex < 0) selectorIndex = (page - 1) * getPageItems();

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
  const bool upReleased = mappedInput.wasReleased(MappedInputManager::Button::Up);
  const bool downReleased = mappedInput.wasReleased(MappedInputManager::Button::Down);
  const bool leftReleased = mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool rightReleased = mappedInput.wasReleased(MappedInputManager::Button::Right);

  const int pageItems = getPageItems();
  const int total = pageItems;

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    onSelectchapter(selectorIndex);
  } 
  // 原有返回键逻辑
  else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
  } 
  else if (upReleased) {
    page -= 1;
    if(page < 1) page = 1;
    selectorIndex = (page - 1) * total;
    updateRequired = true;
  } 
  else if (downReleased) {
    page += 1;
    selectorIndex = page * total - 1;
    updateRequired = true;
  }
  else if (leftReleased) {
    if (selectorIndex == (page - 1) * total) {
      selectorIndex = page * total - 1;
    } else {
      selectorIndex -= 1;
    }
    updateRequired = true;
  }
  else if (rightReleased) {
    if (selectorIndex == page * total - 1) {
      selectorIndex = (page - 1) * total;
    } else {
      selectorIndex += 1;
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
  const int pagebegin=(page-1)*getPageItems();
  const int batchStart = (pagebegin / CHAPTER_PAGE_SIZE) * CHAPTER_PAGE_SIZE;

  renderer.drawCenteredText(UI_12_FONT_ID, 15, "目  录", true, EpdFontFamily::BOLD);

  // 纯章节列表模式：移除顶部快捷项后，列表从更靠上的位置开始。
  const int screenHeight = renderer.getScreenHeight();
  const int BASE_Y_CHAPTER = 45;
  const int bottomPadding = 10;
  int FIX_LINE_HEIGHT = (screenHeight - BASE_Y_CHAPTER - bottomPadding) / getPageItems();
  if (FIX_LINE_HEIGHT < 12) {
    FIX_LINE_HEIGHT = 12;
  }

  // 每次仅在批次变化时解析一次，避免连续翻页时在渲染线程重复触发IO扫描。
  static int parsedBatchStart = -1;
  if (this->txt != nullptr && parsedBatchStart != batchStart) {
    txt->parseChapterIndexAndOffset(batchStart);
    parsedBatchStart = batchStart;
  }

  // ========== 步骤：绘制章节列表 ==========
  int renderedRows = 0;
  for (int i = pagebegin; i <= pagebegin + getPageItems() - 1; i++) {
    if(this->txt == nullptr){
          continue;
      }

    if(!this->txt->isChapterExist(i)){
      continue;
    }
      
      std::string dirTitle = this->txt->getChapterTitleByIndex(i);
    char title[64];
      strncpy(title, dirTitle.c_str(), sizeof(title)-1);
      title[sizeof(title)-1] = '\0';

    // 标题为空时重载当前块并重取，避免偶发空标题。
      if(strlen(title) == 0){
          Serial.printf("[%lu] [TRC] 章节标题为空，主动修复\n", millis());
      txt->parseChapterIndexAndOffset(batchStart);
      parsedBatchStart = batchStart;
      dirTitle = this->txt->getChapterTitleByIndex(i);
      strncpy(title, dirTitle.c_str(), sizeof(title)-1);
          title[sizeof(title)-1] = '\0';
      }

    int drawY = BASE_Y_CHAPTER + renderedRows * FIX_LINE_HEIGHT;
    renderedRows++;

      //renderer.drawText(UI_10_FONT_ID, 20, drawY, title, i != selectorIndex);
      if (i == selectorIndex) {
        renderer.fillRect(0, drawY, renderer.getScreenWidth(), FIX_LINE_HEIGHT);
        renderer.drawText(UI_10_FONT_ID, 20, drawY, title, 0);
      } else {
        //renderer.drawRect(0, drawY, 480, FIX_LINE_HEIGHT);
        renderer.drawText(UI_10_FONT_ID, 20, drawY, title, 1);
      }
      //Serial.printf("[%lu] [TRC] 查看为啥不匹配：i:%d,selectorIndex: %d \n", millis(),i,selectorIndex);
  }

  renderer.displayBuffer();
}