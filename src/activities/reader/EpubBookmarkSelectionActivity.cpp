#include "EpubBookmarkSelectionActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

const BookmarkStore::BookmarkRecord& EpubBookmarkSelectionActivity::getRecordByUiIndex(const int uiIndex) const {
  const int idx = getTotalItems() - 1 - uiIndex;
  return bookmarks[idx];
}

void EpubBookmarkSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<EpubBookmarkSelectionActivity*>(param);
  self->displayTaskLoop();
}

void EpubBookmarkSelectionActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  if (!epub) {
    onGoBack();
    return;
  }

  bookmarks = BookmarkStore::load(epub->getCachePath(), epub->getPath());
  selectorIndex = 0;
  renderingMutex = xSemaphoreCreateMutex();
  updateRequired = true;
  xTaskCreate(&EpubBookmarkSelectionActivity::taskTrampoline, "EpubBookmarkSelectionTask", 4096, this, 1,
              &displayTaskHandle);
}

void EpubBookmarkSelectionActivity::onExit() {
  ActivityWithSubactivity::onExit();
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void EpubBookmarkSelectionActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
    return;
  }

  if (getTotalItems() == 0) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      onGoBack();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto& item = getRecordByUiIndex(selectorIndex);
    onSelect(item);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
      mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    selectorIndex = (selectorIndex + getTotalItems() - 1) % getTotalItems();
    updateRequired = true;
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
      mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    selectorIndex = (selectorIndex + 1) % getTotalItems();
    updateRequired = true;
    return;
  }
}

void EpubBookmarkSelectionActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired && !subActivity) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      renderScreen();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void EpubBookmarkSelectionActivity::renderScreen() {
  renderer.clearScreen();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "书签");

  if (getTotalItems() == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, "暂无书签", true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels("« 返回", "返回", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      std::max(0, pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing);

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, getTotalItems(), selectorIndex,
      [this](int index) {
        const auto& item = getRecordByUiIndex(index);
        const int spineIndex = static_cast<int>(item.pos1);
        std::string title = "章节";
        const int tocIndex = epub->getTocIndexForSpineIndex(spineIndex);
        if (tocIndex >= 0 && tocIndex < epub->getTocItemsCount()) {
          title = epub->getTocItem(tocIndex).title;
        }
        return title + " " + std::to_string(item.progressPercent) + "%";
      },
      nullptr, nullptr, nullptr);

  const auto labels = mappedInput.mapLabels("« 返回", "选择", "向上", "向下");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
