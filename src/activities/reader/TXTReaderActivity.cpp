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
// 进度文件专属魔数和版本号
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
        this->renderer, this->mappedInput, txt, beginbype,
        [this] {
          exitActivity();
          updateRequired = true;
        },
        [this](const uint32_t newbype) {
          beginbype = newbype;
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

  auto pageContent = loadPageFromSectionFile();
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
                                          
    Serial.printf("[TXT] 已进入该函数");
    uint32_t charsPerLine = txt->getCharsPerLine();    
    uint16_t charWidth = txt->getCharWidth();          
    uint16_t lineHeight = txt->getLineHeight();   
    const auto renderableHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
    const auto screenWidth = renderer.getScreenWidth();   
    const int fontId = SETTINGS.getReaderFontId();  

    // ========== 调用splitTxtToWords（确保拆分正确） ==========
    txt->splitTxtToWords(*pageContent, REGULAR);
    Serial.printf("[TXT] 当前beginbype：%d", beginbype);
    
    // ========== 新增：空数组防护 ==========
    if (txt->words.empty()) {
        Serial.printf("[TXT] 警告：words数组为空，跳过渲染！pageContent长度：%zu\n", pageContent->size());
        renderer.drawCenteredText(fontId, renderableHeight/2, "无内容可显示", true, REGULAR);
        renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
        renderer.displayBuffer();
        return;
    }

    // ========== 初始化渲染统计变量 ==========
    size_t renderEndIdx = 0;         // 精准指向屏幕最后一个字的索引
    size_t renderedTotalBytes = 0;   // 最后一个字的总字节数
    int currentLineIdx = 0;  
    int currentCharIdx = 0;  
    bool isRenderComplete = false;  // 标记是否渲染到屏幕底部
    bool previousCharIsNewline = true; // 新增：前一个字符是否是换行符（页面开头默认true）
    bool isChapterTitle = false;    // 新增：当前是否是章节标题

    // ========== 遍历words列表渲染（添加换行后缩进） ==========
    for (size_t i = 0; i < txt->words.size() && !isRenderComplete; i++) {
        const std::string& utf8Char = txt->words[i];
        
        // 1. 处理换行符
        if (utf8Char == "\n") {
            renderEndIdx = i + 1; // 换行符也算字节，索引+1
            currentCharIdx = 0;
            currentLineIdx++;
            previousCharIsNewline = true; // 标记前一个字符是换行符
            isChapterTitle = false;       // 换行后重置章节标题状态
            continue;
        }
        
        // 2. 检测章节标题（换行后以"第"+阿拉伯数字开头）
        if (previousCharIsNewline && utf8Char == "第") {
            // 检查下一个字符是否是阿拉伯数字
            if (i + 1 < txt->words.size()) {
                const std::string& nextChar = txt->words[i + 1];
                if (!nextChar.empty() && nextChar[0] >= '0' && nextChar[0] <= '9') {
                    isChapterTitle = true;
                    Serial.printf("[TXT] 检测到章节标题开始：第%s\n", nextChar.c_str());
                }
            }
        }
        
        // 3. 计算当前字符的渲染坐标
        int xPos = orientedMarginLeft + (currentCharIdx * charWidth);
        int yPos = orientedMarginTop + (currentLineIdx * lineHeight);

        // 4. 边界判断
        if (yPos + lineHeight > renderableHeight) {
            isRenderComplete = true;
            break;
        }
        
        // 5. 如果是换行后且不是章节标题，添加两个空格缩进
        if (previousCharIsNewline && !isChapterTitle && currentCharIdx == 0) {
            // 添加两个空格
            for (int space = 0; space < 2; space++) {
                int spaceXPos = orientedMarginLeft + (currentCharIdx * charWidth);
                int spaceYPos = orientedMarginTop + (currentLineIdx * lineHeight);
                
                // 边界检查
                if (spaceYPos + lineHeight > renderableHeight) {
                    isRenderComplete = true;
                    break;
                }
                
                // 渲染空格
                renderer.drawText(fontId, spaceXPos, spaceYPos, " ");
                
                // 更新字符索引
                currentCharIdx++;
                
                // 检查是否需要换行（空格导致换行）
                if (currentCharIdx >= charsPerLine) {
                    currentCharIdx = 0;
                    currentLineIdx++;
                }
                
                // 更新坐标（因为currentCharIdx可能改变了）
                xPos = orientedMarginLeft + (currentCharIdx * charWidth);
                yPos = orientedMarginTop + (currentLineIdx * lineHeight);
                
                // 再次边界检查
                if (yPos + lineHeight > renderableHeight) {
                    isRenderComplete = true;
                    break;
                }
            }
            
            // 如果添加空格后导致页面结束，退出循环
            if (isRenderComplete) {
                break;
            }
        }
        
        // 6. 渲染当前字符
        renderer.drawText(fontId, xPos, yPos, utf8Char.c_str());

        // 7. 更新索引和状态
        renderEndIdx = i + 1;
        previousCharIsNewline = false; // 当前字符不是换行符

        // 8. 水平换行判断
        currentCharIdx++;
        if (currentCharIdx >= charsPerLine) {
            currentCharIdx = 0;
            currentLineIdx++;
            // 换行后，下一个字符前一个是"换行符"
            // 但这里我们不设置previousCharIsNewline = true
            // 因为这是行尾自动换行，不是段落换行
        }
    }

    // ========== 统计字节数 ==========
    renderedTotalBytes = txt->getTotalBytesByWordRange(0, renderEndIdx);
    beginbype += renderedTotalBytes; // 累加至精准的字节偏移
    Serial.printf("[TXT] 屏幕最后一个字索引：%zu，累计字节数：%zu，当前beginbype：%u\n",
                  renderEndIdx - 1, renderedTotalBytes, beginbype);
    
    // ---------------- 原有状态栏、刷新、灰度层逻辑 ----------------
    renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
  
    if (pagesUntilFullRefresh <= 1) {
        renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
        pagesUntilFullRefresh = pagesPerRefresh;
    } else {
        renderer.displayBuffer();
        pagesUntilFullRefresh--;
    }

    // 灰度层渲染：需要同步缩进逻辑
    renderer.storeBwBuffer();
    {
        renderer.clearScreen(0x00);
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
        
        currentLineIdx = 0;
        currentCharIdx = 0;
        previousCharIsNewline = true; // 重置状态
        isChapterTitle = false;
        
        for (size_t i = 0; i < renderEndIdx; i++) {
            const std::string& utf8Char = txt->words[i];
            if (utf8Char == "\n") {
                currentCharIdx = 0;
                currentLineIdx++;
                previousCharIsNewline = true;
                isChapterTitle = false;
                continue;
            }
            
            // 检测章节标题
            if (previousCharIsNewline && utf8Char == "第") {
                if (i + 1 < txt->words.size()) {
                    const std::string& nextChar = txt->words[i + 1];
                    if (!nextChar.empty() && nextChar[0] >= '0' && nextChar[0] <= '9') {
                        isChapterTitle = true;
                    }
                }
            }
            
            // 计算坐标
            int xPos = orientedMarginLeft + (currentCharIdx * charWidth);
            int yPos = orientedMarginTop + (currentLineIdx * lineHeight);
            if (yPos + lineHeight > renderableHeight) break;
            
            // 处理换行后缩进
            if (previousCharIsNewline && !isChapterTitle && currentCharIdx == 0) {
                for (int space = 0; space < 2; space++) {
                    int spaceXPos = orientedMarginLeft + (currentCharIdx * charWidth);
                    int spaceYPos = orientedMarginTop + (currentLineIdx * lineHeight);
                    
                    if (spaceYPos + lineHeight > renderableHeight) break;
                    
                    renderer.drawText(fontId, spaceXPos, spaceYPos, " ");
                    
                    currentCharIdx++;
                    if (currentCharIdx >= charsPerLine) {
                        currentCharIdx = 0;
                        currentLineIdx++;
                    }
                    
                    xPos = orientedMarginLeft + (currentCharIdx * charWidth);
                    yPos = orientedMarginTop + (currentLineIdx * lineHeight);
                    
                    if (yPos + lineHeight > renderableHeight) break;
                }
            }
            
            // 渲染字符
            if (yPos + lineHeight <= renderableHeight) {
                renderer.drawText(fontId, xPos, yPos, utf8Char.c_str());
            }
            
            previousCharIsNewline = false;
            
            currentCharIdx++;
            if (currentCharIdx >= charsPerLine) {
                currentCharIdx = 0;
                currentLineIdx++;
            }
        }
        renderer.copyGrayscaleLsbBuffers();

        // GRAYSCALE_MSB层
        renderer.clearScreen(0x00);
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
        currentLineIdx = 0;
        currentCharIdx = 0;
        previousCharIsNewline = true;
        isChapterTitle = false;
        
        for (size_t i = 0; i < renderEndIdx; i++) {
            const std::string& utf8Char = txt->words[i];
            if (utf8Char == "\n") {
                currentCharIdx = 0;
                currentLineIdx++;
                previousCharIsNewline = true;
                isChapterTitle = false;
                continue;
            }
            
            if (previousCharIsNewline && utf8Char == "第") {
                if (i + 1 < txt->words.size()) {
                    const std::string& nextChar = txt->words[i + 1];
                    if (!nextChar.empty() && nextChar[0] >= '0' && nextChar[0] <= '9') {
                        isChapterTitle = true;
                    }
                }
            }
            
            int xPos = orientedMarginLeft + (currentCharIdx * charWidth);
            int yPos = orientedMarginTop + (currentLineIdx * lineHeight);
            if (yPos + lineHeight > renderableHeight) break;
            
            if (previousCharIsNewline && !isChapterTitle && currentCharIdx == 0) {
                for (int space = 0; space < 2; space++) {
                    int spaceXPos = orientedMarginLeft + (currentCharIdx * charWidth);
                    int spaceYPos = orientedMarginTop + (currentLineIdx * lineHeight);
                    
                    if (spaceYPos + lineHeight > renderableHeight) break;
                    
                    renderer.drawText(fontId, spaceXPos, spaceYPos, " ");
                    
                    currentCharIdx++;
                    if (currentCharIdx >= charsPerLine) {
                        currentCharIdx = 0;
                        currentLineIdx++;
                    }
                    
                    xPos = orientedMarginLeft + (currentCharIdx * charWidth);
                    yPos = orientedMarginTop + (currentLineIdx * lineHeight);
                    
                    if (yPos + lineHeight > renderableHeight) break;
                }
            }
            
            if (yPos + lineHeight <= renderableHeight) {
                renderer.drawText(fontId, xPos, yPos, utf8Char.c_str());
            }
            
            previousCharIsNewline = false;
            
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
  FsFile f;
  std::string savepath=txt->getCachePath() + "/progress.bin";
  if (SdMan.openFileForWrite("TRA", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    data[0] = beginbype & 0xFF;
    data[1] = (beginbype >> 8) & 0xFF;
    data[2] = (beginbype >> 16) & 0xFF;
    data[3] = (beginbype >> 24) & 0xFF;
    f.write(data, 4);
    Serial.printf("[%lu] [TRA] 读取路径:%s,写入字节: page %lu\n", millis(), savepath.c_str(),beginbype);
    f.sync();  // 强制同步
    f.close();
  }
}




void TXTReaderActivity::loadProgress() {
  FsFile f;
  std::string savepath=txt->getCachePath() + "/progress.bin";
  if (SdMan.openFileForRead("TRA", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      beginbype = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
      Serial.printf("[%lu] [TRA] 读取路径:%s,写入字节: page %lu\n", millis(), savepath.c_str(),beginbype);

    }
    f.close();
  }
  Serial.printf("[%lu] [TRA] 该路径未读取到:%s \n", millis(), savepath.c_str());
}