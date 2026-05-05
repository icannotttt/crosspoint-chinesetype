#include "OtaUpdateActivity.h"

#include <cstdio>

#include <GfxRenderer.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/OtaUpdater.h"

void OtaUpdateActivity::taskTrampoline(void* param) {
  auto* self = static_cast<OtaUpdateActivity*>(param);
  self->displayTaskLoop();
}

void OtaUpdateActivity::onWifiSelectionComplete(const bool success) {
  exitActivity();

  if (!success) {
    Serial.printf("[%lu] [OTA] WiFi connection failed, exiting\n", millis());
    goBack();
    return;
  }

  Serial.printf("[%lu] [OTA] WiFi connected, checking for update\n", millis());

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  state = CHECKING_FOR_UPDATE;
  xSemaphoreGive(renderingMutex);
  updateRequired = true;
  vTaskDelay(10 / portTICK_PERIOD_MS);
  const auto res = updater.checkForUpdate();
  if (res != OtaUpdater::OK) {
    Serial.printf("[%lu] [OTA] Update check failed: %d\n", millis(), res);
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    state = FAILED;
    xSemaphoreGive(renderingMutex);
    updateRequired = true;
    return;
  }

  // if (!updater.isUpdateNewer()) {
  //   Serial.printf("[%lu] [OTA] No new update available\n", millis());
  //   xSemaphoreTake(renderingMutex, portMAX_DELAY);
  //   state = NO_UPDATE;
  //   xSemaphoreGive(renderingMutex);
  //   updateRequired = true;
  //   return;
  // }

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  state = WAITING_CONFIRMATION;
  xSemaphoreGive(renderingMutex);
  updateRequired = true;
}

void OtaUpdateActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  xTaskCreate(&OtaUpdateActivity::taskTrampoline, "OtaUpdateActivityTask",
              2048,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );

  // Turn on WiFi immediately
  Serial.printf("[%lu] [OTA] Turning on WiFi...\n", millis());
  WiFi.mode(WIFI_STA);

  // Launch WiFi selection subactivity
  Serial.printf("[%lu] [OTA] Launching WifiSelectionActivity...\n", millis());
  enterNewActivity(new WifiSelectionActivity(renderer, mappedInput,
                                             [this](const bool connected) { onWifiSelectionComplete(connected); }));
}

void OtaUpdateActivity::onExit() {
  ActivityWithSubactivity::onExit();

  // Turn off wifi
  WiFi.disconnect(false);  // false = don't erase credentials, send disconnect frame
  delay(100);              // Allow disconnect frame to be sent
  WiFi.mode(WIFI_OFF);
  delay(100);  // Allow WiFi hardware to fully power down

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void OtaUpdateActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired || updater.getRender()) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void OtaUpdateActivity::render() {
  if (subActivity) {
    // Subactivity handles its own rendering
    return;
  }

  if (state == UPDATE_IN_PROGRESS) {
    const unsigned long now = millis();
    const size_t downloaded = updater.getProcessedSize();

    if (lastSpeedSampleMs == 0) {
      lastSpeedSampleMs = now;
      lastSpeedSampleBytes = downloaded;
      lastSpeedKBps = 0.0f;
    } else if (now > lastSpeedSampleMs && (now - lastSpeedSampleMs) >= 1000) {
      const size_t deltaBytes = (downloaded >= lastSpeedSampleBytes) ? (downloaded - lastSpeedSampleBytes) : 0;
      const float elapsedSec = static_cast<float>(now - lastSpeedSampleMs) / 1000.0f;
      lastSpeedKBps = (elapsedSec > 0.0f) ? (static_cast<float>(deltaBytes) / 1024.0f / elapsedSec) : 0.0f;
      lastSpeedSampleMs = now;
      lastSpeedSampleBytes = downloaded;
    }

    // Refresh roughly every 400ms, or immediately when size changes.
    if (downloaded == lastShownDownloadedBytes && (now - lastDownloadUiUpdateMs) < 400) {
      return;
    }
    lastShownDownloadedBytes = downloaded;
    lastDownloadUiUpdateMs = now;
  }

  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Update", true, EpdFontFamily::BOLD);

  if (state == CHECKING_FOR_UPDATE) {
    renderer.drawCenteredText(UI_10_FONT_ID, 300, "Checking for update...", true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == WAITING_CONFIRMATION) {
    renderer.drawCenteredText(UI_10_FONT_ID, 200, "New update available!", true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, 20, 250, "Current Version: " CROSSPOINT_VERSION);
    renderer.drawText(UI_10_FONT_ID, 20, 270, ("New Version: " + updater.getLatestVersion()).c_str());

    const auto labels = mappedInput.mapLabels("取消", "更新", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == UPDATE_IN_PROGRESS) {
    auto formatSize = [](size_t bytes) -> std::string {
      const float kb = 1024.0f;
      const float mb = 1024.0f * 1024.0f;
      char buf[32] = {0};
      if (bytes >= mb) {
        snprintf(buf, sizeof(buf), "%.2f MB", static_cast<float>(bytes) / mb);
      } else {
        snprintf(buf, sizeof(buf), "%.1f KB", static_cast<float>(bytes) / kb);
      }
      return std::string(buf);
    };

    const size_t downloaded = updater.getProcessedSize();
    const size_t total = updater.getTotalSize();

    renderer.drawCenteredText(UI_10_FONT_ID, 310, "Updating...", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, 360, "Downloaded", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, 400, formatSize(downloaded).c_str());
    if (total > 0) {
      renderer.drawCenteredText(UI_10_FONT_ID, 440, ("Total: " + formatSize(total)).c_str());
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, 440, "Total size: unknown");
    }
    char speedBuf[32] = {0};
    snprintf(speedBuf, sizeof(speedBuf), "Speed: %.1f KB/s", lastSpeedKBps);
    renderer.drawCenteredText(UI_10_FONT_ID, 460, speedBuf);
    renderer.displayBuffer();
    return;
  }

  // if (state == NO_UPDATE) {
  //   renderer.drawCenteredText(UI_10_FONT_ID, 300, "No update available", true, EpdFontFamily::BOLD);
  //   renderer.displayBuffer();
  //   return;
  // }

  if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, 300, "Update failed", true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == FINISHED) {
    renderer.drawCenteredText(UI_10_FONT_ID, 300, "Update complete", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, 350, "Press and hold power button to turn back on");
    renderer.displayBuffer();
    state = SHUTTING_DOWN;
    return;
  }
}

void OtaUpdateActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (state == WAITING_CONFIRMATION) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      Serial.printf("[%lu] [OTA] New update available, starting download...\n", millis());
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      state = UPDATE_IN_PROGRESS;
      lastDownloadUiUpdateMs = 0;
      lastShownDownloadedBytes = 0;
      lastSpeedSampleMs = 0;
      lastSpeedSampleBytes = 0;
      lastSpeedKBps = 0.0f;
      xSemaphoreGive(renderingMutex);
      updateRequired = true;
      vTaskDelay(10 / portTICK_PERIOD_MS);
      const auto res = updater.installUpdate();
      Serial.printf("[%lu] [OTA] 进入到这里，%d.\n", millis(), res);
          // 判断更新是否成功，成功则强制重启
      if (res == OtaUpdater::OK) {
        Serial.printf("[%lu] [OTA] 更新成功，即将重启...\n", millis());
        vTaskDelay(3000 / portTICK_PERIOD_MS); // 可选延迟3秒，可删
        ESP.restart(); // 核心：强制重启ESP32加载新固件
      }

      if (res != OtaUpdater::OK) {
        Serial.printf("[%lu] [OTA] Update failed: %d\n", millis(), res);
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        state = FAILED;
        xSemaphoreGive(renderingMutex);
        updateRequired = true;
        return;
      }

      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      state = FINISHED;
      xSemaphoreGive(renderingMutex);
      updateRequired = true;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }

    return;
  }

  if (state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }

  if (state == NO_UPDATE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }

  if (state == SHUTTING_DOWN) {
    ESP.restart();
  }
}
