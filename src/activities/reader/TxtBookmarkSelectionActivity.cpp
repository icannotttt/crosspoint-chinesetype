#include "TxtBookmarkSelectionActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

const BookmarkStore::BookmarkRecord& TxtBookmarkSelectionActivity::getRecordByUiIndex(const int uiIndex) const {
  const int idx = getTotalItems() - 1 - uiIndex;
  return bookmarks[idx];
}

void TxtBookmarkSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<TxtBookmarkSelectionActivity*>(param);
  self->displayTaskLoop();
}

void TxtBookmarkSelectionActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  if (!txt) {
    onGoBack();
    return;
  }

  bookmarks = BookmarkStore::load(txt->getCachePath(), txt->getPath());
  selectorIndex = 0;
  renderingMutex = xSemaphoreCreateMutex();
  updateRequired = true;
  xTaskCreate(&TxtBookmarkSelectionActivity::taskTrampoline, "TxtBookmarkSelectionTask", 4096, this, 1,
              &displayTaskHandle);
}

void TxtBookmarkSelectionActivity::onExit() {
  ActivityWithSubactivity::onExit();
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void TxtBookmarkSelectionActivity::loop() {
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
    onSelect(static_cast<int>(item.pos3), static_cast<int>(item.pos1), static_cast<int>(item.progressPercent));
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

void TxtBookmarkSelectionActivity::displayTaskLoop() {
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

void TxtBookmarkSelectionActivity::renderScreen() {
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
        return std::string("第") + std::to_string(item.pos3 + 1) + "章 " +
               std::to_string(item.progressPercent) + "%";
      },
      nullptr, nullptr, nullptr);

  const auto labels = mappedInput.mapLabels("« 返回", "选择", "向上", "向下");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
