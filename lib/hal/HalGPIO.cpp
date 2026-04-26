#include <HalGPIO.h>
#include <SPI.h>

void HalGPIO::begin() {
  inputMgr.begin();
  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS);
  pinMode(UART0_RXD, INPUT);
}

void HalGPIO::update() { 
  // Save previous virtual button state BEFORE updating current state
  // This allows wasReleased() to detect buttons that were pressed last frame but not this frame
  previousVirtualButtonEvents = virtualButtonEvents;
  
  // Move queued virtual buttons to current events for this frame
  // Then clear the queue so only buttons pressed this frame show in events
  virtualButtonEvents = virtualButtonQueue;
  virtualButtonQueue = 0;
  
  inputMgr.update();
}

bool HalGPIO::isPressed(uint8_t buttonIndex) const { return inputMgr.isPressed(buttonIndex); }

bool HalGPIO::wasPressed(uint8_t buttonIndex) const { 
  return inputMgr.wasPressed(buttonIndex) || (virtualButtonEvents & (1 << buttonIndex));
}

bool HalGPIO::wasAnyPressed() const { 
  return inputMgr.wasAnyPressed() || (virtualButtonEvents > 0);
}

bool HalGPIO::wasReleased(uint8_t buttonIndex) const { 
  // Check both physical button releases AND virtual button releases
  // Virtual release = was pressed last frame but not this frame
  const uint8_t virtualRelease = previousVirtualButtonEvents & ~virtualButtonEvents;
  return inputMgr.wasReleased(buttonIndex) || (virtualRelease & (1 << buttonIndex));
}

bool HalGPIO::wasAnyReleased() const { 
  // Check both physical and virtual button releases
  const uint8_t virtualRelease = previousVirtualButtonEvents & ~virtualButtonEvents;
  return inputMgr.wasAnyReleased() || (virtualRelease > 0);
}

unsigned long HalGPIO::getHeldTime() const { return inputMgr.getHeldTime(); }

void HalGPIO::startDeepSleep() {
  // Ensure that the power button has been released to avoid immediately turning back on if you're holding it
  while (inputMgr.isPressed(BTN_POWER)) {
    delay(50);
    inputMgr.update();
  }
  // Arm the wakeup trigger *after* the button is released
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  // Enter Deep Sleep
  esp_deep_sleep_start();
}

int HalGPIO::getBatteryPercentage() const {
  static const BatteryMonitor battery = BatteryMonitor(BAT_GPIO0);
  return battery.readPercentage();
}


void HalGPIO::injectButtonPress(uint8_t buttonIndex) {
  // Queue the button for the next update() call
  // This ensures the reader gets a chance to check wasPressed() 
  // before the button is cleared
  virtualButtonQueue |= (1 << buttonIndex);
}

void HalGPIO::clearVirtualButtons() {
  virtualButtonEvents = 0;
  virtualButtonQueue = 0;
}

bool HalGPIO::isUsbConnected() const {
  // U0RXD/GPIO20 reads HIGH when USB is connected
  return digitalRead(UART0_RXD) == HIGH;
}

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
  const bool usbConnected = isUsbConnected();
  const auto wakeupCause = esp_sleep_get_wakeup_cause();
  const auto resetReason = esp_reset_reason();

  if ((wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && !usbConnected) ||
      (wakeupCause == ESP_SLEEP_WAKEUP_GPIO && resetReason == ESP_RST_DEEPSLEEP && usbConnected)) {
    return WakeupReason::PowerButton;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_UNKNOWN && usbConnected) {
    return WakeupReason::AfterFlash;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && usbConnected) {
    return WakeupReason::AfterUSBPower;
  }
  return WakeupReason::Other;
}