#include <Arduino.h>
#include <EpdFontLoader.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <SDCardManager.h>
#include <SPI.h>
#include <builtinFonts/all.h>

#include <cstring>

#include "Battery.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/boot_sleep/BootActivity.h"
#include "activities/boot_sleep/SleepActivity.h"
#include "activities/browser/OpdsBookBrowserActivity.h"
#include "activities/home/HomeActivity.h"
#include "activities/home/MyLibraryActivity.h"
#include "activities/home/RecentBooksActivity.h"
#include "activities/network/CrossPointWebServerActivity.h"
#include "activities/reader/ReaderActivity.h"
#include "activities/settings/SettingsActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "activities/browser/JianGuoBrowserActivity.h"

#include <BluetoothHIDManager.h>
#include "util/ButtonNavigator.h"



HalDisplay display;
HalGPIO gpio;
MappedInputManager mappedInputManager(gpio);
GfxRenderer renderer(display);
Activity* currentActivity;

// Fonts
EpdFont bookerly14RegularFont(&notosans_18_bold);
EpdFont bookerly14BoldFont(&notosans_18_bold);
EpdFont bookerly14ItalicFont(&notosans_18_bold);
EpdFont bookerly14BoldItalicFont(&notosans_18_bold);
EpdFontFamily bookerly14FontFamily(&bookerly14RegularFont, &bookerly14BoldFont, &bookerly14ItalicFont,
                                   &bookerly14BoldItalicFont);
#ifndef OMIT_FONTS
EpdFont bookerly12RegularFont(&notosans_18_bold);
EpdFont bookerly12BoldFont(&notosans_18_bold);
EpdFont bookerly12ItalicFont(&notosans_18_bold);
EpdFont bookerly12BoldItalicFont(&notosans_18_bold);
EpdFontFamily bookerly12FontFamily(&bookerly12RegularFont, &bookerly12BoldFont, &bookerly12ItalicFont,
                                   &bookerly12BoldItalicFont);
EpdFont bookerly16RegularFont(&notosans_18_bold);
EpdFont bookerly16BoldFont(&notosans_18_bold);
EpdFont bookerly16ItalicFont(&notosans_18_bold);
EpdFont bookerly16BoldItalicFont(&notosans_18_bold);
EpdFontFamily bookerly16FontFamily(&bookerly16RegularFont, &bookerly16BoldFont, &bookerly16ItalicFont,
                                   &bookerly16BoldItalicFont);
EpdFont bookerly18RegularFont(&notosans_18_bold);
EpdFont bookerly18BoldFont(&notosans_18_bold);
EpdFont bookerly18ItalicFont(&notosans_18_bold);
EpdFont bookerly18BoldItalicFont(&notosans_18_bold);
EpdFontFamily bookerly18FontFamily(&bookerly18RegularFont, &bookerly18BoldFont, &bookerly18ItalicFont,
                                   &bookerly18BoldItalicFont);

EpdFont notosans12RegularFont(&notosans_18_bold);
EpdFont notosans12BoldFont(&notosans_18_bold);
EpdFont notosans12ItalicFont(&notosans_18_bold);
EpdFont notosans12BoldItalicFont(&notosans_18_bold);
EpdFontFamily notosans12FontFamily(&notosans12RegularFont, &notosans12BoldFont, &notosans12ItalicFont,
                                   &notosans12BoldItalicFont);
EpdFont notosans14RegularFont(&notosans_18_bold);
EpdFont notosans14BoldFont(&notosans_18_bold);
EpdFont notosans14ItalicFont(&notosans_18_bold);
EpdFont notosans14BoldItalicFont(&notosans_18_bold);
EpdFontFamily notosans14FontFamily(&notosans14RegularFont, &notosans14BoldFont, &notosans14ItalicFont,
                                   &notosans14BoldItalicFont);
EpdFont notosans16RegularFont(&notosans_18_bold);
EpdFont notosans16BoldFont(&notosans_18_bold);
EpdFont notosans16ItalicFont(&notosans_18_bold);
EpdFont notosans16BoldItalicFont(&notosans_18_bold);
EpdFontFamily notosans16FontFamily(&notosans16RegularFont, &notosans16BoldFont, &notosans16ItalicFont,
                                   &notosans16BoldItalicFont);
EpdFont notosans18RegularFont(&notosans_18_bold);
EpdFont notosans18BoldFont(&notosans_18_bold);
EpdFont notosans18ItalicFont(&notosans_18_bold);
EpdFont notosans18BoldItalicFont(&notosans_18_bold);
EpdFontFamily notosans18FontFamily(&notosans18RegularFont, &notosans18BoldFont, &notosans18ItalicFont,
                                   &notosans18BoldItalicFont);

EpdFont opendyslexic8RegularFont(&notosans_18_bold);
EpdFont opendyslexic8BoldFont(&notosans_18_bold);
EpdFont opendyslexic8ItalicFont(&notosans_18_bold);
EpdFont opendyslexic8BoldItalicFont(&notosans_18_bold);
EpdFontFamily opendyslexic8FontFamily(&opendyslexic8RegularFont, &opendyslexic8BoldFont, &opendyslexic8ItalicFont,
                                      &opendyslexic8BoldItalicFont);
EpdFont opendyslexic10RegularFont(&notosans_18_bold);
EpdFont opendyslexic10BoldFont(&notosans_18_bold);
EpdFont opendyslexic10ItalicFont(&notosans_18_bold);
EpdFont opendyslexic10BoldItalicFont(&notosans_18_bold);
EpdFontFamily opendyslexic10FontFamily(&opendyslexic10RegularFont, &opendyslexic10BoldFont, &opendyslexic10ItalicFont,
                                       &opendyslexic10BoldItalicFont);
EpdFont opendyslexic12RegularFont(&notosans_18_bold);
EpdFont opendyslexic12BoldFont(&notosans_18_bold);
EpdFont opendyslexic12ItalicFont(&notosans_18_bold);
EpdFont opendyslexic12BoldItalicFont(&notosans_18_bold);
EpdFontFamily opendyslexic12FontFamily(&opendyslexic12RegularFont, &opendyslexic12BoldFont, &opendyslexic12ItalicFont,
                                       &opendyslexic12BoldItalicFont);
EpdFont opendyslexic14RegularFont(&notosans_18_bold);
EpdFont opendyslexic14BoldFont(&notosans_18_bold);
EpdFont opendyslexic14ItalicFont(&notosans_18_bold);
EpdFont opendyslexic14BoldItalicFont(&notosans_18_bold);
EpdFontFamily opendyslexic14FontFamily(&opendyslexic14RegularFont, &opendyslexic14BoldFont, &opendyslexic14ItalicFont,
                                       &opendyslexic14BoldItalicFont);
#endif  // OMIT_FONTS

EpdFont smallFont(&ubuntu_10_bold);
EpdFontFamily smallFontFamily(&smallFont);

EpdFont ui10RegularFont(&ubuntu_10_bold);
EpdFont ui10BoldFont(&ubuntu_10_bold);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

EpdFont ui12RegularFont(&ubuntu_10_bold);
EpdFont ui12BoldFont(&ubuntu_10_bold);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);


// measurement of power button press duration calibration value
unsigned long t1 = 0;
unsigned long t2 = 0;

void exitActivity() {
  if (currentActivity) {
    currentActivity->onExit();
    delete currentActivity;
    currentActivity = nullptr;
  }
}

void enterNewActivity(Activity* activity) {
  currentActivity = activity;
  currentActivity->onEnter();
}

// Verify power button press duration on wake-up from deep sleep
// Pre-condition: isWakeupByPowerButton() == true
void verifyPowerButtonDuration() {
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) {
    // Fast path for short press
    // Needed because inputManager.isPressed() may take up to ~500ms to return the correct state
    return;
  }

  // Give the user up to 1000ms to start holding the power button, and must hold for SETTINGS.getPowerButtonDuration()
  const auto start = millis();
  bool abort = false;
  // Subtract the current time, because inputManager only starts counting the HeldTime from the first update()
  // This way, we remove the time we already took to reach here from the duration,
  // assuming the button was held until now from millis()==0 (i.e. device start time).
  const uint16_t calibration = start;
  const uint16_t calibratedPressDuration =
      (calibration < SETTINGS.getPowerButtonDuration()) ? SETTINGS.getPowerButtonDuration() - calibration : 1;

  gpio.update();
  // Needed because inputManager.isPressed() may take up to ~500ms to return the correct state
  while (!gpio.isPressed(HalGPIO::BTN_POWER) && millis() - start < 1000) {
    delay(10);  // only wait 10ms each iteration to not delay too much in case of short configured duration.
    gpio.update();
  }

  t2 = millis();
  if (gpio.isPressed(HalGPIO::BTN_POWER)) {
    do {
      delay(10);
      gpio.update();
    } while (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.getHeldTime() < calibratedPressDuration);
    abort = gpio.getHeldTime() < calibratedPressDuration;
  } else {
    abort = true;
  }

  if (abort) {
    // Button released too early. Returning to sleep.
    // IMPORTANT: Re-arm the wakeup trigger before sleeping again
    gpio.startDeepSleep();
  }
}

void waitForPowerRelease() {
  gpio.update();
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }
}

// Enter deep sleep mode
void enterDeepSleep() {
  //等待渲染完成
  uint32_t waitStart = millis();
  const uint32_t MAX_WAIT_TIME = 5000; // 最多等5秒
  while (!APP_STATE.isRenderComplete) {
    Serial.printf("[%lu] [MAIN] Waiting for main render to complete...\n", millis());
    vTaskDelay(100 / portTICK_PERIOD_MS); // 每100ms检查一次
    
    // 超时保护：避免卡死
    if (millis() - waitStart > MAX_WAIT_TIME) {
      Serial.printf("[%lu] [MAIN] Wait timeout, proceed with PNG render\n", millis());
      break;
    }
  }
  //原逻辑

  APP_STATE.lastSleepFromReader = currentActivity && currentActivity->isReaderActivity();
  APP_STATE.saveToFile();

  //bluetooth
  try {
    auto& btMgr = BluetoothHIDManager::getInstance();
    if (btMgr.isEnabled()) {
      Serial.printf("SLP", "Disabling Bluetooth before deep sleep");
      btMgr.disable();
    }
  } catch (...) {
    Serial.printf("SLP", "Could not disable Bluetooth");
  }


  exitActivity();
  enterNewActivity(new SleepActivity(renderer, mappedInputManager));

  display.deepSleep();
  Serial.printf("[%lu] [   ] Power button press calibration value: %lu ms\n", millis(), t2 - t1);
  Serial.printf("[%lu] [   ] Entering deep sleep.\n", millis());

  gpio.startDeepSleep();
}


void onGoHome();
void onGoToMyLibraryWithPath(const std::string& path);
void onGoToRecentBooks();
void onGoToReader(const std::string& initialEpubPath) {
  exitActivity();
  enterNewActivity(
      new ReaderActivity(renderer, mappedInputManager, initialEpubPath, onGoHome, onGoToMyLibraryWithPath));
}

void onGoToFileTransfer() {
  exitActivity();
  enterNewActivity(new CrossPointWebServerActivity(renderer, mappedInputManager, onGoHome));
}

void onGoToSettings() {
  exitActivity();
  enterNewActivity(new SettingsActivity(renderer, mappedInputManager, onGoHome));
}

void onGoToMyLibrary() {
  exitActivity();
  enterNewActivity(new MyLibraryActivity(renderer, mappedInputManager, onGoHome, onGoToReader));
}

void onGoToRecentBooks() {
  exitActivity();
  enterNewActivity(new RecentBooksActivity(renderer, mappedInputManager, onGoHome, onGoToReader));
}

void onGoToMyLibraryWithPath(const std::string& path) {
  exitActivity();
  enterNewActivity(new MyLibraryActivity(renderer, mappedInputManager, onGoHome, onGoToReader, path));
}

void onGoToBrowser() {
  exitActivity();
  enterNewActivity(new OpdsBookBrowserActivity(renderer, mappedInputManager, onGoHome));
}
void onGoToJianGuoYun() {
  exitActivity();
  enterNewActivity(new JianGuoBrowserActivity(renderer, mappedInputManager, onGoHome));
}

void onGoHome() {
  exitActivity();
  enterNewActivity(new HomeActivity(renderer, mappedInputManager, onGoToReader, onGoToMyLibrary, onGoToRecentBooks,
                                    onGoToSettings, onGoToFileTransfer, onGoToBrowser,onGoToJianGuoYun));
}

void setupDisplayAndFonts() {
  display.begin();
  renderer.begin();
  Serial.printf("[%lu] [   ] Display initialized\n", millis());
  renderer.insertFont(BOOKERLY_14_FONT_ID, bookerly14FontFamily);
#ifndef OMIT_FONTS
  renderer.insertFont(BOOKERLY_12_FONT_ID, bookerly12FontFamily);
  renderer.insertFont(BOOKERLY_16_FONT_ID, bookerly16FontFamily);
  renderer.insertFont(BOOKERLY_18_FONT_ID, bookerly18FontFamily);

  renderer.insertFont(NOTOSANS_12_FONT_ID, notosans12FontFamily);
  renderer.insertFont(NOTOSANS_14_FONT_ID, notosans14FontFamily);
  renderer.insertFont(NOTOSANS_16_FONT_ID, notosans16FontFamily);
  renderer.insertFont(NOTOSANS_18_FONT_ID, notosans18FontFamily);
  renderer.insertFont(OPENDYSLEXIC_8_FONT_ID, opendyslexic8FontFamily);
  renderer.insertFont(OPENDYSLEXIC_10_FONT_ID, opendyslexic10FontFamily);
  renderer.insertFont(OPENDYSLEXIC_12_FONT_ID, opendyslexic12FontFamily);
  renderer.insertFont(OPENDYSLEXIC_14_FONT_ID, opendyslexic14FontFamily);
#endif  // OMIT_FONTS
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);
  Serial.printf("[%lu] [   ] Fonts setup\n", millis());
}

void setup() {
    // force serial for debugging
  Serial.begin(115200);
  delay(500);
  Serial.printf("[%lu] [DBG] setup() start - FIRMWARE DEBUG BUILD 001\n", millis());
  Serial.flush();

  t1 = millis();

  gpio.begin();

  // Only start serial if USB connected
  if (gpio.isUsbConnected()) {
    Serial.begin(115200);
    // Wait up to 3 seconds for Serial to be ready to catch early logs
    unsigned long start = millis();
    while (!Serial && (millis() - start) < 3000) {
      delay(10);
    }
  }

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!SdMan.begin()) {
    Serial.printf("[%lu] [   ] SD card initialization failed\n", millis());
    setupDisplayAndFonts();
    exitActivity();
    enterNewActivity(new FullScreenMessageActivity(renderer, mappedInputManager, "SD card error", EpdFontFamily::BOLD));
    return;
  }

  SETTINGS.loadFromFile();
  KOREADER_STORE.loadFromFile();
  UITheme::getInstance().reload();

  ButtonNavigator::setMappedInputManager(mappedInputManager);
  
  // Initialize Bluetooth HID button injection
  try {
    auto& btMgr = BluetoothHIDManager::getInstance();
    btMgr.setButtonInjector([](uint8_t buttonIndex) {
      gpio.injectButtonPress(buttonIndex);
    });
    
    // Enable Bluetooth on boot if configured
    if (SETTINGS.bluetoothEnabled) {
      if (btMgr.enable()) {
        Serial.printf("MAIN", "Bluetooth enabled on boot");

        // Auto-reconnect: attempt to connect to the last paired device up to 3 times
        std::string lastAddr, lastName;
        btMgr.startScan(2000);
        if (btMgr.loadLastConnectedDevice(lastAddr, lastName)) {
          Serial.printf("MAIN", "Auto-connecting to last device %s (%s)", lastName.c_str(), lastAddr.c_str());
          if (btMgr.connectToDeviceWithRetries(lastAddr, 1)) {
            Serial.printf("MAIN", "Auto-connect successful");
          } else {
            Serial.printf("MAIN", "Auto-connect failed after retries");
          }
        }

      } else {
        Serial.printf("MAIN", "Failed to enable Bluetooth on boot");
      }
    }
    
    Serial.printf("MAIN", "Bluetooth HID initialized with button injection");
  } catch (...) {
    Serial.printf("MAIN", "Failed to initialize Bluetooth HID");
  }

  switch (gpio.getWakeupReason()) {
    case HalGPIO::WakeupReason::PowerButton:
      // For normal wakeups, verify power button press duration
      Serial.printf("[%lu] [   ] Verifying power button press duration\n", millis());
      //verifyPowerButtonDuration();
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // If USB power caused a cold boot, go back to sleep
      Serial.printf("[%lu] [   ] Wakeup reason: After USB Power\n", millis());
      gpio.startDeepSleep();
      break;
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
    case HalGPIO::WakeupReason::Other:
    default:
      break;
  }

  // First serial output only here to avoid timing inconsistencies for power button press duration verification
  Serial.printf("[%lu] [   ] Starting CrossPoint version " CROSSPOINT_VERSION "\n", millis());

  setupDisplayAndFonts();
  Serial.printf("[%lu] [DBG] setupDisplayAndFonts done\n", millis());
  Serial.flush();

  EpdFontLoader::loadFontsFromSd(renderer);
  Serial.printf("[%lu] [DBG] loadFontsFromSd done\n", millis());
  Serial.flush();

  exitActivity();
  enterNewActivity(new BootActivity(renderer, mappedInputManager));

  APP_STATE.loadFromFile();
  RECENT_BOOKS.loadFromFile();

  // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
  // crashed (indicated by readerActivityLoadCount > 0)
  if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
      mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
        Serial.printf("home1\n");
    onGoHome();
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    Serial.printf("reader\n");
    onGoToReader(path);
    //onGoHome();
  }

  // Ensure we're not still holding the power button before leaving setup
  waitForPowerRelease();
}



void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  gpio.update();

    // Check for Bluetooth inactivity timeouts and auto-reconnect
  try {
    BluetoothHIDManager::getInstance().updateActivity();
    BluetoothHIDManager::getInstance().checkAutoReconnect();
  } catch (...) {
    // Ignore errors in Bluetooth management
  }

  renderer.setFadingFix(SETTINGS.fadingFix);

  if (Serial && millis() - lastMemPrint >= 10000) {
    Serial.printf("[%lu] [MEM] Free: %d bytes, Total: %d bytes, Min Free: %d bytes\n", millis(), ESP.getFreeHeap(),
                  ESP.getHeapSize(), ESP.getMinFreeHeap());
    lastMemPrint = millis();
  }

  // Check for any user activity (button press or release) or active background work
  static unsigned long lastActivityTime = millis();
  // Check for physical button presses, virtual button presses, or activity prevention
  bool hasActivity = gpio.wasAnyPressed() || gpio.wasAnyReleased() || 
                     (currentActivity && currentActivity->preventAutoSleep());
  
  // Also check for recent BLE activity to prevent power sleep during BLE use
  try {
    const auto& btMgr = BluetoothHIDManager::getInstance();
    if (btMgr.isEnabled()) {
      // If BLE is enabled, check if there's been recent activity
      // We consider that activity if the manager has been tracking it
      // (This prevents the system from sleeping while using BLE controller)
      hasActivity = hasActivity || btMgr.hasRecentActivity();
    }
  } catch (...) {
    // Ignore BLE check errors
  }
  
  if (hasActivity) {
    lastActivityTime = millis();  // Reset inactivity timer
  }

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (millis() - lastActivityTime >= sleepTimeoutMs) {
    Serial.printf("[%lu] [SLP] Auto-sleep triggered after %lu ms of inactivity\n", millis(), sleepTimeoutMs);
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.getHeldTime() > SETTINGS.getPowerButtonDuration()) {
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  const unsigned long activityStartTime = millis();
  if (currentActivity) {
    currentActivity->loop();
  }
  const unsigned long activityDuration = millis() - activityStartTime;

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      Serial.printf("[%lu] [LOOP] New max loop duration: %lu ms (activity: %lu ms)\n", millis(), maxLoopDuration,
                    activityDuration);
    }
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (currentActivity && currentActivity->skipLoopDelay()) {
    yield();  // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    delay(10);  // Normal delay when no activity requires fast response
  }
}
