#include "TXTReaderActivity.h"
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
  section->currentPage =0;
  // 加载进度（调用txt->getCachePath()）
  loadProgress();


  APP_STATE.openEpubPath = txt->getPath();
  APP_STATE.saveToFile();

  pagesUntilFullRefresh = pagesPerRefresh;
  updateRequired = true;

  xTaskCreate(&TXTReaderActivity::taskTrampoline, "TXTReaderActivityTask",
              6144,               // Stack size
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

  // Enter chapter selection activity 加目录
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)&& mappedInput.getHeldTime() < goHomeMs) {
    
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

 //短按退出，长按回home

  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= goHomeMs) {
    onGoHome();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && mappedInput.getHeldTime() < goHomeMs) {
    onGoBack();
    return;
  }

 
 
  //翻页逻辑
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

 // 长按跳章逻辑 【原有不变，只加清空缓存】
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

  // ========== 【上一页核心逻辑】严格按你的思路实现 (无减法、无计算，直接读缓存，极致高效) ==========
  if (prevReleased) {
      // 核心判断：只要缓存游标>0，就有上一页的有效缓存，直接翻页（删掉section->currentPage>0）
      if (section->currentPage > 1) {
        section->currentPage--;
        loadPage();
          updateRequired = true;
      } 
      updateRequired = true;
  } 
  // ========== 【下一页逻辑】原有不变，完全无需修改，完美兼容 ==========
  else {
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
  //弃用原逻辑
  //if (currentChapterIndex == chapters.size()) {
  //  renderer.clearScreen();
  //  renderer.drawCenteredText(UI_12_FONT_ID, 300, "End of book", true,EpdFontFamily::BOLD);
  //  renderer.displayBuffer();
  //  return;
  //}

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
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "Empty chapter", true,  EpdFontFamily::BOLD);
    renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    renderer.displayBuffer();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    Serial.printf("[%lu] [TXT] Page out of bounds: %d (max %d)\n", millis(), section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "Out of bounds", true,  EpdFontFamily::BOLD);
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

void TXTReaderActivity::renderpage(std::unique_ptr<std::string>& pageContent,  // 改为引用，避免所有权转移
                                    const int orientedMarginTop,
                                    const int orientedMarginRight,
                                    const int orientedMarginBottom,
                                    const int orientedMarginLeft,
                                    bool isForGrayscale /* 新增：标记是否为灰度渲染，避免累加beginbype */) {
                                          
    Serial.printf("[TXT] 已进入该函数");

    float charWidth = txt->getCharWidth();          
    float lineHeight = txt->getLineHeight();   
    const auto renderableHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
    const auto screenWidth = renderer.getScreenWidth();   
    const auto renderableWidth = screenWidth - orientedMarginLeft - orientedMarginRight;
    const int fontId = SETTINGS.getReaderFontId();  

    // 空指针防护（避免灰度渲染时pageContent为空）
    if (!pageContent) {
        Serial.printf("[TXT] 警告：pageContent 为空，跳过渲染！\n");
        return;
    }

    // 调用splitTxtToWords（确保拆分正确）
    txt->splitTxtToWords(*pageContent,  EpdFontFamily::REGULAR);
    Serial.printf("[TXT] 当前beginbype：%d", beginbype);
    
    // 仅在非灰度渲染时执行savePage（避免重复保存）
    if (!isForGrayscale) {
        savePage();
    }
    
    // 空数组防护
    if (txt->words.empty()) {
        Serial.printf("[TXT] 警告：words数组为空，跳过渲染！pageContent长度：%zu\n", pageContent->size());
        // 仅在非灰度渲染时绘制提示文本（灰度模式无需提示）
        if (!isForGrayscale) {
            renderer.drawCenteredText(fontId, renderableHeight/2, "无内容可显示", true,  EpdFontFamily::REGULAR);
        }
        return;
    }

    // 初始化渲染统计变量
    size_t renderEndIdx = 0;         // 精准指向屏幕最后一个字的索引
    size_t renderedTotalBytes = 0;   // 最后一个字的总字节数
    int currentLineIdx = 0;          // 屏幕行号（0=第一行，1=第二行...）
    int currentCharIdx = 0;          
    bool isRenderComplete = false;   // 标记是否渲染到屏幕底部
    // ========== 核心修正：仅用段落第一行标记，屏幕第一行通过currentLineIdx判断 ==========
    bool isFirstLineOfParagraph = true; // 段落第一行标记
    const int FIRST_LINE_OFFSET = 2 * charWidth; // 段落第一行偏移量

    // 遍历words列表渲染（宽度判断+偏移控制）
    for (size_t i = 0; i < txt->words.size() && !isRenderComplete; i++) {
        const std::string& utf8Char = txt->words[i];
        
        // 1. 处理手动换行符\n → 重置行索引，前瞻检查后续是否有两个空格，再标记段落第一行
        if (utf8Char == "\n") {
            renderEndIdx = i + 1; // 换行符也算字节，索引+1
            currentCharIdx = 0;   // 重置当前行字符索引
            currentLineIdx++;     // 屏幕行号+1
            
            // ========== 关键修改1：前瞻检查\n后面是否有连续两个空格 ==========
            bool hasTwoSpacesAfterNewline = false;
            if (i + 2 < txt->words.size()) {
                const std::string& nextChar1 = txt->words[i+1];
                const std::string& nextChar2 = txt->words[i+2];
                if (nextChar1 == "　" && nextChar2 == "　") {
                    hasTwoSpacesAfterNewline = true;
                }
            }
            // 只有当\n后面没有连续两个空格时，才标记为段落第一行（需要偏移）
            isFirstLineOfParagraph = !hasTwoSpacesAfterNewline;
            
            continue; // 跳过换行符的绘制
        }

        // ========== 计算当前字符宽度 + 宽度边界判断 ==========
        int currCharWidth = renderer.getTextWidth(fontId, utf8Char.c_str());
        // 核心修正：
        // - 屏幕第一行（currentLineIdx=0）：整行不偏移（baseXOffset=0）
        // - 非屏幕第一行+段落第一行：偏移2*charWidth
        // - 非屏幕第一行+非段落第一行：不偏移
        int baseXOffset = 0;
        if (currentLineIdx > 0 && isFirstLineOfParagraph) {
            baseXOffset = FIRST_LINE_OFFSET;
        }

        // 预期X坐标 = 左内边距 + 基础偏移 + 字符索引*字符宽度
        int expectedXPos = orientedMarginLeft + baseXOffset + (currentCharIdx * charWidth);
        int charRightBound = expectedXPos + currCharWidth;

        // ========== 屏幕宽度判断（超出则自动换行） ==========
        if (charRightBound > orientedMarginLeft + renderableWidth) {
            currentCharIdx = 0;   // 重置行内字符索引
            currentLineIdx++;     // 屏幕行号+1
            isFirstLineOfParagraph = false; // 自动换行后，不再是段落第一行
            // 重新计算换行后的X坐标（非段落第一行，基础偏移=0）
            expectedXPos = orientedMarginLeft + 0 + (currentCharIdx * charWidth);
            // 边界防护：换行后检查高度是否超出
            int newYPos = orientedMarginTop + (currentLineIdx * lineHeight);
            if (newYPos + lineHeight > renderableHeight) {
                isRenderComplete = true;
                break;
            }
        }

        // 2. 计算最终渲染坐标
        int xPos = expectedXPos;
        int yPos = orientedMarginTop + (currentLineIdx * lineHeight);

        // 3. 高度边界判断：字符底部超出可渲染高度 → 停止渲染
        if (yPos + lineHeight > renderableHeight) {
            isRenderComplete = true;
            break;
        }

        // 4. 渲染当前字符（屏幕第一行整行无偏移）
        renderer.drawText(fontId, xPos, yPos, utf8Char.c_str());

        // 5. 更新索引
        renderEndIdx = i + 1;

        // 6. 原有水平字符数满换行（兜底）
        currentCharIdx++;

    }

    // 统计最后一个字的总字节数（仅在非灰度渲染时累加，避免重复统计）
    renderedTotalBytes = txt->getTotalBytesByWordRange(0, renderEndIdx);
    if (section && !isForGrayscale) {
        beginbype += renderedTotalBytes;
        Serial.printf("[TXT] 屏幕最后一个字索引：%zu，累计字节数：%zu，当前beginbype：%u\n",
                     renderEndIdx - 1, renderedTotalBytes, beginbype);
    }
}

void TXTReaderActivity::renderContents(std::unique_ptr<std::string> pageContent, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
                                          
    // ========== 1. 黑白模式渲染（非灰度，传递完整参数） ==========
    // 传递pageContent引用，避免所有权转移，后续灰度渲染可复用
    renderpage(pageContent, orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft, false);
    
    renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    
    // 黑白缓冲区显示（半刷新/全刷新逻辑）
    if (pagesUntilFullRefresh <= 1) {
        renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
        pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    } else {
        renderer.displayBuffer();
        pagesUntilFullRefresh--;
    }

    // 保存黑白缓冲区，用于后续恢复
    bool bufferStored = renderer.storeBwBuffer();

    
   {
        // 灰度LSB模式渲染（修正：先切换模式，再清理缓冲区，清理值改为0xFF全白）
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
        renderer.clearScreen(0x00);
        renderpage(pageContent, orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft, true);
        renderer.copyGrayscaleLsbBuffers();

        // 灰度MSB模式渲染（修正：先切换模式，再清理缓冲区，清理值改为0xFF全白）
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
        renderer.clearScreen(0x00);
        renderpage(pageContent, orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft, true);
        renderer.copyGrayscaleMsbBuffers();

        // 显示灰度缓冲区（灰度显示后重置刷新计数器）
        renderer.displayGrayBuffer();
        pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
        
        // 切换回黑白模式，保证后续渲染正常
        renderer.setRenderMode(GfxRenderer::BW);
    }

    // ========== 3. 恢复黑白缓冲区 ==========
    {
        // 直接调用restoreBwBuffer()
        renderer.restoreBwBuffer();
        
        Serial.printf("[%lu] [TXT] 已执行BW缓冲区恢复操作\n", millis());
    }
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

  //if (showProgress) {
    const float chapterProgress = static_cast<float>(section->currentPage) / section->pageCount;
    const float totalProgress = static_cast<float>(beginbype) / txt->gettotalbytes();
    if (beginbype < txt->gettotalbytes()){
    const uint8_t bookProgress = static_cast<uint8_t>(totalProgress * 100);

    const std::string progress = "当前进度  " + std::to_string(bookProgress) + "%";
    Serial.printf("[%lu] [TRA] 进度:%s \n", millis(), progress.c_str());
    progressTextWidth = renderer.getTextWidth(SMALL_FONT_ID, progress.c_str());
    renderer.drawText(SMALL_FONT_ID, renderer.getScreenWidth() - orientedMarginRight - progressTextWidth, textY,
                      progress.c_str());
    }else{
      renderer.drawText(SMALL_FONT_ID, renderer.getScreenWidth() - orientedMarginRight - progressTextWidth, textY,
                      "暂无进度");
    }
  //}

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
    uint8_t data[8];
    data[0] = beginbype & 0xFF;
    data[1] = (beginbype >> 8) & 0xFF;
    data[2] = (beginbype >> 16) & 0xFF;
    data[3] = (beginbype >> 24) & 0xFF;
    data[4] = section->currentPage & 0xFF;
    data[5] = (section->currentPage >> 8) & 0xFF;
    data[6] = (section->currentPage >> 16) & 0xFF;
    data[7] = (section->currentPage >> 24) & 0xFF;
    f.write(data, 8);
    Serial.printf("[%lu] [TRA] 读取路径:%s,写入字节: page %lu\n", millis(), savepath.c_str(),beginbype);
    f.sync();  // 强制同步
    f.close();
  }
}


void TXTReaderActivity::loadProgress() {
  FsFile f;
  std::string savepath=txt->getCachePath() + "/progress.bin";
  if (SdMan.openFileForRead("TRA", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[8];
    if (f.read(data, 8) == 8) {
      beginbype = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
      Serial.printf("[%lu] [TRA] 读取路径:%s,写入字节: page %lu\n", millis(), savepath.c_str(),beginbype);
      section->currentPage = data[4] | (data[5] << 8) | (data[6] << 16) | (data[6] << 24);
      Serial.printf("[%lu] [TRA] 读取路径:%s,写入字节: page %lu\n", millis(), savepath.c_str(),section->currentPage);
    }else{
      beginbype=0;
      section->currentPage=0;
    }
    f.close();
  }
  Serial.printf("[%lu] [TRA] 该路径未读取到:%s \n", millis(), savepath.c_str());
}
/**
 * @brief 按页码存储当前页的原始beginbype偏移值
 */
  void TXTReaderActivity::savePage()  {
    FsFile f;
    // ✅ 核心：按页码拼接文件名 【页码 + page.bin】
    std::string savepath = txt->getCachePath() +"/"+ std::to_string(section->currentPage) + "page.bin";
    if (SdMan.openFileForWrite("TRA", savepath.c_str(), f)) {
      uint8_t data[4];
      // 存储当前页的原始beginbype（4字节拆分存储，和你原有逻辑一致）
      data[0] = beginbype & 0xFF;
      data[1] = (beginbype >> 8) & 0xFF;
      data[2] = (beginbype >> 16) & 0xFF;
      data[3] = (beginbype >> 24) & 0xFF;
      f.write(data, 4);
      Serial.printf("[%lu] [PAGE] 保存页码%d成功 → %s , 存储偏移值: %lu\n", millis(), section->currentPage, savepath.c_str(), beginbype);
      f.sync();  // 强制同步，保证写入SD卡不丢失
      f.close();
    } else {
      Serial.printf("[%lu] [PAGE] 保存页码%d失败 → %s\n", millis(), section->currentPage, savepath.c_str());
    }
  }

  /**
   * @brief 按页码读取对应页的原始beginbype偏移值
   * 读取规则：根据当前的section->currentPage，加载对应页码的page.bin文件
   * 读取后：直接赋值给beginbype，重置缓存，完美衔接翻页逻辑
   */
  void TXTReaderActivity::loadPage() {
    FsFile f;
    // ✅ 核心：按页码拼接文件名 【页码 + page.bin】
    std::string savepath = txt->getCachePath() +"/"+ std::to_string(section->currentPage) + "page.bin";
    if (SdMan.openFileForRead("TRA", savepath.c_str(), f)) {
      uint8_t data[4];
      if (f.read(data, 4) == 4) {
        beginbype = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
        Serial.printf("[%lu] [PAGE] 加载页码%d成功 → %s , 读取偏移值: %lu\n", millis(), section->currentPage, savepath.c_str(), beginbype);

      } else {
        Serial.printf("[%lu] [PAGE] 加载页码%d失败 → 文件格式错误/字节数不足\n", millis(), section->currentPage);
      }
      f.close();
    } else {
      Serial.printf("[%lu] [PAGE] 加载页码%d失败 → 文件不存在 %s\n", millis(), section->currentPage, savepath.c_str());
    }
  }

  
/**
 * @brief 【适配GfxRenderer】将当前墨水屏显示的页面，完整保存为BMP格式图片文件
 * @param renderer 你的GfxRenderer对象 (直接传你用的renderer即可)
 * @param filename 保存的BMP文件路径+名称，例："/.crosspoint/current_page.bmp"
 * @return bool 保存成功返回true，失败返回false
 * @note 1. 保存的是当前屏幕实时显示的内容，截图100%精准；
 *       2. 黑白高清，无失真，电脑/手机均可直接打开；
 *       3. 无需修改任何原有源码，直接调用即可。
 */
bool TXTReaderActivity::saveScreenToBMP(const char* filename) {
    const int W = renderer.getScreenWidth();   // 逻辑宽度
    const int H = renderer.getScreenHeight();  // 逻辑高度
    const int BMP_BPR = ((W+7)/8 +3)&~3;       // BMP每行字节数（4字节对齐）
    const int BMP_SIZE = BMP_BPR * H;
    uint8_t* bmpBuf = (uint8_t*)malloc(BMP_SIZE);
    if(!bmpBuf) return false;
    memset(bmpBuf, 0xFF, BMP_SIZE);  // 初始化为白色背景

    // 核心：BMP像素写入逻辑（和屏幕一致的MSB位序）
    auto drawBmpPixel = [&](int x, int y, bool black) {
        if(x<0||x>=W||y<0||y>=H) return;
        int byteIdx = y * BMP_BPR + (x/8);
        int bitPos = 7 - (x%8);
        if(black) {
            bmpBuf[byteIdx] &= ~(1<<bitPos); // 黑色：清0
        } else {
            bmpBuf[byteIdx] |= 1<<bitPos;    // 白色：置1
        }
    };

    // ===== 关键修改：坐标变换（顺时针180°+左右镜像 = 垂直翻转） =====
    uint8_t* fb = renderer.getFrameBuffer();
    for(int y=0; y<H; y++){
        for(int x=0; x<W; x++){
            // 1. 先获取原始逻辑坐标对应的屏幕像素状态
            int rx=0, ry=0;
            renderer.rotateCoordinates(x,y,&rx,&ry);
            int byteIdx = ry * EInkDisplay::DISPLAY_WIDTH_BYTES + (rx/8);
            int bitPos =7 - (rx%8);
            bool isBlack = ((fb[byteIdx] >> bitPos) &1) ==0;

            // 2. 应用变换：顺时针180°+左右镜像 → 等效坐标 (x, H-1-y)
            int finalX = x;                  // 左右镜像抵消了旋转的水平翻转
            int finalY = H - 1 - y;          // 最终只有垂直方向颠倒

            // 3. 绘制变换后的像素到BMP
            drawBmpPixel(finalX, finalY, isBlack);
        }
    }

    // ===== 以下BMP文件写入逻辑 【加2行修复代码，必加】 =====
    FsFile f;
    if(!SdMan.openFileForWrite("TRA", filename, f)) {
        free(bmpBuf);
        return false;
    }

    // BMP文件头
    uint8_t fh[14]={'B','M',0,0,0,0,0,0,0,0,54,0,0,0};
    // BMP信息头
    uint8_t ih[40]={
        40,0,0,0,
        (uint8_t)W,(uint8_t)(W>>8),0,0,  // 宽度
        (uint8_t)H,(uint8_t)(H>>8),0,0,  // 高度（正数，无需特殊处理）
        1,0, 1,0, 0,0,0,0, 0,0,0,0,
        0,0,0,0, 0,0,0,0, 2,0, 2,0
    };
    // 黑白调色板
    uint8_t pal[8]={0,0,0,0,255,255,255,0};

    // 计算文件总大小
    uint32_t ts=14+40+8+BMP_SIZE;
    fh[2]=ts&0xFF;fh[3]=(ts>>8)&0xFF;fh[4]=(ts>>16)&0xFF;fh[5]=(ts>>24)&0xFF;

    // 写入文件
    f.write(fh,14);
    f.write(ih,40);
    f.write(pal,8);
    f.write(bmpBuf,BMP_SIZE);

    // ✅ 只保留这两行，强制落盘，缺一不可
    f.flush();
    f.sync();

    // 释放资源
    f.close();
    free(bmpBuf);
    Serial.printf("[%lu] [BMP] 变换完成！已保存为 %s\n", millis(), filename);
    return true;
}


