#include "ReaderEntryModeSelectionActivity.h"

#include <GfxRenderer.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
uint8_t clampAutoPageTurnSeconds(const uint8_t seconds) {
  if (seconds < CrossPointSettings::AUTO_PAGE_TURN_TIME_MIN) {
    return CrossPointSettings::AUTO_PAGE_TURN_TIME_MIN;
  }
  if (seconds > CrossPointSettings::AUTO_PAGE_TURN_TIME_MAX) {
    return CrossPointSettings::AUTO_PAGE_TURN_TIME_MAX;
  }
  return seconds;
}
}

void ReaderEntryModeSelectionActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();
  updateRequired = true;
  xTaskCreate(&ReaderEntryModeSelectionActivity::taskTrampoline, "ReaderEntryModeSelectionTask", 4096, this, 1,
              &displayTaskHandle);
}

void ReaderEntryModeSelectionActivity::onExit() {
  ActivityWithSubactivity::onExit();
  if (displayTaskHandle) {
    if (xSemaphoreTake(renderingMutex, portMAX_DELAY) == pdTRUE) {
      vTaskDelete(displayTaskHandle);
      displayTaskHandle = nullptr;
      xSemaphoreGive(renderingMutex);
    }
  }

  if (renderingMutex) {
    vSemaphoreDelete(renderingMutex);
  }
  renderingMutex = nullptr;
}

void ReaderEntryModeSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<ReaderEntryModeSelectionActivity*>(param);
  self->displayTaskLoop();
}

[[noreturn]] void ReaderEntryModeSelectionActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      if (xSemaphoreTake(renderingMutex, portMAX_DELAY) == pdTRUE) {
        renderScreen();
        xSemaphoreGive(renderingMutex);
      }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void ReaderEntryModeSelectionActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto mode = items[selectedIndex].mode;
    if (mode == Mode::AUTO_PAGE_TURN) {
      SETTINGS.autoPageTurn = SETTINGS.autoPageTurn ? 0 : 1;
      SETTINGS.saveToFile();
      updateRequired = true;
      return;
    }
    if (mode == Mode::AUTO_PAGE_TURN_TIME) {
      uint8_t next = clampAutoPageTurnSeconds(SETTINGS.autoPageTurnTime);
      next = (next >= CrossPointSettings::AUTO_PAGE_TURN_TIME_MAX) ? CrossPointSettings::AUTO_PAGE_TURN_TIME_MIN
                                                                    : static_cast<uint8_t>(next + 1);
      SETTINGS.autoPageTurnTime = next;
      SETTINGS.saveToFile();
      updateRequired = true;
      return;
    }
    onSelect(mode);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
      mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    selectedIndex = (selectedIndex + static_cast<int>(items.size()) - 1) % static_cast<int>(items.size());
    updateRequired = true;
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
      mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    selectedIndex = (selectedIndex + 1) % static_cast<int>(items.size());
    updateRequired = true;
    return;
  }
}

void ReaderEntryModeSelectionActivity::renderScreen() {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "选择进入方式");

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      std::max(0, pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing);

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(items.size()), selectedIndex,
      [this](int index) { return std::string(items[index].label); }, nullptr, nullptr,
      [this](int index) {
        const auto mode = items[index].mode;
        if (mode == Mode::AUTO_PAGE_TURN) {
          return std::string(SETTINGS.autoPageTurn ? "开启" : "关闭");
        }
        if (mode == Mode::AUTO_PAGE_TURN_TIME) {
          const uint8_t seconds = clampAutoPageTurnSeconds(SETTINGS.autoPageTurnTime);
          return std::to_string(seconds) + "s";
        }
        return std::string();
      });

  const auto labels = mappedInput.mapLabels("« 返回", "选择", "向上", "向下");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
