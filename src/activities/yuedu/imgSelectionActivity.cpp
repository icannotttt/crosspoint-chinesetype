#include "imgSelectionActivity.h"

#include <GfxRenderer.h>
#include <InputManager.h>
#include <SD.h>

#include "config.h"
#include <GfxRenderer.h>
#include "Epub.h"

namespace {
constexpr int PAGE_ITEMS = 23;
constexpr int SKIP_PAGE_MS = 700;
const std::string EPUB_CACHE_ROOT = "/.crosspoint/"; // 缓存根目录
}  // namespace

void sortimgList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    if (str1.back() == '/' && str2.back() != '/') return true;
    if (str1.back() != '/' && str2.back() == '/') return false;
    return lexicographical_compare(
        begin(str1), end(str1), begin(str2), end(str2),
        [](const char& char1, const char& char2) { return tolower(char1) < tolower(char2); });
  });
}

void imgSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<imgSelectionActivity*>(param);
  self->displayTaskLoop();
}

// ===== 修复：恢复文件夹显示，确保目录和EPUB都被正确加载 =====
void imgSelectionActivity::loadFiles() {
  files.clear();
  selectorIndex = 0;
  auto root = SD.open(basepath.c_str());
  if (!root) { // 新增：目录打开失败防护
    Serial.printf("[%lu] [IMG_SEL] 打开目录失败：%s\n", millis(), basepath.c_str());
    return;
  }

  for (File file = root.openNextFile(); file; file = root.openNextFile()) {
    auto filename = std::string(file.name());
    // 跳过隐藏文件（以.开头）
    if (filename.empty() || filename[0] == '.') {
      file.close();
      continue;
    }

    if (file.isDirectory()) {
      // 目录：拼接/标记，确保显示
      files.emplace_back(filename + "/");
      Serial.printf("[%lu] [IMG_SEL] 加载目录：%s\n", millis(), (filename + "/").c_str());
    } else {
      // 文件：仅保留.epub文件
      if (filename.length() >= 5 && filename.substr(filename.length() - 5) == ".epub") {
        files.emplace_back(filename);
        Serial.printf("[%lu] [IMG_SEL] 加载EPUB文件：%s\n", millis(), filename.c_str());
      }
    }
    file.close();
  }
  root.close();
  sortimgList(files);
}

void imgSelectionActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  basepath = "/";
  loadFiles();
  selectorIndex = 0;
  updateRequired = true;

  xTaskCreate(&imgSelectionActivity::taskTrampoline, "imgSelectionActivityTask",
              2048, this, 1, &displayTaskHandle);
}

void imgSelectionActivity::onExit() {
  Activity::onExit();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  files.clear();
}

// ===== 核心修复：确保点击目录/文件都正常，且保留原有逻辑 =====
void imgSelectionActivity::loop() {
  const bool prevReleased =
      inputManager.wasReleased(InputManager::BTN_UP) || inputManager.wasReleased(InputManager::BTN_LEFT);
  const bool nextReleased =
      inputManager.wasReleased(InputManager::BTN_DOWN) || inputManager.wasReleased(InputManager::BTN_RIGHT);

  const bool skipPage = inputManager.getHeldTime() > SKIP_PAGE_MS;

  if (inputManager.wasPressed(InputManager::BTN_CONFIRM)) {
    if (files.empty()) {
      return;
    }

    std::string selectedItem = files[selectorIndex];
    Serial.printf("[%lu] [IMG_SEL] 选中项：%s\n", millis(), selectedItem.c_str());

    if (basepath.back() != '/') basepath += "/";

    // 选中的是目录：进入目录
    if (selectedItem.back() == '/') {
      basepath += selectedItem.substr(0, selectedItem.length() - 1);
      loadFiles();
      updateRequired = true;
      Serial.printf("[%lu] [IMG_SEL] 进入目录：%s\n", millis(), basepath.c_str());
    } 
    // 选中的是EPUB文件：处理缓存+封面，然后调用onSelect
    else if (selectedItem.length() >= 5 && selectedItem.substr(selectedItem.length() - 5) == ".epub") {
      std::string fullEpubPath = basepath + selectedItem;
      
      // 处理EPUB缓存和封面判断
      Epub epub(fullEpubPath, EPUB_CACHE_ROOT);
      if (epub.load()) {
        std::string cachePath = epub.getCachePath();
        std::string coverBmpPath = cachePath + "/cover.bmp";
        bool coverExists = SD.exists(coverBmpPath.c_str());
        
        if (coverExists) {
          Serial.printf("[%lu] [IMG_SEL] ✅ 封面BMP存在：%s\n", millis(), coverBmpPath.c_str());
        } else {
          Serial.printf("[%lu] [IMG_SEL] ❌ 封面BMP不存在：%s\n", millis(), coverBmpPath.c_str());
          epub.generateCoverBmp(); // 自动生成
        }
      } else {
        Serial.printf("[%lu] [IMG_SEL] ❌ 加载EPUB失败：%s\n", millis(), fullEpubPath.c_str());
      }

      // 关键：确保调用onSelect，进入EPUB阅读
      Serial.printf("[%lu] [IMG_SEL] 打开EPUB文件：%s\n", millis(), fullEpubPath.c_str());
      onSelect(fullEpubPath);
    }
  } 
  // 返回上级目录逻辑
  else if (inputManager.wasPressed(InputManager::BTN_BACK)) {
    if (basepath != "/") {
      basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
      if (basepath.empty()) basepath = "/";
      loadFiles();
      updateRequired = true;
      Serial.printf("[%lu] [IMG_SEL] 返回上级目录：%s\n", millis(), basepath.c_str());
    } else {
      onGoHome();
    }
  } 
  // 上下翻页逻辑
  else if (prevReleased) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / PAGE_ITEMS - 1) * PAGE_ITEMS + files.size()) % files.size();
    } else {
      selectorIndex = (selectorIndex + files.size() - 1) % files.size();
    }
    updateRequired = true;
  } else if (nextReleased) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / PAGE_ITEMS + 1) * PAGE_ITEMS) % files.size();
    } else {
      selectorIndex = (selectorIndex + 1) % files.size();
    }
    updateRequired = true;
  }
}

void imgSelectionActivity::displayTaskLoop() {
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

// ===== 修复：确保目录和文件都能正常渲染 =====
void imgSelectionActivity::render() const {
  renderer.clearScreen();

  const auto screenWidth = GfxRenderer::getScreenWidth();
  const auto screenHeight = GfxRenderer::getScreenHeight();
  
  if (screenWidth < 10 || screenHeight < 10) {
    renderer.displayBuffer();
    return;
  }

  renderer.drawCenteredText(READER_FONT_ID, 10, "Books", true, BOLD);
  renderer.drawText(SMALL_FONT_ID, 20, screenHeight - 30, "Press BACK for Home");

  // 安全筛选：保留目录（以/结尾）和EPUB文件
  std::vector<std::string> displayFiles;
  for (const auto& file : files) {
    if (file.back() == '/' || (file.length() >= 5 && file.substr(file.length() - 5) == ".epub")) {
      displayFiles.push_back(file);
    }
  }

  if (displayFiles.empty()) {
    renderer.drawText(UI_FONT_ID, 20, 60, "No files or directories");
    renderer.displayBuffer();
    return;
  }

  const int COLUMNS_PER_ROW = 3;          
  const int ROWS_PER_PAGE = 3;            
  const int PAGE_ITEMS_GRID = 9;          
  const int ITEM_WIDTH = std::max(screenWidth / 4, 10);
  const int ITEM_HEIGHT = std::max(screenHeight / 4, 10);

  const int totalGridWidth = COLUMNS_PER_ROW * ITEM_WIDTH + (COLUMNS_PER_ROW - 1) * 15; 
  const int totalGridHeight = ROWS_PER_PAGE * ITEM_HEIGHT + (ROWS_PER_PAGE - 1) * 20;   
  const int startX = std::max((screenWidth - totalGridWidth) / 2, 0);   
  const int startY = std::min(100, screenHeight - totalGridHeight - 20);

  // 安全处理选中索引
  int safeSelectorIndex = std::min((int)selectorIndex, (int)displayFiles.size() - 1);
  safeSelectorIndex = std::max(safeSelectorIndex, 0);
  const auto pageStartIndex = safeSelectorIndex / PAGE_ITEMS_GRID * PAGE_ITEMS_GRID;

  // 渲染文件/目录列表
  for (int i = pageStartIndex; i < displayFiles.size() && i < pageStartIndex + PAGE_ITEMS_GRID; i++) {
    const auto& item = displayFiles[i];
    if (item.empty()) continue;

    int itemOffset = i - pageStartIndex;
    int row = itemOffset / COLUMNS_PER_ROW;
    int col = itemOffset % COLUMNS_PER_ROW;

    int xPos = startX + col * (ITEM_WIDTH + 10);
    int yPos = startY + row * (ITEM_HEIGHT + 10);
    xPos = std::max(xPos, 0);
    yPos = std::max(yPos, 0);

    // 绘制选中态
    if (ITEM_WIDTH > 0 && ITEM_HEIGHT > 0) {
      if (i == safeSelectorIndex) {
        renderer.fillRect(xPos, yPos, ITEM_WIDTH, ITEM_HEIGHT);
        renderer.drawRect(xPos, yPos, ITEM_WIDTH, ITEM_HEIGHT);
      } else {
        renderer.drawRect(xPos, yPos, ITEM_WIDTH, ITEM_HEIGHT);
      }
    }

    // 处理文件名截断
    std::string displayText = item;
    int textWidth = renderer.getTextWidth(UI_FONT_ID, displayText.c_str());
    while (textWidth > ITEM_WIDTH - 10 && displayText.length() > 4) {
      displayText = displayText.substr(0, displayText.length() - 4) + "...";
      textWidth = renderer.getTextWidth(UI_FONT_ID, displayText.c_str());
    }

    // 居中绘制文字
    int textX = xPos + (ITEM_WIDTH - textWidth) / 2;
    int textY = yPos + (ITEM_HEIGHT - 20) / 2;
    textX = std::max(textX, 0);
    textY = std::max(textY, 0);
    renderer.drawText(UI_FONT_ID, textX, textY, displayText.c_str(), i != safeSelectorIndex);
  }

  renderer.displayBuffer();
}

void imgSelectionActivity::renderBitmapScreen(const Bitmap& bitmap) const {
  int x, y;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    const float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    if (ratio > screenRatio) {
      x = 0;
      y = (pageHeight - pageWidth / ratio) / 2;
    } else {
      x = (pageWidth - pageHeight * ratio) / 2;
      y = 0;
    }
  } else {
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight);
  renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
}