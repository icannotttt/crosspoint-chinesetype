#include "EpubReaderMenuActivity.h"

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

void EpubReaderMenuActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();
  updateRequired = true;

  xTaskCreate(&EpubReaderMenuActivity::taskTrampoline, "EpubMenuTask", 4096, this, 1, &displayTaskHandle);
}

void EpubReaderMenuActivity::onExit() {
  ActivityWithSubactivity::onExit();
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void EpubReaderMenuActivity::taskTrampoline(void* param) {
  auto* self = static_cast<EpubReaderMenuActivity*>(param);
  self->displayTaskLoop();
}

void EpubReaderMenuActivity::displayTaskLoop() {
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

void EpubReaderMenuActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  // Use local variables for items we need to check after potential deletion
  if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
      mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    selectedIndex = (selectedIndex + menuItems.size() - 1) % menuItems.size();
    updateRequired = true;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
             mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    selectedIndex = (selectedIndex + 1) % menuItems.size();
    updateRequired = true;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto selectedAction = menuItems[selectedIndex].action;
    if (selectedAction == MenuAction::ROTATE_SCREEN) {
      // Cycle orientation preview locally; actual rotation happens on menu exit.
      pendingOrientation = (pendingOrientation + 1) % orientationLabels.size();
      updateRequired = true;
      return;
    }
    if (selectedAction == MenuAction::AUTO_PAGE_TURN_TOGGLE) {
      SETTINGS.autoPageTurn = SETTINGS.autoPageTurn ? 0 : 1;
      SETTINGS.saveToFile();
      updateRequired = true;
      return;
    }
    if (selectedAction == MenuAction::AUTO_PAGE_TURN_TIME) {
      uint8_t next = clampAutoPageTurnSeconds(SETTINGS.autoPageTurnTime);
      next = (next >= CrossPointSettings::AUTO_PAGE_TURN_TIME_MAX) ? CrossPointSettings::AUTO_PAGE_TURN_TIME_MIN
                                                                    : static_cast<uint8_t>(next + 1);
      SETTINGS.autoPageTurnTime = next;
      SETTINGS.saveToFile();
      updateRequired = true;
      return;
    }

    // 1. Capture the callback and action locally
    auto actionCallback = onAction;

    // 2. Execute the callback
    actionCallback(selectedAction);

    // 3. CRITICAL: Return immediately. 'this' is likely deleted now.
    return;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Return the pending orientation to the parent so it can apply on exit.
    onBack(pendingOrientation);
    return;  // Also return here just in case
  }
}

void EpubReaderMenuActivity::renderScreen() {
  renderer.clearScreen();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title.c_str());

  std::string progressLine;
  if (totalPages > 0) {
    progressLine = "本章: " + std::to_string(currentPage) + "/" + std::to_string(totalPages) + " pages  |  ";
  }
  progressLine += "全书: " + std::to_string(bookProgressPercent) + "%";

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int progressBoxX = metrics.contentSidePadding;
  const int progressBoxY = contentTop - metrics.verticalSpacing + 1;
  const int progressBoxWidth = pageWidth - metrics.contentSidePadding * 2;
  const int progressBoxHeight = renderer.getLineHeight(UI_10_FONT_ID) + 10;

  // Draw progress as a dedicated info strip so the menu hierarchy matches Lyra's card-like sections.
  renderer.drawCenteredText(UI_10_FONT_ID, progressBoxY + 5, progressLine.c_str());

  const int listTop = progressBoxY + progressBoxHeight + metrics.verticalSpacing;
  const int listHeight =
      std::max(0, pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing);

  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, listHeight}, static_cast<int>(menuItems.size()), selectedIndex,
      [this](int index) { return menuItems[index].label; }, nullptr, nullptr,
      [this](int index) {
        if (menuItems[index].action == MenuAction::ROTATE_SCREEN) {
          return std::string(orientationLabels[pendingOrientation]);
        }
        if (menuItems[index].action == MenuAction::AUTO_PAGE_TURN_TOGGLE) {
          return std::string(SETTINGS.autoPageTurn ? "开启" : "关闭");
        }
        if (menuItems[index].action == MenuAction::AUTO_PAGE_TURN_TIME) {
          const uint8_t seconds = clampAutoPageTurnSeconds(SETTINGS.autoPageTurnTime);
          return std::to_string(seconds) + "s";
        }
        return std::string();
      });

  // Footer / Hints
  const auto labels = mappedInput.mapLabels("« 返回", "选择", "向上", "向下");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
