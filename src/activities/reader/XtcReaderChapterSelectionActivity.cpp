#include "XtcReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "Xtc.h"

//目录跟随旋转
#include "CrossPointSettings.h"

namespace {
constexpr int SKIP_PAGE_MS = 700;
int page = 1;
}  // namespace

int XtcReaderChapterSelectionActivity::getPageItems() const {
  return std::max(1, UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false));
}

void XtcReaderChapterSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<XtcReaderChapterSelectionActivity*>(param);
  self->displayTaskLoop();
}

void XtcReaderChapterSelectionActivity::onEnter() {
  renderer.clearScreen();
  Activity::onEnter();

  // 屏幕方向配置
  switch (SETTINGS.orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  };


  updateRequired = true;
  //循环找所在章节

 selectorIndex = xtc->getchapter(currentPage); 
 page = selectorIndex/getPageItems()+1;

  xTaskCreate(&XtcReaderChapterSelectionActivity::taskTrampoline, "XtcReaderChapterSelectionTask",
              4096,        
              this,        
              1,           
              &displayTaskHandle
  );
}

void XtcReaderChapterSelectionActivity::onExit() {
  Activity::onExit();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

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
    const int pagebegin=(page-1)*getPageItems();
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
      selectorIndex = (page-1)*getPageItems(); 
    } else {
      selectorIndex--; 
      if(selectorIndex < 0) selectorIndex = 0; 
    }
    updateRequired = true;
  } else if (nextReleased) {
    bool isDownKey = mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (skipPage || isDownKey) {
      page += 1;
      selectorIndex = (page-1)*getPageItems(); 
    } else {
      selectorIndex++; 
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
  const int pagebegin=(page-1)*getPageItems();
  int page_chapter=getPageItems();
  static int parsedPage = -1; // ✅ 保留页码缓存，只解析1次

  if (parsedPage != page) {
    xtc->readChapters_gd(pagebegin);
    parsedPage = page;
  }

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "目录");

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      std::max(0, pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing);
    const int selectedRow = std::max(0, std::min(page_chapter - 1, selectorIndex - pagebegin));

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, page_chapter, selectedRow,
      [this, pagebegin](int index) {
        const int chapterIndex = pagebegin + index;
        std::string title = this->xtc->getChapterTitleByIndex(chapterIndex);
        if (title.empty()) {
          title = "(未命名章节)";
        }
        return title;
      },
      nullptr, nullptr, [this, pagebegin](int index) {
        const int chapterIndex = pagebegin + index;
        return std::to_string(this->xtc->getChapterstartpage(chapterIndex));
      });

  const auto labels = mappedInput.mapLabels("« 返回", "选择", "向上", "向下");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}