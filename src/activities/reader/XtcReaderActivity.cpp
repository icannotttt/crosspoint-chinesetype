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

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "XtcReaderChapterSelectionActivity.h"
#include "fontIds.h"
#include "../../lib/Xtc/Xtc/XtcTypes.h"

namespace {
constexpr unsigned long skipPageMs = 700;
constexpr unsigned long goHomeMs = 1000;
constexpr int loadedMaxPage_per= 500;//新增
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

  xTaskCreate(&XtcReaderActivity::taskTrampoline, "XtcReaderActivityTask",
              8192,               // Stack size (smaller than EPUB since no parsing needed)
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
    subActivity->loop();
    return;
  }

  // Enter chapter selection activity
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
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
      xSemaphoreGive(renderingMutex);
    }
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
  const bool nextTriggered = usePressForPageTurn
                                 ? (mappedInput.wasPressed(MappedInputManager::Button::PageForward) || powerPageTurn ||
                                    mappedInput.wasPressed(MappedInputManager::Button::Right))
                                 : (mappedInput.wasReleased(MappedInputManager::Button::PageForward) || powerPageTurn ||
                                    mappedInput.wasReleased(MappedInputManager::Button::Right));

  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // Handle end of book
  if (currentPage >= xtc->getPageCount()) {
    currentPage = xtc->getPageCount() - 1;
    updateRequired = true;
    return;
  }

  const bool skipPages = SETTINGS.longPressChapterSkip && mappedInput.getHeldTime() > skipPageMs;
  const int skipAmount = skipPages ? 10 : 1;

  if (prevTriggered) {
    if (currentPage >= static_cast<uint32_t>(skipAmount)) {
      currentPage -= skipAmount;
    } else {
      currentPage = 0;
    }
    updateRequired = true;
  } else if (nextTriggered) {
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

  // Bounds check
  if (currentPage >= xtc->getPageCount()) {
    // Show end of book screen
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

  // Calculate buffer size for one page
  // XTG (1-bit): Row-major, ((width+7)/8) * height bytes
  // XTH (2-bit): Two bit planes, column-major, ((width * height + 7) / 8) * 2 bytes
  size_t pageBufferSize;

  if (bitDepth == 2) {
    pageBufferSize = ((static_cast<size_t>(pageWidth) * pageHeight + 7) / 8) * 2;
  } else {
    pageBufferSize = ((pageWidth + 7) / 8) * pageHeight;
  }

  // ========== 修改1：替换原有局部 malloc 逻辑为复用缓冲区 ==========
  // 复用全局缓冲区，仅尺寸不足时重新分配（删除原有局部 pageBuffer 定义）
  if (pageBufferCapacity < pageBufferSize) {
    // 释放旧缓冲区
    if (pageBuffer) {
      free(pageBuffer);
      pageBuffer = nullptr;
    }
    // 用 calloc 初始化（避免脏数据），替代原有的 malloc
    pageBuffer = static_cast<uint8_t*>(calloc(1, pageBufferSize));
    if (!pageBuffer) {
      Serial.printf("[%lu] [XTR] Failed to allocate page buffer (%lu bytes)\n", millis(), pageBufferSize);
      renderer.clearScreen();
      renderer.drawCenteredText(UI_12_FONT_ID, 300, "Memory error", true, EpdFontFamily::BOLD);
      renderer.displayBuffer();
      return;
    }
    pageBufferCapacity = pageBufferSize; // 更新缓冲区容量
  }
  // ========== 修改1 结束 ==========

  // Load page data
  // 注意：这里直接用全局 pageBuffer，不再用局部变量
  size_t bytesRead = xtc->loadPage(currentPage, pageBuffer, pageBufferSize);

  if (bytesRead == 0) {
    Serial.printf("[%lu] [提示] 页码%lu加载中...\n", millis(), currentPage);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "Loading...", true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    updateRequired = true; 
    return;
  }
  // ========== 修改2：删除重复的 bytesRead == 0 判断（避免重复释放） ==========
  // 注释/删除以下这段重复逻辑
  // if (bytesRead == 0) {
  //   Serial.printf("[%lu] [XTR] Failed to load page %lu\n", millis(), currentPage);
  //   free(pageBuffer);
  //   renderer.clearScreen();
  //   renderer.drawCenteredText(UI_12_FONT_ID, 300, "Page load error", true, EpdFontFamily::BOLD);
  //   renderer.displayBuffer();
  //   return;
  // }
  // ========== 修改2 结束 ==========

  // Clear screen first
  renderer.clearScreen();

  // Copy page bitmap using GfxRenderer's drawPixel
  // XTC/XTCH pages are pre-rendered with status bar included, so render full page
  const uint16_t maxSrcY = pageHeight;

  if (bitDepth == 2) {
    // XTH 2-bit mode: Two bit planes, column-major order
    // - Columns scanned right to left (x = width-1 down to 0)
    // - 8 vertical pixels per byte (MSB = topmost pixel in group)
    // - First plane: Bit1, Second plane: Bit2
    // - Pixel value = (bit1 << 1) | bit2
    // - Grayscale: 0=White, 1=Dark Grey, 2=Light Grey, 3=Black

    const size_t planeSize = (static_cast<size_t>(pageWidth) * pageHeight + 7) / 8;
    const uint8_t* plane1 = pageBuffer;              // Bit1 plane
    const uint8_t* plane2 = pageBuffer + planeSize;  // Bit2 plane
    const size_t colBytes = (pageHeight + 7) / 8;    // Bytes per column (100 for 800 height)

    // Lambda to get pixel value at (x, y)
    auto getPixelValue = [&](uint16_t x, uint16_t y) -> uint8_t {
      const size_t colIndex = pageWidth - 1 - x;
      const size_t byteInCol = y / 8;
      const size_t bitInByte = 7 - (y % 8);
      const size_t byteOffset = colIndex * colBytes + byteInCol;
      const uint8_t bit1 = (plane1[byteOffset] >> bitInByte) & 1;
      const uint8_t bit2 = (plane2[byteOffset] >> bitInByte) & 1;
      return (bit1 << 1) | bit2;
    };

    // Optimized grayscale rendering without storeBwBuffer (saves 48KB peak memory)
    // Flow: BW display → LSB/MSB passes → grayscale display → re-render BW for next frame

    // Count pixel distribution for debugging
    uint32_t pixelCounts[4] = {0, 0, 0, 0};
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        pixelCounts[getPixelValue(x, y)]++;
      }
    }
    Serial.printf("[%lu] [XTR] Pixel distribution: White=%lu, DarkGrey=%lu, LightGrey=%lu, Black=%lu\n", millis(),
                  pixelCounts[0], pixelCounts[1], pixelCounts[2], pixelCounts[3]);

    // Pass 1: BW buffer - draw all non-white pixels as black
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) >= 1) {
          renderer.drawPixel(x, y, true);
        }
      }
    }

    // Display BW with conditional refresh based on pagesUntilFullRefresh
    if (pagesUntilFullRefresh <= 1) {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    } else {
      renderer.displayBuffer();
      pagesUntilFullRefresh--;
    }

    // Pass 2: LSB buffer - mark DARK gray only (XTH value 1)
    // In LUT: 0 bit = apply gray effect, 1 bit = untouched
    renderer.clearScreen(0x00);
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) == 1) {  // Dark grey only
          renderer.drawPixel(x, y, false);
        }
      }
    }
    renderer.copyGrayscaleLsbBuffers();

    // Pass 3: MSB buffer - mark LIGHT AND DARK gray (XTH value 1 or 2)
    // In LUT: 0 bit = apply gray effect, 1 bit = untouched
    renderer.clearScreen(0x00);
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        const uint8_t pv = getPixelValue(x, y);
        if (pv == 1 || pv == 2) {  // Dark grey or Light grey
          renderer.drawPixel(x, y, false);
        }
      }
    }
    renderer.copyGrayscaleMsbBuffers();

    // Display grayscale overlay
    renderer.displayGrayBuffer();

    // Pass 4: Re-render BW to framebuffer (restore for next frame, instead of restoreBwBuffer)
    renderer.clearScreen();
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) >= 1) {
          renderer.drawPixel(x, y, true);
        }
      }
    }

    // Cleanup grayscale buffers with current frame buffer
    renderer.cleanupGrayscaleWithFrameBuffer();

    // ========== 修改3：删除 2-bit 分支里的 free(pageBuffer) ==========
    // free(pageBuffer);  // 注释/删除这行，全局缓冲区统一在 onExit 释放
    // ========== 修改3 结束 ==========

    Serial.printf("[%lu] [XTR] Rendered page %lu/%lu (2-bit grayscale)\n", millis(), currentPage + 1,
                  xtc->getPageCount());
    return;
  } else {
    // 1-bit mode: 8 pixels per byte, MSB first
    const size_t srcRowBytes = (pageWidth + 7) / 8;  // 60 bytes for 480 width

    for (uint16_t srcY = 0; srcY < maxSrcY; srcY++) {
      const size_t srcRowStart = srcY * srcRowBytes;

      for (uint16_t srcX = 0; srcX < pageWidth; srcX++) {
        // Read source pixel (MSB first, bit 7 = leftmost pixel)
        const size_t srcByte = srcRowStart + srcX / 8;
        const size_t srcBit = 7 - (srcX % 8);
        const bool isBlack = !((pageBuffer[srcByte] >> srcBit) & 1);  // XTC: 0 = black, 1 = white

        if (isBlack) {
          renderer.drawPixel(srcX, srcY, true);
        }
      }
    }
  }
  // White pixels are already cleared by clearScreen()

  // ========== 修改4：删除函数末尾的 free(pageBuffer) ==========
  // free(pageBuffer);  // 注释/删除这行，全局缓冲区统一在 onExit 释放
  // ========== 修改4 结束 ==========

  // XTC pages already have status bar pre-rendered, no need to add our own

  // Display with appropriate refresh
  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }

  Serial.printf("[%lu] [XTR] Rendered page %lu/%lu (%u-bit)\n", millis(), currentPage + 1, xtc->getPageCount(),
                bitDepth);
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