#pragma once

#include <Arduino.h>
#include <string>
#include <vector>
#include <functional>
#include <map>
#include "DeviceProfiles.h"

// Forward declarations
class NimBLEClient;
class NimBLERemoteCharacteristic;
class NimBLEAdvertisedDevice;

struct BluetoothDevice {
  std::string address;
  std::string name;
  int rssi;
  bool isHID = false;
  uint8_t addrType = 0;
};

struct ConnectedDevice {
  std::string address;
  std::string name;
  NimBLEClient* client = nullptr;
  std::vector<NimBLERemoteCharacteristic*> reportChars;
  bool subscribed = false;
  unsigned long lastActivityTime = 0;  // Timestamp of last HID report received
  uint8_t lastHIDKeycode = 0x00;       // Track last keycode to detect press/release transitions
  unsigned long lastInjectionTime = 0; // Cooldown for button injection to prevent flooding
  bool wasConnected = false;           // Track if this device was previously connected for auto-reconnect
  bool lastButtonState = false;        // Track button pressed state (from byte[0])
  const DeviceProfiles::DeviceProfile* profile = nullptr;  // Device-specific HID profile
};

class BluetoothHIDManager {
public:
  // Singleton access
  static BluetoothHIDManager& getInstance();

  // Lifecycle
  bool enable();
  bool disable();
  bool isEnabled() const { return _enabled; }

  // Scanning
  void startScan(uint32_t durationMs = 10000);
  void stopScan();
  bool isScanning() const { return _scanning; }
  const std::vector<BluetoothDevice>& getDiscoveredDevices() const { return _discoveredDevices; }

  // Connection
  bool connectToDevice(const std::string& address);
  /**
   * Attempt to connect up to `maxAttempts` times before giving up.
   * This is useful for UI code that may try to connect to non‑responsive
   * devices and must not crash the system.
   */
  bool connectToDeviceWithRetries(const std::string& address, int maxAttempts = 3);
  bool disconnectFromDevice(const std::string& address);
  bool isConnected(const std::string& address) const;
  std::vector<std::string> getConnectedDevices() const;

  // Input handling
  void processInputEvents();
  void setInputCallback(std::function<void(uint16_t keycode)> callback);
  void setButtonInjector(std::function<void(uint8_t buttonIndex)> injector);
  // Key mapping mode: report effective keycode and byte index from raw HID report.
  void beginKeyMapping(const std::function<void(uint8_t keycode, uint8_t reportByteIndex)>& callback);
  void endKeyMapping();
  bool isKeyMappingActive() const { return _keyMappingActive; }
  void updateActivity();  // Call periodically to check inactivity timeout
  void checkAutoReconnect();  // Auto-reconnect to previously connected devices if disconnected
  
  // Check if BLE has had activity recently (within last 4 minutes)
  // Used by power manager to prevent sleep during BLE use
  bool hasRecentActivity() const;

  // State persistence
  void saveState();
  void loadState();
  void saveLastConnectedDevice(const std::string& address, const std::string& name);
  bool loadLastConnectedDevice(std::string& address, std::string& name);

  std::string lastError;
  //防止崩溃，让外部可读取信息
  void onScanResult(NimBLEAdvertisedDevice* advertisedDevice);

private:

  // BLE callbacks (public for NimBLE callbacks)
  
  static void onHIDNotify(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify);

private:
  BluetoothHIDManager();
  ~BluetoothHIDManager();
  BluetoothHIDManager(const BluetoothHIDManager&) = delete;
  BluetoothHIDManager& operator=(const BluetoothHIDManager&) = delete;

  void cleanup();
  uint16_t parseHIDReport(uint8_t* data, size_t length);
  ConnectedDevice* findConnectedDevice(const std::string& address);
  uint8_t mapKeycodeToButton(uint8_t keycode, const DeviceProfiles::DeviceProfile* profile);
  static bool extractEffectiveKeycode(const uint8_t* pData, size_t length, uint8_t& keycode,
                                      uint8_t& reportByteIndex);

  bool _enabled = false;
  bool _scanning = false;
  std::vector<BluetoothDevice> _discoveredDevices;
  std::vector<ConnectedDevice> _connectedDevices;
  std::string _lastConnectedAddress;
  std::string _lastConnectedName;
  std::function<void(uint16_t)> _inputCallback;
  std::function<void(uint8_t)> _buttonInjector;
  bool _keyMappingActive = false;
  std::function<void(uint8_t, uint8_t)> _keyMappingCallback;
  
  // Inactivity timeout (milliseconds)
  static constexpr unsigned long INACTIVITY_TIMEOUT_MS = 120000;  // 2 minutes
  unsigned long lastMaintenanceCheck = 0;
};
