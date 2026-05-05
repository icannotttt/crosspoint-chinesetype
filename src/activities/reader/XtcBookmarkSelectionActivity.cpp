#include "XtcBookmarkSelectionActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

const BookmarkStore::BookmarkRecord& XtcBookmarkSelectionActivity::getRecordByUiIndex(const int uiIndex) const {
  const int idx = getTotalItems() - 1 - uiIndex;
  return bookmarks[idx];
}

void XtcBookmarkSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<XtcBookmarkSelectionActivity*>(param);
  self->displayTaskLoop();
}

void XtcBookmarkSelectionActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  if (!xtc) {
    onGoBack();
    return;
  }

  bookmarks = BookmarkStore::load(xtc->getCachePath(), xtc->getPath());
  selectorIndex = 0;
  renderingMutex = xSemaphoreCreateMutex();
  updateRequired = true;
  xTaskCreate(&XtcBookmarkSelectionActivity::taskTrampoline, "XtcBookmarkSelectionTask", 4096, this, 1,
              &displayTaskHandle);
}

void XtcBookmarkSelectionActivity::onExit() {
  ActivityWithSubactivity::onExit();
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void XtcBookmarkSelectionActivity::loop() {
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
    onSelect(static_cast<uint32_t>(item.pos1));
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

void XtcBookmarkSelectionActivity::displayTaskLoop() {
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

void XtcBookmarkSelectionActivity::renderScreen() {
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
        return std::string("第") + std::to_string(item.pos1 + 1) + "页";
      },
      nullptr, nullptr,
      [this](int index) { return std::to_string(getRecordByUiIndex(index).progressPercent) + "%"; });

  const auto labels = mappedInput.mapLabels("« 返回", "选择", "向上", "向下");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
