#include "TXTReaderActivity.h"

// 保留原有依赖（确保路径正确）
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <SDCardManager.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "MappedInputManager.h"
#include "ScreenComponents.h"
#include "fontIds.h"

#include <../../lib/Utf8/Utf8.h>  

namespace {
// 墨水屏刷新阈值：每渲染15页执行一次全刷新（解决残影），15页内执行半刷新（更快）
constexpr int pagesPerRefresh = 15;
// 跳章长按阈值：长按翻页键超过700ms，直接跳转到上/下一章（短按仅翻页）
constexpr unsigned long skipChapterMs = 700;
// 返回主页长按阈值：长按返回键超过1000ms，直接返回主界面（短按仅退出章节选择）
constexpr unsigned long goHomeMs = 1000;
// 顶部内边距：文本区域顶部到屏幕顶部的距离（控制文本顶部留白）
constexpr int topPadding = 5;
// 左右内边距：文本区域左右到屏幕边缘的距离（控制文本左右留白）
constexpr int horizontalPadding = 5;
// 状态栏底部边距：状态栏到屏幕底部的距离（防止文本与状态栏重叠）
constexpr int statusBarMargin = 19;
// 文本行高：每行文本的垂直间距（数值越大，行间距越宽）
constexpr int LINE_HEIGHT = 24;    
// 单行最大宽度：单行文本的最大像素宽度（超过该宽度自动换行）
constexpr int MAX_LINE_WIDTH = 500;
}  // namespace

void TXTReaderActivity::taskTrampoline(void* param) {
  auto* self = static_cast<TXTReaderActivity*>(param);
  self->displayTaskLoop();
}

void TXTReaderActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  if (!txt) {
    return;
  }

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

  // 加载进度（调用txt->getCachePath()）
  FsFile f;
  if (SdMan.openFileForRead("TXT", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentChapterIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      Serial.printf("[%lu] [TXT] Loaded cache: %d, %d\n", millis(), currentChapterIndex, nextPageNumber);
    }
    f.close();
  }

  APP_STATE.openEpubPath = txt->getPath();
  APP_STATE.saveToFile();

  pagesUntilFullRefresh = pagesPerRefresh;
  updateRequired = true;

  xTaskCreate(&TXTReaderActivity::taskTrampoline, "TXTReaderActivityTask",
              8192, this, 1, &displayTaskHandle);
}

void TXTReaderActivity::onExit() {
  if (txt && section && currentChapterIndex < txt->getChapters().size()) {
    std::string progressPath = txt->getCachePath() + "/progress.bin";
    
    
    FsFile f;
    if (SdMan.openFileForWrite("TXT", progressPath, f)) {
      uint8_t data[4];
      data[0] = currentChapterIndex & 0xFF;        
      data[1] = (currentChapterIndex >> 8) & 0xFF; 
      data[2] = section->currentPage & 0xFF;       
      data[3] = (section->currentPage >> 8) & 0xFF;
      f.write(data, 4);
      f.close();
      Serial.printf("[%lu] [TXT] 保存进度成功：%s → 章节%d，页码%d\n", millis(), progressPath.c_str(), currentChapterIndex, section->currentPage);
    } else {
      // 新增错误日志：定位保存失败原因
      Serial.printf("[%lu] [TXT] 保存进度失败：无法打开文件 %s\n", millis(), progressPath.c_str());
    }
  }
  ActivityWithSubactivity::onExit();

  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  section.reset();
  txt.reset();
}

void TXTReaderActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    exitActivity();
    enterNewActivity(new EpubReaderChapterSelectionActivity(
        this->renderer, this->mappedInput, nullptr, currentChapterIndex,
        [this] {
          exitActivity();
          updateRequired = true;
        },
        [this](const int newChapterIndex) {
          if (currentChapterIndex != newChapterIndex) {
            currentChapterIndex = newChapterIndex;
            nextPageNumber = 0;
            section.reset();
          }
          exitActivity();
          updateRequired = true;
        }));
    xSemaphoreGive(renderingMutex);
  }

  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= goHomeMs) {
    onGoHome();
    return;
  }
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

  const auto& chapters = txt->getChapters();
  if (currentChapterIndex < 0) currentChapterIndex = 0;
  if (currentChapterIndex >= chapters.size()) {
    currentChapterIndex = chapters.size() - 1;
    nextPageNumber = UINT16_MAX;
    updateRequired = true;
    return;
  }

  const bool skipChapter = mappedInput.getHeldTime() > skipChapterMs;

  if (skipChapter) {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    nextPageNumber = 0;
    currentChapterIndex = nextReleased ? currentChapterIndex + 1 : currentChapterIndex - 1;
    section.reset();
    xSemaphoreGive(renderingMutex);
    updateRequired = true;
    return;
  }

  if (!section) {
    updateRequired = true;
    return;
  }

  if (prevReleased) {
    if (section->currentPage > 0) {
      section->currentPage--;
    } else {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      nextPageNumber = UINT16_MAX;
      currentChapterIndex--;
      section.reset();
      xSemaphoreGive(renderingMutex);
    }
    updateRequired = true;
  } else {
    if (section->currentPage < section->pageCount - 1) {
      section->currentPage++;
    } else {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      nextPageNumber = 0;
      currentChapterIndex++;
      section.reset();
      xSemaphoreGive(renderingMutex);
    }
    updateRequired = true;
  }
}

[[noreturn]] void TXTReaderActivity::displayTaskLoop() {
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

void TXTReaderActivity::renderScreen() {
  if (!txt) {
    return;
  }

  const auto& chapters = txt->getChapters();
  if (currentChapterIndex < 0) {
    currentChapterIndex = 0;
  }
  if (currentChapterIndex >= chapters.size()) {
    currentChapterIndex = chapters.size();
  }

  if (currentChapterIndex == chapters.size()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "End of book", true, BOLD);
    renderer.displayBuffer();
    return;
  }

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += topPadding;
  orientedMarginLeft += horizontalPadding;
  orientedMarginRight += horizontalPadding;
  orientedMarginBottom += statusBarMargin;

  // 4. 核心修复：创建命名空间的Section
  if (!section) {
    Serial.printf("[%lu] [TXT] Loading chapter: %d\n", millis(), currentChapterIndex);
    section = std::unique_ptr<TXTReaderNS::Section>(new TXTReaderNS::Section(txt));

    if (nextPageNumber == UINT16_MAX) {
      section->currentPage = section->pageCount - 1;
    } else {
      section->currentPage = nextPageNumber;
    }
  }

  renderer.clearScreen();

  if (section->pageCount == 0) {
    Serial.printf("[%lu] [TXT] No pages to render\n", millis());
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "Empty chapter", true, BOLD);
    renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    renderer.displayBuffer();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    Serial.printf("[%lu] [TXT] Page out of bounds: %d (max %d)\n", millis(), section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "Out of bounds", true, BOLD);
    renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    renderer.displayBuffer();
    return;
  }

  auto pageContent = section->loadPageFromSectionFile();
  if (!pageContent) {
    Serial.printf("[%lu] [TXT] Failed to load page from cache\n", millis());
    section->clearCache();
    section.reset();
    return renderScreen();
  }
  const auto start = millis();
  renderContents(std::move(pageContent), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
  Serial.printf("[%lu] [TXT] Rendered page in %dms\n", millis(), millis() - start);

  // 保存进度
  FsFile f;
  if (SdMan.openFileForWrite("TXT", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    data[0] = currentChapterIndex & 0xFF;
    data[1] = (currentChapterIndex >> 8) & 0xFF;
    data[2] = section->currentPage & 0xFF;
    data[3] = (section->currentPage >> 8) & 0xFF;
    f.write(data, 4);
    f.close();
  }
}

void TXTReaderActivity::renderContents(std::unique_ptr<std::string> pageContent, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
    // 原有变量定义保留...
    uint32_t charsPerLine = txt->getCharsPerLine();    
    uint16_t charWidth = txt->getCharWidth();          
    uint16_t lineHeight = txt->getLineHeight();   
    const auto screenHeight = renderer.getScreenHeight();
    const auto screenWidth = renderer.getScreenWidth();   
    const int fontId = SETTINGS.getReaderFontId();  

    // ========== 调用splitTxtToWords（确保拆分正确） ==========
    txt->splitTxtToWords(*pageContent, REGULAR);

    // ========== 遍历words列表渲染（确保每个字符完整） ==========
    int currentLineIdx = 0;  
    int currentCharIdx = 0;  
    for (size_t i = 0; i < txt->words.size(); i++) {
        const std::string& utf8Char = txt->words[i];
        if (utf8Char == "\n") {
            currentCharIdx = 0;
            currentLineIdx++;
            continue;
        }

        int xPos = currentCharIdx * charWidth;
        int yPos = currentLineIdx * lineHeight;
        if (yPos >= screenHeight) break;

        // 核心：传完整的UTF-8字符串，而非截断的字节
        renderer.drawText(fontId, xPos, yPos, utf8Char.c_str());

        currentCharIdx++;
        if (currentCharIdx >= charsPerLine) {
            currentCharIdx = 0;
            currentLineIdx++;
        }
    }


  
  // ---------------- 保留原有状态栏逻辑 ----------------
  renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
  
  // ---------------- 保留原有刷新逻辑 ----------------
  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = pagesPerRefresh;
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }

  // ---------------- 灰度层同步修改：改用words列表遍历（仅改遍历逻辑，其他不变） ----------------
  renderer.storeBwBuffer();
  {
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    
    // 重置坐标，遍历words列表
    currentLineIdx = 0;
    currentCharIdx = 0;
    for (size_t i = 0; i < txt->words.size(); i++) {
      const std::string& utf8Char = txt->words[i];
      if (utf8Char == "\n") {
        currentCharIdx = 0;
        currentLineIdx++;
        continue;
      }

      int xPos = currentCharIdx * charWidth;
      int yPos = currentLineIdx * lineHeight;
      if (yPos >= screenHeight) break;

      renderer.drawText(fontId, xPos, yPos, utf8Char.c_str());

      currentCharIdx++;
      if (currentCharIdx >= charsPerLine) {
        currentCharIdx = 0;
        currentLineIdx++;
      }
    }
    renderer.copyGrayscaleLsbBuffers();

    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    // 重置坐标，遍历words列表
    currentLineIdx = 0;
    currentCharIdx = 0;
    for (size_t i = 0; i < txt->words.size(); i++) {
      const std::string& utf8Char = txt->words[i];
      if (utf8Char == "\n") {
        currentCharIdx = 0;
        currentLineIdx++;
        continue;
      }

      int xPos = currentCharIdx * charWidth;
      int yPos = currentLineIdx * lineHeight;
      if (yPos >= screenHeight) break;

      renderer.drawText(fontId, xPos, yPos, utf8Char.c_str());

      currentCharIdx++;
      if (currentCharIdx >= charsPerLine) {
        currentCharIdx = 0;
        currentLineIdx++;
      }
    }
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }
  renderer.restoreBwBuffer();
}




void TXTReaderActivity::renderStatusBar(const int orientedMarginRight, const int orientedMarginBottom,
                                         const int orientedMarginLeft) const {
  const bool showProgress = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL;
  const bool showBattery = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::NO_PROGRESS ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL;
  const bool showChapterTitle = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::NO_PROGRESS ||
                                SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL;

  const auto screenHeight = renderer.getScreenHeight();
  const auto textY = screenHeight - orientedMarginBottom - 4;
  int progressTextWidth = 0;

  if (showProgress) {
    const float chapterProgress = static_cast<float>(section->currentPage) / section->pageCount;
    const float totalProgress = (static_cast<float>(currentChapterIndex) + chapterProgress) / txt->getChapters().size();
    const uint8_t bookProgress = static_cast<uint8_t>(totalProgress * 100);

    const std::string progress = std::to_string(section->currentPage + 1) + "/" + std::to_string(section->pageCount) +
                                 "  " + std::to_string(bookProgress) + "%";
    progressTextWidth = renderer.getTextWidth(SMALL_FONT_ID, progress.c_str());
    renderer.drawText(SMALL_FONT_ID, renderer.getScreenWidth() - orientedMarginRight - progressTextWidth, textY,
                      progress.c_str());
  }

  if (showBattery) {
    ScreenComponents::drawBattery(renderer, orientedMarginLeft, textY);
  }

  if (showChapterTitle) {
    const int titleMarginLeft = 50 + 30 + orientedMarginLeft;
    const int titleMarginRight = progressTextWidth + 30 + orientedMarginRight;
    const int availableTextWidth = renderer.getScreenWidth() - titleMarginLeft - titleMarginRight;

    std::string title;
    int titleWidth;
    if (currentChapterIndex < 0 || currentChapterIndex >= txt->getChapters().size()) {
      title = "Unnamed";
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, "Unnamed");
    } else {
      title = txt->getChapters()[currentChapterIndex].title;
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
      while (titleWidth > availableTextWidth && title.length() > 11) {
        title.replace(title.length() - 8, 8, "...");
        titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
      }
    }

    renderer.drawText(SMALL_FONT_ID, titleMarginLeft + (availableTextWidth - titleWidth) / 2, textY, title.c_str());
  }
}