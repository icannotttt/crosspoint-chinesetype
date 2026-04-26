#include "BluetoothSettingsActivity.h"

#include <GfxRenderer.h>

#include "CrossPointSettings.h"
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
    if (millis() - lastScanTime > 500) { // Small delay to see final results
      lastScanTime = 0;
      updateRequired = true;
    }
  }

  if (viewMode == ViewMode::MAIN_MENU) {
    handleMainMenuInput();
  } else {
    handleDeviceListInput();
  }
}

void BluetoothSettingsActivity::handleMainMenuInput() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    selectedIndex = (selectedIndex > 0) ? selectedIndex - 1 : 1;
    updateRequired = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    selectedIndex = (selectedIndex < 1) ? selectedIndex + 1 : 0;
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
    }
  }
}

void BluetoothSettingsActivity::handleDeviceListInput() {
  if (!btMgr) return;

  // 过滤掉名称为Unknown的设备
  std::vector<BluetoothDevice> filteredDevices;
  for (const auto& dev : btMgr->getDiscoveredDevices()) {
    if (dev.name != "Unknown" && dev.name != "mobike") { // 核心过滤逻辑
      filteredDevices.push_back(dev);
    }
  }
  const auto& devices = filteredDevices; // 用过滤后的列表
  const auto& connectedDevices = btMgr->getConnectedDevices();
  
  // Calculate menu items: devices + "Refresh" + "Disconnect" (if connected)
  int menuItems = devices.size() + 1; // +1 for Refresh
  if (!connectedDevices.empty()) {
    menuItems++; // +1 for Disconnect
  }
  int maxIndex = menuItems - 1;

  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    selectedIndex = (selectedIndex > 0) ? selectedIndex - 1 : maxIndex;
    updateRequired = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    selectedIndex = (selectedIndex < maxIndex) ? selectedIndex + 1 : 0;
    updateRequired = true;
  }
  
  // Left/Right for back/refresh
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    // Go back to main menu
    viewMode = ViewMode::MAIN_MENU;
    selectedIndex = 0;
    if (btMgr && btMgr->isScanning()) {
      btMgr->stopScan();
    }
    updateRequired = true;
    return;
  }
  
  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    // Quick rescan
    Serial.printf("BT Quick rescan...");
    lastError = "Scanning...";
    btMgr->startScan(10000);
    lastScanTime = millis();
    selectedIndex = 0;
    updateRequired = true;
    return;
  }
  
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    // Check if "Refresh" is selected
    if (selectedIndex == static_cast<int>(devices.size())) {
      Serial.printf("BT Refreshing scan...");
      lastError = "Scanning...";
      btMgr->startScan(10000);
      lastScanTime = millis();
      selectedIndex = 0;
      updateRequired = true;
      return;
    }
    
    // Check if "Disconnect" is selected
    if (!connectedDevices.empty() && selectedIndex == static_cast<int>(devices.size()) + 1) {
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
    
    // Otherwise, connect to selected device
    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(devices.size())) {
      const auto& device = devices[selectedIndex];
      
      Serial.printf("BT Connecting to %s (%s)", device.name.c_str(), device.address.c_str());
      lastError = "Connecting...";
      updateRequired = true;
      
      // try up to 3 times to avoid crashing if the peripheral is bad
      if (btMgr->connectToDeviceWithRetries(device.address, 3)) {
        lastError = std::string("Connected to ") + device.name;
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
  } else {
    renderDeviceList();
  }
}

void BluetoothSettingsActivity::renderMainMenu() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  // Header
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "蓝牙设置", true, EpdFontFamily::BOLD);

  // Status line
  std::string statusLine;
  if (btMgr) {
    if (btMgr->isEnabled()) {
      auto connDevices = btMgr->getConnectedDevices();
      char buf[64];
      snprintf(buf, sizeof(buf), "已启用 (%zu 个已连接设备)", connDevices.size());
      statusLine = buf;
    } else {
      statusLine = "已禁用";
    }
  } else {
    statusLine = "蓝牙错误";
  }
  renderer.drawText(SMALL_FONT_ID, 20, 45, statusLine.c_str());

  // Error message if any
  if (!lastError.empty()) {
    renderer.drawText(UI_10_FONT_ID, 20, 75, lastError.c_str());
  }

  // Menu items
  constexpr int startY = 110;
  constexpr int lineHeight = 40;
  const char* items[] = {
      btMgr && btMgr->isEnabled() ? "禁用蓝牙" : "启用蓝牙",
      "扫描设备"
  };

  for (int i = 0; i < 2; i++) {
    const int itemY = startY + i * lineHeight;
    
    // Draw selection indicator
    if (i == selectedIndex) {
      renderer.drawText(UI_10_FONT_ID, 5, itemY, ">");
    }
    
    renderer.drawText(UI_10_FONT_ID, 25, itemY, items[i]);
  }

  // Button hints
  const auto labels = mappedInput.mapLabels("返回", "打开", "左", "右");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void BluetoothSettingsActivity::renderDeviceList() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  if (!btMgr) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "蓝牙错误");
    renderer.displayBuffer();
    return;
  }

  // 过滤掉名称为Unknown的设备
  std::vector<BluetoothDevice> filteredDevices;
  for (const auto& dev : btMgr->getDiscoveredDevices()) {
    if (dev.name != "Unknown" && dev.name != "mobike") {
      filteredDevices.push_back(dev);
    }
  }
  const auto& devices = filteredDevices;
  const auto& connectedDevices = btMgr->getConnectedDevices();

  // Header
  std::string headerText = "蓝牙设备";
  if (btMgr->isScanning()) {
    headerText += " (扫描中...)";
  }
  renderer.drawCenteredText(UI_12_FONT_ID, 15, headerText.c_str(), true, EpdFontFamily::BOLD);

  // Device count
  char countStr[32];
  if (btMgr->isScanning()) {
    snprintf(countStr, sizeof(countStr), "扫描中 - %zu 个设备", devices.size());
  } else {
    snprintf(countStr, sizeof(countStr), "找到 %zu 个设备", devices.size());
  }
  renderer.drawText(SMALL_FONT_ID, 20, 45, countStr);

  // Device list
  constexpr int startY = 70;
  constexpr int lineHeight = 35;
  const int maxVisibleDevices = (pageHeight - startY - 60) / lineHeight;

  // Calculate scroll offset
  int scrollOffset = 0;
  if (selectedIndex >= maxVisibleDevices) {
    scrollOffset = selectedIndex - maxVisibleDevices + 1;
  }

  int displayIndex = 0;
  for (size_t i = scrollOffset; i < devices.size() && displayIndex < maxVisibleDevices; i++, displayIndex++) {
    const int deviceY = startY + displayIndex * lineHeight;
    const auto& device = devices[i];

    // Check if selected
    if (static_cast<int>(i) == selectedIndex) {
      renderer.drawText(UI_10_FONT_ID, 5, deviceY, ">");
    }

    // Device name and connection status
    bool connected = btMgr->isConnected(device.address);
    const char* connMark = connected ? "[*]" : "";
    const char* hidMark = device.isHID ? "[H]" : "";
    
    char deviceStr[64];
    snprintf(deviceStr, sizeof(deviceStr), "%s%s %s", connMark, hidMark, device.name.c_str());
    renderer.drawText(UI_10_FONT_ID, 25, deviceY, deviceStr);

    // Signal strength on next line
    std::string signalStr = getSignalStrengthIndicator(device.rssi);
    char rssiStr[32];
    snprintf(rssiStr, sizeof(rssiStr), "%s (%d dBm)", signalStr.c_str(), device.rssi);
    renderer.drawText(SMALL_FONT_ID, 35, deviceY + 15, rssiStr);
  }

  // Action buttons
  const int actionStartY = startY + maxVisibleDevices * lineHeight + 5;
  int actionIndex = static_cast<int>(devices.size());
  
  // Refresh button
  if (static_cast<int>(devices.size()) == selectedIndex) {
    renderer.drawText(UI_10_FONT_ID, 5, actionStartY, ">");
  }
  renderer.drawText(UI_10_FONT_ID, 25, actionStartY, "< 刷新扫描 >");
  
  // Disconnect button (if any connected)
  if (!connectedDevices.empty()) {
    int disconnectY = actionStartY + lineHeight;
    if (actionIndex + 1 == selectedIndex) {
      renderer.drawText(UI_10_FONT_ID, 5, disconnectY, ">");
    }
    renderer.drawText(UI_10_FONT_ID, 25, disconnectY, "< 断开连接 >");
  }

  // Button hints
  const auto labels = mappedInput.mapLabels("返回", "连接", "刷新", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

std::string BluetoothSettingsActivity::getSignalStrengthIndicator(const int32_t rssi) const {
  // Convert RSSI to signal bars representation (matching WiFi scanner style)
  if (rssi >= -50) {
    return "||||";  // Excellent
  }
  if (rssi >= -60) {
    return " |||";  // Good
  }
  if (rssi >= -70) {
    return "  ||";  // Fair
  }
  return "   |";  // Very weak
}