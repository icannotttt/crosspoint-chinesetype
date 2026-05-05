#include "BluetoothHIDManager.h"
#include <NimBLEDevice.h>
#include <HalGPIO.h>
#include <WiFi.h>
#include <SDCardManager.h>
#include <Serialization.h>
#include <utility>
#include "../../src/CrossPointSettings.h"

// HID Service and characteristic UUIDs
static const char* HID_SERVICE_UUID = "1812";
static const char* HID_REPORT_UUID = "2A4D";
static const char* HID_INFO_UUID = "2A4A";

// Global static for singleton
static BluetoothHIDManager* g_instance = nullptr;

// Scan callbacks for NimBLE 2.x - keep as static to ensure it stays alive
class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
    Serial.printf("BT onResult callback triggered!\n");
    if (g_instance) {
      // onScanResult expects non-const pointer, need to cast
      g_instance->onScanResult(const_cast<NimBLEAdvertisedDevice*>(advertisedDevice));
    } else {
      Serial.printf("BT onResult called but g_instance is NULL!");
    }
  }
  
  void onScanEnd(const NimBLEScanResults& results, int reason) override {
    Serial.printf("BT onScanEnd callback: %d devices, reason: %d", results.getCount(), reason);
  }
};

// Static instance to keep callbacks alive during scan
static ScanCallbacks scanCallbacks;

// Client connection callbacks
class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* pClient) override {
    Serial.printf("BT Client connected: %s", pClient->getPeerAddress().toString().c_str());
  }
  
  void onDisconnect(NimBLEClient* pClient, int reason) override {
    Serial.printf("BT Client disconnected: %s (reason: %d)", pClient->getPeerAddress().toString().c_str(), reason);
    // Could trigger auto-reconnect here
  }
};

BluetoothHIDManager& BluetoothHIDManager::getInstance() {
  if (!g_instance) {
    g_instance = new BluetoothHIDManager();
    Serial.printf("BT BluetoothHIDManager instance created");
  }
  return *g_instance;
}

BluetoothHIDManager::BluetoothHIDManager() {
  Serial.printf("BT BluetoothHIDManager constructor");
}

BluetoothHIDManager::~BluetoothHIDManager() {
  cleanup();
}

void BluetoothHIDManager::cleanup() {
  if (_enabled) {
    disable();
  }
}

bool BluetoothHIDManager::enable() {
  if (_enabled) {
    Serial.printf("BT Already enabled");
    return true;
  }
  
  Serial.printf("BT Enabling Bluetooth...");
  
  // CRITICAL: Disable WiFi when enabling Bluetooth
  // ESP32-C3 cannot have both WiFi and BLE enabled simultaneously
  if (WiFi.getMode() != WIFI_OFF) {
    Serial.printf("BT Disabling WiFi to enable Bluetooth (mutual exclusion)");
    WiFi.disconnect(true);  // true = turn off WiFi radio
    WiFi.mode(WIFI_OFF);
    delay(100);  // Brief delay to ensure WiFi is fully powered down
  }
  
  try {
    // Initialize NimBLE stack
    NimBLEDevice::init("CrossPoint");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9); // +9dBm
    // 绑定(Bonding) / 加密(Encryption) / 认证(Authentication)
    NimBLEDevice::setSecurityAuth(true, true, true);
    
    _enabled = true;
    lastError = "";
    
    Serial.printf("BT Bluetooth enabled successfully");
    loadState();
    
    // NOTE: background auto‑connect was removed in order to conserve memory
    // and avoid multiple concurrent connection attempts.  callers such as
    // main.cpp or the settings UI should invoke connectToDeviceWithRetries()
    // explicitly when appropriate.
    
    return true;
  } catch (const std::exception& e) {
    Serial.printf("BT Failed to enable Bluetooth: %s", e.what());
    lastError = std::string("Init failed: ") + e.what();
    _enabled = false;
    return false;
  } catch (...) {
    Serial.printf("BT Failed to enable Bluetooth: unknown error");
    lastError = "Init failed: unknown error";
    _enabled = false;
    return false;
  }
}

bool BluetoothHIDManager::disable() {
  if (!_enabled) {
    Serial.printf("BT Already disabled");
    return true;
  }
  
  Serial.printf("BT Disabling Bluetooth...");
  
  if (_scanning) {
    stopScan();
  }
  
  // Disconnect all devices
  while (!_connectedDevices.empty()) {
    disconnectFromDevice(_connectedDevices[0].address);
  }
  
  // Deinitialize NimBLE stack
  NimBLEDevice::deinit(true);
  
  _enabled = false;
  lastError = "";
  
  Serial.printf("BT Bluetooth disabled");
  return true;
}

void BluetoothHIDManager::startScan(uint32_t durationMs) {
  if (!_enabled || _scanning) {
    Serial.printf("BT Cannot scan: enabled=%d scanning=%d", _enabled, _scanning);
    return;
  }
  
  Serial.printf("BT Starting BLE scan for %lu ms", durationMs);
  _scanning = true;
  _discoveredDevices.clear();
  
  try {
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (!pScan) {
      Serial.printf("BT Failed to get scan object");
      _scanning = false;
      return;
    }
    
    Serial.printf("BT Setting up scan callbacks...");
    // Use static callbacks object to ensure it stays alive
    pScan->setScanCallbacks(&scanCallbacks, false);
    pScan->setActiveScan(true);
    pScan->setInterval(100);
    pScan->setWindow(99);
    
    Serial.printf("BT Starting continuous scan (duration: 0 = continuous)...");
    // In NimBLE 2.x, duration=0 means scan continuously until stop() is called
    // Parameter 1: 0 = continuous scan
    // Parameter 2: isContinue (false = clear old results)
    bool started = pScan->start(0, false);
    
    if (!started) {
      Serial.printf("BT Failed to start scan!");
      _scanning = false;
      return;
    }
    
    Serial.printf("BT Scan started, waiting %lu ms...", durationMs);
    // Wait for the specified duration
    delay(durationMs);
    
    Serial.printf("BT Stopping scan after %lu ms...", durationMs);
    // Stop the scan
    pScan->stop();
    
    _scanning = false;
    Serial.printf("BT Scan complete, found %d devices", _discoveredDevices.size());
  } catch (const std::exception& e) {
    Serial.printf("BT Scan failed: %s", e.what());
    _scanning = false;
    lastError = std::string("Scan failed: ") + e.what();
  }
}


void BluetoothHIDManager::stopScan() {
  if (!_scanning) return;
  
  Serial.printf("BT Stopping scan");
  
  try {
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan) {
      pScan->stop();
    }
  } catch (...) {
    Serial.printf("BT Error stopping scan");
  }
  
  _scanning = false;
}

void BluetoothHIDManager::onScanResult(NimBLEAdvertisedDevice* advertisedDevice) {
  if (!advertisedDevice) return;
  
  std::string address = advertisedDevice->getAddress().toString();
  std::string name = advertisedDevice->getName();
  int rssi = advertisedDevice->getRSSI();
  
  // Check if device advertises HID service
  bool isHID = advertisedDevice->isAdvertisingService(NimBLEUUID(HID_SERVICE_UUID));
  
  // Check if we already have this device
  for (auto& dev : _discoveredDevices) {
    if (dev.address == address) {
      dev.rssi = rssi; // Update RSSI
      if (isHID) dev.isHID = true;
      dev.addrType = advertisedDevice->getAddress().getType();
      return;
    }
  }
  
  // Add new device
  BluetoothDevice device;
  device.address = address;
  device.name = name.empty() ? "Unknown" : name;
  device.rssi = rssi;
  device.isHID = isHID;
  device.addrType = advertisedDevice->getAddress().getType();
  
  _discoveredDevices.push_back(device);

    const std::string prefix = (address.size() >= 8) ? address.substr(0, 8) : address;
    Serial.printf("BT Scan device: %s (%s) prefix=%s RSSI:%d HID:%d",
      device.name.c_str(), device.address.c_str(), prefix.c_str(), rssi, isHID);
  
  Serial.printf("BT Found device: %s (%s) RSSI:%d HID:%d", 
          device.name.c_str(), device.address.c_str(), rssi, isHID);
}



bool BluetoothHIDManager::connectToDevice(const std::string& address) {
  if (!_enabled) {
    Serial.printf("BT Cannot connect: Bluetooth not enabled");
    lastError = "Bluetooth not enabled";
    return false;
  }
  if (address.empty()) {
    Serial.printf("BT Invalid address (empty) passed to connectToDevice");
    lastError = "Invalid address";
    return false;
  }
  
  // Check if already connected
  if (isConnected(address)) {
    Serial.printf("BT Already connected to %s", address.c_str());
    return true;
  }
  
  // 保留你要求的拦截逻辑：必须在扫描列表中
  bool seen = false;
  for (const auto& dev : _discoveredDevices) {
    if (dev.address == address) {
      seen = true;
      if (!dev.isHID) {
        Serial.printf("BT Device %s not advertising HID, aborting connect", address.c_str());
        lastError = "Not HID device";
        return false;
      }
      break;
    }
  }
  // 保留拦截：不在扫描结果里 → 直接返回失败
  if (!seen) {
    Serial.printf("BT Device %s not in scan results (skipping requirement)", address.c_str());
    return false;
  }
  
  Serial.printf("BT Connecting to device %s", address.c_str());
  
  NimBLEClient* pClient = nullptr;
  try {
    pClient = NimBLEDevice::createClient();
    if (!pClient) {
      lastError = "Failed to create client";
      Serial.printf("BT Failed to create BLE client");
      return false;
    }
    pClient->setSelfDelete(false, false);
    
    static ClientCallbacks clientCallbacks;
    pClient->setClientCallbacks(&clientCallbacks);

    // Try both address types for compatibility:
    // 1) use scanned type if available (otherwise public)
    // 2) fallback to the other type if first attempt fails
    uint8_t primaryType = BLE_ADDR_PUBLIC;
    for (const auto& dev : _discoveredDevices) {
      if (dev.address == address) {
        primaryType = dev.addrType;
        break;
      }
    }

    uint8_t fallbackType = (primaryType == BLE_ADDR_RANDOM) ? BLE_ADDR_PUBLIC : BLE_ADDR_RANDOM;

    auto tryConnectWithType = [&](uint8_t type) -> bool {
      NimBLEAddress bleAddress(address, type);
      Serial.printf("BT Trying connect to %s with addr type: %s",
                    address.c_str(),
                    (type == BLE_ADDR_RANDOM) ? "RANDOM" : "PUBLIC");
      return pClient->connect(bleAddress);
    };

    bool connected = tryConnectWithType(primaryType);
    if (!connected) {
      Serial.printf("BT First address type failed, trying fallback type...");
      // If scan type wasn't known we still try the opposite type for robustness.
      connected = tryConnectWithType(fallbackType);
    }

    if (!connected) {
      lastError = "Connection failed";
      Serial.printf("BT Failed to connect to %s with both address types", address.c_str());
      try {
        NimBLEDevice::deleteClient(pClient);
      } catch (...) {
        Serial.printf("BT Warning: failed to delete client after connection failure");
      }
      return false;
    }
    
    Serial.printf("BT Connected, discovering services...");
    
    NimBLERemoteService* pService = pClient->getService(HID_SERVICE_UUID);
    if (!pService) {
      lastError = "HID service not found";
      Serial.printf("BT Device %s doesn't have HID service", address.c_str());
      pClient->disconnect();
      try {
        NimBLEDevice::deleteClient(pClient);
      } catch (...) {
        Serial.printf("BT Warning: failed to delete client after HID service failure");
      }
      return false;
    }
    
    Serial.printf("BT Found HID service, enumerating report characteristics...");
    
    auto pCharacteristics = pService->getCharacteristics(true);
    NimBLERemoteCharacteristic* pReportChar = nullptr;
    
    int reportCount = 0;
    std::vector<NimBLERemoteCharacteristic*> reportChars;
    
    for (auto it = pCharacteristics.begin(); it != pCharacteristics.end(); ++it) {
      auto* pChar = *it;
      Serial.printf("BT Characteristic UUID: %s, canRead:%d canWrite:%d canNotify:%d canIndicate:%d",
              pChar->getUUID().toString().c_str(),
              pChar->canRead(), pChar->canWrite(), pChar->canNotify(), pChar->canIndicate());
      
      if (pChar->getUUID().equals(NimBLEUUID(HID_REPORT_UUID))) {
        reportCount++;
        Serial.printf("BT Found Report char #%d, notify:%d indicate:%d UUID:%s", 
                reportCount, pChar->canNotify(), pChar->canIndicate(),
                pChar->getUUID().toString().c_str());
        
        if (pChar->canNotify() || pChar->canIndicate()) {
          reportChars.push_back(pChar);
          Serial.printf("BT Added Report char #%d for subscription", reportCount);
        }
      }
    }
    
    if (reportChars.empty()) {
      lastError = "No input report characteristic found";
      Serial.printf("BT No Report characteristic with notify/indicate found");
      pClient->disconnect();
      try {
        NimBLEDevice::deleteClient(pClient);
      } catch (...) {
        Serial.printf("BT Warning: failed to delete client after report char failure");
      }
      return false;
    }
    
    Serial.printf("BT Subscribing to %d Report characteristics...", reportChars.size());
    
    for (size_t i = 0; i < reportChars.size(); i++) {
      auto* pChar = reportChars[i];
      Serial.printf("BT Subscribing to Report char #%d...", i + 1);
      bool subResult = pChar->subscribe(true, onHIDNotify);
      Serial.printf("BT Report char #%d subscribe result: %d", i + 1, subResult);
      if (!subResult) {
        Serial.printf("BT Failed to subscribe to Report char #%d (continuing)", i + 1);
      }
    }
    
    ConnectedDevice connDev;
    connDev.address = address;
    connDev.client = pClient;
    connDev.reportChars = reportChars;
    connDev.subscribed = true;
    connDev.lastActivityTime = millis();
    connDev.wasConnected = true;
    
    bool foundInScan = false;
    for (const auto& dev : _discoveredDevices) {
      if (dev.address == address) {
        connDev.name = dev.name;
        foundInScan = true;
        Serial.printf("BT Device found in scan results: %s (%s)", dev.name.c_str(), address.c_str());
        break;
      }
    }
    
    if (!foundInScan) {
      Serial.printf("BT Device not in scan results (may be previously paired): %s", address.c_str());
    }
    
    connDev.profile = DeviceProfiles::findDeviceProfile(address.c_str(), connDev.name.c_str());
    
    if (connDev.profile) {
      Serial.printf("BT ✓ Using device profile: %s (byte[%d] for keycode)", 
              connDev.profile->name, connDev.profile->reportByteIndex);
    } else {
      Serial.printf("BT No known profile matched for %s, will auto-detect from HID codes", address.c_str());
    }
    
    _connectedDevices.push_back(connDev);
    
    Serial.printf("BT Successfully connected to %s", address.c_str());
    lastError = "Connected";
    
    saveLastConnectedDevice(address, connDev.name);
    
    return true;
    
  } catch (const std::exception& e) {
    lastError = std::string("Connection error: ") + e.what();
    Serial.printf("BT %s", lastError.c_str());
    if (pClient) {
      try {
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
      } catch (...) {
        Serial.printf("BT Warning: failed to cleanup client after exception");
      }
    }
    return false;
  } catch (...) {
    lastError = "Unknown connection error";
    Serial.printf("BT %s", lastError.c_str());
    if (pClient) {
      try {
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
      } catch (...) {
        Serial.printf("BT Warning: failed to cleanup client after unknown exception");
      }
    }
    return false;
  }
}

// Simple retry wrapper around connectToDevice.  It invokes the single-
// attempt function up to `maxAttempts` times with a short delay between
// tries.  This keeps higher-level code (UI) from having to loop itself.
bool BluetoothHIDManager::connectToDeviceWithRetries(const std::string& address, int maxAttempts) {
  if (!_enabled) {
    Serial.printf("BT Cannot connect (retries): Bluetooth not enabled");
    lastError = "Bluetooth not enabled";
    return false;
  }
  if (address.empty()) {
    Serial.printf("BT Invalid address (empty) passed to connectToDeviceWithRetries");
    lastError = "Invalid address";
    return false;
  }
  if (isConnected(address)) {
    Serial.printf("BT Already connected to %s (retries)", address.c_str());
    return true;
  }
  if (maxAttempts <= 0) maxAttempts = 1;
  for (int i = 0; i < maxAttempts; ++i) {
    Serial.printf("BT retry %d/%d for %s", i + 1, maxAttempts, address.c_str());
    if (connectToDevice(address)) {
      return true;
    }
    delay(200);
  }
  return false;
}

bool BluetoothHIDManager::disconnectFromDevice(const std::string& address) {
  Serial.printf("BT Disconnecting from device %s", address.c_str());
  
  auto it = std::find_if(_connectedDevices.begin(), _connectedDevices.end(),
    [&address](const ConnectedDevice& dev) { return dev.address == address; });
  
  if (it != _connectedDevices.end()) {
    // Just disconnect - don't delete the client as NimBLE manages it
    // and the disconnect callback will be called
    if (it->client && it->client->isConnected()) {
      try {
        Serial.printf("BT Calling disconnect on client...");
        it->client->disconnect();
      } catch (const std::exception& e) {
        Serial.printf("BT Error during disconnect: %s", e.what());
      } catch (...) {
        Serial.printf("BT Unknown error during disconnect");
      }
    }
    
    // Remove from our list
    _connectedDevices.erase(it);
    Serial.printf("BT Disconnected from %s", address.c_str());
    return true;
  }
  
  Serial.printf("BT Device %s not in connected list", address.c_str());
  return false;
}

bool BluetoothHIDManager::isConnected(const std::string& address) const {
  return std::find_if(_connectedDevices.begin(), _connectedDevices.end(),
    [&address](const ConnectedDevice& dev) { return dev.address == address; }) != _connectedDevices.end();
}

std::vector<std::string> BluetoothHIDManager::getConnectedDevices() const {
  std::vector<std::string> addresses;
  for (const auto& dev : _connectedDevices) {
    addresses.push_back(dev.address);
  }
  return addresses;
}

ConnectedDevice* BluetoothHIDManager::findConnectedDevice(const std::string& address) {
  auto it = std::find_if(_connectedDevices.begin(), _connectedDevices.end(),
    [&address](const ConnectedDevice& dev) { return dev.address == address; });
  
  if (it != _connectedDevices.end()) {
    return &(*it);
  }
  return nullptr;
}

void BluetoothHIDManager::processInputEvents() {
  // Input events are processed via notifications callback
  // This method is kept for potential polling-based implementations
}

void BluetoothHIDManager::setInputCallback(std::function<void(uint16_t)> callback) {
  _inputCallback = callback;
  Serial.printf("BT Input callback registered");
}

void BluetoothHIDManager::setButtonInjector(std::function<void(uint8_t)> injector) {
  _buttonInjector = injector;
  Serial.printf("BT Button injector registered");
}

void BluetoothHIDManager::beginKeyMapping(
    const std::function<void(uint8_t keycode, uint8_t reportByteIndex)>& callback) {
  _keyMappingCallback = callback;
  _keyMappingActive = true;
  Serial.printf("BT Key mapping mode started");
}

void BluetoothHIDManager::endKeyMapping() {
  _keyMappingActive = false;
  _keyMappingCallback = nullptr;
  for (auto& device : _connectedDevices) {
    device.lastButtonState = false;
    device.lastHIDKeycode = 0x00;
  }
  Serial.printf("BT Key mapping mode ended");
}

bool BluetoothHIDManager::extractEffectiveKeycode(const uint8_t* pData, size_t length, uint8_t& keycode,
                                                  uint8_t& reportByteIndex) {
  if (!pData || length == 0) {
    return false;
  }
  // Remove all 0x00 fields and keep the first effective byte as keycode.
  for (size_t i = 0; i < length; ++i) {
    if (pData[i] != 0x00) {
      keycode = pData[i];
      reportByteIndex = static_cast<uint8_t>(i);
      return true;
    }
  }
  return false;
}

bool BluetoothHIDManager::hasRecentActivity() const {
  // Check if any connected device has had activity in the last 4 minutes
  // This prevents power sleep while using BLE controller
  unsigned long now = millis();
  for (const auto& device : _connectedDevices) {
    if (device.lastActivityTime > 0) {
      unsigned long timeSinceActivity = now - device.lastActivityTime;
      if (timeSinceActivity < 240000) {  // 4 minute (240 second) threshold to keep BLE alive
        return true;
      }
    }
  }
  return false;
}

// Static callback for HID notifications
void BluetoothHIDManager::onHIDNotify(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
  if (!g_instance || !pData || length == 0) return;
  
  // Log raw data
  char hexStr[128] = {0};
  int offset = 0;
  for (size_t i = 0; i < length && i < 16; i++) {
    offset += snprintf(hexStr + offset, sizeof(hexStr) - offset, "%02X ", pData[i]);
  }
  Serial.printf("BT HID Report (%d bytes): %s", length, hexStr);
  
  // Get device
  ConnectedDevice* device = nullptr;
  if (pChar && pChar->getRemoteService()) {
    auto client = pChar->getRemoteService()->getClient();
    if (client) {
      std::string deviceAddr = client->getPeerAddress().toString();
      device = g_instance->findConnectedDevice(deviceAddr);
    }
  }
  if (!device) return;

  device->lastActivityTime = millis();

  if (g_instance->_keyMappingActive) {
    uint8_t effectiveKeycode = 0x00;
    uint8_t reportByteIndex = 0xFF;
    const bool pressed = extractEffectiveKeycode(pData, length, effectiveKeycode, reportByteIndex);

    if (!pressed) {
      device->lastButtonState = false;
      device->lastHIDKeycode = 0x00;
      return;
    }

    const bool isRepeat = device->lastButtonState && (device->lastHIDKeycode == effectiveKeycode);
    if (isRepeat) {
      return;
    }

    device->lastButtonState = true;
    device->lastHIDKeycode = effectiveKeycode;
    Serial.printf("BT Mapping key captured: code=0x%02X byte=%u", effectiveKeycode, reportByteIndex);
    if (g_instance->_keyMappingCallback) {
      g_instance->_keyMappingCallback(effectiveKeycode, reportByteIndex);
    }
    return;
  }

  uint8_t  keycode = 0x00;
  bool    isPressed = false;

  const DeviceProfiles::DeviceProfile* activeProfile = device->profile;
  if (const DeviceProfiles::DeviceProfile* customProfile = DeviceProfiles::getCustomProfile()) {
    activeProfile = customProfile;
  }

  if (activeProfile) {
    // 原有设备逻辑不变
    if (length >= activeProfile->reportByteIndex + 1) {
      keycode = pData[activeProfile->reportByteIndex];
    }
    if (strcmp(activeProfile->name, "IINE Game Brick") == 0) {
      isPressed = (pData[0] & 0x01) != 0;
    } else {
      isPressed = (keycode != 0x00);
    }
  } else {
    if (length == 3) {
      uint8_t modifier = pData[0];
      keycode = pData[2];
      if (modifier == 0x01 || keycode == 0x4B) {
        keycode = 0x4B; isPressed = true;
      } else if (modifier == 0x02 || keycode == 0x4E) {
        keycode = 0x4E; isPressed = true;
      } else {
        isPressed = (keycode != 0x00);
      }
    }
    // ====================== 6字节翻页器（正确边沿触发版）======================
    else if (length == 6) {
      uint8_t key = pData[0];
      
      if (key == 0x01) {
        keycode = 0x4B;
        isPressed = true;
        Serial.printf("BT 6键翻页器 [上一页] 按下\n");
      }
      else if (key == 0x02) {
        keycode = 0x4E;
        isPressed = true;
        Serial.printf("BT 6键翻页器 [下一页] 按下\n");
      }
      else {
        keycode = 0x00;
        isPressed = false;
      }
    }
    // ======================================================================
    else if (length >= 5) {
      uint8_t byte4 = pData[4];
      if (byte4 == 0x07 || byte4 == 0x09) {
        keycode = byte4;
        isPressed = (pData[0] & 0x01) != 0;
      } else {
        keycode = (length > 2) ? pData[2] : 0;
        isPressed = (keycode != 0);
      }
    } else {
      keycode = (length > 2) ? pData[2] : 0;
      isPressed = (keycode != 0);
    }
  }

  // ====================== 【保留边沿触发】======================
  bool wasPressedPrevious = device->lastButtonState;

  // 松开 → 更新状态，不动作
  if (!isPressed) {
    device->lastButtonState = false;
    device->lastHIDKeycode = 0x00;
    return;
  }

  // 已经按住 → 不重复触发
  if (wasPressedPrevious) {
    return;
  }

  // ====================== 【唯一一次触发：按下瞬间】======================
  device->lastButtonState = true;
  device->lastHIDKeycode = keycode;

  Serial.printf("BT >>> BUTTON PRESSED: keycode=0x%02X <<<\n", keycode);

  if (g_instance->_buttonInjector) {
    uint8_t btn = g_instance->mapKeycodeToButton(keycode, device->profile);
    if (btn != 0xFF) {
      unsigned long now = millis();
      if (now - device->lastInjectionTime >= 150) {
        String buttonName = (btn == HalGPIO::BTN_DOWN) ? "PageForward" : "PageBack";
        Serial.printf("BT Mapped key -> %s\n", buttonName.c_str());
        Serial.printf("BT Injecting button: %d\n", btn);
        g_instance->_buttonInjector(btn);
        device->lastInjectionTime = now;
      }
    }
  }

  if (g_instance->_inputCallback) {
    g_instance->_inputCallback(keycode);
  }
}


uint16_t BluetoothHIDManager::parseHIDReport(uint8_t* data, size_t length) {
  if (length < 3) {
    Serial.printf("BT Invalid HID report length: %d", length);
    return 0;
  }
  
  uint8_t modifier = data[0];
  uint8_t keycode = data[2]; // First key in the report
  
  // If no key pressed (all zeros), return 0
  if (keycode == 0 && modifier == 0) {
    return 0;
  }
  
  // Log non-empty reports
  Serial.printf("BT HID Report: mod=0x%02X key=0x%02X", modifier, keycode);
  
  // Combine modifier and keycode (modifier in upper byte, keycode in lower)
  uint16_t combined = (static_cast<uint16_t>(modifier) << 8) | keycode;
  
  return combined;
}

// Map HID keycodes to navigator buttons based on device profile
// Only maps keycodes that match the current device's profile to prevent
// unwanted D-pad or other button inputs from triggering page turns
uint8_t BluetoothHIDManager::mapKeycodeToButton(uint8_t keycode, const DeviceProfiles::DeviceProfile* profile) {
  // Log keycode for debugging
  if (keycode != 0x00) {
    Serial.printf("BT mapKeycodeToButton() called with keycode: 0x%02X", keycode);
  }
  
  const DeviceProfiles::DeviceProfile* effectiveProfile = profile;
  const DeviceProfiles::DeviceProfile* customProfile = DeviceProfiles::getCustomProfile();
  if (customProfile) {
    effectiveProfile = customProfile;
  }

  // If we have a profile, ONLY map keycodes specific to that profile
  if (effectiveProfile) {
    if (keycode == effectiveProfile->pageUpCode) {
      Serial.printf("BT Matched profile pageUpCode 0x%02X (%s) -> PageBack", keycode,
                    effectiveProfile->name);
      return HalGPIO::BTN_UP;
    } else if (keycode == effectiveProfile->pageDownCode) {
      Serial.printf("BT Matched profile pageDownCode 0x%02X (%s) -> PageForward", keycode,
                    effectiveProfile->name);
      return HalGPIO::BTN_DOWN;
    } else {
      // Not a profile-mapped keycode - ignore it
      Serial.printf("BT Keycode 0x%02X not in profile %s (expecting 0x%02X/0x%02X), ignoring", 
              keycode, effectiveProfile->name, effectiveProfile->pageUpCode,
              effectiveProfile->pageDownCode);
      return 0xFF;
    }
  }
  
  // No profile - fall back to generic HID consumer codes only
  switch (keycode) {
    // 翻页器核心映射（按需修改）
    case 0x01:   // 左Ctrl → 上一页
    case 0x4B:   // PageUp → 上一页
    case 0xE9:   // Consumer PageUp → 上一页
      Serial.printf("BT Mapped key 0x%02X -> PageBack", keycode);
      return HalGPIO::BTN_UP;
    
    case 0x02:   // 左Shift → 下一页
    case 0x4E:   // PageDown → 下一页
    case 0xEA:   // Consumer PageDown → 下一页
      Serial.printf("BT Mapped key 0x%02X -> PageForward", keycode);
      return HalGPIO::BTN_DOWN;

    case 0x00:   // 忽略释放事件
      return 0xFF;

    default:
      Serial.printf("BT Unmapped keycode: 0x%02X (翻页器未匹配)", keycode);
      return 0xFF;
  }
}

void BluetoothHIDManager::updateActivity() {
  // Check inactivity timeouts every 10 seconds
  unsigned long now = millis();
  if (now - lastMaintenanceCheck < 10000) {
    return;
  }
  lastMaintenanceCheck = now;
  
  // Check for inactive connections
  for (auto& device : _connectedDevices) {
    if (device.lastActivityTime > 0) {
      unsigned long inactiveTime = now - device.lastActivityTime;
      //和休眠时间绑定
      if (inactiveTime >= SETTINGS.getSleepTimeoutMs()) {
        Serial.printf("BT Device %s inactive for %lu ms, disconnecting", device.address.c_str(), inactiveTime);
        disconnectFromDevice(device.address);
        break;  // List modified, exit loop
      }
    }
  }
}

void BluetoothHIDManager::checkAutoReconnect() {
  // Check for devices that were previously connected but are now disconnected
  // Attempt to reconnect to them automatically
  static unsigned long lastReconnectCheck = 0;
  unsigned long now = millis();
  
  // Only check every 5 seconds to avoid hammering
  if (now - lastReconnectCheck < 5000) {
    return;
  }
  lastReconnectCheck = now;
  
  // Look for devices that should be auto-reconnected
  for (auto& device : _connectedDevices) {
    if (device.wasConnected && device.client) {
      // Check if client is still connected
      if (!device.client->isConnected()) {
        Serial.printf("BT Device %s was disconnected, attempting auto-reconnect...", device.address.c_str());
        
        // Remove from list and reconnect
        std::string addr = device.address;
        disconnectFromDevice(addr);
        
        // Attempt reconnection
        if (connectToDevice(addr)) {
          Serial.printf("BT Auto-reconnected to %s", addr.c_str());
        } else {
          Serial.printf("BT Auto-reconnect to %s failed: %s", addr.c_str(), lastError.c_str());
        }
        break;  // Only reconnect one device per check
      }
    }
  }
}

void BluetoothHIDManager::saveState() {
  Serial.printf("BT Saving state");

  SdMan.mkdir("/.crosspoint");

  FsFile outputFile;
  if (!SdMan.openFileForWrite("BT", "/.crosspoint/bluetooth.bin", outputFile)) {
    Serial.printf("BT Failed to open bluetooth.bin for writing");
    return;
  }

  const uint8_t version = 2;
  serialization::writePod(outputFile, version);

  serialization::writeString(outputFile, _lastConnectedAddress);
  serialization::writeString(outputFile, _lastConnectedName);

  const DeviceProfiles::DeviceProfile* custom = DeviceProfiles::getCustomProfile();
  const uint8_t hasCustomProfile = custom ? 1 : 0;
  serialization::writePod(outputFile, hasCustomProfile);
  if (custom) {
    serialization::writePod(outputFile, custom->pageUpCode);
    serialization::writePod(outputFile, custom->pageDownCode);
    serialization::writePod(outputFile, custom->reportByteIndex);
    Serial.printf("BT Saved custom mapping: up=0x%02X down=0x%02X byte=%u", custom->pageUpCode,
                  custom->pageDownCode, custom->reportByteIndex);
  }

  outputFile.close();
  Serial.printf("BT State saved");
}

void BluetoothHIDManager::loadState() {
  Serial.printf("BT Loading state");

  FsFile inputFile;
  if (!SdMan.openFileForRead("BT", "/.crosspoint/bluetooth.bin", inputFile)) {
    Serial.printf("BT No bluetooth.bin found");
    return;
  }

  uint8_t version = 0;
  serialization::readPod(inputFile, version);
  if (version == 1) {
    serialization::readString(inputFile, _lastConnectedAddress);
    serialization::readString(inputFile, _lastConnectedName);
    DeviceProfiles::clearCustomProfile();
    inputFile.close();
    Serial.printf("BT Loaded v1 state: %s (%s)", _lastConnectedName.c_str(), _lastConnectedAddress.c_str());
    return;
  }

  if (version != 2) {
    Serial.printf("BT Unknown bluetooth.bin version: %u", version);
    inputFile.close();
    return;
  }

  serialization::readString(inputFile, _lastConnectedAddress);
  serialization::readString(inputFile, _lastConnectedName);

  uint8_t hasCustomProfile = 0;
  serialization::readPod(inputFile, hasCustomProfile);
  if (hasCustomProfile) {
    uint8_t pageUpCode = 0x00;
    uint8_t pageDownCode = 0x00;
    uint8_t reportByteIndex = 2;
    serialization::readPod(inputFile, pageUpCode);
    serialization::readPod(inputFile, pageDownCode);
    serialization::readPod(inputFile, reportByteIndex);
    DeviceProfiles::setCustomProfile(pageUpCode, pageDownCode, reportByteIndex);
    Serial.printf("BT Loaded custom mapping: up=0x%02X down=0x%02X byte=%u", pageUpCode, pageDownCode,
                  reportByteIndex);
  } else {
    DeviceProfiles::clearCustomProfile();
  }

  inputFile.close();
  Serial.printf("BT State loaded: %s (%s)", _lastConnectedName.c_str(), _lastConnectedAddress.c_str());
}

void BluetoothHIDManager::saveLastConnectedDevice(const std::string& address, const std::string& name) {
  Serial.printf("BT Saving last connected device: %s (%s)", name.c_str(), address.c_str());

  _lastConnectedAddress = address;
  _lastConnectedName = name;
  saveState();
  Serial.printf("BT Last connected device saved successfully");
}

bool BluetoothHIDManager::loadLastConnectedDevice(std::string& address, std::string& name) {
  if (_lastConnectedAddress.empty()) {
    loadState();
  }

  address = _lastConnectedAddress;
  name = _lastConnectedName;
  if (address.empty()) {
    Serial.printf("BT Loaded bluetooth.bin but address field is empty");
    return false;
  }
  Serial.printf("BT Loaded last connected device: %s (%s)", name.c_str(), address.c_str());
  return true;
}

