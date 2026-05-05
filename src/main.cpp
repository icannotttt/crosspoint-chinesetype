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

// 全局：只用来保存一次屏幕
uint8_t* g_savedBWBuffer = nullptr;
size_t g_bufferSize = 0;

// 直接用已保存的缓冲区生成 BMP（不再新分配内存）
bool saveBmpFromSavedBuffer(const char* filename) {
  if (!g_savedBWBuffer || g_bufferSize == 0) return false;

  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const int bmpBytesPerRow = (width + 7) / 8;
  const int bmpRowAligned = (bmpBytesPerRow + 3) & ~3;
  const size_t bmpSize = (size_t)bmpRowAligned * height;

  auto rotateLogicalToPhysical = [&](int x, int y, int* phyX, int* phyY) {
    switch (renderer.getOrientation()) {
      case GfxRenderer::Portrait:
        *phyX = y;
        *phyY = HalDisplay::DISPLAY_HEIGHT - 1 - x;
        break;
      case GfxRenderer::LandscapeClockwise:
        *phyX = HalDisplay::DISPLAY_WIDTH - 1 - x;
        *phyY = HalDisplay::DISPLAY_HEIGHT - 1 - y;
        break;
      case GfxRenderer::PortraitInverted:
        *phyX = HalDisplay::DISPLAY_WIDTH - 1 - y;
        *phyY = x;
        break;
      case GfxRenderer::LandscapeCounterClockwise:
        *phyX = x;
        *phyY = y;
        break;
    }
  };

  FsFile file;
  if (!SdMan.openFileForWrite("SCP", filename, file)) {
    return false;
  }

  const uint32_t fileSize = 14 + 40 + 8 + bmpSize;
  const uint32_t pixelDataOffset = 14 + 40 + 8;

  uint8_t fileHeader[14] = {'B','M', 0,0,0,0, 0,0, 0,0, 0,0,0,0};
  fileHeader[2] = fileSize & 0xFF;
  fileHeader[3] = (fileSize >> 8) & 0xFF;
  fileHeader[4] = (fileSize >> 16) & 0xFF;
  fileHeader[5] = (fileSize >> 24) & 0xFF;
  fileHeader[10] = pixelDataOffset & 0xFF;
  fileHeader[11] = (pixelDataOffset >> 8) & 0xFF;

  uint8_t infoHeader[40] = {0};
  infoHeader[0] = 40;
  infoHeader[4] = width & 0xFF;
  infoHeader[5] = (width >> 8) & 0xFF;
  infoHeader[6] = (width >> 16) & 0xFF;
  infoHeader[7] = (width >> 24) & 0xFF;
  infoHeader[8] = height & 0xFF;
  infoHeader[9] = (height >> 8) & 0xFF;
  infoHeader[10] = (height >> 16) & 0xFF;
  infoHeader[11] = (height >> 24) & 0xFF;
  infoHeader[12] = 1;
  infoHeader[14] = 1;
  infoHeader[32] = 2;
  infoHeader[36] = 2;

  const uint8_t palette[8] = {0,0,0,0, 255,255,255,0};
  file.write(fileHeader, 14);
  file.write(infoHeader, 40);
  file.write(palette, 8);

  // Stream BMP rows directly to SD to avoid allocating a full-frame BMP buffer.
  uint8_t* rowBuf = static_cast<uint8_t*>(malloc(bmpRowAligned));
  if (!rowBuf) {
    Serial.printf("[SCP] 行缓冲分配失败\n");
    file.close();
    return false;
  }

  const uint8_t* frameBuffer = g_savedBWBuffer;
  for (int row = 0; row < height; row++) {
    memset(rowBuf, 0xFF, bmpRowAligned);

    // Positive BMP height means rows are stored bottom-up.
    const int logicalY = height - 1 - row;

    for (int x = 0; x < width; x++) {
      int rx = 0;
      int ry = 0;
      rotateLogicalToPhysical(x, logicalY, &rx, &ry);
      if (rx < 0 || rx >= HalDisplay::DISPLAY_WIDTH || ry < 0 || ry >= HalDisplay::DISPLAY_HEIGHT) {
        continue;
      }

      const size_t fbByteIdx = (size_t)ry * HalDisplay::DISPLAY_WIDTH_BYTES + (rx / 8);
      const uint8_t fbBitPos = 7 - (rx % 8);
      const bool isBlack = ((frameBuffer[fbByteIdx] >> fbBitPos) & 0x01) == 0;

      const size_t outByteIdx = static_cast<size_t>(x / 8);
      const uint8_t outBitPos = 7 - (x % 8);
      if (isBlack) {
        rowBuf[outByteIdx] &= ~(1U << outBitPos);
      }
    }

    file.write(rowBuf, bmpRowAligned);
  }

  free(rowBuf);
  file.flush();
  file.sync();
  file.close();
  return true;
}

bool captureGlobalScreenshot() {
  // 0. 先获取真实帧缓冲
  uint8_t* mainFB = renderer.getFrameBuffer();
  size_t fbSize = renderer.getBufferSize();
  if (!mainFB || fbSize == 0) return false;

  // ============================
  // 1. 保存当前BW层 → **只 malloc 这一次！**
  // ============================
  g_savedBWBuffer = (uint8_t*)malloc(fbSize);
  g_bufferSize = fbSize;
  if (!g_savedBWBuffer) {
    Serial.printf("[SCP] 内存不足，无法保存屏幕\n");
    return false;
  }
  memcpy(g_savedBWBuffer, mainFB, fbSize);

  // 2. 生成截图文件名
  SdMan.mkdir("/screenshots");
  uint32_t stamp = millis();
  std::string filePath = "/screenshots/screen_" + std::to_string(stamp) + ".bmp";

  // 3. 用已保存的缓冲区写入BMP
  bool ok = saveBmpFromSavedBuffer(filePath.c_str());
  if (!ok) {
    free(g_savedBWBuffer);
    g_savedBWBuffer = nullptr;
    return false;
  }

  // ============================
  // 4. 绘制“已截屏”提示
  // ============================
  int boxW = 180, boxH = 44;
  int boxX = (renderer.getScreenWidth() - boxW) / 2;
  int boxY = 24;
  renderer.fillRect(boxX, boxY, boxW, boxH, false);
  renderer.drawRect(boxX, boxY, boxW, boxH, true);
  renderer.drawCenteredText(UI_10_FONT_ID, boxY+13, "已截屏", true, EpdFontFamily::BOLD);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  delay(1000);

  // ============================
  // 5. 用保存的BW层恢复之前界面 ✅
  // ============================
  memcpy(mainFB, g_savedBWBuffer, fbSize);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);

  // ============================
  // 6. 释放保存的BW层 ✅
  // ============================
  free(g_savedBWBuffer);
  g_savedBWBuffer = nullptr;

  Serial.printf("[SCP] 截图完成，内存已释放\n");
  return true;
}

// Two-phase clean refresh for the current screen content:
// 1) clear to white and FAST refresh, 2) restore previous frame and FAST refresh.
bool performCleanRefreshFromCurrentFrame() {
  uint8_t* mainFB = renderer.getFrameBuffer();
  size_t fbSize = renderer.getBufferSize();
  if (!mainFB || fbSize == 0) {
    return false;
  }

  uint8_t* savedBuffer = static_cast<uint8_t*>(malloc(fbSize));
  if (!savedBuffer) {
    Serial.printf("[PWR] Clean refresh failed: not enough memory\n");
    return false;
  }

  memcpy(savedBuffer, mainFB, fbSize);

  renderer.clearScreen(0xFF);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);

  memcpy(mainFB, savedBuffer, fbSize);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);

  free(savedBuffer);
  return true;
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

  switch (gpio.getWakeupReason()) {
    case HalGPIO::WakeupReason::PowerButton:
      // Verify hold duration before any heavy initialization (e.g. BLE scan/connect),
      // otherwise delayed verification may incorrectly force deep sleep.
      Serial.printf("[%lu] [   ] Verifying power button press duration\n", millis());
      verifyPowerButtonDuration();
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
  static bool powerShortClickPending = false;
  static unsigned long powerLastReleaseMs = 0;
  static uint8_t pendingShortPwrAction = CrossPointSettings::SHORT_PWRBTN::IGNORE;



  constexpr unsigned long POWER_DOUBLE_CLICK_WINDOW_MS = 350;

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

  const bool powerReleased = mappedInputManager.wasReleased(MappedInputManager::Button::Power);
  const unsigned long nowMs = millis();

  if (powerReleased) {
    if (powerShortClickPending && (nowMs - powerLastReleaseMs) <= POWER_DOUBLE_CLICK_WINDOW_MS) {
      powerShortClickPending = false;

      // Double-click cycles short power action:
      // IGNORE -> PAGE_TURN -> FULL_REFRESH -> SCREENSHOT -> WIFI_TRANSFER -> IGNORE.
      switch (SETTINGS.shortPwrBtn) {
        case CrossPointSettings::SHORT_PWRBTN::IGNORE:
          SETTINGS.shortPwrBtn = CrossPointSettings::SHORT_PWRBTN::PAGE_TURN;
          break;
        case CrossPointSettings::SHORT_PWRBTN::PAGE_TURN:
          SETTINGS.shortPwrBtn = CrossPointSettings::SHORT_PWRBTN::FULL_REFRESH;
          break;
        case CrossPointSettings::SHORT_PWRBTN::FULL_REFRESH:
          SETTINGS.shortPwrBtn = CrossPointSettings::SHORT_PWRBTN::SCREENSHOT;
          break;
        case CrossPointSettings::SHORT_PWRBTN::SCREENSHOT:
          SETTINGS.shortPwrBtn = CrossPointSettings::SHORT_PWRBTN::WIFI_TRANSFER;
          break;
        case CrossPointSettings::SHORT_PWRBTN::WIFI_TRANSFER:
        default:
          SETTINGS.shortPwrBtn = CrossPointSettings::SHORT_PWRBTN::IGNORE;
          break;
      }

      SETTINGS.saveToFile();
      const char* shortPwrName = "忽略";
      switch (SETTINGS.shortPwrBtn) {
        case CrossPointSettings::SHORT_PWRBTN::PAGE_TURN:
          shortPwrName = "翻页";
          break;
        case CrossPointSettings::SHORT_PWRBTN::FULL_REFRESH:
          shortPwrName = "全刷";
          break;
        case CrossPointSettings::SHORT_PWRBTN::SCREENSHOT:
          shortPwrName = "截屏";
          break;
        case CrossPointSettings::SHORT_PWRBTN::WIFI_TRANSFER:
          shortPwrName = "wifi传书";
          break;
        case CrossPointSettings::SHORT_PWRBTN::IGNORE:
        default:
          shortPwrName = "忽略";
          break;
      }
      Serial.printf("[%lu] [PWR] Double-click: short power action switched to %s\n", nowMs,
                    shortPwrName);
        // ============================
        int boxW = 180, boxH = 44;
        int boxX = (renderer.getScreenWidth() - boxW) / 2;
        int boxY = 24;
        renderer.fillRect(boxX, boxY, boxW, boxH, false);
        renderer.drawRect(boxX, boxY, boxW, boxH, true);
        char switchMsg[64];
        snprintf(switchMsg, sizeof(switchMsg), "已切换为%s", shortPwrName);
        renderer.drawCenteredText(UI_10_FONT_ID, boxY + 13, switchMsg, true, EpdFontFamily::BOLD);
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
        delay(1000);
    } else {
      powerShortClickPending = true;
      powerLastReleaseMs = nowMs;
      pendingShortPwrAction = SETTINGS.shortPwrBtn;
    }
  }

  // Delay single-click handling to ensure a potential second click can be detected.
  if (powerShortClickPending && (nowMs - powerLastReleaseMs) > POWER_DOUBLE_CLICK_WINDOW_MS) {
    powerShortClickPending = false;

    if (pendingShortPwrAction == CrossPointSettings::SHORT_PWRBTN::FULL_REFRESH) {

      //还是先改回半刷
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    } else if (pendingShortPwrAction == CrossPointSettings::SHORT_PWRBTN::SCREENSHOT) {
      captureGlobalScreenshot();
    } else if (pendingShortPwrAction == CrossPointSettings::SHORT_PWRBTN::WIFI_TRANSFER) {
      onGoToFileTransfer();
    }
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
