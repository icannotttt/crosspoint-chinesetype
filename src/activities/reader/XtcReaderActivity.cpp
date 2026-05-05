/**
 * XtcReaderActivity.cpp
 *
 * XTC ebook reader activity implementation
 * Displays pre-rendered XTC pages on e-ink display
 */

#include "XtcReaderActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <SDCardManager.h>
#include <BluetoothHIDManager.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "BookmarkActivity.h"
#include "ReaderEntryModeSelectionActivity.h"
#include "XtcBookmarkSelectionActivity.h"
#include "XtcReaderChapterSelectionActivity.h"
#include "../settings/BluetoothSettingsActivity.h"
#include "../../util/ReaderBluetoothBootstrap.h"
#include "fontIds.h"
#include "../../lib/Xtc/Xtc/XtcTypes.h"

namespace {
constexpr unsigned long skipPageMs = 700;
constexpr unsigned long goHomeMs = 1000;
constexpr unsigned long bookmarkPressMs = 700;
constexpr int loadedMaxPage_per= 500;//新增

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
}  // namespace

void XtcReaderActivity::taskTrampoline(void* param) {
  auto* self = static_cast<XtcReaderActivity*>(param);
  self->displayTaskLoop();
}

void XtcReaderActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  if (!xtc) {
    return;
  }
  // 新增：进入时清理旧缓冲区（避免脏数据）
  if (pageBuffer) {
    free(pageBuffer);
    pageBuffer = nullptr;
    pageBufferCapacity = 0;
  }
  renderingMutex = xSemaphoreCreateMutex();

  xtc->setupCacheDir();

  // Load saved progress
  loadProgress();

  // Save current XTC as last opened book and add to recent books
  APP_STATE.openEpubPath = xtc->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(xtc->getPath(), xtc->getTitle(), xtc->getAuthor(), xtc->getThumbBmpPath());

  // Trigger first update
  updateRequired = true;
  lastAutoPageTurnMs = millis();

  xTaskCreate(&XtcReaderActivity::taskTrampoline, "XtcReaderActivityTask",
              4096,               // Stack size (smaller than EPUB since no parsing needed)
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void XtcReaderActivity::onExit() {
  ActivityWithSubactivity::onExit();

  // Force-release BLE resources when leaving reader to maximize memory for parsing/rendering.
  auto& btMgr = BluetoothHIDManager::getInstance();
  if (btMgr.isEnabled()) {
    btMgr.disable();
  }

  // Wait until not rendering to delete task
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  xtc.reset();
    // 新增：退出时释放复用的缓冲区
  if (pageBuffer) {
    free(pageBuffer);
    pageBuffer = nullptr;
    pageBufferCapacity = 0;
  }
}

void XtcReaderActivity::loop() {
  // Pass input responsibility to sub activity if exists
  if (subActivity) {
    lastAutoPageTurnMs = millis();
    subActivity->loop();
    return;
  }

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

  // Short confirm opens mode selector (chapter/bookmark)
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (mappedInput.getHeldTime() >= bookmarkPressMs) {
      const int totalPages = static_cast<int>(xtc->getPageCount());
      const int percent = totalPages > 0
                              ? static_cast<int>(((static_cast<float>(currentPage + 1) * 100.0f) /
                                                   static_cast<float>(totalPages)) +
                                                  0.5f)
                              : 0;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new BookmarkActivity(
          renderer, mappedInput, xtc->getPath(), xtc->getCachePath(), std::max(0, std::min(100, percent)),
          static_cast<int32_t>(currentPage), totalPages, static_cast<int32_t>(m_loadedMax), [this](bool) {
            exitActivity();
            updateRequired = true;
          }));
      xSemaphoreGive(renderingMutex);
      return;
    }

    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    exitActivity();
    enterNewActivity(new ReaderEntryModeSelectionActivity(
        this->renderer, this->mappedInput,
        [this]() {
          exitActivity();
          updateRequired = true;
        },
        [this](ReaderEntryModeSelectionActivity::Mode mode) {
          exitActivity();
          if (mode == ReaderEntryModeSelectionActivity::Mode::CHAPTER) {
            enterNewActivity(new XtcReaderChapterSelectionActivity(
                this->renderer, this->mappedInput, xtc, currentPage,
                [this] {
                  exitActivity();
                  updateRequired = true;
                },
                [this](const uint32_t newPage) {
                  this->gotoPage(newPage);
                  exitActivity();
                  updateRequired = true;
                }));
          } else if (mode == ReaderEntryModeSelectionActivity::Mode::BOOKMARK) {
            enterNewActivity(new XtcBookmarkSelectionActivity(
                this->renderer, this->mappedInput, xtc,
                [this]() {
                  exitActivity();
                  updateRequired = true;
                },
                [this](const uint32_t newPage) {
                  this->gotoPage(newPage);
                  exitActivity();
                  updateRequired = true;
                }));
          } else if (mode == ReaderEntryModeSelectionActivity::Mode::BLUETOOTH) {
            enterNewActivity(new BluetoothSettingsActivity(
                this->renderer, this->mappedInput,
                [this] {
                  exitActivity();
                  skipNextButtonCheck = true;
                  updateRequired = true;
                }));
          }
        }));
    xSemaphoreGive(renderingMutex);
    return;
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

  // Handle end of book
  if (currentPage >= xtc->getPageCount()) {
    if (autoTurnTriggered) {
      return;
    }
    currentPage = xtc->getPageCount() - 1;
    updateRequired = true;
    return;
  }

  const bool skipPages = SETTINGS.longPressChapterSkip && mappedInput.getHeldTime() > skipPageMs;
  const int skipAmount = skipPages ? 10 : 1;

  if (prevTriggered) {
    lastAutoPageTurnMs = millis();
    if (currentPage >= static_cast<uint32_t>(skipAmount)) {
      currentPage -= skipAmount;
    } else {
      currentPage = 0;
    }
    updateRequired = true;
  } else if (nextTriggered) {
    if (!autoTurnTriggered) {
      lastAutoPageTurnMs = millis();
    }
    currentPage += skipAmount;
    if (currentPage >= xtc->getPageCount()) {
      currentPage = xtc->getPageCount();  // Allow showing "End of book"
    }
    updateRequired = true;
  }
}

void XtcReaderActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      renderScreen();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}


void XtcReaderActivity::renderScreen() {
  if (!xtc) {
    return;
  }

  if (currentPage >= xtc->getPageCount()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "End of book", true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  renderFullPage(); // 替换为新的批量渲染函数
  saveProgress();

  if (!bluetoothBootstrapDone) {
    bluetoothBootstrapDone = true;
    initializeBluetoothAfterReaderRender();
  }
}

// 核心：半页内存 + 批量渲染（完全对齐原始renderPage逻辑）
void XtcReaderActivity::renderFullPage() {
  const uint16_t fullPageWidth = xtc->getPageWidth(); // 480
  const uint16_t fullPageHeight = xtc->getPageHeight(); // 800
  const int SLICE_COUNT = 8;
  const uint16_t sliceWidth = fullPageWidth / SLICE_COUNT; // 竖切：60列/片（仅改这行，替换原sliceHeight）
  const size_t sliceColBytes = (fullPageHeight + 7) / 8; // 竖切：每列100字节（仅改这行）
  const uint8_t bitDepth = xtc->getBitDepth();

  // ========== 切片buffer计算（适配竖切，仅改计算逻辑） ==========
  size_t slicePageBufferSize;
  if (bitDepth == 2) {
    slicePageBufferSize = sliceWidth * sliceColBytes * 2; // 竖切：60×100×2=12000（仅改这行）
  } else {
    const uint16_t sliceHeight = fullPageHeight / SLICE_COUNT;
    slicePageBufferSize = ((static_cast<size_t>(fullPageWidth) + 7) / 8) * sliceHeight;
  }

  // 复用全局buffer（逻辑不变）
  if (pageBufferCapacity < slicePageBufferSize) {
    if (pageBuffer) {
      free(pageBuffer);
      pageBuffer = nullptr;
    }
    pageBuffer = static_cast<uint8_t*>(calloc(1, slicePageBufferSize));
    if (!pageBuffer) {
      Serial.printf("[%lu] [XTR] Alloc failed: %lu bytes\n", millis(), slicePageBufferSize);
      renderer.clearScreen();
      renderer.drawCenteredText(UI_12_FONT_ID, 300, "Memory error", true, EpdFontFamily::BOLD);
      renderer.displayBuffer();
      return;
    }
    pageBufferCapacity = slicePageBufferSize;
  }

  // 锁定当前页（逻辑不变）
  static uint32_t renderingPage = 0;
  renderingPage = currentPage;

// ========== 第一步：批量绘制全页BW（8个竖切片，循环逻辑不变） ==========
//xtch
if (bitDepth == 2) {
renderer.clearScreen(); 

for (int n=0; n<8; n++) {
    size_t bytesRead = xtc->loadPage(renderingPage, pageBuffer, slicePageBufferSize, true, n);
    if (bytesRead == 0) {
        Serial.printf("[%lu] [XTR] Load slice failed (page=%lu, slice=%d)\n", millis(), renderingPage, n);
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 300, "Load error", true, EpdFontFamily::BOLD);
        renderer.displayBuffer();
        return;
    }
    drawSliceContent(n, false, true); // 仍调用原函数，仅内部适配竖切
}

// 全页BW刷新（逻辑不变）
if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer();
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
} else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
}

// ========== 第二步：批量绘制全页灰阶（逻辑不变，仅适配竖切参数） ==========
  if (bitDepth == 2) {
    renderer.clearScreen(0x00); 
    for (int n=0; n<8; n++) {
        size_t bytesRead = xtc->loadPage(renderingPage, pageBuffer, slicePageBufferSize, true, n);
        if (bytesRead == 0) {
            Serial.printf("[%lu] [XTR] Load slice failed (page=%lu, slice=%d)\n", millis(), renderingPage, n);
            renderer.clearScreen();
            renderer.drawCenteredText(UI_12_FONT_ID, 300, "Load error", true, EpdFontFamily::BOLD);
            renderer.displayBuffer();
            return;
        }
        drawSliceContent(n, true, true);
    }
      renderer.copyGrayscaleLsbBuffers();

    renderer.clearScreen(0x00); 
    for (int n=0; n<8; n++) {
        size_t bytesRead = xtc->loadPage(renderingPage, pageBuffer, slicePageBufferSize, true, n);
        if (bytesRead == 0) {
            Serial.printf("[%lu] [XTR] Load slice failed (page=%lu, slice=%d)\n", millis(), renderingPage, n);
            renderer.clearScreen();
            renderer.drawCenteredText(UI_12_FONT_ID, 300, "Load error", true, EpdFontFamily::BOLD);
            renderer.displayBuffer();
            return;
        }
        drawSliceContent(n, true, false);
    }
      renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();


    //BW
    renderer.clearScreen();
    for (int n=0; n<8; n++) {
        size_t bytesRead = xtc->loadPage(renderingPage, pageBuffer, slicePageBufferSize, true, n);
        if (bytesRead == 0) {
            Serial.printf("[%lu] [XTR] Load slice failed (page=%lu, slice=%d)\n", millis(), renderingPage, n);
            renderer.clearScreen();
            renderer.drawCenteredText(UI_12_FONT_ID, 300, "Load error", true, EpdFontFamily::BOLD);
            renderer.displayBuffer();
            return;
        }
        drawSliceContent(n, false, true);
    } 
    renderer.cleanupGrayscaleWithFrameBuffer();
  }
}
else{
    // 1-bit 仍按行切
    renderer.clearScreen();
    for (int n=0; n<8; n++) {
        size_t bytesRead = xtc->loadPage(renderingPage, pageBuffer, slicePageBufferSize, true, n);
        if (bytesRead == 0) {
            Serial.printf("[%lu] [XTR] Load slice failed (page=%lu, slice=%d)\n", millis(), renderingPage, n);
            renderer.clearScreen();
            renderer.drawCenteredText(UI_12_FONT_ID, 300, "Load error", true, EpdFontFamily::BOLD);
            renderer.displayBuffer();
            return;
        }
        drawSliceContent(n, false, true); 
    }
    renderer.displayBuffer();

}


}
// 核心绘制函数：修复参数接收+变量名
// 核心绘制函数：最小侵入适配竖切
void XtcReaderActivity::drawSliceContent(int sliceN, bool isGrayscale, bool isLSB) {
  const uint16_t fullPageWidth = xtc->getPageWidth();
  const uint16_t fullPageHeight = xtc->getPageHeight();
  const int SLICE_COUNT = 8;
  const uint8_t bitDepth = xtc->getBitDepth();
  if (bitDepth == 2) {
  const uint16_t sliceWidth = fullPageWidth / SLICE_COUNT; // 竖切：60列/片（仅改这行）
  const size_t sliceColBytes = (fullPageHeight + 7) / 8; // 竖切：每列100字节（仅改这行）
  const uint16_t xBase = sliceN * sliceWidth; // 竖切：起始列（仅改这行，替换原yBase）
  // 边界防护：适配竖切（仅改边界判断）
  if (xBase + sliceWidth > fullPageWidth) {
    Serial.printf("[%lu] [XTR] Invalid xBase: %u + %u > %u\n", millis(), xBase, sliceWidth, fullPageWidth);
    return;
  }

    // 1. 竖切精准参数（仅改参数计算）
    const uint16_t sliceStartCol = (7 - sliceN) * sliceWidth;
    const uint16_t sliceEndCol = sliceStartCol + sliceWidth - 1;
    const size_t planeSize = sliceWidth * sliceColBytes; // 竖切：60×100=6000

    const uint8_t* plane1 = pageBuffer;
    const uint8_t* plane2 = pageBuffer + planeSize;

    // 2. 精准像素读取（适配竖切，仅改这部分逻辑）
    auto getPixelValue = [&](uint16_t xLocal, uint16_t y) -> uint8_t {
        const uint16_t absoluteCol = sliceStartCol + xLocal;
        if (absoluteCol > sliceEndCol || y >= fullPageHeight || xLocal >= sliceWidth) return 0;

        // 竖切适配：XTCH列从右到左存储，垂直8像素打包
        const size_t colIndexInSlice = sliceWidth - 1 - xLocal;
        const size_t byteInCol = y / 8;
        const size_t bitInByte = 7 - (y % 8);
        const size_t byteOffset = colIndexInSlice * sliceColBytes + byteInCol;
      
        if (byteOffset >= planeSize) return 0;
        const uint8_t bit1 = (plane1[byteOffset] >> bitInByte) & 1;
        const uint8_t bit2 = (plane2[byteOffset] >> bitInByte) & 1;
        return (bit1 << 1) | bit2;
    };

    // 3. 绘制逻辑（仅改坐标计算，其余不变）
    if (!isGrayscale) {
        for (uint16_t y = 0; y < fullPageHeight; y++) { // 竖切：遍历高度
            for (uint16_t xLocal = 0; xLocal < sliceWidth; xLocal++) { // 竖切：遍历切片内列
                if (getPixelValue(xLocal, y) >= 1) {
                    renderer.drawPixel(sliceStartCol + xLocal, y, true); // 竖切坐标
                }
            }
        }
    }else {
      if (isLSB) {
        for (uint16_t y = 0; y < fullPageHeight; y++) {
          for (uint16_t xLocal = 0; xLocal < sliceWidth; xLocal++) {
            if (getPixelValue(xLocal, y) == 1) {
              renderer.drawPixel(sliceStartCol + xLocal, y, false); // 竖切坐标
            }
          }
        }
      } else {
        for (uint16_t y = 0; y < fullPageHeight; y++) {
          for (uint16_t xLocal = 0; xLocal < sliceWidth; xLocal++) {
            const uint8_t pv = getPixelValue(xLocal, y);
            if (pv == 1 || pv == 2) {
              renderer.drawPixel(sliceStartCol + xLocal, y, false); // 竖切坐标
            }
          }
        }
      }
    }
  } else {
    // 1-bit 绘制（适配横切）服了这个补全了，懒得改注释了，底下的注释不一定对，如想看具体格式
    //https://gist.github.com/CrazyCoder/b125f26d6987c0620058249f59f1327d
    const uint16_t sliceHeight = fullPageHeight / SLICE_COUNT; // 横切：100行/片
    const size_t rowBytes = (fullPageWidth + 7) / 8;
    const uint16_t yBase = sliceN * sliceHeight; // 横切：起始行
    // 边界防护：适配横切（仅改边界判断）
    if (yBase + sliceHeight > fullPageHeight) {
      Serial.printf("[%lu] [XTR] Invalid yBase: %u + %u > %u\n", millis(), yBase, sliceHeight, fullPageHeight);
      return;
    }

    auto getPixelValue = [&](uint16_t x, uint16_t yLocal) -> uint8_t {
      if (yLocal >= sliceHeight || x >= fullPageWidth) return 0;
      const size_t byteInRow = x / 8;
        const size_t bitInByte = 7 - (x % 8);
      const size_t byteOffset = static_cast<size_t>(yLocal) * rowBytes + byteInRow;

        return (pageBuffer[byteOffset] >> bitInByte) & 1;
    };
    //绘制
    for (uint16_t x = 0; x < fullPageWidth; x++) { // 横切：遍历宽度
    for (uint16_t yLocal = 0; yLocal < sliceHeight; yLocal++) { // 横切：遍历切片内行
      if (getPixelValue(x, yLocal) == 0) {
        renderer.drawPixel(x, yBase + yLocal, true); // 横切坐标
        }
    }
}
}
}



// 跳转函数
// XtcReaderActivity.cpp
void XtcReaderActivity::gotoPage(uint32_t targetPage) {
    // 前置空指针+合法性校验（终极防护）
    if (!xtc) {
        Serial.printf("[%lu] [XTR] gotoPage失败：xtc为空\n", millis());
        currentPage = 0;
        updateRequired = true;
        return;
    }

    const uint32_t totalPages = xtc->getPageCount();
    if (totalPages == 0) {
        Serial.printf("[%lu] [XTR] gotoPage失败：总页数为0\n", millis());
        currentPage = 0;
        updateRequired = true;
        return;
    }

    // 强制校正目标页（避免越界）
    uint32_t safeTargetPage = targetPage;
    safeTargetPage = (safeTargetPage >= totalPages) ? (totalPages - 1) : safeTargetPage;
    safeTargetPage = (safeTargetPage < 0) ? 0 : safeTargetPage;

    // 计算批次起始页（避免0页批次）
    uint32_t targetBatchStart = (safeTargetPage / loadedMaxPage_per) * loadedMaxPage_per;
    targetBatchStart = (targetBatchStart >= totalPages) ? ((totalPages / loadedMaxPage_per) * loadedMaxPage_per) : targetBatchStart;
    if (targetBatchStart < 0) targetBatchStart = 0;

    // 释放旧批次（仅当批次变化时）
    static uint32_t lastBatchStart = 0;
    if (targetBatchStart != lastBatchStart) {
        xtc->releasePageBatchByStart(lastBatchStart);
        lastBatchStart = targetBatchStart;
    }

    // 加载新批次（添加返回值校验）
    xtc::XtcError loadErr = xtc->loadPageBatchByStart((uint16_t)targetBatchStart);
    if (loadErr != xtc::XtcError::OK) {
        Serial.printf("[%lu] [XTR] 加载批次失败：%u，降级为仅加载当前页\n", millis(), (uint8_t)loadErr);
    }

    // 校正加载最大页
    m_loadedMax = targetBatchStart + loadedMaxPage_per - 1;
    m_loadedMax = (m_loadedMax >= totalPages) ? (totalPages - 1) : m_loadedMax;

    // 更新当前页并标记刷新
    currentPage = safeTargetPage;
    updateRequired = true;

    // 打印最终状态（方便调试）
    Serial.printf("[%lu] [跳转] 最终状态：目标页=%lu, 批次[%lu~%lu], 总页数=%lu\n", 
                 millis(), safeTargetPage, targetBatchStart, m_loadedMax, totalPages);
}
void XtcReaderActivity::saveProgress() const {
  FsFile f;
  if (SdMan.openFileForWrite("XTR", xtc->getCachePath() + "/progress.bin", f)) {
    uint8_t data[8]; // 8字节，前4字节存页码，后4字节存页表上限
    // 前4字节：保存当前阅读页码 currentPage
    data[0] = currentPage & 0xFF;
    data[1] = (currentPage >> 8) & 0xFF;
    data[2] = (currentPage >> 16) & 0xFF;
    data[3] = (currentPage >> 24) & 0xFF;
    // 后4字节：保存当前页表上限 m_loadedMax
    data[4] = m_loadedMax & 0xFF;
    data[5] = (m_loadedMax >> 8) & 0xFF;
    data[6] = (m_loadedMax >> 16) & 0xFF;
    data[7] = (m_loadedMax >> 24) & 0xFF;
    
    f.write(data, 8);
    f.close();
    Serial.printf("[%lu] [进度] 保存成功 → 页码: %lu | 页表上限: %lu\n", millis(), currentPage, m_loadedMax);
  }
}

void XtcReaderActivity::loadProgress() {
  FsFile f;
  if (SdMan.openFileForRead("XTR", xtc->getCachePath() + "/progress.bin", f)) {
    uint8_t data[8];
    if (f.read(data, 8) == 8) {
      // 恢复两个核心变量
      currentPage = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
      uint32_t savedLoadedMax = data[4] | (data[5] << 8) | (data[6] << 16) | (data[7] << 24);

      Serial.printf("[%lu] [进度] 恢复成功 → 页码: %lu | 保存的页表上限: %lu\n", millis(), currentPage, savedLoadedMax);

      // 1. 边界防护：和gotoPage完全一致
      const uint32_t totalPages = xtc->getPageCount();
      if (currentPage >= totalPages) currentPage = totalPages - 1;
      if (currentPage < 0) currentPage = 0;

      uint32_t targetBatchStart = (currentPage / loadedMaxPage_per) * loadedMaxPage_per;
      xtc->loadPageBatchByStart(targetBatchStart); // 👈 和gotoPage一模一样！
      
      // 3. ✅ 同步状态：和gotoPage完全一致
      m_loadedMax = targetBatchStart + loadedMaxPage_per - 1;
      if(m_loadedMax >= totalPages) m_loadedMax = totalPages - 1;

      Serial.printf("[进度] 恢复进度后加载批次 → 页码%lu → 批次[%lu~%lu]\n", currentPage, targetBatchStart, m_loadedMax);
    }
    f.close();
  } else {
    // 无进度文件：初始化默认值（和gotoPage逻辑一致）
    const uint32_t totalPages = xtc->getPageCount();
    currentPage = 0;
    m_loadedMax = loadedMaxPage_per - 1;
    if(m_loadedMax >= totalPages) m_loadedMax = totalPages - 1;
    Serial.printf("[%lu] [进度] 无进度文件 → 初始化页码: 0 | 页表上限: %lu\n", millis(), m_loadedMax);
  }
}