#include "HomeActivity.h"

#include <GfxRenderer.h>
#include <InputManager.h>
#include <SD.h>

#include "config.h"

#include "images/zhuye.h" //主页加图片
#include "images/beijing.h" //主页加图片

namespace {
constexpr int menuItemCount = 4;
}

void HomeActivity::taskTrampoline(void* param) {
  auto* self = static_cast<HomeActivity*>(param);
  self->displayTaskLoop();
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  selectorIndex = 0;

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&HomeActivity::taskTrampoline, "HomeActivityTask",
              2048,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void HomeActivity::loop() {
  const bool prevPressed =
      inputManager.wasPressed(InputManager::BTN_UP) || inputManager.wasPressed(InputManager::BTN_LEFT);
  const bool nextPressed =
      inputManager.wasPressed(InputManager::BTN_DOWN) || inputManager.wasPressed(InputManager::BTN_RIGHT);

  if (inputManager.wasPressed(InputManager::BTN_CONFIRM)) {
    if (selectorIndex == 0) {
      onReaderOpen();
    } else if (selectorIndex == 1) {
      onFileTransferOpen();
    } else if (selectorIndex == 2) {
      onSettingsOpen();
      } else if (selectorIndex == 3) {
      onyueduOpen();
    }
  } else if (prevPressed) {
    selectorIndex = (selectorIndex + menuItemCount - 1) % menuItemCount;
    updateRequired = true;
  } else if (nextPressed) {
    selectorIndex = (selectorIndex + 1) % menuItemCount;
    updateRequired = true;
  }
}

void HomeActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void HomeActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  renderer.drawCenteredText(READER_FONT_ID, 10, "主页", true, BOLD);

  // Draw selection
  renderer.fillRect(0, 70 + selectorIndex * 70 + 2, pageWidth - 1, 70);
  renderer.drawImage(zhuye, 0 , 290, 480, 480);
  renderer.drawText(READER_FONT_ID, 20, 70, "文件管理", selectorIndex != 0);
  renderer.drawText(READER_FONT_ID, 20, 140, "文件传输", selectorIndex != 1);
  renderer.drawText(READER_FONT_ID, 20, 210, "设置", selectorIndex != 2);
  // 新增：在设置下方绘制“阅读”选项，y坐标=280（间隔70）
  renderer.drawText(READER_FONT_ID, 20, 280, "阅读", selectorIndex != 3);

  renderer.drawImage(beijing, 275 , 25, 40, 40);
  renderer.drawImage(beijing, 150 , 25, 40, 40);

  //下面的四个框
  renderer.drawRect(25, pageHeight - 40, 106, 40);
  renderer.drawText(UI_FONT_ID, 25 + (105 - renderer.getTextWidth(UI_FONT_ID, "Back")) / 2, pageHeight - 35, "Back");

  renderer.drawRect(130, pageHeight - 40, 106, 40);
  renderer.drawText(UI_FONT_ID, 130 + (105 - renderer.getTextWidth(UI_FONT_ID, "Confirm")) / 2, pageHeight - 35,
                    "Confirm");

  renderer.drawRect(245, pageHeight - 40, 106, 40);
  renderer.drawText(UI_FONT_ID, 245 + (105 - renderer.getTextWidth(UI_FONT_ID, "Left")) / 2, pageHeight - 35, "Left");

  renderer.drawRect(350, pageHeight - 40, 106, 40);
  renderer.drawText(UI_FONT_ID, 350 + (105 - renderer.getTextWidth(UI_FONT_ID, "Right")) / 2, pageHeight - 35, "Right");

  renderer.displayBuffer();
}
