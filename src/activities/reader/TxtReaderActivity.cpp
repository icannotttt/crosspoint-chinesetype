#include "TxtReaderActivity.h"

#include <GfxRenderer.h>
#include <SDCardManager.h>
#include <Serialization.h>
#include <Utf8.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

#include "TxtReaderChapterSelectionActivity.h"

namespace {
constexpr unsigned long goHomeMs = 1000;
constexpr int statusBarMargin = 20;
constexpr int progressBarMarginTop = 1;
constexpr size_t CHUNK_SIZE = 8 * 1024;  // 8KB chunk for reading

// Cache file magic and version
constexpr uint32_t CACHE_MAGIC = 0x54585449;  // "TXTI"
constexpr uint8_t CACHE_VERSION = 3;          // Increment when cache format changes

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

  xTaskCreate(&TxtReaderActivity::taskTrampoline, "TxtReaderActivityTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void TxtReaderActivity::onExit() {
  ActivityWithSubactivity::onExit();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  // Wait until not rendering to delete task
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  pageOffsets.clear();
  currentPageLines.clear();
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  txt.reset();
}

void TxtReaderActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }
  // 进入章节目录
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
        exitActivity();
        enterNewActivity(new TxtReaderChapterSelectionActivity(
            this->renderer, this->mappedInput, txt, chapternum,
            [this] {
              exitActivity();
              updateRequired = true;
            },
            [this](const int newChapterNum) {
              chapternum = newChapterNum;       // 更新章节号
              chapter_initialized = false;  // 重置初始化标记
              pageOffsets.clear();          // 清空上一章节页码
              totalPages = 0;               // 重置总页数
              // 强制设置为0页（关键：覆盖后续loadProgress可能带来的干扰）
              currentPage = 0;
              updateRequired = true;
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

  if (prevTriggered) {
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
    if (updateRequired) {
      updateRequired = false;
      APP_STATE.isRenderComplete = false; // 标记渲染开始
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      renderScreen();
      APP_STATE.isRenderComplete = true;  // 标记渲染完成（包括 saveProgress）
      APP_STATE.saveToFile();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}


void TxtReaderActivity::chapter_initializeReader(int chapter_num) {
  if (chapter_initialized) {
    return;
  }

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

  if (!chapter_loadPageIndexCache(chapter_num)) {
    // Cache not found, build page index for current chapter
    const int page=chapter_num/25+1;
    static int parsedPage = -1;
    const int pagebegin=(page-1)*25;
    // 相隔24章加载一次
    if (parsedPage != page) {
      txt->parseChapterIndexAndOffset(pagebegin);
      parsedPage = page;
    }
    Serial.printf("[%lu] [TRS] load txtchapter: %d \n", millis(), chapter_num);
    //当前章节的范围
    //加一个多次尝试，避免empty file出现过多
    // 带5次重试的章节起始偏移量获取
    size_t chapterOffsetbegin = 0;
    for(int r=0; r<5 && (chapterOffsetbegin=txt->getChapterOffsetByIndex(chapter_num))==0; r++) {
      txt->parseChapterIndexAndOffset(pagebegin);
        vTaskDelay(50/portTICK_PERIOD_MS);
        Serial.printf("[TRS] Retry get chapter %d offset (attempt %d)\n", chapter_num, r+1);
    }

    // 带5次重试的章节结束偏移量获取
    size_t chapterOffsetend = 0;
    for(int r=0; r<5 && (chapterOffsetend=txt->getChapterendOffsetByIndex(chapter_num))==0; r++) {
      txt->parseChapterIndexAndOffset(pagebegin);
        vTaskDelay(50/portTICK_PERIOD_MS);
        Serial.printf("[TRS] Retry get chapter %d offset (attempt %d)\n", chapter_num, r+1);
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
    buildPageIndex(chapterOffsetbegin, chapterOffsetend - 1);
    //保存为章节缓存
    chapter_savePageIndexCache(chapter_num);
  }

  // 修改为章节进度
  //loadProgress();

  chapter_initialized = true;
}

void TxtReaderActivity::buildPageIndex(size_t beginByte, size_t endByte) {
  pageOffsets.clear();
  
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
}



bool TxtReaderActivity::loadPageAtOffset(size_t offset,size_t endOffset, std::vector<std::string>& outLines, size_t& nextOffset) {
  outLines.clear();
  const size_t fileSize = txt->getFileSize();
 // const size_t fileSize = endOffset; // 章节结束位置作为文件末尾，避免越界

  if (offset >= fileSize) {
    return false;
  }

  // Read a chunk from file
  size_t chunkSize = std::min(CHUNK_SIZE, fileSize - offset);
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
  const std::string indentStr = "\xe3\x80\x80\xe3\x80\x80"; // 两个全角空格
  //const std::string indentStr ="\u200B\u200B"; // 两个普通空格（测试用，实际使用全角空格）
  const int indentWidth = renderer.getTextWidth(cachedFontId, "中")*2; // 缩进宽度
  //bool needIndent = true; // 换段后需要缩进（仅对原生行生效）
  bool isFirstLineOfPage = true; // 每页第一行不缩进
  bool isOriginalLine = true; // 标记是否是原生行（未拆行）

  while (pos < chunkSize && static_cast<int>(outLines.size()) < linesPerPage) {
    // Find end of line
    size_t lineEnd = pos;
    while (lineEnd < chunkSize && buffer[lineEnd] != '\n') {
      lineEnd++;
    }

    // Check if we have a complete line
    bool lineComplete = (lineEnd < chunkSize) || (offset + lineEnd >= fileSize);

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
      isOriginalLine = true; // 重置原生行标记
      continue;
    }

    // 检测行首是否已有两个全角空格（仅对原生行检测）
    bool hasLeadingIndent = false;
    if (isOriginalLine && line.length() >= 6) {
      std::string leadingChars = line.substr(0, 6);
      if (leadingChars == indentStr) {
        hasLeadingIndent = true; // 行首已有两个全角空格
        needIndent = false;      // 重置缩进标记，避免重复缩进
      }
    }

    // Track position within this source line (in bytes from pos)
    size_t lineBytePos = 0;

    // 重置拆行标记：进入拆行逻辑后，后续行都是拆行
    isOriginalLine = false;

    // Word wrap if needed
    while (!line.empty() && static_cast<int>(outLines.size()) < linesPerPage) {
      // 计算行宽：仅原生行需要考虑缩进宽度，拆行完全不考虑
      int lineWidth = renderer.getTextWidth(cachedFontId, line.c_str());
      // 缩进判断：仅原生行 + 需要缩进 + 不是页首 + 无已有空格
      const bool doIndent = isOriginalLine && needIndent && !isFirstLineOfPage && !hasLeadingIndent;
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
          outLines.push_back(indentStr + line);
          needIndent = false; // 原生行缩进后，该段落后续行（包括拆行）都不缩进
        } else {
          outLines.push_back(line);
        }
        lineBytePos = displayLen;  // Consumed entire display content
        line.clear();
        isFirstLineOfPage = false; // 每页第一行已处理
        break;
      }

      // Find break point（拆行逻辑）
      size_t breakPos = line.length();
      // 拆行宽度：完全不考虑缩进（拆行不缩进）
      int allowedWidth = viewportWidth - (cachedParagraphAlignment == CrossPointSettings::LEFT_ALIGN ? wordSpacing : 0);
      while (breakPos > 0 && renderer.getTextWidth(cachedFontId, line.substr(0, breakPos).c_str()) > allowedWidth) {
        // Try to break at space
        size_t spacePos = line.rfind(' ', breakPos - 1);
        if (spacePos != std::string::npos && spacePos > 0) {
          breakPos = spacePos;
        } else {
          // Break at character boundary for UTF-8
          breakPos--;
          // Make sure we don't break in the middle of a UTF-8 sequence
          while (breakPos > 0 && (line[breakPos] & 0xC0) == 0x80) {
            breakPos--;
          }
        }
      }

      if (breakPos == 0) {
        breakPos = 1;
      }

      // 拆行后的行：完全不缩进，直接添加
      outLines.push_back(line.substr(0, breakPos));

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
      isOriginalLine = true; // 重置原生行标记，下一行是新的原生行
    } else {
      // Partially consumed - page is full mid-line
      // Move pos to where we stopped in the line (NOT past the line)
      pos = pos + lineBytePos;
      isOriginalLine = true; // 未消费完的行，下一次处理仍视为原生行
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
  if (nextOffset > endOffset) {
    nextOffset = endOffset;
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
    for (const auto& line : currentPageLines) {
      if (!line.empty()) {
        int x = orientedMarginLeft;

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
      }
      y += lineHeight;
    }
  };

  // First pass: BW rendering
  renderLines();
  renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);

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


  if (currentPage < 0) currentPage = 0;
  // 仅当currentPage超过总页数时修正（避免无效页码）
  if (currentPage >= totalPages) currentPage = totalPages - 1;

  // Load current page content
  size_t offset = pageOffsets[currentPage];
  size_t nextOffset;
  currentPageLines.clear();
  size_t endoffset=txt->getChapterOffsetByIndex(chapternum );
  loadPageAtOffset(offset,endoffset, currentPageLines, nextOffset);

  renderer.clearScreen();
  renderPage();

  // Save progress
  saveProgress();
}



void TxtReaderActivity::renderStatusBar(const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) const {
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

  auto metrics = UITheme::getInstance().getMetrics();
  const auto screenHeight = renderer.getScreenHeight();
  // Adjust text position upward when progress bar is shown to avoid overlap
  const auto textY = screenHeight - orientedMarginBottom - 8;
  int progressTextWidth = 0;

  const float progress = totalPages > 0 ? (currentPage + 1) * 100.0f / totalPages : 0;

  if (showProgressText || showProgressPercentage || showBookPercentage) {
    char progressStr[32];
    if (showProgressPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%d/%d %.0f%%", currentPage + 1, totalPages, progress);
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
    // Draw progress bar at the very bottom of the screen, from edge to edge of viewable area
    GUI.drawReadingProgressBar(renderer, static_cast<size_t>(progress));
  }

  if (showChapterProgressBar) {
    // For text mode, treat the entire book as one chapter, so chapter progress == book progress
    GUI.drawReadingProgressBar(renderer, static_cast<size_t>(progress));
  }

  if (showBattery) {
    GUI.drawBattery(renderer, Rect{orientedMarginLeft, textY, metrics.batteryWidth, metrics.batteryHeight},
                    showBatteryPercentage);
  }

  if (showTitle) {
    const int titleMarginLeft = 50 + 30 + orientedMarginLeft;
    const int titleMarginRight = progressTextWidth + 30 + orientedMarginRight;
    const int availableTextWidth = renderer.getScreenWidth() - titleMarginLeft - titleMarginRight;

    std::string title = txt->getChapterTitleByIndex(chapternum);
    int titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    if (titleWidth > availableTextWidth) {
      title = renderer.truncatedText(SMALL_FONT_ID, title.c_str(), availableTextWidth);
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    }

    renderer.drawText(SMALL_FONT_ID, titleMarginLeft + (availableTextWidth - titleWidth) / 2, textY, title.c_str());
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

  bool needIndent;
  Serial.printf("[%lu] [TRS] first line indent: %d\n", millis(), needIndent);
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