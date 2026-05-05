#include "TxtReaderActivity.h"

#include <GfxRenderer.h>
#include <SDCardManager.h>
#include <Serialization.h>
#include <BluetoothHIDManager.h>
#include <Utf8.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

#include "BookmarkActivity.h"
#include "ReaderEntryModeSelectionActivity.h"
#include "TxtBookmarkSelectionActivity.h"
#include "TxtReaderChapterSelectionActivity.h"
#include "../settings/BluetoothSettingsActivity.h"
#include "../../util/ReaderBluetoothBootstrap.h"

namespace {
constexpr unsigned long goHomeMs = 1000;
constexpr unsigned long bookmarkPressMs = 700;
constexpr int statusBarMargin = 20;
constexpr int progressBarMarginTop = 1;
constexpr size_t CHUNK_SIZE = 8 * 1024;  // 8KB chunk for reading

// Cache file magic and version
constexpr uint32_t CACHE_MAGIC = 0x54585449;  // "TXTI"
constexpr uint8_t CACHE_VERSION = 3;          // Increment when cache format changes
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

void drawDashedLine(GfxRenderer& renderer, int x1, int y, int x2, const bool isDark) {
  const int startX = std::min(x1, x2);
  const int endX = std::max(x1, x2);
  int currentX = startX;

  const int dashLen = 20;
  const int gapLen = 10;

  while (currentX < endX) {
    const int segmentEndX = std::min(currentX + dashLen, endX);
    renderer.drawLine(currentX, y, segmentEndX, y, isDark);
    currentX = segmentEndX + gapLen;
  }
}

uint32_t utf8NextCodepointAt(const std::string& s, size_t& pos) {
  if (pos >= s.size()) {
    return 0;
  }

  const unsigned char c = static_cast<unsigned char>(s[pos]);
  uint32_t cp = 0;
  size_t len = 0;

  if (c < 0x80) {
    cp = c;
    len = 1;
  } else if (c < 0xE0 && pos + 1 < s.size()) {
    cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(s[pos + 1]) & 0x3F);
    len = 2;
  } else if (c < 0xF0 && pos + 2 < s.size()) {
    cp = ((c & 0x0F) << 12) | ((static_cast<unsigned char>(s[pos + 1]) & 0x3F) << 6) |
         (static_cast<unsigned char>(s[pos + 2]) & 0x3F);
    len = 3;
  } else if (pos + 3 < s.size()) {
    cp = ((c & 0x07) << 18) | ((static_cast<unsigned char>(s[pos + 1]) & 0x3F) << 12) |
         ((static_cast<unsigned char>(s[pos + 2]) & 0x3F) << 6) |
         (static_cast<unsigned char>(s[pos + 3]) & 0x3F);
    len = 4;
  } else {
    cp = c;
    len = 1;
  }

  pos += len;
  return cp;
}

bool isCjkLeadingPunctuation(const std::string& unit) {
  size_t pos = 0;
  const uint32_t cp = utf8NextCodepointAt(unit, pos);
  const uint32_t leadingPuncts[] = {
      0x3002,  // 。
      0xFF0C,  // ，
      0xFF01,  // ！
      0xFF1F,  // ？
      0xFF1B,  // ；
      0xFF1A,  // ：
      0x3001,  // 、
      0xFF09,  // ）
      0x301B,  // 】
      0x300B,  // 》
      0x201D,  // ”
      0x2019,  // ’
  };

  for (const auto p : leadingPuncts) {
    if (cp == p) {
      return true;
    }
  }
  return false;
}

size_t utf8CharLenAt(const std::string& s, const size_t pos) {
  if (pos >= s.size()) {
    return 0;
  }
  const unsigned char c = static_cast<unsigned char>(s[pos]);
  if (c < 0x80) {
    return 1;
  }
  if (c < 0xE0) {
    return (pos + 1 < s.size()) ? 2 : 1;
  }
  if (c < 0xF0) {
    return (pos + 2 < s.size()) ? 3 : 1;
  }
  return (pos + 3 < s.size()) ? 4 : 1;
}

size_t prevUtf8Boundary(const std::string& s, size_t idx) {
  if (idx == 0) {
    return 0;
  }
  if (idx > s.size()) {
    idx = s.size();
  }
  idx--;
  while (idx > 0 && (static_cast<unsigned char>(s[idx]) & 0xC0) == 0x80) {
    idx--;
  }
  return idx;
}

size_t leadingTrimBytesForIndent(const std::string& s) {
  size_t pos = 0;
  while (pos < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[pos]);
    if (c == ' ' || c == '\t') {
      pos += 1;
      continue;
    }
    // U+00A0 NO-BREAK SPACE
    if (pos + 1 < s.size() && c == 0xC2 && static_cast<unsigned char>(s[pos + 1]) == 0xA0) {
      pos += 2;
      continue;
    }
    // U+2003 EM SPACE
    if (pos + 2 < s.size() && c == 0xE2 && static_cast<unsigned char>(s[pos + 1]) == 0x80 &&
        static_cast<unsigned char>(s[pos + 2]) == 0x83) {
      pos += 3;
      continue;
    }
    // U+3000 IDEOGRAPHIC SPACE
    if (pos + 2 < s.size() && c == 0xE3 && static_cast<unsigned char>(s[pos + 1]) == 0x80 &&
        static_cast<unsigned char>(s[pos + 2]) == 0x80) {
      pos += 3;
      continue;
    }
    break;
  }
  return pos;
}

}  // namespace

void TxtReaderActivity::taskTrampoline(void* param) {
  auto* self = static_cast<TxtReaderActivity*>(param);
  self->displayTaskLoop();
}

void TxtReaderActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
 

  if (!txt) {
    return;
  }

  shuttingDown = false;

  // Configure screen orientation based on settings
  switch (SETTINGS.orientation) {
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

  renderingMutex = xSemaphoreCreateMutex();

  txt->setupCacheDir();
  loadProgress();

  // Save current txt as last opened file and add to recent books
  auto filePath = txt->getPath();
  auto fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(filePath, fileName, "", "");

  // Trigger first update
  updateRequired = true;
  lastAutoPageTurnMs = millis();

  xTaskCreate(&TxtReaderActivity::taskTrampoline, "TxtReaderActivityTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void TxtReaderActivity::onExit() {
  shuttingDown = true;
  updateRequired = false;

  // Turn off auto page turning first to avoid triggering render updates during teardown.
  SETTINGS.autoPageTurn = 0;

  ActivityWithSubactivity::onExit();

  // Force-release BLE resources when leaving reader to maximize memory for parsing/rendering.
  auto& btMgr = BluetoothHIDManager::getInstance();
  if (btMgr.isEnabled()) {
    btMgr.disable();
  }
    // Save progress
  saveProgress();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  if (displayTaskHandle) {
    if (xSemaphoreTake(renderingMutex, portMAX_DELAY) == pdTRUE) {
      vTaskDelete(displayTaskHandle);
      displayTaskHandle = nullptr;
      xSemaphoreGive(renderingMutex);
    }
  }

  if (renderingMutex) {
    vSemaphoreDelete(renderingMutex);
  }
  renderingMutex = nullptr;
  pageOffsets.clear();
  currentPageLines.clear();
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  txt.reset();
}

void TxtReaderActivity::loop() {
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
  // 短按确认后选择进入目录或书签
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (mappedInput.getHeldTime() >= bookmarkPressMs) {
      const int percent = totalPages > 0
                              ? clampPercent(static_cast<int>(((currentPage + 1) * 100.0f) / totalPages + 0.5f))
                              : 0;
      if (xSemaphoreTake(renderingMutex, portMAX_DELAY) == pdTRUE) {
        exitActivity();
        enterNewActivity(new BookmarkActivity(
            renderer, mappedInput, txt->getPath(), txt->getCachePath(), percent, currentPage, totalPages, chapternum,
            [this](bool) {
              exitActivity();
              updateRequired = true;
            }));
        xSemaphoreGive(renderingMutex);
      }
      return;
    }

    if (xSemaphoreTake(renderingMutex, portMAX_DELAY) == pdTRUE) {
      exitActivity();
      enterNewActivity(new ReaderEntryModeSelectionActivity(
          this->renderer, this->mappedInput,
          [this]() {
            exitActivity();
            skipNextButtonCheck = true;
            updateRequired = true;
          },
          [this](ReaderEntryModeSelectionActivity::Mode mode) {
            exitActivity();
            if (mode == ReaderEntryModeSelectionActivity::Mode::CHAPTER) {
              enterNewActivity(new TxtReaderChapterSelectionActivity(
                  this->renderer, this->mappedInput, txt, chapternum,
                  [this] {
                    exitActivity();
                    updateRequired = true;
                  },
                  [this](const int newChapterNum) {
                    chapternum = newChapterNum;
                    chapter_initialized = false;
                    pageOffsets.clear();
                    totalPages = 0;
                    currentPage = 0;
                    updateRequired = true;
                    exitActivity();
                    updateRequired = true;
                  }));
            } else if (mode == ReaderEntryModeSelectionActivity::Mode::BOOKMARK) {
              enterNewActivity(new TxtBookmarkSelectionActivity(
                  this->renderer, this->mappedInput, txt,
                  [this]() {
                    exitActivity();
                    updateRequired = true;
                  },
                  [this](int newChapterNum, int newPage, int percent) {
                    chapternum = newChapterNum;
                    chapter_initialized = false;
                    pageOffsets.clear();
                    totalPages = 0;
                    currentPage = std::max(0, newPage);
                    pendingBookmarkPercent = clampPercent(percent);
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
    }
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

  if (prevTriggered) {
    lastAutoPageTurnMs = millis();
    if (currentPage > 0) {
      currentPage--;
      updateRequired = true;
    } else if (chapternum > 0) {
      // 上一章：重置状态 + 切换章节
      chapternum--;
      chapter_initialized = false;  // 重置初始化标记，强制重新初始化
      pageOffsets.clear();          // 清空上一章节页码
      totalPages = 0;               // 重置总页数
      if (!chapter_initialized) {
        chapter_initializeReader(chapternum);
      }
      currentPage = totalPages;
      updateRequired = true;
      Serial.printf("[%lu] [TRS] Switch to chapter %d (prev), start from page 0\n", millis(), chapternum);
    }
  } else if (nextTriggered) {
    if (!autoTurnTriggered) {
      lastAutoPageTurnMs = millis();
    }
    if (currentPage < totalPages - 1) {
      currentPage++;
      updateRequired = true;
    } else {
      // 下一章：先获取总章节数，避免越界
      //int totalChapters = txt->getTotalChapters(); // todo
      //if (chapternum < totalChapters - 1) {
        chapternum++;
        chapter_initialized = false;  // 重置初始化标记
        pageOffsets.clear();          // 清空上一章节页码
        totalPages = 0;               // 重置总页数
        currentPage = 0;
        updateRequired = true;
        Serial.printf("[%lu] [TRS] Switch to chapter %d (next), start from page 0\n", millis(), chapternum);
      //}
    }
  }
}



void TxtReaderActivity::displayTaskLoop() {
  while (true) {
    if (shuttingDown) {
      vTaskDelete(nullptr);
    }

    if (updateRequired) {
      updateRequired = false;
      APP_STATE.isRenderComplete = false; // 标记渲染开始
      if (xSemaphoreTake(renderingMutex, portMAX_DELAY) == pdTRUE) {
        renderScreen();
        APP_STATE.isRenderComplete = true;  // 标记渲染完成（包括 saveProgress）
        APP_STATE.saveToFile();
        xSemaphoreGive(renderingMutex);
      }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}


void TxtReaderActivity::chapter_initializeReader(int chapter_num) {
  if (chapter_initialized) {
    return;
  }

  const unsigned long initStartMs = millis();

  // 校验章节索引合法性
  if (chapter_num < 0 ) {
    chapter_initialized = true;
    return;
  }

  // Store current settings for cache validation
  cachedFontId = SETTINGS.getReaderFontId();
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;

  // Calculate viewport dimensions
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);


  auto metrics = UITheme::getInstance().getMetrics();

  // Add status bar margin
  if (SETTINGS.statusBar != CrossPointSettings::STATUS_BAR_MODE::NONE) {
    const bool showProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                                 SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::ONLY_BOOK_PROGRESS_BAR ||
                                 SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
    orientedMarginBottom += statusBarMargin  +
                            (showProgressBar ? (metrics.bookProgressBarHeight + progressBarMarginTop) : 0);
  }
  orientedMarginTop += SETTINGS.screenMargin_Top;
  orientedMarginLeft += SETTINGS.screenMargin_Left;
  orientedMarginRight += SETTINGS.screenMargin_Right;
  orientedMarginBottom += SETTINGS.screenMargin_Bottom;
  
  viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const int viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  //行距加这里？
  float lineHeight = renderer.getLineHeight(cachedFontId)* SETTINGS.getReaderLineCompression();

  linesPerPage = viewportHeight / lineHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  Serial.printf("[%lu] [TRS] Viewport: %dx%d, lines per page: %d (chapter %d)\n", millis(), viewportWidth, viewportHeight,
                linesPerPage, chapter_num);

  const unsigned long cacheCheckStartMs = millis();
  const bool pageCacheHit = chapter_loadPageIndexCache(chapter_num);
  const unsigned long cacheCheckCostMs = millis() - cacheCheckStartMs;
  Serial.printf("[%lu] [TRS-TIME] chapter=%d cache_check=%lums hit=%d\n", millis(), chapter_num,
                cacheCheckCostMs, pageCacheHit ? 1 : 0);

  if (!pageCacheHit) {
    // Cache not found, build page index for current chapter
    constexpr int CHAPTER_BATCH_SIZE = 15;
    const int page = chapter_num / CHAPTER_BATCH_SIZE + 1;
    static int parsedPage = -1;
    const int pagebegin = (page - 1) * CHAPTER_BATCH_SIZE;
    // 每个章节批次加载一次
    unsigned long chapterParseCostMs = 0;
    if (parsedPage != page) {
      const unsigned long chapterParseStartMs = millis();
      txt->parseChapterIndexAndOffset(pagebegin);
      chapterParseCostMs = millis() - chapterParseStartMs;
      parsedPage = page;
      Serial.printf("[%lu] [TRS-TIME] chapter=%d parseChapterIndexAndOffset=%lums batchStart=%d\n", millis(),
                    chapter_num, chapterParseCostMs, pagebegin);
    }
    Serial.printf("[%lu] [TRS] load txtchapter: %d \n", millis(), chapter_num);
    //当前章节的范围
    //加一个多次尝试，避免empty file出现过多
    // 带重试的章节起止偏移获取：重试时仅等待，不重复触发重解析
    size_t chapterOffsetbegin = txt->getChapterOffsetByIndex(chapter_num);
    size_t chapterOffsetend = txt->getChapterendOffsetByIndex(chapter_num);
    for (int r = 0; r < 5 && (chapterOffsetbegin == 0 || chapterOffsetend == 0); r++) {
      vTaskDelay(20 / portTICK_PERIOD_MS);
      chapterOffsetbegin = txt->getChapterOffsetByIndex(chapter_num);
      chapterOffsetend = txt->getChapterendOffsetByIndex(chapter_num);
      Serial.printf("[TRS] Retry get chapter %d range (attempt %d)\n", chapter_num, r + 1);
    }


    // 处理最后一章：结束位置为文件末尾
    if (chapterOffsetend == 0 || chapterOffsetend <= chapterOffsetbegin) {
      chapterOffsetend = txt->getFileSize();
    }
    //加个判断防止解析全书
    if (chapterOffsetend - chapterOffsetbegin > 100000) {
    Serial.printf("[%lu] [TRS] 章节读取失败，确认键进入目录重选\n", 
                  millis());
    return;
   }
    const unsigned long buildIndexStartMs = millis();
    buildPageIndex(chapterOffsetbegin, chapterOffsetend - 1);
    const unsigned long buildIndexCostMs = millis() - buildIndexStartMs;
    Serial.printf("[%lu] [TRS-TIME] chapter=%d buildPageIndex=%lums pages=%d range=%zu\n", millis(), chapter_num,
                  buildIndexCostMs, totalPages, (chapterOffsetend > chapterOffsetbegin) ? (chapterOffsetend - chapterOffsetbegin) : 0);

    //保存为章节缓存
    const unsigned long saveCacheStartMs = millis();
    chapter_savePageIndexCache(chapter_num);
    const unsigned long saveCacheCostMs = millis() - saveCacheStartMs;
    Serial.printf("[%lu] [TRS-TIME] chapter=%d savePageIndexCache=%lums\n", millis(), chapter_num, saveCacheCostMs);
  }

  // 修改为章节进度
  //loadProgress();

  chapter_initialized = true;
  Serial.printf("[%lu] [TRS-TIME] chapter=%d initialize_total=%lums\n", millis(), chapter_num,
                millis() - initStartMs);
}

void TxtReaderActivity::buildPageIndex(size_t beginByte, size_t endByte) {
  pageOffsets.clear();
  const unsigned long buildStartMs = millis();
  
  // 1. 参数合法性校验，避免越界
  const size_t fileSize = txt->getFileSize();
  beginByte = std::min(beginByte, fileSize);  
  endByte = std::min(endByte, fileSize);    
  if (beginByte >= endByte) {
    Serial.printf("[%lu] [TRS] Invalid range: begin=%zu, end=%zu (file size=%zu)\n", 
                  millis(), beginByte, endByte, fileSize);
    totalPages = 0;
    return;
  }

  // 2. 初始页从指定的beginByte开始
  pageOffsets.push_back(beginByte);  

  size_t offset = beginByte;
  Serial.printf("[%lu] [TRS] Building page index from %zu to %zu bytes...\n", 
                millis(), beginByte, endByte);

  GUI.drawPopup(renderer, "Indexing...");

  // 3. 循环终止条件改为：offset < endByte
  while (offset < endByte) {
    std::vector<std::string> tempLines;
    size_t nextOffset = offset;

    if (!loadPageAtOffset(offset, endByte,tempLines, nextOffset)) {
      Serial.printf("[%lu] [TRS] Failed to load page at offset %zu, stopping index build\n", millis(), offset);
      break;
    }

    if (nextOffset <= offset) {
      // 无进度，避免死循环
      Serial.printf("[%lu] [TRS] No progress at offset %zu, stopping index build\n", millis(), offset);
      break;
    }

    offset = nextOffset;
    // 仅当偏移量未到结束位置时，才添加到页码索引
    if (offset < endByte) {
      pageOffsets.push_back(offset);
    }

    // 定期让出CPU，避免阻塞其他任务
    if (pageOffsets.size() % 20 == 0) {
      vTaskDelay(1);
    }
  }

  totalPages = pageOffsets.size();
  Serial.printf("[%lu] [TRS] Built page index: %d pages (range %zu-%zu bytes)\n", 
                millis(), totalPages, beginByte, endByte);
  Serial.printf("[%lu] [TRS-TIME] buildPageIndex_loop=%lums pages=%d\n", millis(), millis() - buildStartMs, totalPages);
}



bool TxtReaderActivity::loadPageAtOffset(size_t offset, size_t endOffset, std::vector<std::string>& outLines,
                                         size_t& nextOffset, std::vector<int>* outIndentOffsets) {
  outLines.clear();
  if (outIndentOffsets) {
    outIndentOffsets->clear();
  }
  const size_t fileSize = txt->getFileSize();
  const size_t virtualFileEnd = std::min(endOffset, fileSize);

  if (offset >= virtualFileEnd) {
    return false;
  }

  // Read a chunk from file
  size_t chunkSize = std::min(CHUNK_SIZE, virtualFileEnd - offset);
  auto* buffer = static_cast<uint8_t*>(malloc(chunkSize + 1));
  if (!buffer) {
    Serial.printf("[%lu] [TRS] Failed to allocate %zu bytes\n", millis(), chunkSize);
    return false;
  }

  if (!txt->readContent(buffer, offset, chunkSize)) {
    free(buffer);
    return false;
  }
  buffer[chunkSize] = '\0';

  // Parse lines from buffer
  size_t pos = 0;

  // 首行缩进控制变量
  const int indentWidth = renderer.getTextWidth(cachedFontId, "中")*2; // 缩进宽度
  const bool enableFirstLineIndent = SETTINGS.firstlineintented;
  bool isFirstLineOfPage = true; // 每页第一行不缩进

  auto appendOutputLine = [&](const std::string& text, const int indentOffsetPx) {
    outLines.push_back(text);
    if (outIndentOffsets) {
      outIndentOffsets->push_back(indentOffsetPx);
    }
  };

  while (pos < chunkSize && static_cast<int>(outLines.size()) < linesPerPage) {
    // Find end of line
    size_t lineEnd = pos;
    while (lineEnd < chunkSize && buffer[lineEnd] != '\n') {
      lineEnd++;
    }

    // Check if we have a complete line
    bool lineComplete = (lineEnd < chunkSize) || (offset + lineEnd >= virtualFileEnd);

    if (!lineComplete && static_cast<int>(outLines.size()) > 0) {
      // Incomplete line and we already have some lines, stop here
      break;
    }

    // Calculate the actual length of line content in the buffer (excluding newline)
    size_t lineContentLen = lineEnd - pos;

    // Check for carriage return
    bool hasCR = (lineContentLen > 0 && buffer[pos + lineContentLen - 1] == '\r');
    size_t displayLen = hasCR ? lineContentLen - 1 : lineContentLen;

    // Extract line content for display (without CR/LF)
    std::string line(reinterpret_cast<char*>(buffer + pos), displayLen);

    // 空行标记段落结束，下一段需要缩进（仅对原生行生效）
    if (displayLen == 0) {
      pos = lineEnd + 1;
      needIndent = true; // 空行后，下一段原生行需要缩进
      continue;
    }

    // 段首空格清洗：无论是否启用首行缩进，都去掉源文本前导空白。
    size_t leadingTrimBytes = leadingTrimBytesForIndent(line);
    if (leadingTrimBytes > 0) {
      line.erase(0, leadingTrimBytes);
    }
    if (line.empty()) {
      pos = lineEnd + 1;
      needIndent = true;
      continue;
    }

    // 当前源行的首个渲染片段才允许缩进
    bool isFirstWrappedLineOfSource = true;

    // Track position within this source line (in bytes from pos)
    size_t lineBytePos = leadingTrimBytes;

    // Word wrap if needed
    while (!line.empty() && static_cast<int>(outLines.size()) < linesPerPage) {
      // 计算行宽：仅原生行需要考虑缩进宽度，拆行完全不考虑
      int lineWidth = renderer.getTextWidth(cachedFontId, line.c_str());
      // 缩进判断：仅原生行 + 需要缩进 + 不是页首 + 无已有空格
      const bool doIndent = enableFirstLineIndent && isFirstWrappedLineOfSource && needIndent && !isFirstLineOfPage;
      //测试
      //const bool doIndent = true;
      
      if (doIndent) {
        lineWidth += indentWidth; // 仅原生行预留缩进宽度
      }

      // 字距处理（原有逻辑）
      switch (cachedParagraphAlignment) {
        case CrossPointSettings::LEFT_ALIGN:
        lineWidth = lineWidth+wordSpacing;
        //Serial.printf("左对齐字间距生效：wordSpacing=%d\n", wordSpacing);
      }

      if (lineWidth <= viewportWidth) {
        // 仅原生行添加缩进，拆行完全不添加
        if (doIndent) {
          appendOutputLine(line, indentWidth);
          needIndent = false; // 原生行缩进后，该段落后续行（包括拆行）都不缩进
        } else {
          appendOutputLine(line, 0);
        }
        isFirstWrappedLineOfSource = false;
        lineBytePos = displayLen;  // Consumed entire display content
        line.clear();
        isFirstLineOfPage = false; // 每页第一行已处理
        break;
      }

        // Find break point（拆行逻辑，二分优化）
        // 若当前源行首段需要缩进，则首段可用宽度要扣除缩进宽度。
        const int allowedWidth =
          viewportWidth - (cachedParagraphAlignment == CrossPointSettings::LEFT_ALIGN ? wordSpacing : 0) -
          (doIndent ? indentWidth : 0);

      auto alignUtf8Boundary = [&](size_t idx) -> size_t {
        if (idx >= line.length()) return line.length();
        while (idx > 0 && (static_cast<uint8_t>(line[idx]) & 0xC0) == 0x80) {
          idx--;
        }
        return idx;
      };

      size_t low = 1;
      size_t high = line.length();
      size_t bestFit = 0;
      while (low <= high) {
        const size_t mid = low + (high - low) / 2;
        size_t testPos = alignUtf8Boundary(mid);
        if (testPos == 0) {
          testPos = 1;
        }

        const int testWidth = renderer.getTextWidth(cachedFontId, line.substr(0, testPos).c_str());
        if (testWidth <= allowedWidth) {
          bestFit = testPos;
          low = mid + 1;
        } else {
          if (testPos <= 1) {
            break;
          }
          high = testPos - 1;
        }
      }

      size_t breakPos = bestFit;
      if (breakPos == 0) {
        breakPos = alignUtf8Boundary(1);
        if (breakPos == 0) {
          breakPos = 1;
        }
      }

      // 优先在可容纳范围内按空格断开（英文更自然）
      if (breakPos > 1) {
        const size_t spacePos = line.rfind(' ', breakPos - 1);
        if (spacePos != std::string::npos && spacePos > 0) {
          breakPos = spacePos;
        }
      }

      // 行首标点避讳：尽量避免下一行以中文结束标点开头。
      if (breakPos < line.length()) {
        size_t nextLen = utf8CharLenAt(line, breakPos);
        if (nextLen > 0) {
          std::string nextUnit = line.substr(breakPos, nextLen);
          if (isCjkLeadingPunctuation(nextUnit)) {
            const int maxOverflow = std::max(1, allowedWidth / 20);  // 允许最多 5% 溢出
            const int prevWidth = renderer.getTextWidth(cachedFontId, line.substr(0, breakPos).c_str()) +
                                  (doIndent ? indentWidth : 0);
            const int punctWidth = renderer.getTextWidth(cachedFontId, nextUnit.c_str());

            // 优先将标点并入上一行（与 EPUB 的策略一致）
            if (prevWidth + punctWidth <= allowedWidth + maxOverflow) {
              breakPos += nextLen;
            } else {
              // 放不下则尝试回退断点，避免下一行首字符是禁忌标点。
              while (breakPos > 1) {
                size_t testLen = utf8CharLenAt(line, breakPos);
                if (testLen == 0) {
                  break;
                }
                const std::string testUnit = line.substr(breakPos, testLen);
                if (!isCjkLeadingPunctuation(testUnit)) {
                  break;
                }
                const size_t prevPos = prevUtf8Boundary(line, breakPos);
                if (prevPos == breakPos || prevPos == 0) {
                  break;
                }
                breakPos = prevPos;
              }
            }
          }
        }
      }

      // 拆行后的首段如果属于段首，仍需执行首行缩进。
      if (doIndent) {
        appendOutputLine(line.substr(0, breakPos), indentWidth);
        needIndent = false;
      } else {
        appendOutputLine(line.substr(0, breakPos), 0);
      }
      isFirstWrappedLineOfSource = false;

      // Skip space at break point
      size_t skipChars = breakPos;
      if (breakPos < line.length() && line[breakPos] == ' ') {
        skipChars++;
      }
      lineBytePos += skipChars;
      line = line.substr(skipChars);
      isFirstLineOfPage = false; // 每页第一行已处理
    }

    // Determine how much of the source buffer we consumed
    if (line.empty()) {
      // Fully consumed this source line, move past the newline
      pos = lineEnd + 1;
      needIndent = true; // 换行了，下一段原生行需要缩进
    } else {
      // Partially consumed - page is full mid-line
      // Move pos to where we stopped in the line (NOT past the line)
      pos = pos + lineBytePos;
      break;
    }
  }

  // Ensure we make progress even if calculations go wrong
  if (pos == 0 && !outLines.empty()) {
    // Fallback: at minimum, consume something to avoid infinite loop
    pos = 1;
  }

  nextOffset = offset + pos;

  // Make sure we don't go past the file
  // 章节结束位置作为文件末尾，避免越界
  if (nextOffset > virtualFileEnd) {
    nextOffset = virtualFileEnd;
  }

  free(buffer);

  return !outLines.empty();
}


void TxtReaderActivity::renderPage() {
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin_Top;
  orientedMarginLeft += SETTINGS.screenMargin_Left;
  orientedMarginRight += SETTINGS.screenMargin_Right;
  orientedMarginBottom += SETTINGS.screenMargin_Bottom; 

  float lineHeight = renderer.getLineHeight(cachedFontId)* SETTINGS.getReaderLineCompression();
  const int contentWidth = viewportWidth;

  // Render text lines with alignment
  auto renderLines = [&]() {
    int y = orientedMarginTop;
    const int lineVisualHeight = renderer.getLineHeight(cachedFontId);
    const int lineXStart = orientedMarginLeft;
    const int lineXEnd = renderer.getScreenWidth() - orientedMarginRight;
    for (size_t i = 0; i < currentPageLines.size(); i++) {
      const auto& line = currentPageLines[i];
      if (!line.empty()) {
        const int indentOffset =
            (i < currentPageIndentOffsets.size()) ? std::max(0, currentPageIndentOffsets[i]) : 0;
        int x = orientedMarginLeft + indentOffset;

        // Apply text alignment
        switch (cachedParagraphAlignment) {
          case CrossPointSettings::LEFT_ALIGN:
          default:
            // x already set to left margin
            break;
          case CrossPointSettings::CENTER_ALIGN: {
            int textWidth = renderer.getTextWidth(cachedFontId, line.c_str());
            x = orientedMarginLeft + (contentWidth - textWidth) / 2;
            break;
          }
          case CrossPointSettings::RIGHT_ALIGN: {
            int textWidth = renderer.getTextWidth(cachedFontId, line.c_str());
            x = orientedMarginLeft + contentWidth - textWidth;
            break;
          }
          case CrossPointSettings::JUSTIFIED:
            // For plain text, justified is treated as left-aligned
            // (true justification would require word spacing adjustments)
            break;
        }

        renderer.drawText(cachedFontId, x, y, line.c_str());

        // Keep line decoration in BW pass only; grayscale layers are text-AA data.
        if (CrossPointSettings::getInstance().extraline && renderer.getRenderMode() == GfxRenderer::BW) {
          const int dashY = y + lineVisualHeight + 2;
          drawDashedLine(renderer, lineXStart, dashY, lineXEnd, true);
        }
      }
      y += lineHeight;
    }
  };

  // First pass: BW rendering
  renderLines();
  renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginTop, orientedMarginLeft);

  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }

  // Grayscale rendering pass (for anti-aliased fonts)
  if (SETTINGS.textAntiAliasing) {
    // Save BW buffer for restoration after grayscale pass
    renderer.storeBwBuffer();

    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderLines();
    renderer.copyGrayscaleLsbBuffers();

    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderLines();
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);

    // Restore BW buffer
    renderer.restoreBwBuffer();
  }

  if (!bluetoothBootstrapDone) {
    bluetoothBootstrapDone = true;
    initializeBluetoothAfterReaderRender();
  }
}





void TxtReaderActivity::renderScreen() {
  if (!txt) {
    return;
  }

  // Initialize reader if not done
  if (!chapter_initialized) {
    chapter_initializeReader(chapternum);
  }

  if (pageOffsets.empty()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "Empty file", true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (pendingBookmarkPercent >= 0 && totalPages > 0) {
    if (pendingBookmarkPercent >= 100) {
      currentPage = totalPages - 1;
    } else {
      currentPage = static_cast<int>((static_cast<float>(pendingBookmarkPercent) / 100.0f) * totalPages);
    }
    pendingBookmarkPercent = -1;
  }


  if (currentPage < 0) currentPage = 0;
  // 仅当currentPage超过总页数时修正（避免无效页码）
  if (currentPage >= totalPages) currentPage = totalPages - 1;

  // Load current page content
  size_t offset = pageOffsets[currentPage];
  size_t nextOffset;
  currentPageLines.clear();
  currentPageIndentOffsets.clear();
  size_t endoffset = txt->getChapterendOffsetByIndex(chapternum);
  if (endoffset == 0 || endoffset <= offset) {
    endoffset = txt->getFileSize();
  }
  loadPageAtOffset(offset, endoffset, currentPageLines, nextOffset, &currentPageIndentOffsets);

  renderer.clearScreen();
    //加背景
  if(SETTINGS.ReadingScreenEnabled){
    Serial.printf("[%lu] [ERS] 壁纸屏幕开启，渲染壁纸屏幕\n");
    renderPngSleepScreen(renderer);
  }
  renderPage();
}



void TxtReaderActivity::renderStatusBar(const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginTop, const int orientedMarginLeft) const {
  auto metrics = UITheme::getInstance().getMetrics();

  // determine visible status bar elements (same rules as Epub)
  const bool showProgressPercentage = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL;
  const bool showProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                               SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::ONLY_BOOK_PROGRESS_BAR;
  const bool showChapterProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showProgressText = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                                SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR;
  const bool showBookPercentage = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showBattery = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::NO_PROGRESS ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showTitle = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::NO_PROGRESS ||
                         SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                         SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                         SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage == CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_NEVER;

  // Position status bar near the bottom of the logical screen, regardless of orientation
  // add extra upward offset to avoid being clipped by the very bottom edge
  const auto screenHeight = renderer.getScreenHeight();
  constexpr int extraYOffset = 10;
  const auto textY = screenHeight - orientedMarginBottom - 8 - extraYOffset;
  int progressTextWidth = 0;

  // Calculate progress in book (for txt treat whole file as one chapter)
  const float progress = totalPages > 0 ? (currentPage + 1) * 100.0f / totalPages : 0;

  if (showProgressText || showProgressPercentage || showBookPercentage) {
    char progressStr[32];
    if (showProgressPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%d/%d  %.0f%%", currentPage + 1, totalPages, progress);
    } else if (showBookPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%.0f%%", progress);
    } else {
      snprintf(progressStr, sizeof(progressStr), "%d/%d", currentPage + 1, totalPages);
    }

    progressTextWidth = renderer.getTextWidth(SMALL_FONT_ID, progressStr);
    renderer.drawText(SMALL_FONT_ID, renderer.getScreenWidth() - orientedMarginRight - progressTextWidth, textY,
                      progressStr);
  }

  if (showProgressBar) {
    GUI.drawReadingProgressBar(renderer, static_cast<size_t>(progress));
  }

  if (showChapterProgressBar) {
    GUI.drawReadingProgressBar(renderer, static_cast<size_t>(progress));
  }

  if (showBattery) {
    GUI.drawBattery(renderer, Rect{orientedMarginLeft + 1, textY, metrics.batteryWidth, metrics.batteryHeight},
                    showBatteryPercentage);
  }

  if (showTitle) {
    const int rendererableScreenWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
    const int batterySize = showBattery ? (showBatteryPercentage ? 50 : 20) : 0;
    const int titleMarginLeft = batterySize + 30;
    const int titleMarginRight = progressTextWidth + 30;

    int titleMarginLeftAdjusted = std::max(titleMarginLeft, titleMarginRight);
    int availableTitleSpace = rendererableScreenWidth - 2 * titleMarginLeftAdjusted;

    std::string title = txt->getChapterTitleByIndex(chapternum);
    int titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    if (titleWidth > availableTitleSpace) {
      availableTitleSpace = rendererableScreenWidth - titleMarginLeft - titleMarginRight;
      titleMarginLeftAdjusted = titleMarginLeft;
    }
    if (titleWidth > availableTitleSpace) {
      title = renderer.truncatedText(SMALL_FONT_ID, title.c_str(), availableTitleSpace);
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    }

    renderer.drawText(SMALL_FONT_ID,
                      titleMarginLeftAdjusted + orientedMarginLeft + (availableTitleSpace - titleWidth) / 2, textY,
                      title.c_str());
  }
}

void TxtReaderActivity::saveProgress() const {

  FsFile f;
  if (SdMan.openFileForWrite("TRS", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[8];
    data[0] = currentPage & 0xFF;
    data[1] = (currentPage >> 8) & 0xFF;
    data[2] = 0;
    data[3] = 0;

    data[4] = chapternum & 0xFF;
    data[5] = (chapternum >> 8) & 0xFF;
    data[6] = 0;
    data[7] = 0;
    f.write(data, 8);
    f.close();
    Serial.printf("[%lu] [TRS] saveed progress: page %d/%d, chapter %d\n", millis(), currentPage, totalPages, chapternum);
  }
}

void TxtReaderActivity::loadProgress() {
  chapter_initialized = false;  // 重置初始化标记

  FsFile f;
  if (SdMan.openFileForRead("TRS", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[8];
    if (f.read(data, 8) == 8) {
      currentPage = data[0] + (data[1] << 8);
      chapternum = data[4] + (data[5] << 8);
      Serial.printf("[%lu] [TRS] Loaded progress: page %d/%d, chapter %d\n", millis(), currentPage, totalPages, chapternum);
    }
    f.close();
  }
}




bool TxtReaderActivity::chapter_loadPageIndexCache(int chapternum) {
  // Cache file format (using serialization module):
  // - uint32_t: magic "TXTI"
  // - uint8_t: cache version
  // - uint32_t: file size (to validate cache)
  // - int32_t: viewport width
  // - int32_t: lines per page
  // - int32_t: font ID (to invalidate cache on font change)
  // - int32_t: screen margin (to invalidate cache on margin change)
  // - uint8_t: paragraph alignment (to invalidate cache on alignment change)
  // - uint32_t: total pages count
  // - N * uint32_t: page offsets

  std::string cachePath = txt->getCachePath() +"/chapter"+ std::to_string(chapternum) + ".bin";
  FsFile f;
  if (!SdMan.openFileForRead("TRS", cachePath, f)) {
    Serial.printf("[%lu] [TRS] No page index cache found\n", millis());
    return false;
  }

  // Read and validate header using serialization module
  uint32_t magic;
  serialization::readPod(f, magic);
  if (magic != CACHE_MAGIC) {
    Serial.printf("[%lu] [TRS] Cache magic mismatch, rebuilding\n", millis());
    f.close();
    return false;
  }

  uint8_t version;
  serialization::readPod(f, version);
  if (version != CACHE_VERSION) {
    Serial.printf("[%lu] [TRS] Cache version mismatch (%d != %d), rebuilding\n", millis(), version, CACHE_VERSION);
    f.close();
    return false;
  }

  uint32_t fileSize;
  serialization::readPod(f, fileSize);
  if (fileSize != txt->getFileSize()) {
    Serial.printf("[%lu] [TRS] Cache file size mismatch, rebuilding\n", millis());
    f.close();
    return false;
  }

  int32_t cachedWidth;
  serialization::readPod(f, cachedWidth);
  if (cachedWidth != viewportWidth) {
    Serial.printf("[%lu] [TRS] Cache viewport width mismatch, rebuilding\n", millis());
    f.close();
    return false;
  }

  int32_t cachedLines;
  serialization::readPod(f, cachedLines);
  if (cachedLines != linesPerPage) {
    Serial.printf("[%lu] [TRS] Cache lines per page mismatch, rebuilding\n", millis());
    f.close();
    return false;
  }

  int32_t fontId;
  serialization::readPod(f, fontId);
  if (fontId != cachedFontId) {
    Serial.printf("[%lu] [TRS] Cache font ID mismatch (%d != %d), rebuilding\n", millis(), fontId, cachedFontId);
    f.close();
    return false;
  }
  //把字距行间距首行缩进记录进去
  uint8_t wordSpacing;
  serialization::readPod(f, wordSpacing);
  if (wordSpacing != this->wordSpacing) {
    Serial.printf("[%lu] [TRS] Cache word spacing mismatch, rebuilding\n", millis());
    f.close();
    return false;
  }

  uint8_t lineSpacing;
  serialization::readPod(f, lineSpacing);
  if (lineSpacing != SETTINGS.lineSpacing) {
    Serial.printf("[%lu] [TRS] Cache line spacing mismatch, rebuilding\n", millis());
    f.close();
    return false;
  }

  bool needIndent = false;
  serialization::readPod(f, needIndent);
  if (needIndent != SETTINGS.firstlineintented) {
    Serial.printf("[%lu] [TRS] Cache first line indent mismatch, rebuilding\n", millis());
    f.close();
    return false;
  }
//结束
  int32_t margin;
  serialization::readPod(f, margin);
  if (margin != cachedScreenMargin) {
    Serial.printf("[%lu] [TRS] Cache screen margin mismatch, rebuilding\n", millis());
    f.close();
    return false;
  }

  uint8_t alignment;
  serialization::readPod(f, alignment);
  if (alignment != cachedParagraphAlignment) {
    Serial.printf("[%lu] [TRS] Cache paragraph alignment mismatch, rebuilding\n", millis());
    f.close();
    return false;
  }

  uint32_t numPages;
  serialization::readPod(f, numPages);

  // Read page offsets
  pageOffsets.clear();
  pageOffsets.reserve(numPages);

  for (uint32_t i = 0; i < numPages; i++) {
    uint32_t offset;
    serialization::readPod(f, offset);
    pageOffsets.push_back(offset);
  }

  f.close();
  totalPages = pageOffsets.size();
  Serial.printf("[%lu] [TRS] Loaded page index cache: %d pages\n", millis(), totalPages);
  return true;
}

void TxtReaderActivity::chapter_savePageIndexCache(int chapternum) const {
  std::string cachePath = txt->getCachePath() +"/chapter"+ std::to_string(chapternum) + ".bin";
  FsFile f;
  if (!SdMan.openFileForWrite("TRS", cachePath, f)) {
    Serial.printf("[%lu] [TRS] Failed to save page index cache\n", millis());
    return;
  }

  // Write header using serialization module
  serialization::writePod(f, CACHE_MAGIC);
  serialization::writePod(f, CACHE_VERSION);
  serialization::writePod(f, static_cast<uint32_t>(txt->getFileSize()));
  serialization::writePod(f, static_cast<int32_t>(viewportWidth));
  serialization::writePod(f, static_cast<int32_t>(linesPerPage));
  serialization::writePod(f, static_cast<int32_t>(cachedFontId));
  //把字距行间距首行缩进记录进去
  serialization::writePod(f, wordSpacing);
  serialization::writePod(f, SETTINGS.lineSpacing);
  serialization::writePod(f, SETTINGS.firstlineintented);
  //结束
  serialization::writePod(f, static_cast<int32_t>(cachedScreenMargin));
  serialization::writePod(f, cachedParagraphAlignment);
  serialization::writePod(f, static_cast<uint32_t>(pageOffsets.size()));

  // Write page offsets
  for (size_t offset : pageOffsets) {
    serialization::writePod(f, static_cast<uint32_t>(offset));
  }

  f.close();
  Serial.printf("[%lu] [TRS] Saved page index cache: %d pages\n", millis(), totalPages);
}
void TxtReaderActivity::renderPngSleepScreen(GfxRenderer& renderer) const {
  const std::string pxcPath = WALLPAPER_PXC_PATH;
  if (loadWallpaperPxcToFramebuffer(pxcPath, renderer)) {
    Serial.printf("[%lu] [SLP] Loaded wallpaper PXC cache\n", millis());
    return;
  }
  Serial.printf("[%lu] [SLP] Wallpaper PXC missing, skip reader background\n", millis());
}