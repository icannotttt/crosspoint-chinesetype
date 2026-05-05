#include "BluetoothSettingsActivity.h"

#include <GfxRenderer.h>
#include <SDCardManager.h>

#include "CrossPointSettings.h"
#include "DeviceProfiles.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void BluetoothSettingsActivity::taskTrampoline(void* param) {
  auto* self = static_cast<BluetoothSettingsActivity*>(param);
  self->displayTaskLoop();
}

void BluetoothSettingsActivity::displayTaskLoop() {
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

void BluetoothSettingsActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();
  selectedIndex = 0;
  viewMode = ViewMode::MAIN_MENU;
  lastError = "";
  lastScanTime = 0;

  // Get BLE manager instance
  try {
    btMgr = &BluetoothHIDManager::getInstance();
    Serial.printf("BT BluetoothHIDManager ready");

    // Restore Bluetooth persistent state on entry
    if (SETTINGS.bluetoothEnabled && !btMgr->isEnabled()) {
      Serial.printf("BT Restoring Bluetooth from settings (enabled)");
      if (btMgr->enable()) {
        lastError = "Bluetooth restored";
      } else {
        lastError = "Failed to restore BT";
        SETTINGS.bluetoothEnabled = 0;
      }
    } else if (!SETTINGS.bluetoothEnabled && btMgr->isEnabled()) {
      Serial.printf("BT Disabling Bluetooth per settings (disabled)");
      btMgr->disable();
      lastError = "Bluetooth disabled per settings";
    }
  } catch (const std::exception& e) {
    Serial.printf("BT Failed to get BLE manager: %s", e.what());
    lastError = "BLE manager error";
    btMgr = nullptr;
  } catch (...) {
    Serial.printf("BT Unknown error getting BLE manager");
    lastError = "Unknown error";
    btMgr = nullptr;
  }

  updateRequired = true;
  xTaskCreate(&BluetoothSettingsActivity::taskTrampoline, "BluetoothSettingsActivity",
              4096,               // Stack size (larger for HTTP operations)
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void BluetoothSettingsActivity::onExit() {
  Activity::onExit();

  if (btMgr && btMgr->isKeyMappingActive()) {
    btMgr->endKeyMapping();
  }

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  if (renderingMutex) {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    if (displayTaskHandle) {
      vTaskDelete(displayTaskHandle);
      displayTaskHandle = nullptr;
    }
    xSemaphoreGive(renderingMutex);
    vSemaphoreDelete(renderingMutex);
    renderingMutex = nullptr;
  }

  // Stop any ongoing scan
  if (btMgr && btMgr->isScanning()) {
    btMgr->stopScan();
  }
}

void BluetoothSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (viewMode == ViewMode::DEVICE_LIST) {
      // Return to main menu
      viewMode = ViewMode::MAIN_MENU;
      selectedIndex = 0;
      if (btMgr && btMgr->isScanning()) {
        btMgr->stopScan();
      }
      updateRequired = true;
      return;
    } else {
      if (onComplete) onComplete();
      return;
    }
  }

  // Check if scan completed
  if (btMgr && viewMode == ViewMode::DEVICE_LIST && !btMgr->isScanning() && lastScanTime > 0) {
    if (millis() - lastScanTime > 500) {  // Small delay to see final results
      lastScanTime = 0;
      updateRequired = true;
    }
  }

  if (viewMode == ViewMode::MAIN_MENU) {
    handleMainMenuInput();
  } else if (viewMode == ViewMode::DEVICE_LIST) {
    handleDeviceListInput();
  } else {
    handleKeyMappingInput();
  }
}

void BluetoothSettingsActivity::beginKeyMappingCapture() {
  if (!btMgr) {
    return;
  }

  if (btMgr->isKeyMappingActive()) {
    btMgr->endKeyMapping();
  }

  btMgr->beginKeyMapping([this](uint8_t keycode, uint8_t reportByteIndex) {
    if (viewMode != ViewMode::KEY_MAPPING) {
      return;
    }

    // Only capture after explicit confirm, and ignore key-release or empty reports.
    if (!mappingCaptureArmed || keycode == 0x00) {
      return;
    }

    if (mappingStep == MappingStep::WAIT_PREV) {
      mappedPrevKey = keycode;
      mappedPrevByte = reportByteIndex;
      mappingStep = MappingStep::WAIT_NEXT;
      mappingCaptureArmed = false;
      lastError = "已记录上一页键，按确认开始下一页键采集";
      updateRequired = true;
      return;
    }

    if (mappingStep == MappingStep::WAIT_NEXT) {
      mappedNextKey = keycode;
      mappedNextByte = reportByteIndex;
      mappingStep = MappingStep::DONE;
      mappingCaptureArmed = false;

      uint8_t reportByteIndexToUse = (mappedPrevByte != 0xFF) ? mappedPrevByte : mappedNextByte;
      if (mappedNextByte != 0xFF && mappedPrevByte != mappedNextByte) {
        Serial.printf("BT Mapping warning: prev byte=%u next byte=%u, using prev byte", mappedPrevByte,
                      mappedNextByte);
      }

      const bool sideButtonsSwapped =
          static_cast<CrossPointSettings::SIDE_BUTTON_LAYOUT>(SETTINGS.sideButtonLayout) ==
          CrossPointSettings::SIDE_BUTTON_LAYOUT::NEXT_PREV;
      const uint8_t pageUpCodeToSave = sideButtonsSwapped ? mappedNextKey : mappedPrevKey;
      const uint8_t pageDownCodeToSave = sideButtonsSwapped ? mappedPrevKey : mappedNextKey;

      DeviceProfiles::setCustomProfile(pageUpCodeToSave, pageDownCodeToSave, reportByteIndexToUse);
      btMgr->saveState();
      lastError = "按键映射已保存，按返回退出";
      if (btMgr && btMgr->isKeyMappingActive()) {
        btMgr->endKeyMapping();
      }
      updateRequired = true;
    }
  });
}

void BluetoothSettingsActivity::handleMainMenuInput() {
  constexpr int menuCount = 5;

  if (mappedInput.wasPressed(MappedInputManager::Button::Up)||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    selectedIndex = (selectedIndex > 0) ? selectedIndex - 1 : (menuCount - 1);
    updateRequired = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    selectedIndex = (selectedIndex < menuCount - 1) ? selectedIndex + 1 : 0;
    updateRequired = true;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (!btMgr) {
      lastError = "BLE not available";
      Serial.printf("BT BLE manager not available");
      updateRequired = true;
      return;
    }

    if (selectedIndex == 0) {
      // Toggle Bluetooth
      try {
        if (btMgr->isEnabled()) {
          Serial.printf("BT Disabling Bluetooth...");
          if (btMgr->disable()) {
            lastError = "Bluetooth disabled";
            SETTINGS.bluetoothEnabled = 0;
            SETTINGS.saveToFile();
          } else {
            lastError = "Failed to disable";
          }
        } else {
          Serial.printf("BT Enabling Bluetooth...");
          if (btMgr->enable()) {
            lastError = "Bluetooth enabled";
            SETTINGS.bluetoothEnabled = 1;
            SETTINGS.saveToFile();
          } else {
            lastError = btMgr->lastError.empty() ? "Failed to enable" : btMgr->lastError;
          }
        }
      } catch (const std::exception& e) {
        lastError = std::string("Error: ") + e.what();
        Serial.printf("BT Toggle error: %s", e.what());
      } catch (...) {
        lastError = "Unknown toggle error";
        Serial.printf("BT Unknown error toggling Bluetooth");
      }
      updateRequired = true;
    } else if (selectedIndex == 1) {
      // Connect to the last successfully paired device
      try {
        if (!btMgr->isEnabled()) {
          Serial.printf("BT Enabling Bluetooth for auto-connect...");
          if (!btMgr->enable()) {
            lastError = btMgr->lastError.empty() ? "Failed to enable" : btMgr->lastError;
            updateRequired = true;
            return;
          }
          SETTINGS.bluetoothEnabled = 1;
          SETTINGS.saveToFile();
        }

        if (btMgr->isScanning()) {
          btMgr->stopScan();
        }

        std::string lastAddr;
        std::string lastName;
        if (!btMgr->loadLastConnectedDevice(lastAddr, lastName)) {
          lastError = "没有上次设备";
          updateRequired = true;
          return;
        }

        Serial.printf("BT Auto-connecting to last device %s (%s)", lastName.c_str(), lastAddr.c_str());
        lastError = "正在连接上次设备...";
        updateRequired = true;

        if (btMgr->connectToDeviceWithRetries(lastAddr, 3)) {
          lastError = lastName.empty() ? "已连接上次设备" : (std::string("已连接到 ") + lastName + "，请选择开始映射");
        } else {
          lastError = btMgr->lastError.empty() ? "连接失败" : btMgr->lastError;
        }
      } catch (const std::exception& e) {
        lastError = std::string("Error: ") + e.what();
        Serial.printf("BT Auto-connect error: %s", e.what());
      } catch (...) {
        lastError = "Unknown auto-connect error";
        Serial.printf("BT Unknown error auto-connecting last device");
      }
      updateRequired = true;
    } else if (selectedIndex == 2) {
      // Start scan and switch to device list
      if (btMgr->isEnabled()) {
        btMgr->startScan(10000);
        lastScanTime = millis();
        viewMode = ViewMode::DEVICE_LIST;
        selectedIndex = 0;
        lastError = "";
      } else {
        lastError = "Enable BT first";
      }
      updateRequired = true;
    } else if (selectedIndex == 3) {
      static constexpr const char* kBluetoothStatePath = "/.crosspoint/bluetooth.bin";
      if (SdMan.exists(kBluetoothStatePath)) {
        if (SdMan.remove(kBluetoothStatePath)) {
          lastError = "已删除蓝牙配置";
          Serial.printf("BT Deleted state file: %s", kBluetoothStatePath);
        } else {
          lastError = "删除失败";
          Serial.printf("BT Failed to delete state file: %s", kBluetoothStatePath);
        }
      } else {
        lastError = "无蓝牙配置文件";
        Serial.printf("BT State file not found: %s", kBluetoothStatePath);
      }
      updateRequired = true;
    } else if (selectedIndex == 4) {
      if (!btMgr->isEnabled()) {
        lastError = "请先启用蓝牙";
        updateRequired = true;
        return;
      }

      const auto& connectedDevices = btMgr->getConnectedDevices();
      if (connectedDevices.empty()) {
        lastError = "请先连接蓝牙设备";
        updateRequired = true;
        return;
      }

      mappedPrevKey = 0x00;
      mappedNextKey = 0x00;
      mappedPrevByte = 0xFF;
      mappedNextByte = 0xFF;
      mappingCaptureArmed = false;
      mappingStep = MappingStep::WAIT_START_CONFIRM;
      viewMode = ViewMode::KEY_MAPPING;
      lastError = "";
      selectedIndex = 0;
      updateRequired = true;
    }
  }
}

void BluetoothSettingsActivity::handleDeviceListInput() {
  if (!btMgr) return;

  // 过滤掉名称为Unknown的设备
  std::vector<BluetoothDevice> filteredDevices;
  for (const auto& dev : btMgr->getDiscoveredDevices()) {
    if (dev.name != "Unknown" && dev.name != "mobike") {  // 核心过滤逻辑
      filteredDevices.push_back(dev);
    }
  }
  const auto& devices = filteredDevices;  // 用过滤后的列表
  const auto& connectedDevices = btMgr->getConnectedDevices();

  if (devices.empty() && connectedDevices.empty()) {
    selectedIndex = 0;
    lastError = "暂无蓝牙设备";
    updateRequired = true;
    return;
  }

  // Calculate menu items: devices + connected actions (if connected)
  int menuItems = static_cast<int>(devices.size());
  if (!connectedDevices.empty()) {
    menuItems += 2;  // +1 for Disconnect, +1 for Start key mapping
  }
  if (menuItems <= 0) {
    updateRequired = true;
    return;
  }
  const int maxIndex = menuItems - 1;

  const int disconnectIndex = connectedDevices.empty() ? -1 : static_cast<int>(devices.size());
  const int keyMappingIndex = connectedDevices.empty() ? -1 : static_cast<int>(devices.size()) + 1;

  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    selectedIndex = (selectedIndex > 0) ? selectedIndex - 1 : maxIndex;
    updateRequired = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    selectedIndex = (selectedIndex < maxIndex) ? selectedIndex + 1 : 0;
    updateRequired = true;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    // Check if "Disconnect" is selected
    if (disconnectIndex >= 0 && selectedIndex == disconnectIndex) {
      Serial.printf("BT Disconnecting from all devices...");
      // Make a copy of addresses to avoid iterator invalidation
      std::vector<std::string> deviceAddresses = connectedDevices;
      for (const auto& addr : deviceAddresses) {
        Serial.printf("BT Disconnecting from %s", addr.c_str());
        btMgr->disconnectFromDevice(addr);
      }
      lastError = "Disconnected";
      selectedIndex = 0;
      updateRequired = true;
      return;
    }

    // Check if "Start key mapping" is selected
    if (keyMappingIndex >= 0 && selectedIndex == keyMappingIndex) {
  const auto labels = mappedInput.mapLabels("返回", "连接", "上", "下");
      mappedNextKey = 0x00;
      mappedPrevByte = 0xFF;
      mappedNextByte = 0xFF;
      mappingCaptureArmed = false;
      mappingStep = MappingStep::WAIT_START_CONFIRM;
      viewMode = ViewMode::KEY_MAPPING;
      lastError = "";
      selectedIndex = 0;
      updateRequired = true;
      return;
    }

    // Otherwise, connect to selected device
    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(devices.size())) {
      const auto& device = devices[selectedIndex];

      Serial.printf("BT Connecting to %s (%s)", device.name.c_str(), device.address.c_str());
      lastError = "Connecting...";
      updateRequired = true;

      // try up to 3 times to avoid crashing if the peripheral is bad
      if (btMgr->connectToDeviceWithRetries(device.address, 3)) {
        lastError = std::string("Connected to ") + device.name + ", 请选择开始映射";
        Serial.printf("BT Successfully connected to %s", device.name.c_str());
      } else {
        lastError = btMgr->lastError.empty() ? "Connection failed" : btMgr->lastError;
        Serial.printf("BT Failed to connect after retries: %s", lastError.c_str());
      }
      updateRequired = true;
    }
  }
}

void BluetoothSettingsActivity::render() {
  if (viewMode == ViewMode::MAIN_MENU) {
    renderMainMenu();
  } else if (viewMode == ViewMode::DEVICE_LIST) {
    renderDeviceList();
  } else {
    renderKeyMapping();
  }
}

void BluetoothSettingsActivity::handleKeyMappingInput() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (btMgr && btMgr->isKeyMappingActive()) {
      btMgr->endKeyMapping();
    }
    mappingCaptureArmed = false;
    viewMode = ViewMode::DEVICE_LIST;
    selectedIndex = 0;
    lastError = (mappingStep == MappingStep::DONE) ? "已退出按键映射" : "已取消按键映射";
    updateRequired = true;
    return;
  }

  if (mappingStep == MappingStep::WAIT_START_CONFIRM &&
      mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    mappingStep = MappingStep::WAIT_PREV;
    mappingCaptureArmed = true;
    lastError = "开始采集上一页键，请按上一页键";
    beginKeyMappingCapture();
    updateRequired = true;
    return;
  }

  if (mappingStep == MappingStep::WAIT_NEXT &&
      mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    mappingCaptureArmed = true;
    lastError = "开始采集下一页键，请按下一页键";
    updateRequired = true;
  }
}

void BluetoothSettingsActivity::renderKeyMapping() {
  const auto pageHeight = renderer.getScreenHeight();
  renderer.clearScreen();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, "蓝牙按键映射", true, EpdFontFamily::BOLD);

  const char* prompt = "按确认开始按键映射";
  if (mappingStep == MappingStep::WAIT_PREV) {
    prompt = mappingCaptureArmed ? "请按 上一页 键" : "按确认开始采集上一页键";
  } else if (mappingStep == MappingStep::WAIT_NEXT) {
    prompt = mappingCaptureArmed ? "请按 下一页 键" : "按确认开始采集下一页键";
  } else if (mappingStep == MappingStep::DONE) {
    prompt = "映射完成，按返回退出";
  }

  renderer.drawCenteredText(UI_12_FONT_ID, 60, prompt, true, EpdFontFamily::BOLD);

  char prevStr[64];
  if (mappedPrevByte != 0xFF) {
    snprintf(prevStr, sizeof(prevStr), "上一页: 0x%02X (byte %u)", mappedPrevKey, mappedPrevByte);
  } else {
    snprintf(prevStr, sizeof(prevStr), "上一页: 等待按键...");
  }
  renderer.drawCenteredText(UI_10_FONT_ID, 100, prevStr);

  char nextStr[64];
  if (mappedNextByte != 0xFF) {
    snprintf(nextStr, sizeof(nextStr), "下一页: 0x%02X (byte %u)", mappedNextKey, mappedNextByte);
  } else {
    snprintf(nextStr, sizeof(nextStr), "下一页: 等待按键...");
  }
  renderer.drawCenteredText(UI_10_FONT_ID, 130, nextStr);

  if (!lastError.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, 160, lastError.c_str());
  }

  const auto labels = (mappingStep == MappingStep::WAIT_START_CONFIRM)
                          ? mappedInput.mapLabels("取消", "开始", "", "")
                          : mappedInput.mapLabels("返回", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void BluetoothSettingsActivity::renderMainMenu() {
  const auto pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, "蓝牙设置", true, EpdFontFamily::BOLD);

  std::string statusLine;
  if (btMgr) {
    if (btMgr->isEnabled()) {
      const auto connectedDevices = btMgr->getConnectedDevices();
      char buf[64];
      snprintf(buf, sizeof(buf), "已启用 (%zu 个已连接设备)", connectedDevices.size());
      statusLine = buf;
    } else {
      statusLine = "已禁用";
    }
  } else {
    statusLine = "蓝牙错误";
  }
  renderer.drawText(SMALL_FONT_ID, 20, 45, statusLine.c_str());

  if (!lastError.empty()) {
    renderer.drawText(UI_10_FONT_ID, 20, 75, lastError.c_str());
  }

  constexpr int startY = 110;
  constexpr int lineHeight = 40;
  const char* items[] = {
      btMgr && btMgr->isEnabled() ? "禁用蓝牙" : "启用蓝牙",
      "使用上次设备直接连接",
      "扫描设备",
      "删除蓝牙设备",
      "开始按键映射",
  };

  for (int i = 0; i < 5; ++i) {
    const int itemY = startY + i * lineHeight;
    if (i == selectedIndex) {
      renderer.drawText(UI_10_FONT_ID, 5, itemY, ">");
    }
    renderer.drawText(UI_10_FONT_ID, 25, itemY, items[i]);
  }

  const auto labels = mappedInput.mapLabels("返回", "打开", "上", "下");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void BluetoothSettingsActivity::renderDeviceList() {
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  if (!btMgr) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "蓝牙错误");
    renderer.displayBuffer();
    return;
  }

  std::vector<BluetoothDevice> filteredDevices;
  for (const auto& dev : btMgr->getDiscoveredDevices()) {
    if (dev.name != "Unknown" && dev.name != "mobike") {
      filteredDevices.push_back(dev);
    }
  }

  const auto& devices = filteredDevices;
  const auto& connectedDevices = btMgr->getConnectedDevices();

  if (devices.empty() && connectedDevices.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, "暂无蓝牙设备");
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, "请返回后重新扫描");
    const auto labels = mappedInput.mapLabels("返回", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  std::string headerText = "蓝牙设备";
  if (btMgr->isScanning()) {
    headerText += " (扫描中...)";
  }
  renderer.drawCenteredText(UI_12_FONT_ID, 15, headerText.c_str(), true, EpdFontFamily::BOLD);

  char countStr[32];
  if (btMgr->isScanning()) {
    snprintf(countStr, sizeof(countStr), "扫描中 - %zu 个设备", devices.size());
  } else {
    snprintf(countStr, sizeof(countStr), "找到 %zu 个设备", devices.size());
  }
  renderer.drawText(SMALL_FONT_ID, 20, 45, countStr);

  constexpr int startY = 70;
  constexpr int lineHeight = 35;
  const int maxVisibleDevices = (pageHeight - startY - 60) / lineHeight;

  int scrollOffset = 0;
  if (selectedIndex >= maxVisibleDevices) {
    scrollOffset = selectedIndex - maxVisibleDevices + 1;
  }

  int displayIndex = 0;
  for (size_t i = scrollOffset; i < devices.size() && displayIndex < maxVisibleDevices; ++i, ++displayIndex) {
    const int deviceY = startY + displayIndex * lineHeight;
    const auto& device = devices[i];

    if (static_cast<int>(i) == selectedIndex) {
      renderer.drawText(UI_10_FONT_ID, 5, deviceY, ">");
    }

    const bool connected = btMgr->isConnected(device.address);
    const char* connMark = connected ? "[*]" : "";
    const char* hidMark = device.isHID ? "[H]" : "";

    char deviceStr[64];
    snprintf(deviceStr, sizeof(deviceStr), "%s%s %s", connMark, hidMark, device.name.c_str());
    renderer.drawText(UI_10_FONT_ID, 25, deviceY, deviceStr);

    std::string signalStr = getSignalStrengthIndicator(device.rssi);
    char rssiStr[32];
    snprintf(rssiStr, sizeof(rssiStr), "%s (%d dBm)", signalStr.c_str(), device.rssi);
    renderer.drawText(SMALL_FONT_ID, 35, deviceY + 15, rssiStr);
  }

  const int actionStartY = startY + maxVisibleDevices * lineHeight + 5;
  const int actionIndex = static_cast<int>(devices.size());

  if (!connectedDevices.empty()) {
    const int disconnectY = actionStartY;
    if (actionIndex + 1 == selectedIndex) {
      renderer.drawText(UI_10_FONT_ID, 5, disconnectY, ">");
    }
    renderer.drawText(UI_10_FONT_ID, 25, disconnectY, "< 断开连接 >");

    const int keyMappingY = disconnectY + lineHeight;
    if (actionIndex + 2 == selectedIndex) {
      renderer.drawText(UI_10_FONT_ID, 5, keyMappingY, ">");
    }
    renderer.drawText(UI_10_FONT_ID, 25, keyMappingY, "< 开始按键映射 >");
  }

  const auto labels = mappedInput.mapLabels("返回", "连接", "上", "下");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

std::string BluetoothSettingsActivity::getSignalStrengthIndicator(const int32_t rssi) const {
  if (rssi >= -50) {
    return "||||";
  }
  if (rssi >= -60) {
    return " |||";
  }
  if (rssi >= -70) {
    return "  ||";
  }
  return "   |";
}
