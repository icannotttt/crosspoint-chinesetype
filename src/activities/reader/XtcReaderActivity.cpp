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

#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "XtcReaderChapterSelectionActivity.h"
#include "fontIds.h"
//gd 新增电池显示支持
#include "ScreenComponents.h"
#include "CrossPointSettings.h"


namespace {
constexpr int pagesPerRefresh = 15;
constexpr unsigned long skipPageMs = 700;
constexpr unsigned long goHomeMs = 1000;
constexpr int loadedMaxPage_per= 500;

// ✅✅✅ 核心新增：预分配全局静态页面缓冲区，大小适配480*800的2bit XTCH格式
// 缓冲区大小 = (480*800 +7)/8 *2 = 96000 Bytes，足够容纳最大的页面数据
constexpr size_t MAX_PAGE_BUFFER_SIZE = (480 * 800 + 7) / 8 * 2;
static uint8_t s_pageBuffer[MAX_PAGE_BUFFER_SIZE] = {0}; // 全局静态内存，一次分配，终身复用
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

  renderingMutex = xSemaphoreCreateMutex();

  xtc->setupCacheDir();

  // Load saved progress
  loadProgress();
  

  // Save current XTC as last opened book
  APP_STATE.openEpubPath = xtc->getPath();
  APP_STATE.saveToFile();

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&XtcReaderActivity::taskTrampoline, "XtcReaderActivityTask",
              4096,               // Stack size (smaller than EPUB since no parsing needed)
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
 
}

void XtcReaderActivity::onExit() {
  ActivityWithSubactivity::onExit();

  // Wait until not rendering to delete task
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  xtc.reset();
}

void XtcReaderActivity::loop() {
  // Pass input responsibility to sub activity if exists
  if (subActivity) {
    subActivity->loop();
    return;
  }

  // Enter chapter selection activity
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    exitActivity();
    enterNewActivity(new XtcReaderChapterSelectionActivity(
      this->renderer, this->mappedInput, xtc, currentPage,
      [this] { // 目录返回按钮的回调，不变
        exitActivity();
        updateRequired = true;
      },
      [this](const uint32_t newPage) { // 章节选中跳转的回调，核心修改
        this->gotoPage(newPage); // ✅ 调用完美跳转加载函数，所有逻辑交给它处理
        exitActivity(); // ✅ 跳转完成后退出章节页
        updateRequired = true;
    }));
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

  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);

  if (!prevReleased && !nextReleased) {
    return;
  }

  // Handle end of book
  if (currentPage >= xtc->getPageCount()) {
    currentPage = xtc->getPageCount() - 1;
    updateRequired = true;
    return;
  }

  const bool skipPages = mappedInput.getHeldTime() > skipPageMs;
  const int skipAmount = skipPages ? 10 : 1;

  if (prevReleased) {
    if (currentPage >= static_cast<uint32_t>(skipAmount)) {
      currentPage -= skipAmount;
    } else {
      currentPage = 0;
    }
    updateRequired = true;
  } else if (nextReleased) {
    const uint16_t totalPages = xtc->getPageCount();
    currentPage += skipAmount;
    if (currentPage >= totalPages) {
      currentPage = totalPages - 1;
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

  // Bounds check
  if (currentPage >= xtc->getPageCount()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "End of book", true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  
  renderPage();
  saveProgress();
}

void XtcReaderActivity::renderPage() {
  const uint16_t pageWidth = xtc->getPageWidth();
  const uint16_t pageHeight = xtc->getPageHeight();
  const uint8_t bitDepth = xtc->getBitDepth();

  size_t pageBufferSize;
  if (bitDepth == 2) {
    pageBufferSize = ((static_cast<size_t>(pageWidth) * pageHeight + 7) / 8) * 2;
  } else {
    pageBufferSize = ((pageWidth + 7) / 8);
  }

  uint8_t* pageBuffer = s_pageBuffer;


  // 继续加载页面数据（动态加载部分）
  size_t bytesRead = xtc->loadPage(currentPage, pageBuffer, pageBufferSize);
  if (bytesRead == 0) {
    Serial.printf("[%lu] [提示] 页码%lu加载中...\n", millis(), currentPage);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "Loading...", true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    updateRequired = true; 
    return;
  }

  renderer.clearScreen();
  const uint16_t maxSrcY = pageHeight;

  if (bitDepth == 2) {
    const size_t planeSize = (static_cast<size_t>(pageWidth) * pageHeight + 7) / 8;
    const uint8_t* plane1 = pageBuffer;
    const uint8_t* plane2 = pageBuffer + planeSize;
    const size_t colBytes = (pageHeight + 7) / 8;

    auto getPixelValue = [&](uint16_t x, uint16_t y) -> uint8_t {
      const size_t colIndex = pageWidth - 1 - x;
      const size_t byteInCol = y / 8;
      const size_t bitInByte = 7 - (y % 8);
      const size_t byteOffset = colIndex * colBytes + byteInCol;
      const uint8_t bit1 = (plane1[byteOffset] >> bitInByte) & 1;
      const uint8_t bit2 = (plane2[byteOffset] >> bitInByte) & 1;
      return (bit1 << 1) | bit2;
    };

    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) >= 1) {
          renderer.drawPixel(x, y, true);
        }
      }
    }

    if (pagesUntilFullRefresh <= 1) {
      renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
      pagesUntilFullRefresh = pagesPerRefresh;
    } else {
      renderer.displayBuffer();
      pagesUntilFullRefresh--;
    }

    renderer.clearScreen(0x00);
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) == 1) {
          renderer.drawPixel(x, y, false);
        }
      }
    }
    renderer.copyGrayscaleLsbBuffers();

    renderer.clearScreen(0x00);
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        const uint8_t pv = getPixelValue(x, y);
        if (pv == 1 || pv == 2) {
          renderer.drawPixel(x, y, false);
        }
      }
    }
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.clearScreen();
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) >= 1) {
          renderer.drawPixel(x, y, true);
        }
      }
    }
    renderer.cleanupGrayscaleWithFrameBuffer();
  } else {
    const size_t srcRowBytes = (pageWidth + 7) / 8;
    for (uint16_t srcY = 0; srcY < maxSrcY; srcY++) {
      const size_t srcRowStart = srcY * srcRowBytes;
      for (uint16_t srcX = 0; srcX < pageWidth; srcX++) {
        const size_t srcByte = srcRowStart + srcX / 8;
        const size_t srcBit = 7 - (srcX % 8);
        const bool isBlack = !((pageBuffer[srcByte] >> srcBit) & 1);
        if (isBlack) {
          renderer.drawPixel(srcX, srcY, true);
        }
      }
    }
    if (pagesUntilFullRefresh <= 1) {
      renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
      pagesUntilFullRefresh = pagesPerRefresh;
    } else {
      renderer.displayBuffer();
      pagesUntilFullRefresh--;
    }
  }

  Serial.printf("[%lu] [成功] 显示页码: %lu/%lu\n", millis(), currentPage+1, xtc->getPageCount());
}

// 跳转函数
void XtcReaderActivity::gotoPage(uint32_t targetPage) {
  const uint32_t totalPages = xtc->getPageCount();
  // 1. 边界防护：页码不能越界
  if (targetPage >= totalPages) targetPage = totalPages - 1;
  if (targetPage < 0) targetPage = 0;

  
  uint32_t targetBatchStart = (targetPage / loadedMaxPage_per) * loadedMaxPage_per;
  
  //按批次加载
  xtc->loadPageBatchByStart(targetBatchStart);
  
  m_loadedMax = targetBatchStart + loadedMaxPage_per - 1; // Activity的最大值
  if(m_loadedMax >= totalPages) m_loadedMax = totalPages - 1;

  currentPage = targetPage;
  updateRequired = true;
  Serial.printf("[跳转] 目标页%lu → 加载批次[%lu~%lu] | 内存已释放\n", targetPage, targetBatchStart, m_loadedMax);
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

//gd:新增电池显示支持
void XtcReaderActivity::renderStatusBar(const int orientedMarginRight, const int orientedMarginBottom,
                                         const int orientedMarginLeft) const {
  // determine visible status bar elements
  const bool showBattery = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::NO_PROGRESS ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL;

  // Position status bar near the bottom of the logical screen, regardless of orientation
  const auto screenHeight = renderer.getScreenHeight();
  const auto textY = screenHeight - orientedMarginBottom - 4;
  int progressTextWidth = 0;

  if (showBattery) {
    ScreenComponents::drawBattery(renderer, orientedMarginLeft, textY);
  }
}