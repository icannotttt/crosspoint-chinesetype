#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <SDCardManager.h>
#include <Serialization.h>
#include <BluetoothHIDManager.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubBookmarkSelectionActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "BookmarkActivity.h"
#include "../settings/BluetoothSettingsActivity.h"
#include "../../util/ReaderBluetoothBootstrap.h"
#include "components/UITheme.h"
#include "fontIds.h"

#include "JianGuoSyncActivity.h"


namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
constexpr unsigned long skipChapterMs = 700;
constexpr unsigned long goHomeMs = 1000;
constexpr int statusBarMargin = 20;
constexpr int progressBarMarginTop = 1;
constexpr uint32_t WALLPAPER_PXC_MAGIC = 0x31584350;   // "PXC1"
constexpr uint16_t WALLPAPER_PXC_VERSION = 1;
constexpr char WALLPAPER_PXC_PATH[] = "/.crosspoint/wallpaper_bg.pxc";
constexpr uint8_t WALLPAPER_PXC_FIXED_ORIENTATION = CrossPointSettings::ORIENTATION::PORTRAIT;

bool loadWallpaperPxcToFramebuffer(const std::string& pxcPath, GfxRenderer& renderer) {
  FsFile input;
  if (!SdMan.openFileForRead("SLP", pxcPath, input)) {
    return false;
  }

  uint32_t magic = 0;
  uint16_t version = 0;
  uint8_t cachedOrientation = 0;
  uint8_t reserved = 0;
  uint32_t payloadSize = 0;

  serialization::readPod(input, magic);
  serialization::readPod(input, version);
  serialization::readPod(input, cachedOrientation);
  serialization::readPod(input, reserved);
  serialization::readPod(input, payloadSize);

  const uint32_t expectedPayload = static_cast<uint32_t>(renderer.getBufferSize());
    if (magic != WALLPAPER_PXC_MAGIC || version != WALLPAPER_PXC_VERSION ||
      cachedOrientation != WALLPAPER_PXC_FIXED_ORIENTATION ||
      payloadSize != expectedPayload) {
    input.close();
    return false;
  }

  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    input.close();
    return false;
  }

  size_t totalRead = 0;
  while (totalRead < payloadSize) {
    const size_t toRead = std::min(static_cast<size_t>(1024), static_cast<size_t>(payloadSize - totalRead));
    const int bytesRead = input.read(frameBuffer + totalRead, toRead);
    if (bytesRead <= 0) {
      input.close();
      return false;
    }
    totalRead += static_cast<size_t>(bytesRead);
  }

  input.close();
  return true;
}




bool saveWallpaperPxcFromFramebuffer(const std::string& pxcPath, GfxRenderer& renderer) {
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return false;
  }

  SdMan.mkdir("/.crosspoint");

  FsFile output;
  if (!SdMan.openFileForWrite("SLP", pxcPath, output)) {
    return false;
  }

  const uint32_t payloadSize = static_cast<uint32_t>(renderer.getBufferSize());
  const uint8_t reserved = 0;

  serialization::writePod(output, WALLPAPER_PXC_MAGIC);
  serialization::writePod(output, WALLPAPER_PXC_VERSION);
  serialization::writePod(output, WALLPAPER_PXC_FIXED_ORIENTATION);
  serialization::writePod(output, reserved);
  serialization::writePod(output, payloadSize);

  size_t totalWritten = 0;
  while (totalWritten < payloadSize) {
    const size_t toWrite = std::min(static_cast<size_t>(1024), static_cast<size_t>(payloadSize - totalWritten));
    const size_t bytesWritten = output.write(frameBuffer + totalWritten, toWrite);
    if (bytesWritten != toWrite) {
      output.close();
      return false;
    }
    totalWritten += bytesWritten;
  }

  output.sync();
  output.close();
  return true;
}



int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

uint8_t clampAutoPageTurnSeconds(const uint8_t seconds) {
  if (seconds < CrossPointSettings::AUTO_PAGE_TURN_TIME_MIN) {
    return CrossPointSettings::AUTO_PAGE_TURN_TIME_MIN;
  }
  if (seconds > CrossPointSettings::AUTO_PAGE_TURN_TIME_MAX) {
    return CrossPointSettings::AUTO_PAGE_TURN_TIME_MAX;
  }
  return seconds;
}

bool shouldTriggerAutoPageTurn(unsigned long& lastTriggerMs) {
  const unsigned long now = millis();
  if (!SETTINGS.autoPageTurn) {
    lastTriggerMs = now;
    return false;
  }

  const uint8_t seconds = clampAutoPageTurnSeconds(SETTINGS.autoPageTurnTime);
  if (now - lastTriggerMs >= static_cast<unsigned long>(seconds) * 1000UL) {
    lastTriggerMs = now;
    return true;
  }
  return false;
}



// Apply the logical reader orientation to the renderer.
// This centralizes orientation mapping so we don't duplicate switch logic elsewhere.
void applyReaderOrientation(GfxRenderer& renderer, const uint8_t orientation) {
  switch (orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }
}

}  // namespace

EpubReaderActivity::EPUBState EpubReaderActivity::state;

void EpubReaderActivity::taskTrampoline(void* param) {
  auto* self = static_cast<EpubReaderActivity*>(param);
  self->displayTaskLoop();
}

void EpubReaderActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  state = EPUBState::READING;


  if (!epub) {
    return;
  }

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  applyReaderOrientation(renderer, SETTINGS.orientation);

  renderingMutex = xSemaphoreCreateMutex();

  epub->setupCacheDir();


  FsFile f;
  if (SdMan.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
            // Validation: If loaded index is invalid, reset to 0
      if (currentSpineIndex >= epub->getSpineItemsCount()) {
        Serial.printf("[%lu] [ERS] Loaded invalid spine index %d (max %d), resetting\n", millis(), currentSpineIndex,
                      epub->getSpineItemsCount());
        currentSpineIndex = 0;
        nextPageNumber = 0;
      }
      cachedSpineIndex = currentSpineIndex;
      Serial.printf("[%lu] [ERS] Loaded cache: %d, %d\n", millis(), currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    }
    f.close();
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      Serial.printf("[%lu] [ERS] Opened for first time, navigating to text reference at index %d\n", millis(),
                    textSpineIndex);
    }
  }

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());

  // Trigger first update
  updateRequired = true;
  lastAutoPageTurnMs = millis();

  xTaskCreate(&EpubReaderActivity::taskTrampoline, "EpubReaderActivityTask",
              8192,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void EpubReaderActivity::onExit() {
  ActivityWithSubactivity::onExit();

  // Force-release BLE resources when leaving reader to maximize memory for parsing/rendering.
  auto& btMgr = BluetoothHIDManager::getInstance();
  if (btMgr.isEnabled()) {
    btMgr.disable();
  }

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  section.reset();
  epub.reset();
}

void EpubReaderActivity::loop() {
  if(state== EPUBState::READING){
    // Pass input responsibility to sub activity if exists
    if (subActivity) {
      lastAutoPageTurnMs = millis();
      subActivity->loop();
      // Deferred exit: process after subActivity->loop() returns to avoid use-after-free
      if (pendingSubactivityExit) {
        pendingSubactivityExit = false;
        exitActivity();
        updateRequired = true;
        skipNextButtonCheck = true;  // Skip button processing to ignore stale events
      }
      // Deferred go home: process after subActivity->loop() returns to avoid race condition
      if (pendingGoHome) {
        pendingGoHome = false;
        exitActivity();
        if (onGoHome) {
          onGoHome();
        }
        return;  // Don't access 'this' after callback
      }
      return;
    }

    if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= 1000) {
      Serial.printf("[%lu] [ERS] Long press detected, entering settings\n", millis());
      state = EPUBState::SETTING;
      skipNextButtonCheck = true; // 避免按钮事件冲突
      updateRequired = true;
      return;
    }

    // Handle pending go home when no subactivity (e.g., from long press back)
    if (pendingGoHome) {
      pendingGoHome = false;
      if (onGoHome) {
        onGoHome();
      }
      return;  // Don't access 'this' after callback
    }

    // Skip button processing after returning from subactivity
    // This prevents stale button release events from triggering actions
    // We wait until: (1) all relevant buttons are released, AND (2) wasReleased events have been cleared
    if (skipNextButtonCheck) {
      lastAutoPageTurnMs = millis();
      const bool confirmCleared = !mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
                                  !mappedInput.wasReleased(MappedInputManager::Button::Confirm);
      const bool backCleared = !mappedInput.isPressed(MappedInputManager::Button::Back) &&
                              !mappedInput.wasReleased(MappedInputManager::Button::Back);
      if (confirmCleared && backCleared) {
        skipNextButtonCheck = false;
      }
      return;
    }

    // Enter reader menu activity.
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Don't start activity transition while rendering
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      const int currentPage = section ? section->currentPage + 1 : 0;
      const int totalPages = section ? section->pageCount : 0;
      float bookProgress = 0.0f;
      if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      exitActivity();
      enterNewActivity(new EpubReaderMenuActivity(
          this->renderer, this->mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent,
          SETTINGS.orientation, [this](const uint8_t orientation) { onReaderMenuBack(orientation); },
          [this](EpubReaderMenuActivity::MenuAction action) { onReaderMenuConfirm(action); }));
      xSemaphoreGive(renderingMutex);
    }

    // Long press BACK (1s+) goes directly to home
    if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= goHomeMs) {
      onGoHome();
      return;
    }

    // Short press BACK goes to file selection
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) && mappedInput.getHeldTime() < goHomeMs) {
      onGoBack();
      return;
    }

    // When long-press chapter skip is disabled, turn pages on press instead of release.
    const bool usePressForPageTurn = !SETTINGS.longPressChapterSkip;
    const bool prevTriggered = usePressForPageTurn ? (mappedInput.wasPressed(MappedInputManager::Button::PageBack) ||
                                                      mappedInput.wasPressed(MappedInputManager::Button::Left))
                                                  : (mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                                                      mappedInput.wasReleased(MappedInputManager::Button::Left));
    const bool powerPageTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
                              mappedInput.wasReleased(MappedInputManager::Button::Power);
    const bool autoTurnTriggered = shouldTriggerAutoPageTurn(lastAutoPageTurnMs);
    const bool nextTriggered = usePressForPageTurn
                                  ? (mappedInput.wasPressed(MappedInputManager::Button::PageForward) || powerPageTurn ||
                                      mappedInput.wasPressed(MappedInputManager::Button::Right) || autoTurnTriggered)
                                  : (mappedInput.wasReleased(MappedInputManager::Button::PageForward) || powerPageTurn ||
                                      mappedInput.wasReleased(MappedInputManager::Button::Right) || autoTurnTriggered);

    if (!prevTriggered && !nextTriggered) {
      return;
    }

    // any botton press when at end of the book goes back to the last page
    if (!autoTurnTriggered && currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
      currentSpineIndex = epub->getSpineItemsCount() - 1;
      nextPageNumber = UINT16_MAX;
      updateRequired = true;
      return;
    }

    const bool skipChapter = SETTINGS.longPressChapterSkip && mappedInput.getHeldTime() > skipChapterMs;

    if (skipChapter) {
      // We don't want to delete the section mid-render, so grab the semaphore
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      nextPageNumber = 0;
      currentSpineIndex = nextTriggered ? currentSpineIndex + 1 : currentSpineIndex - 1;
      section.reset();
      xSemaphoreGive(renderingMutex);
      updateRequired = true;
      return;
    }

    // No current section, attempt to rerender the book
    if (!section) {
      updateRequired = true;
      return;
    }

    if (prevTriggered) {
      lastAutoPageTurnMs = millis();
      if (section->currentPage > 0) {
        section->currentPage--;
      } else {
        // We don't want to delete the section mid-render, so grab the semaphore
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        nextPageNumber = UINT16_MAX;
        currentSpineIndex--;
        section.reset();
        xSemaphoreGive(renderingMutex);
      }
      updateRequired = true;
    } else {
      if (!autoTurnTriggered) {
        lastAutoPageTurnMs = millis();
      }
      if (section->currentPage < section->pageCount - 1) {
        section->currentPage++;
      } else {
        // We don't want to delete the section mid-render, so grab the semaphore
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        nextPageNumber = 0;
        currentSpineIndex++;
        section.reset();
        xSemaphoreGive(renderingMutex);
      }
      updateRequired = true;
    }
    //暂不启用，易起冲突，后面修改
  }else if (state == EPUBState::SETTING) {
    lastAutoPageTurnMs = millis();
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ) {
      Serial.printf("[%lu] [ERS] Long press detected, entering reading\n", millis());
      if (pendingMarginRelayout) {
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        if (section) {
          cachedSpineIndex = currentSpineIndex;
          cachedChapterTotalPageCount = section->pageCount;
          nextPageNumber = section->currentPage;
        }
        section.reset();
        xSemaphoreGive(renderingMutex);
        pendingMarginRelayout = false;
      }
      state = EPUBState::READING;
      skipNextButtonCheck = true;
      SETTINGS.saveToFile();
      updateRequired = true;
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      Serial.printf("[%lu] [ERS] 进入左边距设置\n", millis());
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      SETTINGS.screenMargin_Left+=5;
      xSemaphoreGive(renderingMutex);
      pendingMarginRelayout = true;
      updateRequired = true;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      Serial.printf("[%lu] [ERS] 进入右边距设置\n", millis());
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      SETTINGS.screenMargin_Right+=5;
      xSemaphoreGive(renderingMutex);

      pendingMarginRelayout = true;
      updateRequired = true;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      Serial.printf("[%lu] [ERS] 进入上边距设置\n", millis());
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      SETTINGS.screenMargin_Top+=5;
      xSemaphoreGive(renderingMutex);
      pendingMarginRelayout = true;
      updateRequired = true;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      Serial.printf("[%lu] [ERS] 进入下边距设置\n", millis());
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      SETTINGS.screenMargin_Bottom+=5;
      Serial.printf("[%lu] [ERS] Bottom为%d\n", millis(), SETTINGS.screenMargin_Bottom);
      xSemaphoreGive(renderingMutex);
      pendingMarginRelayout = true;
      updateRequired = true;
    }
    if (mappedInput.isPressed(MappedInputManager::Button::Left) && mappedInput.getHeldTime() >= 2000) {
      Serial.printf("[%lu] [ERS] 进入左边距设置\n", millis());
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      SETTINGS.screenMargin_Left-=5;
      xSemaphoreGive(renderingMutex);
      pendingMarginRelayout = true;
      updateRequired = true;
    }
    if (mappedInput.isPressed(MappedInputManager::Button::Right) && mappedInput.getHeldTime() >= 2000) {
      Serial.printf("[%lu] [ERS] 进入右边距设置\n", millis());
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      SETTINGS.screenMargin_Right-=5;
      xSemaphoreGive(renderingMutex);

      pendingMarginRelayout = true;
      updateRequired = true;
    }
    if (mappedInput.isPressed(MappedInputManager::Button::Up) && mappedInput.getHeldTime() >= 2000) {
      Serial.printf("[%lu] [ERS] 进入上边距设置\n", millis());
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      SETTINGS.screenMargin_Top-=5;
      xSemaphoreGive(renderingMutex);
      pendingMarginRelayout = true;
      updateRequired = true;
    }
    if (mappedInput.isPressed(MappedInputManager::Button::Down) && mappedInput.getHeldTime() >= 2000) {
      Serial.printf("[%lu] [ERS] 进入下边距设置\n", millis());
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      SETTINGS.screenMargin_Bottom-=5;
      Serial.printf("[%lu] [ERS] Bottom为%d\n", millis(), SETTINGS.screenMargin_Bottom);
      xSemaphoreGive(renderingMutex);
      pendingMarginRelayout = true;
      updateRequired = true;
    }
}


}

void EpubReaderActivity::onReaderMenuBack(const uint8_t orientation) {
  exitActivity();
  // Apply the user-selected orientation when the menu is dismissed.
  // This ensures the menu can be navigated without immediately rotating the screen.
  applyOrientation(orientation);
  updateRequired = true;
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize % 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  // Store a normalized position within the spine so it can be applied once loaded.
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  // Reset state so renderScreen() reloads and repositions on the target spine.
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  currentSpineIndex = targetSpineIndex;
  nextPageNumber = 0;
  pendingPercentJump = true;
  section.reset();
  xSemaphoreGive(renderingMutex);
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      // Calculate values BEFORE we start destroying things
      const int currentP = section ? section->currentPage : 0;
      const int totalP = section ? section->pageCount : 0;
      const int spineIdx = currentSpineIndex;
      const std::string path = epub->getPath();

      xSemaphoreTake(renderingMutex, portMAX_DELAY);

      // 1. Close the menu
      exitActivity();

      // 2. Open the Chapter Selector
      enterNewActivity(new EpubReaderChapterSelectionActivity(
          this->renderer, this->mappedInput, epub, path, spineIdx, currentP, totalP,
          [this] {
            exitActivity();
            updateRequired = true;
          },
          [this](const int newSpineIndex) {
            if (currentSpineIndex != newSpineIndex) {
              currentSpineIndex = newSpineIndex;
              nextPageNumber = 0;
              section.reset();
            }
            exitActivity();
            updateRequired = true;
          },
          [this](const int newSpineIndex, const int newPage) {
            if (currentSpineIndex != newSpineIndex || (section && section->currentPage != newPage)) {
              currentSpineIndex = newSpineIndex;
              nextPageNumber = newPage;
              section.reset();
            }
            exitActivity();
            updateRequired = true;
          }));

      xSemaphoreGive(renderingMutex);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      // Launch the slider-based percent selector and return here on confirm/cancel.
      float bookProgress = 0.0f;
      if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new EpubReaderPercentSelectionActivity(
          renderer, mappedInput, initialPercent,
          [this](const int percent) {
            // Apply the new position and exit back to the reader.
            jumpToPercent(percent);
            exitActivity();
            updateRequired = true;
          },
          [this]() {
            // Cancel selection and return to the reader.
            exitActivity();
            updateRequired = true;
          }));
      xSemaphoreGive(renderingMutex);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARK_LIST: {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new EpubBookmarkSelectionActivity(
          renderer, mappedInput, epub,
          [this]() {
            exitActivity();
            updateRequired = true;
          },
          [this](const BookmarkStore::BookmarkRecord& record) {
            const int spineCount = epub ? epub->getSpineItemsCount() : 0;
            const int bookmarkSpine = static_cast<int>(record.pos1);
            const int bookmarkPage = static_cast<int>(record.pos2);
            const int bookmarkPageCount = static_cast<int>(record.pos3);

            if (spineCount > 0 && bookmarkSpine >= 0 && bookmarkSpine < spineCount && bookmarkPage >= 0) {
              xSemaphoreTake(renderingMutex, portMAX_DELAY);
              currentSpineIndex = bookmarkSpine;
              nextPageNumber = bookmarkPage;
              // Keep cached spine metadata aligned so section reload can remap page by ratio
              // when page count changes after font/spacing/margin updates.
              cachedSpineIndex = bookmarkSpine;
              cachedChapterTotalPageCount = bookmarkPageCount > 0 ? bookmarkPageCount : 0;
              section.reset();
              xSemaphoreGive(renderingMutex);
            } else {
              // Fallback for old or malformed bookmark records.
              jumpToPercent(static_cast<int>(record.progressPercent));
            }
            exitActivity();
            updateRequired = true;
          }));
      xSemaphoreGive(renderingMutex);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::ADD_BOOKMARK: {
      float bookProgress = 0.0f;
      int currentPage = 0;
      int totalPages = 0;
      if (section) {
        currentPage = section->currentPage;
        totalPages = section->pageCount;
      }
      if (epub && epub->getBookSize() > 0 && totalPages > 0) {
        const float chapterProgress = static_cast<float>(currentPage) / static_cast<float>(totalPages);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int percent = clampPercent(static_cast<int>(bookProgress + 0.5f));

      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new BookmarkActivity(
          renderer, mappedInput, epub->getPath(), epub->getCachePath(), percent, currentSpineIndex, currentPage,
          totalPages, [this](bool) {
            exitActivity();
            updateRequired = true;
          }));
      xSemaphoreGive(renderingMutex);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BLUETOOTH: {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new BluetoothSettingsActivity(
          renderer, mappedInput,
          [this] {
            exitActivity();
            skipNextButtonCheck = true;
            updateRequired = true;
          }));
      xSemaphoreGive(renderingMutex);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::AUTO_PAGE_TURN_TOGGLE:
    case EpubReaderMenuActivity::MenuAction::AUTO_PAGE_TURN_TIME:
      // These actions are handled directly inside EpubReaderMenuActivity.
      break;
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      // Defer go home to avoid race condition with display task
      pendingGoHome = true;
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      if (epub) {
        // 2. BACKUP: Read current progress
        // We use the current variables that track our position
        uint16_t backupSpine = currentSpineIndex;
        uint16_t backupPage = section->currentPage;
        uint16_t backupPageCount = section->pageCount;

        section.reset();
        // 3. WIPE: Clear the cache directory
        epub->clearCache();

        // 4. RESTORE: Re-setup the directory and rewrite the progress file
        epub->setupCacheDir();

        saveProgress(backupSpine, backupPage, backupPageCount);
      }
      xSemaphoreGive(renderingMutex);
      // Defer go home to avoid race condition with display task
      pendingGoHome = true;
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      if (KOREADER_STORE.hasCredentials()) {
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        const int currentPage = section ? section->currentPage : 0;
        const int totalPages = section ? section->pageCount : 0;
        exitActivity();
        enterNewActivity(new KOReaderSyncActivity(
            renderer, mappedInput, epub, epub->getPath(), currentSpineIndex, currentPage, totalPages,
            [this]() {
              // On cancel - defer exit to avoid use-after-free
              pendingSubactivityExit = true;
            },
            [this](int newSpineIndex, int newPage) {
              // On sync complete - update position and defer exit
              if (currentSpineIndex != newSpineIndex || (section && section->currentPage != newPage)) {
                currentSpineIndex = newSpineIndex;
                nextPageNumber = newPage;
                section.reset();
              }
              pendingSubactivityExit = true;
            }));
        xSemaphoreGive(renderingMutex);
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNCY: {
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        exitActivity();
        enterNewActivity(new JianGuoSyncActivity(
            renderer, mappedInput, epub, epub->getPath(),currentSpineIndex,
            [this]() {
              exitActivity();
              updateRequired = true;
            },
            [this](int newSpineIndex) {
              Serial.printf("[%lu] [JG] 同步完成，新的 spine index: %d\n", millis(), newSpineIndex);
              // On sync complete - update position and defer exit
              if (currentSpineIndex != newSpineIndex) {
                currentSpineIndex = newSpineIndex;
                exitActivity();
                nextPageNumber = 0;
                section.reset();
                updateRequired = true;
              }
            }
          
          ));
        xSemaphoreGive(renderingMutex);
      break;
    }

  }
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (section) {
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = section->pageCount;
    nextPageNumber = section->currentPage;
  }

  // Persist the selection so the reader keeps the new orientation on next launch.
  SETTINGS.orientation = orientation;
  SETTINGS.saveToFile();

  // Update renderer orientation to match the new logical coordinate system.
  applyReaderOrientation(renderer, SETTINGS.orientation);

  // Reset section to force re-layout in the new orientation.
  section.reset();
  xSemaphoreGive(renderingMutex);
}

void EpubReaderActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      // 加锁保证渲染过程独占
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      APP_STATE.isRenderComplete = false; // 标记渲染开始
      renderScreen(); // 执行核心渲染逻辑
      APP_STATE.isRenderComplete = true;  // 标记渲染完成（包括 saveProgress）
      APP_STATE.saveToFile();
      xSemaphoreGive(renderingMutex);     // 释放锁
    }
    vTaskDelay(10 / portTICK_PERIOD_MS); // 降低轮询频率，节省资源
  }
}

// TODO: Failure handling
void EpubReaderActivity::renderScreen() {
  if (!epub) {
    return;
  }

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Show end of book screen
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "End of book", true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Apply screen viewable areas and additional padding
  // 1. 先获取屏幕原始可视边距（无任何叠加）
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);

  auto metrics = UITheme::getInstance().getMetrics();

  // 2. 先处理状态栏（核心：这里用 SETTINGS.screenMargin_Bottom 调整状态栏位置）
  if (SETTINGS.statusBar != CrossPointSettings::STATUS_BAR_MODE::NONE) {
    const bool showProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                                 SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::ONLY_BOOK_PROGRESS_BAR ||
                                 SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
    //这个地方是留位置，位置可以多留一点
    orientedMarginBottom += statusBarMargin  +
                            (showProgressBar ? (metrics.bookProgressBarHeight + progressBarMarginTop) : 0);
  }

  // 3. 再叠加用户设置的所有边距（此时Bottom边距不会被抵消）
  orientedMarginTop += SETTINGS.screenMargin_Top;
  orientedMarginLeft += SETTINGS.screenMargin_Left;
  orientedMarginRight += SETTINGS.screenMargin_Right;
  orientedMarginBottom += SETTINGS.screenMargin_Bottom; 


  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    Serial.printf("[%lu] [ERS] Loading file: %s, index: %d\n", millis(), filepath.c_str(), currentSpineIndex);
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));

    const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
    const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
    Serial.printf("[%lu] [ERS] Calculated viewport: %dx%d (Screen: %dx%d, Margins L:%d R:%d T:%d B:%d)\n", millis(),
                  viewportWidth, viewportHeight, renderer.getScreenWidth(), renderer.getScreenHeight(),
                  orientedMarginLeft, orientedMarginRight, orientedMarginTop, orientedMarginBottom);

    if (!section->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled,SETTINGS.wordSpacing,SETTINGS.firstlineintented, SETTINGS.embeddedStyle)) {
      Serial.printf("[%lu] [ERS] Cache not found, building...\n", millis());

      const auto popupFn = [this]() { GUI.drawPopup(renderer, "Indexing..."); };

      if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                      SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                      viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.wordSpacing, SETTINGS.firstlineintented, SETTINGS.embeddedStyle, popupFn)) {
        Serial.printf("[%lu] [ERS] Failed to persist page data to SD\n", millis());
        section.reset();
        return;
      }
    } else {
      Serial.printf("[%lu] [ERS] Cache found, skipping build...\n", millis());
    }

    if (nextPageNumber == UINT16_MAX) {
      section->currentPage = section->pageCount - 1;
    } else {
      section->currentPage = nextPageNumber;
    }

    // handles changes in reader settings and reset to approximate position based on cached progress
    if (cachedChapterTotalPageCount > 0) {
      // only goes to relative position if spine index matches cached value
      if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
        float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
        int newPage = static_cast<int>(progress * section->pageCount);
        section->currentPage = newPage;
      }
      cachedChapterTotalPageCount = 0;  // resets to 0 to prevent reading cached progress again
    }

    if (pendingPercentJump && section->pageCount > 0) {
      // Apply the pending percent jump now that we know the new section's page count.
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) {
        newPage = section->pageCount - 1;
      }
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

  renderer.clearScreen();
    //加背景
    if(SETTINGS.ReadingScreenEnabled){
      Serial.printf("[%lu] [ERS] 壁纸屏幕开启，渲染壁纸屏幕\n");
      renderPngSleepScreen(renderer);
    }


  if (section->pageCount == 0) {
    Serial.printf("[%lu] [ERS] No pages to render\n", millis());
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "Empty chapter", true, EpdFontFamily::BOLD);
    renderStatusBar(orientedMarginRight, orientedMarginBottom,orientedMarginTop, orientedMarginLeft);
    renderer.displayBuffer();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    Serial.printf("[%lu] [ERS] Page out of bounds: %d (max %d)\n", millis(), section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "Out of bounds", true, EpdFontFamily::BOLD);
    renderStatusBar(orientedMarginRight, orientedMarginBottom,orientedMarginTop, orientedMarginLeft);
    renderer.displayBuffer();
    return;
  }

  {
    auto p = section->loadPageFromSectionFile();
    if (!p) {
      Serial.printf("[%lu] [ERS] Failed to load page from SD - clearing section cache\n", millis());
      section->clearCache();
      section.reset();
      return renderScreen();
    }
    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    Serial.printf("[%lu] [ERS] Rendered page in %dms\n", millis(), millis() - start);
  }
  saveProgress(currentSpineIndex, section->currentPage, section->pageCount);
}



void EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  FsFile f;
  if (SdMan.openFileForWrite("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    data[0] = currentSpineIndex & 0xFF;
    data[1] = (currentSpineIndex >> 8) & 0xFF;
    data[2] = currentPage & 0xFF;
    data[3] = (currentPage >> 8) & 0xFF;
    data[4] = pageCount & 0xFF;
    data[5] = (pageCount >> 8) & 0xFF;
    f.write(data, 6);
    f.close();
    Serial.printf("[ERS] Progress saved: Chapter %d, Page %d\n", spineIndex, currentPage);
  } else {
    Serial.printf("[ERS] Could not save progress!\n");
  }
}
void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  auto applySettingMarginPreviewOverlay =
      [this, orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft]() {
    if (state != EPUBState::SETTING) {
      return;
    }

    const int screenWidth = renderer.getScreenWidth();
    const int screenHeight = renderer.getScreenHeight();
    const int contentX = orientedMarginLeft;
    const int contentY = orientedMarginTop;
    const int contentWidth = screenWidth - orientedMarginLeft - orientedMarginRight;
    const int contentHeight = screenHeight - orientedMarginTop - orientedMarginBottom;

    if (contentWidth > 2 && contentHeight > 2) {
      renderer.drawRect(contentX, contentY, contentWidth, contentHeight, 3, true);
    }

    const char* line1 = "进入边距设置";
    const char* line2 = "请注意边框";
    const char* line3 = "短按加边距";
    const char* line4 = "长按减边距";
    const int textW1 = renderer.getTextWidth(UI_12_FONT_ID, line1);
    const int textW2 = renderer.getTextWidth(UI_12_FONT_ID, line2);
    const int boxWidth = std::max(textW1, textW2) + 24;
    const int lineHeight = 18;
    const int boxHeight = lineHeight * 4 + 20;
    const int boxX = (screenWidth - boxWidth) / 2;
    const int boxY = (screenHeight - boxHeight) / 2;

    renderer.fillRect(boxX, boxY, boxWidth, boxHeight,  false);
    renderer.drawCenteredText(UI_12_FONT_ID, boxY + 8, line1, true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_12_FONT_ID, boxY + 8 + lineHeight, line2, true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_12_FONT_ID, boxY + 8 + 2*lineHeight, line3, true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_12_FONT_ID, boxY + 8 + 3*lineHeight, line4, true, EpdFontFamily::BOLD);    
  };

    // Double FAST_REFRESH with selective image blanking (pablohc's technique):
    // HALF_REFRESH sets particles too firmly for the grayscale LUT to adjust.
    // Instead, blank only the image area and do two fast refreshes.
    // Step 1: Display page with image area blanked (text appears, image area white)
    // Step 2: Re-render with images and display again (images appear clean)
    // 采用双 FAST_REFRESH 并选择性区域消隐（pablohc 技术）：
    // HALF_REFRESH 会将电子墨粒固定得过死，导致灰度 LUT 无法正常调整。
    // 改为仅清空图像区域，并执行两次快速刷新。
    // 步骤 1：显示时将图像区域置空（文字正常显示，图像区域为白色）
    // 步骤 2：重新渲染图像并再次刷新（图像显示干净无残影）
  const bool imagePageWithAA = page->hasImages() && SETTINGS.textAntiAliasing;
  page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
  renderStatusBar(orientedMarginRight, orientedMarginBottom,orientedMarginTop, orientedMarginLeft);
  applySettingMarginPreviewOverlay();
  // if (imagePageWithAA) {
  //   // NOTE(half-refresh-fix): Selective image-area blanking sequence:
  //   // 1) clear image area and FAST refresh, 2) redraw full page and FAST refresh.
  //   int16_t imgX = 0, imgY = 0, imgW = 0, imgH = 0;
  //   if (page->getImageBoundingBox(imgX, imgY, imgW, imgH)) {
  //     renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
  //     applySettingMarginPreviewOverlay();
  //     renderer.displayBuffer(HalDisplay::FAST_REFRESH);

  //     // Re-render page content to restore images into the blanked area.
  //     page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
  //     renderStatusBar(orientedMarginRight, orientedMarginBottom,orientedMarginTop, orientedMarginLeft);
  //     applySettingMarginPreviewOverlay();
  //     renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  //   } else {
  //     // NOTE(fallback): If image bounds are unavailable, keep the safer double FAST path.
  //     applySettingMarginPreviewOverlay();
  //     renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  //     page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
  //     renderStatusBar(orientedMarginRight, orientedMarginBottom,orientedMarginTop, orientedMarginLeft);
  //     applySettingMarginPreviewOverlay();
  //     renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  //   }
  // } else 
  if (pagesUntilFullRefresh <= 1) {
    applySettingMarginPreviewOverlay();
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    applySettingMarginPreviewOverlay();
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }

  // Save bw buffer to reset buffer state after grayscale data sync
  renderer.storeBwBuffer();

  // grayscale rendering
  // TODO: Only do this if font supports it
  if (SETTINGS.textAntiAliasing) {
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
    renderer.copyGrayscaleLsbBuffers();

    // Render and copy to MSB buffer
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
    renderer.copyGrayscaleMsbBuffers();

    // display grayscale part
    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }

  // restore the bw data
  renderer.restoreBwBuffer();

  if (!bluetoothBootstrapDone) {
    bluetoothBootstrapDone = true;
    initializeBluetoothAfterReaderRender();
  }
}

void EpubReaderActivity::renderStatusBar(const int orientedMarginRight, const int orientedMarginBottom,
                                         const int orientedMarginTop, const int orientedMarginLeft) const {
  auto metrics = UITheme::getInstance().getMetrics();

  // determine visible status bar elements
  const bool showProgressPercentage = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL;
  const bool showBookProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                                   SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::ONLY_BOOK_PROGRESS_BAR;
  const bool showChapterProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showProgressText = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                                SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR;
  const bool showBookPercentage = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showBattery = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::NO_PROGRESS ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showChapterTitle = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::NO_PROGRESS ||
                                SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                                SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                                SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage == CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_NEVER;

  // Position status bar near the bottom of the logical screen, regardless of orientation
  const auto screenHeight = renderer.getScreenHeight();
  //int statusBarMargin = renderer.getFontAscenderSize(SMALL_FONT_ID)/2;
  const auto textY = screenHeight - orientedMarginBottom - 8;
  int progressTextWidth = 0;
  //Serial.printf("[%lu] [ERS] 测试一下位置变了吗: %d", millis(),textY);

  // Calculate progress in book
  const float sectionChapterProg = static_cast<float>(section->currentPage) / section->pageCount;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  if (showProgressText || showProgressPercentage || showBookPercentage) {
    // Right aligned text for progress counter
    char progressStr[32];

    // Hide percentage when progress bar is shown to reduce clutter
    if (showProgressPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%d/%d  %.0f%%", section->currentPage + 1, section->pageCount,
               bookProgress);
    } else if (showBookPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%.0f%%", bookProgress);
    } else {
      snprintf(progressStr, sizeof(progressStr), "%d/%d", section->currentPage + 1, section->pageCount);
    }

    progressTextWidth = renderer.getTextWidth(SMALL_FONT_ID, progressStr);
    renderer.drawText(SMALL_FONT_ID, renderer.getScreenWidth() - orientedMarginRight - progressTextWidth, textY,
                      progressStr);
  }

  if (showBookProgressBar) {
    // Draw progress bar at the very bottom of the screen, from edge to edge of viewable area
    GUI.drawReadingProgressBar(renderer, static_cast<size_t>(bookProgress));
  }

  if (showChapterProgressBar) {
    // Draw chapter progress bar at the very bottom of the screen, from edge to edge of viewable area
    const float chapterProgress =
        (section->pageCount > 0) ? (static_cast<float>(section->currentPage + 1) / section->pageCount) * 100 : 0;
    GUI.drawReadingProgressBar(renderer, static_cast<size_t>(chapterProgress));
  }

  if (showBattery) {
    GUI.drawBattery(renderer, Rect{orientedMarginLeft + 1, textY, metrics.batteryWidth, metrics.batteryHeight},
                    showBatteryPercentage);
  }

  if (showChapterTitle) {
    // Centered chatper title text
    // Page width minus existing content with 30px padding on each side
    const int rendererableScreenWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;

    const int batterySize = showBattery ? (showBatteryPercentage ? 50 : 20) : 0;
    const int titleMarginLeft = batterySize + 30;
    const int titleMarginRight = progressTextWidth + 30;

    // Attempt to center title on the screen, but if title is too wide then later we will center it within the
    // available space.
    int titleMarginLeftAdjusted = std::max(titleMarginLeft, titleMarginRight);
    int availableTitleSpace = rendererableScreenWidth - 2 * titleMarginLeftAdjusted;
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);

    std::string title;
    int titleWidth;
    if (tocIndex == -1) {
      title = "Unnamed";
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, "Unnamed");
    } else {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
      if (titleWidth > availableTitleSpace) {
        // Not enough space to center on the screen, center it within the remaining space instead
        availableTitleSpace = rendererableScreenWidth - titleMarginLeft - titleMarginRight;
        titleMarginLeftAdjusted = titleMarginLeft;
      }
      if (titleWidth > availableTitleSpace) {
        title = renderer.truncatedText(SMALL_FONT_ID, title.c_str(), availableTitleSpace);
        titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
      }
    }

    renderer.drawText(SMALL_FONT_ID,
                      titleMarginLeftAdjusted + orientedMarginLeft + (availableTitleSpace - titleWidth) / 2, textY,
                      title.c_str());
  }
}




void EpubReaderActivity::renderPngSleepScreen(GfxRenderer& renderer) const {
  const std::string pxcPath = "/.crosspoint/wallpaper_bg.pxc";
  if (loadWallpaperPxcToFramebuffer(pxcPath, renderer)) {
    Serial.printf("[%lu] [SLP] Loaded wallpaper PXC cache\n", millis());
    return;
  }
  Serial.printf("[%lu] [SLP] Wallpaper PXC missing, skip reader background\n", millis());
}
