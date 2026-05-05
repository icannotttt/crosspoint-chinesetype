#pragma once

#include <BluetoothHIDManager.h>
#include <HalGPIO.h>

#include "CrossPointSettings.h"

extern HalGPIO gpio;

inline void initializeBluetoothAfterReaderRender() {
  auto& btMgr = BluetoothHIDManager::getInstance();
  btMgr.setButtonInjector([](uint8_t buttonIndex) {
    gpio.injectButtonPress(buttonIndex);
  });

  if (!SETTINGS.bluetoothEnabled) {
    return;
  }

  if (!btMgr.enable()) {
    Serial.printf("[BT] Failed to enable Bluetooth after reader render\n");
    return;
  }

  Serial.printf("[BT] Bluetooth enabled after reader render\n");

  std::string lastAddr, lastName;
  btMgr.startScan(2000);
  if (btMgr.loadLastConnectedDevice(lastAddr, lastName)) {
    Serial.printf("[BT] Auto-connecting to last device %s (%s)\n", lastName.c_str(), lastAddr.c_str());
    if (btMgr.connectToDeviceWithRetries(lastAddr, 1)) {
      Serial.printf("[BT] Auto-connect successful\n");
    } else {
      Serial.printf("[BT] Auto-connect failed after retries\n");
    }
  }
}