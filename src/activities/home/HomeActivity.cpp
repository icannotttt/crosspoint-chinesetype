#include "HomeActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <SDCardManager.h>

#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ScreenComponents.h"
#include "fontIds.h"
//gd:新增封面支持
#include <Xtc.h>
#include "CrossPointSettings.h"
#include "images/CrossLarge.h"

namespace {
// Check if path has XTC extension (.xtc or .xtch)
bool isXtcFile(const std::string& path) {
  if (path.length() < 4) return false;
  std::string ext4 = path.substr(path.length() - 4);
  if (ext4 == ".xtc") return true;
  if (path.length() >= 5) {
    std::string ext5 = path.substr(path.length() - 5);
    if (ext5 == ".xtch") return true;
  }
  return false;
}
bool isEpubFile(const std::string& path) {
  if (path.length() < 5) return false;
  if (path.length() >= 5) {
    std::string ext5 = path.substr(path.length() - 5);
    if (ext5 == ".epub") return true;
  }
  return false;
}
}  // namespace

void HomeActivity::taskTrampoline(void* param) {
  auto* self = static_cast<HomeActivity*>(param);
  self->displayTaskLoop();
}

int HomeActivity::getMenuItemCount() const { return hasContinueReading ? 4 : 3; }

void HomeActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  // Check if we have a book to continue reading
  hasContinueReading = !APP_STATE.openEpubPath.empty() && SdMan.exists(APP_STATE.openEpubPath.c_str());

  if (hasContinueReading) {
    // Extract filename from path for display
    lastBookTitle = APP_STATE.openEpubPath;
    const size_t lastSlash = lastBookTitle.find_last_of('/');
    if (lastSlash != std::string::npos) {
      lastBookTitle = lastBookTitle.substr(lastSlash + 1);
    }

    const std::string ext4 = lastBookTitle.length() >= 4 ? lastBookTitle.substr(lastBookTitle.length() - 4) : "";
    const std::string ext5 = lastBookTitle.length() >= 5 ? lastBookTitle.substr(lastBookTitle.length() - 5) : "";
    // If epub, try to load the metadata for title/author
    if (ext5 == ".epub") {
      Epub epub(APP_STATE.openEpubPath, "/.crosspoint");
      epub.load(false);
      if (!epub.getTitle().empty()) {
        lastBookTitle = std::string(epub.getTitle());
      }
      if (!epub.getAuthor().empty()) {
        lastBookAuthor = std::string(epub.getAuthor());
      }
    } else if (ext5 == ".xtch") {
      lastBookTitle.resize(lastBookTitle.length() - 5);
    } else if (ext4 == ".xtc") {
      lastBookTitle.resize(lastBookTitle.length() - 4);
    }

  }

  selectorIndex = 0;

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&HomeActivity::taskTrampoline, "HomeActivityTask",
              6144,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void HomeActivity::loop() {
  const bool prevPressed = mappedInput.wasPressed(MappedInputManager::Button::Up) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Left);
  const bool nextPressed = mappedInput.wasPressed(MappedInputManager::Button::Down) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Right);

  const int menuCount = getMenuItemCount();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (hasContinueReading) {
      // Menu: Continue Reading, Browse, File transfer, Settings
      if (selectorIndex == 0) {
        onContinueReading();
      } else if (selectorIndex == 1) {
        onReaderOpen();
      } else if (selectorIndex == 2) {
        onFileTransferOpen();
      } else if (selectorIndex == 3) {
        onSettingsOpen();
      }
    } else {
      // Menu: Browse, File transfer, Settings
      if (selectorIndex == 0) {
        onReaderOpen();
      } else if (selectorIndex == 1) {
        onFileTransferOpen();
      } else if (selectorIndex == 2) {
        onSettingsOpen();
      }
    }
  } else if (prevPressed) {
    selectorIndex = (selectorIndex + menuCount - 1) % menuCount;
    updateRequired = true;
  } else if (nextPressed) {
    selectorIndex = (selectorIndex + 1) % menuCount;
    updateRequired = true;
  }
}

void HomeActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void HomeActivity::render() const {
  int currentCount = ++const_cast<HomeActivity*>(this)->renderCallCount;
  Serial.printf("[HA] render()第%d次调用\n", currentCount);
  if (currentCount==1){
    renderer.clearScreen();
  }else{
    renderer.clearArea(400, 0, 400, 480);
  };
  

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  constexpr int margin = 20;
  constexpr int bottomMargin = 60;

  // --- Top "book" card for the current title (selectorIndex == 0) ---
  const bool bookSelected = hasContinueReading && selectorIndex == 0;
  const int bookWidth = pageWidth / 2;
  const int bookHeight = pageHeight / 2;
  const int bookX = (pageWidth - bookWidth) / 2;
  constexpr int bookY = 30;
    // Bookmark icon in the top-right corner of the card
  const int bookmarkWidth = bookWidth / 8;
  const int bookmarkHeight = bookHeight / 5;
  const int bookmarkX = bookX + bookWidth - bookmarkWidth - 8;
  constexpr int bookmarkY = bookY + 1;
  if(currentCount == 1){

  

  // Draw book card regardless, fill with message based on `hasContinueReading`
  {
    if (bookSelected) {
      //renderer.fillRect(bookX, bookY, bookWidth, bookHeight);
    } else {
      //renderer.drawRect(bookX, bookY, bookWidth, bookHeight);
    }



    // Main bookmark body (solid)
    //gd书签隐去
    //renderer.fillRect(bookmarkX, bookmarkY, bookmarkWidth, bookmarkHeight, !bookSelected);
   


    // Carve out an inverted triangle notch at the bottom center to create angled points
    const int notchHeight = bookmarkHeight / 2;  // depth of the notch
    for (int i = 0; i < notchHeight; ++i) {
      const int y = bookmarkY + bookmarkHeight - 1 - i;
      const int xStart = bookmarkX + i;
      const int width = bookmarkWidth - 2 * i;
      if (width <= 0) {
        break;
      }
      // Draw a horizontal strip in the opposite color to "cut" the notch画的三角
      //renderer.fillRect(xStart, y, width, 1, bookSelected);
    }
  }


  if (hasContinueReading) {
    // Split into words (avoid stringstream to keep this light on the MCU)
    std::vector<std::string> words;
    words.reserve(8);
    size_t pos = 0;
    while (pos < lastBookTitle.size()) {
      while (pos < lastBookTitle.size() && lastBookTitle[pos] == ' ') {
        ++pos;
      }
      if (pos >= lastBookTitle.size()) {
        break;
      }
      const size_t start = pos;
      while (pos < lastBookTitle.size() && lastBookTitle[pos] != ' ') {
        ++pos;
      }
      words.emplace_back(lastBookTitle.substr(start, pos - start));
    }

    std::vector<std::string> lines;
    std::string currentLine;
    // Extra padding inside the card so text doesn't hug the border
    const int maxLineWidth = bookWidth - 40;
    const int spaceWidth = renderer.getSpaceWidth(UI_12_FONT_ID);

    for (auto& i : words) {
      // If we just hit the line limit (3), stop processing words
      if (lines.size() >= 3) {
        // Limit to 3 lines
        // Still have words left, so add ellipsis to last line
        lines.back().append("...");

        while (!lines.back().empty() && renderer.getTextWidth(UI_12_FONT_ID, lines.back().c_str()) > maxLineWidth) {
          lines.back().resize(lines.back().size() - 5);
          lines.back().append("...");
        }
        break;
      }

      int wordWidth = renderer.getTextWidth(UI_12_FONT_ID, i.c_str());
      while (wordWidth > maxLineWidth && i.size() > 5) {
        // Word itself is too long, trim it
        i.resize(i.size() - 5);
        i.append("...");
        wordWidth = renderer.getTextWidth(UI_12_FONT_ID, i.c_str());
      }

      int newLineWidth = renderer.getTextWidth(UI_12_FONT_ID, currentLine.c_str());
      if (newLineWidth > 0) {
        newLineWidth += spaceWidth;
      }
      newLineWidth += wordWidth;

      if (newLineWidth > maxLineWidth && !currentLine.empty()) {
        // New line too long, push old line
        lines.push_back(currentLine);
        currentLine = i;
      } else {
        currentLine.append(" ").append(i);
      }
    }

    // If lower than the line limit, push remaining words
    if (!currentLine.empty() && lines.size() < 3) {
      lines.push_back(currentLine);
    }

    // Book title text
    int totalTextHeight = renderer.getLineHeight(UI_12_FONT_ID) * static_cast<int>(lines.size());
    if (!lastBookAuthor.empty()) {
      totalTextHeight += renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2;
    }

    // Vertically center the title block within the card
    int titleYStart = bookY + (bookHeight - totalTextHeight) / 2;

    for (const auto& line : lines) {
      renderer.drawCenteredText(UI_12_FONT_ID, titleYStart, line.c_str(), !bookSelected);
      titleYStart += renderer.getLineHeight(UI_12_FONT_ID);
    }

    if (!lastBookAuthor.empty()) {
      titleYStart += renderer.getLineHeight(UI_10_FONT_ID) / 2;
      std::string trimmedAuthor = lastBookAuthor;
      // Trim author if too long
      while (renderer.getTextWidth(UI_10_FONT_ID, trimmedAuthor.c_str()) > maxLineWidth && !trimmedAuthor.empty()) {
        trimmedAuthor.resize(trimmedAuthor.size() - 5);
        trimmedAuthor.append("...");
      }
      //renderer.drawCenteredText(UI_10_FONT_ID, titleYStart, trimmedAuthor.c_str(), !bookSelected);
    }

    //renderer.drawCenteredText(UI_10_FONT_ID, bookY + bookHeight - renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2,
    //                          "继续阅读", !bookSelected);
  } else {
    // No book to continue reading
    const int y =
        bookY + (bookHeight - renderer.getLineHeight(UI_12_FONT_ID) - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, y, "无阅读记录");
    renderer.drawCenteredText(UI_10_FONT_ID, y + renderer.getLineHeight(UI_12_FONT_ID), "Start reading below");
  }
  //gd:在框里加一下封面
    // Check if we have a book to continue reading
    // Extract filename from path for display
 // Check if we have a book to continue reading
    Serial.println("[HA] 进入判断");
    if (isXtcFile(APP_STATE.openEpubPath)) {
      Serial.println("[HA] 是xtch");  
      Xtc lastXtc(APP_STATE.openEpubPath, "/.crosspoint");  
      // 2. 直接调用getCachePath()获取路径（构造后立即可用）
      const std::string& coverBmpPath = lastXtc.getCoverBmpPath();
      // 3. 打印验证（记得加.c_str()适配printf）
      Serial.printf("[%lu] [HA] xtc缓存路径：%s\n", millis(), coverBmpPath.c_str());
      FsFile file;
      if (SdMan.openFileForRead("SLP", coverBmpPath, file)) {
        Serial.printf("[%lu] [HA] 封面成功打开\n", millis());
        Bitmap bitmap(file);
        BmpReaderError parseResult = bitmap.parseHeaders();
        
        // 2. 打印BMP文件自身的宽高（核心：从Bitmap类获取）
        Serial.printf("[%lu] [HA] BMP解析结果：%d（0=成功）\n", millis(), (int)parseResult);
        Serial.printf("[%lu] [HA] BMP文件原始宽：%d 像素，高：%d 像素\n", millis(), bitmap.getWidth(), bitmap.getHeight());

        //renderBitmapInBookCard(bitmap, 0, 0, 480, 800);
    
        renderBitmapInBookCard(bitmap, bookX, bookY, bookWidth, bookHeight);
        }


    }
    else if (isEpubFile(APP_STATE.openEpubPath)) {
      Serial.println("[HA] 是epub");    
      // 1. 创建Epub对象 → 构造函数自动生成cachePath
      Epub lastEpub(APP_STATE.openEpubPath, "/.crosspoint");

      // 2. 直接调用getCachePath()获取路径（构造后立即可用）
      const std::string& coverBmpPath = lastEpub.getCoverBmpPath();

      // 3. 打印验证（记得加.c_str()适配printf）
      Serial.printf("[%lu] [HA] epub缓存路径：%s\n", millis(), coverBmpPath.c_str());
        FsFile file;
      if (SdMan.openFileForRead("SLP", coverBmpPath, file)) {
        Serial.printf("[%lu] [HA] 封面成功打开\n", millis());
        Bitmap bitmap(file);
        BmpReaderError parseResult = bitmap.parseHeaders();
        
        // 2. 打印BMP文件自身的宽高（核心：从Bitmap类获取）
        Serial.printf("[%lu] [HA] BMP解析结果：%d（0=成功）\n", millis(), (int)parseResult);
        Serial.printf("[%lu] [HA] BMP文件原始宽：%d 像素，高：%d 像素\n", millis(), bitmap.getWidth(), bitmap.getHeight());

        //renderBitmapInBookCard(bitmap, 0, 0, 480, 800);
    
        renderBitmapInBookCard(bitmap, bookX, bookY, bookWidth, bookHeight);
        }
      file.close();
  

    }else{
      Serial.println("[HA] 格式判断错误"); 
    }
  }
    //gd自加部分结束
 
      // --- Bottom menu tiles (indices 0-3) ---
    const int menuTileWidth = pageWidth - 2 * margin; // gd:修正宽度计算
    constexpr int menuTileHeight = 50;
    constexpr int menuSpacing = 10;
    constexpr int totalMenuHeight = 4 * menuTileHeight + 3 * menuSpacing; //gd:4菜单+3间距

    int menuStartY = bookY + bookHeight + 20;
    // 确保菜单不超出屏幕底部
    const int maxMenuStartY = pageHeight - bottomMargin - totalMenuHeight - margin;
    if (menuStartY > maxMenuStartY) {
      menuStartY = maxMenuStartY;
    }

    for (int i = 0; i < 4; ++i) {
      constexpr const char* items[4] = {"继续阅读","文件选择", "wifi传书", "设置"};//gd
      const int overallIndex = i; // 索引直接对应selectorIndex 0-3
      constexpr int tileX = margin;
      const int tileY = menuStartY + i * (menuTileHeight + menuSpacing);
      const bool selected = selectorIndex == overallIndex;

      if (selected) {
        renderer.fillRect(tileX, tileY, menuTileWidth, menuTileHeight);
      } else {
        renderer.drawRect(tileX, tileY, menuTileWidth, menuTileHeight);
      }

      const char* label = items[i];
      const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
      const int textX = tileX + (menuTileWidth - textWidth) / 2;
      const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
      const int textY = tileY + (menuTileHeight - lineHeight) / 2;  // vertically centered assuming y is top of text

      // Invert text when the tile is selected, to contrast with the filled background
      renderer.drawText(UI_10_FONT_ID, textX, textY, label, !selected);
      }

  const auto labels = mappedInput.mapLabels("", "确认", "向上", "向下");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  ScreenComponents::drawBattery(renderer, 20, pageHeight - 70);

  
  if (currentCount == 1) {
    renderer.displayBuffer();
  } else {
    renderer.displayWindow(400, 0, 400, 480); // 非首次仅刷菜单区域
  }
  
}

void HomeActivity::renderBitmapInBookCard(const Bitmap& bitmap, int bookX, int bookY, int bookWidth, int bookHeight) const {
  int x, y;
  // 核心修改：将基准尺寸从「屏幕1/4」改为「书籍卡片的尺寸」
  const auto cardWidth = bookWidth;   // 卡片宽度（替代原pageWidth）
  const auto cardHeight = bookHeight; // 卡片高度（替代原pageHeight）

  if (bitmap.getWidth() > cardWidth || bitmap.getHeight() > cardHeight) {
    // 图片超过卡片尺寸 → 等比缩放后居中
    const float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float cardRatio = static_cast<float>(cardWidth) / static_cast<float>(cardHeight);

    if (ratio > cardRatio) {
      // 图片宽高比 > 卡片宽高比 → 按宽度缩放，垂直居中
      x = 0; // 卡片内X轴起点（相对卡片左上角）
      y = (cardHeight - cardWidth / ratio) / 2; // 垂直居中偏移
    } else {
      // 图片宽高比 < 卡片宽高比 → 按高度缩放，水平居中
      x = (cardWidth - cardHeight * ratio) / 2; // 水平居中偏移
      y = 0; // 卡片内Y轴起点（相对卡片左上角）
    }
  } else {
    // 图片小于卡片尺寸 → 直接居中
    x = (cardWidth - bitmap.getWidth()) / 2;
    y = (cardHeight - bitmap.getHeight()) / 2;
  }

  // 核心修改：绘制到「书籍卡片的位置」（x/y是卡片内的相对坐标，需叠加bookX/bookY）
  renderer.drawBitmap(bitmap, bookX+x , bookY + y, cardWidth, cardHeight);
  // 若需立即刷新显示（可选，建议卡片绘制完成后统一刷新）
  renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
   if (bitmap.hasGreyscale()) {
    bitmap.rewindToData();
    //renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawBitmap(bitmap, bookX+x , bookY + y, cardWidth, cardHeight);
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    //renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.drawBitmap(bitmap, bookX+x , bookY + y, cardWidth, cardHeight);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }

}
void HomeActivity::renderDefaultScreen() const {
  //判断
  Serial.println("[HA] 已进入此函数");
  renderer.clearScreen();
}