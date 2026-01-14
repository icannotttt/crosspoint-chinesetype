#include "TXTReaderActivity.h"

// 保留原有依赖（确保路径正确）
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <SDCardManager.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "TxtReaderChapterSelectionActivity.h"
#include "MappedInputManager.h"
#include "ScreenComponents.h"
#include "fontIds.h"

#include <../../lib/Utf8/Utf8.h>  
#include <Serialization.h> //存进度需要

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
// 进度文件专属魔数和版本号 - 和项目写法一模一样 ✔️
constexpr uint32_t PROGRESS_MAGIC  = 0x50524F47;  // "PROG" 对应 progress.bin
constexpr uint8_t  PROGRESS_VERSION = 1;          // 版本号，改格式就+1
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

  // ========== 新增：创建目录 ==========
  txt->initCache();
  if (!section) {
    section = std::unique_ptr<TXTReaderNS::Section>(new TXTReaderNS::Section(txt));
    Serial.printf("[%lu] [TXT] section初始化成功 ✅ 空指针问题修复\n", millis());
  }

  // 加载进度（调用txt->getCachePath()）
  loadProgress();


  APP_STATE.openEpubPath = txt->getPath();
  APP_STATE.saveToFile();

  pagesUntilFullRefresh = pagesPerRefresh;
  updateRequired = true;

  xTaskCreate(&TXTReaderActivity::taskTrampoline, "TXTReaderActivityTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void TXTReaderActivity::onExit() {
  saveProgress();
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

  // Enter chapter selection activity
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    exitActivity();
    enterNewActivity(new TxtReaderChapterSelectionActivity(
        this->renderer, this->mappedInput, txt, section->beginbype,
        [this] {
          exitActivity();
          updateRequired = true;
        },
        [this](const uint32_t newbype) {
          section->beginbype = newbype;
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


}

void TXTReaderActivity::renderContents(std::unique_ptr<std::string> pageContent, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
                                          
    // 原有变量定义保留...
    Serial.printf("[TXT] 已进入该函数");
    uint32_t charsPerLine = txt->getCharsPerLine();    
    uint16_t charWidth = txt->getCharWidth();          
    uint16_t lineHeight = txt->getLineHeight();   
    // 关键修正：屏幕可渲染高度 = 总高度 - 顶部/底部内边距（之前用了全屏高度，包含状态栏）
    const auto renderableHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
    const auto screenWidth = renderer.getScreenWidth();   
    const int fontId = SETTINGS.getReaderFontId();  

    // ========== 调用splitTxtToWords（确保拆分正确） ==========
    txt->splitTxtToWords(*pageContent, REGULAR);
    Serial.printf("[TXT] 当前beginbype：%d",section->beginbype);
        // ========== 新增：空数组防护（核心紧急修复） ==========
    if (txt->words.empty()) {
        Serial.printf("[TXT] 警告：words数组为空，跳过渲染！pageContent长度：%zu\n", pageContent->size());
        // 渲染空提示，避免白屏
        renderer.drawCenteredText(fontId, renderableHeight/2, "无内容可显示", true, REGULAR);
        renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
        renderer.displayBuffer();
        return;
    }

    // ========== 初始化渲染统计变量（核心修正） ==========
    size_t renderEndIdx = 0;         // 精准指向屏幕最后一个字的索引
    size_t renderedTotalBytes = 0;   // 最后一个字的总字节数
    int currentLineIdx = 0;  
    int currentCharIdx = 0;  
    bool isRenderComplete = false;  // 标记是否渲染到屏幕底部

    // ========== 遍历words列表渲染（精准控制边界） ==========
    for (size_t i = 0; i < txt->words.size() && !isRenderComplete; i++) {
        const std::string& utf8Char = txt->words[i];
        
        // 1. 计算当前字符的渲染坐标（叠加内边距，之前漏了！）
        int xPos = orientedMarginLeft + (currentCharIdx * charWidth);
        int yPos = orientedMarginTop + (currentLineIdx * lineHeight);

        // 2. 边界判断：当前字符的Y坐标 + 行高 超过可渲染高度 → 停止渲染
        // （关键：判断字符底部是否超出，而非顶部，避免少渲染最后一行）
        if (yPos + lineHeight > renderableHeight) {
            isRenderComplete = true;
            break;
        }

        // 3. 处理换行符（统计字节数，且不渲染但更新坐标）
        if (utf8Char == "\n") {
            renderEndIdx = i + 1; // 换行符也算字节，索引+1
            currentCharIdx = 0;
            currentLineIdx++;
            continue;
        }

        // 4. 渲染当前字符（精准对应屏幕位置）
        renderer.drawText(fontId, xPos, yPos, utf8Char.c_str());

        // 5. 更新索引：当前字符已渲染，索引+1
        renderEndIdx = i + 1;

        // 6. 水平换行判断（到行尾重置X坐标）
        currentCharIdx++;
        if (currentCharIdx >= charsPerLine) {
            currentCharIdx = 0;
            currentLineIdx++;
        }
    }

    // ========== 核心：统计最后一个字的总字节数（精准匹配屏幕） ==========
    renderedTotalBytes = txt->getTotalBytesByWordRange(0, renderEndIdx);
    if (section) {
        section->beginbype += renderedTotalBytes; // 累加至精准的字节偏移
        Serial.printf("[TXT] 屏幕最后一个字索引：%zu，累计字节数：%zu，当前beginbype：%u\n",
                     renderEndIdx - 1, renderedTotalBytes, section->beginbype);
    }

    // ---------------- 原有状态栏、刷新、灰度层逻辑（无需改） ----------------
    renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
  
    if (pagesUntilFullRefresh <= 1) {
        renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
        pagesUntilFullRefresh = pagesPerRefresh;
    } else {
        renderer.displayBuffer();
        pagesUntilFullRefresh--;
    }

    // 灰度层渲染：仅渲染到renderEndIdx（精准匹配屏幕内容）
    renderer.storeBwBuffer();
    {
        renderer.clearScreen(0x00);
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
        
        currentLineIdx = 0;
        currentCharIdx = 0;
        // 只渲染到屏幕最后一个字的索引（renderEndIdx）
        for (size_t i = 0; i < renderEndIdx; i++) {
            const std::string& utf8Char = txt->words[i];
            if (utf8Char == "\n") {
                currentCharIdx = 0;
                currentLineIdx++;
                continue;
            }

            int xPos = orientedMarginLeft + (currentCharIdx * charWidth);
            int yPos = orientedMarginTop + (currentLineIdx * lineHeight);
            if (yPos + lineHeight > renderableHeight) break;

            renderer.drawText(fontId, xPos, yPos, utf8Char.c_str());

            currentCharIdx++;
            if (currentCharIdx >= charsPerLine) {
                currentCharIdx = 0;
                currentLineIdx++;
            }
        }
        renderer.copyGrayscaleLsbBuffers();

        // 同理，GRAYSCALE_MSB层也只渲染到renderEndIdx
        renderer.clearScreen(0x00);
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
        currentLineIdx = 0;
        currentCharIdx = 0;
        for (size_t i = 0; i < renderEndIdx; i++) {
            const std::string& utf8Char = txt->words[i];
            if (utf8Char == "\n") {
                currentCharIdx = 0;
                currentLineIdx++;
                continue;
            }

            int xPos = orientedMarginLeft + (currentCharIdx * charWidth);
            int yPos = orientedMarginTop + (currentLineIdx * lineHeight);
            if (yPos + lineHeight > renderableHeight) break;

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
void TXTReaderActivity::saveProgress()  {
  // ✅ 修复1：txt为空 或 section为空，直接返回，不保存
  if (!txt || !section) {
    Serial.printf("[%lu] [TXT] txt/section为空，跳过保存进度 ✅\n", millis());
    return;
  }

  std::string savePath = txt->getCachePath() + "/progress.bin";
  FsFile f;
  if (!SdMan.openFileForWrite("TXT", savePath.c_str(), f)) {
    Serial.printf("[%lu] [TXT] 进度保存失败 ❌\n", millis());
    return;
  }

  serialization::writePod(f, PROGRESS_MAGIC);
  serialization::writePod(f, PROGRESS_VERSION);
  serialization::writePod(f, section->beginbype);
  serialization::writePod(f, section->currentPage);

  f.flush(); // ✅ 必加，强制刷写到SD卡，永不丢进度
  f.close();
  Serial.printf("[%lu] [TXT] 进度保存成功 ✅ beginbype=%u, page=%u\n", millis(), section->beginbype, section->currentPage);
}


void TXTReaderActivity::loadProgress() {
  // ✅ 修复1：txt为空 或 section为空，直接返回，不读取
  if (!txt || !section) {
    Serial.printf("[%lu] [TXT] txt/section为空，跳过读取进度 ✅\n", millis());
    return;
  }

  std::string loadPath = txt->getCachePath() + "/progress.bin";
  FsFile f;

  if (!SdMan.openFileForWrite("TXT", loadPath.c_str(), f)) {
    Serial.printf("[%lu] [TXT] 打开进度文件失败 ❌\n", millis());
    return;
  }

  f.seek(0);

  uint32_t magic;
	uint8_t version;
  serialization::readPod(f, magic);
  serialization::readPod(f, version);

  if (magic != PROGRESS_MAGIC || version != PROGRESS_VERSION) {
    Serial.printf("[%lu] [TXT] 进度文件非法/版本不匹配 ❌，读到魔数:0x%08X,版本:%d\n", millis(), magic, version);
    f.close();
    return;
  }

  serialization::readPod(f, section->beginbype);
  serialization::readPod(f, section->currentPage);

  f.close();
  Serial.printf("[%lu] [TXT] 进度读取成功 ✅ beginbype=%u, page=%u\n", millis(), section->beginbype, section->currentPage);
}