#include "FileSelectionActivity.h"

#include <GfxRenderer.h>
#include <SDCardManager.h>

#include "MappedInputManager.h"
#include "fontIds.h"

namespace {
constexpr int PAGE_ITEMS = 23;
constexpr int SKIP_PAGE_MS = 700;
constexpr unsigned long GO_HOME_MS = 1000;
constexpr int COPY_BUF_SIZE = 256; // 256字节缓冲区，适配小运存
}  // namespace

// 你的删除函数（完全保留，无修改）
bool deleteFileOrDir(const std::string& fullPath) {
  if (fullPath.back() == '/') {
    std::string dirPath = fullPath.substr(0, fullPath.length() - 1);
    bool deleted = SdMan.removeDir(dirPath.c_str());
    if (deleted) {
      Serial.printf("[删除] 成功删除一级文件夹：%s\n", dirPath.c_str());
    } else {
      Serial.printf("[删除] 失败删除一级文件夹（非空/不存在）：%s\n", dirPath.c_str());
    }
    return deleted;
  } else {
    if (!SdMan.exists(fullPath.c_str())) {
      Serial.printf("[删除] 文件不存在：%s\n", fullPath.c_str());
      return false;
    }
    bool deleted = SdMan.remove(fullPath.c_str());
    if (deleted) {
      Serial.printf("[删除] 成功删除文件：%s\n", fullPath.c_str());
    } else {
      Serial.printf("[删除] 失败删除文件：%s\n", fullPath.c_str());
    }
    return deleted;
  }
}

// ✅ 新增：256字节流式复制文件（省内存）
bool copyFile(const char* srcPath, const char* dstPath) {
  // 检查源文件是否存在
  if (!SdMan.exists(srcPath)) {
    Serial.printf("[复制] 源文件不存在：%s\n", srcPath);
    return false;
  }
  // 检查目标文件是否已存在
  if (SdMan.exists(dstPath)) {
    Serial.printf("[复制] 目标文件已存在：%s\n", dstPath);
    return false;
  }

  FsFile srcFile, dstFile;
  // 打开源文件
  if (!SdMan.openFileForRead("FileSelection", srcPath, srcFile)) {
    Serial.printf("[复制] 打开源文件失败：%s\n", srcPath);
    return false;
  }
  // 打开目标文件（创建新文件）
  if (!SdMan.openFileForWrite("FileSelection", dstPath, dstFile)) {
    Serial.printf("[复制] 创建目标文件失败：%s\n", dstPath);
    srcFile.close();
    return false;
  }

  // 256字节缓冲区，边读边写
  uint8_t buf[COPY_BUF_SIZE];
  size_t readBytes = 0;
  while ((readBytes = srcFile.read(buf, COPY_BUF_SIZE)) > 0) {
    dstFile.write(buf, readBytes);
  }

  // 关闭文件句柄，释放资源
  srcFile.close();
  dstFile.close();
  
  Serial.printf("[复制] 成功：%s → %s\n", srcPath, dstPath);
  return true;
}

// ✅ 新增：复制空文件夹
bool copyDir(const char* srcPath, const char* dstPath) {
  // 检查源文件夹是否存在
  if (!SdMan.exists(srcPath)) {
    Serial.printf("[复制] 源文件夹不存在：%s\n", srcPath);
    return false;
  }
  // 创建目标文件夹
  if (!SdMan.mkdir(dstPath, true)) {
    Serial.printf("[复制] 创建目标文件夹失败：%s\n", dstPath);
    return false;
  }
  Serial.printf("[复制] 文件夹成功：%s → %s\n", srcPath, dstPath);
  return true;
}

// 你原有排序函数（正序）
void sortFileList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    if (str1.back() == '/' && str2.back() != '/') return true;
    if (str1.back() != '/' && str2.back() == '/') return false;
    return lexicographical_compare(
        begin(str1), end(str1), begin(str2), end(str2),
        [](const char& char1, const char& char2) { return tolower(char1) < tolower(char2); });
  });
}

// 按名称倒序排序
void FileSelectionActivity::sortFileListByName(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    if (str1.back() == '/' && str2.back() != '/') return true;
    if (str1.back() != '/' && str2.back() == '/') return false;
    return lexicographical_compare(
        begin(str2), end(str2), begin(str1), end(str1),
        [](const char& char1, const char& char2) { return tolower(char1) < tolower(char2); });
  });
}

// 原有函数（完全保留）
void FileSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<FileSelectionActivity*>(param);
  self->displayTaskLoop();
}

// 你的loadFiles（完全保留）
void FileSelectionActivity::loadFiles() {
  files.clear();
  selectorIndex = 0;

  // ✅ 新增调试日志：打印当前要打开的目录路径
  Serial.printf("[加载文件] 尝试打开目录：%s\n", basepath.c_str());

  auto root = SdMan.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    // ✅ 新增：打印打开失败的原因
    if (!root) {
      Serial.printf("[加载文件] 打开目录失败！路径不存在或权限错误：%s\n", basepath.c_str());
    } else {
      Serial.printf("[加载文件] 路径不是目录：%s\n", basepath.c_str());
      root.close();
    }
    return;
  }

  Serial.printf("[加载文件] 成功打开目录：%s，开始遍历文件\n", basepath.c_str());

  root.rewindDirectory();

  char name[128];
  int fileCount = 0; // 统计遍历到的文件数
  int bookCount = 0; // 统计符合后缀的书籍数
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    fileCount++;

    if (file.isDirectory()) {
      files.emplace_back(std::string(name) + "/");
      Serial.printf("[遍历] 找到文件夹：%s\n", name);
    } else {
      auto filename = std::string(name);
      std::string ext4 = filename.length() >= 4 ? filename.substr(filename.length() - 4) : "";
      std::string ext5 = filename.length() >= 5 ? filename.substr(filename.length() - 5) : "";
      std::string ext7 = filename.length() >= 7 ? filename.substr(filename.length() - 7) : "";
      // ✅ 打印文件后缀，排查过滤是否错误
      Serial.printf("[遍历] 找到文件：%s，ext4=%s, ext5=%s\n", name, ext4.c_str(), ext5.c_str());
      
      if (ext5 == ".epub" || ext5 == ".xtch" || ext4 == ".xtc" || ext4 == ".txt" || ext4 == ".bmp" || ext4 == ".jpg" || ext7 == ".pngtxt") {
        files.emplace_back(filename);
        bookCount++;
      }
    }
    file.close();
  }
  root.close();

  // ✅ 打印统计信息
  Serial.printf("[加载完成] 遍历到%d个文件/文件夹，符合后缀的书籍数：%d\n", fileCount, bookCount);
  sortFileList(files);
}


// 原有函数（初始化新增复制状态）
void FileSelectionActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  loadFiles();
  selectorIndex = 0;
  topSelectorIndex = TopOption::OPEN;
  // 初始化复制/剪切状态
  copySourcePath = "";
  hasCopyData = false;
  isCutMode = false; // ✅ 初始化剪切标记

  updateRequired = true;

  xTaskCreate(&FileSelectionActivity::taskTrampoline, "FileSelectionActivityTask",
              2048,               
              this,               
              1,                  
              &displayTaskHandle  
  );
}

// 原有函数（完全保留）
void FileSelectionActivity::onExit() {
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

// ✅ 核心loop（修复switch-case变量作用域问题）
// ✅ 核心loop（终极修复多级目录路径拼接）
void FileSelectionActivity::loop() {
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= GO_HOME_MS) {
    if (basepath != "/") {
      basepath = "/";
      loadFiles();
      updateRequired = true;
    }
    return;
  }

  const bool topPrevPressed = mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool topNextPressed = mappedInput.wasReleased(MappedInputManager::Button::Right);
  
  if (topPrevPressed) {
    topSelectorIndex = (TopOption)(((int)topSelectorIndex - 1 + topOptionCount) % topOptionCount);
    updateRequired = true;
  } else if (topNextPressed) {
    topSelectorIndex = (TopOption)(((int)topSelectorIndex + 1) % topOptionCount);
    updateRequired = true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (files.empty()) return;

    std::string fullPath = basepath;
    if (fullPath.back() != '/') fullPath += "/";
    fullPath += files[selectorIndex];

    switch (topSelectorIndex) {
      case TopOption::OPEN: 
        if (files[selectorIndex].back() == '/') {
          std::string parentPath = basepath;
          std::string childDir = files[selectorIndex].substr(0, files[selectorIndex].length() - 1);
          if (parentPath == "/") {
            basepath = parentPath + childDir;
          } else {
            basepath = parentPath + "/" + childDir;
          }
          Serial.printf("[打开目录] 父路径：%s + 子目录：%s → 最终路径：%s\n", 
                        parentPath.c_str(), childDir.c_str(), basepath.c_str());
          loadFiles();
        } else {
          onSelect(fullPath);
        }
        break;

      case TopOption::DELETE: 
        if (mappedInput.getHeldTime() >= 500) {
          deleteFileOrDir(fullPath);
          loadFiles();
        } else {
          Serial.printf("[删除] 需长按Confirm确认删除\n");
        }
        break;

      case TopOption::COPY: 
        copySourcePath = fullPath;
        hasCopyData = true;
        isCutMode = false;
        Serial.printf("[复制] 已选中：%s\n", copySourcePath.c_str());
        break;

      case TopOption::CUT: // 新增剪切
        copySourcePath = fullPath;
        hasCopyData = true;
        isCutMode = true;
        Serial.printf("[剪切] 已选中：%s（粘贴后将删除源文件）\n", copySourcePath.c_str());
        break;

      case TopOption::PASTE: // 粘贴（支持剪切）
      {
        if (!hasCopyData) {
          Serial.printf("[粘贴] 无待复制/剪切内容\n");
          break;
        }
        std::string dstPath = basepath;
        if (dstPath.back() != '/') dstPath += "/";
        size_t lastSlash = copySourcePath.find_last_of('/');
        std::string fileName = copySourcePath.substr(lastSlash + 1);
        dstPath += fileName;

        bool pasteSuccess = false;
        if (copySourcePath.back() == '/') {
          pasteSuccess = copyDir(copySourcePath.c_str(), dstPath.c_str());
        } else {
          pasteSuccess = copyFile(copySourcePath.c_str(), dstPath.c_str());
        }

        // 剪切模式：粘贴成功后删除源文件
        if (pasteSuccess && isCutMode) {
          Serial.printf("[剪切] 粘贴成功，删除源文件：%s\n", copySourcePath.c_str());
          deleteFileOrDir(copySourcePath);
          isCutMode = false;
        }

        hasCopyData = false;
        copySourcePath = "";
        loadFiles(); 
        break;
      }

      case TopOption::SORT_DESC: 
        sortFileListByName(files);
        Serial.printf("[排序] 按名称倒序\n");
        break;

      case TopOption::SORT_ASC: 
        sortFileList(files);
        Serial.printf("[排序] 按名称正序\n");
        break;
    }
    updateRequired = true;
    return;
  }

  const bool filePrevPressed = mappedInput.wasReleased(MappedInputManager::Button::Up);
  const bool fileNextPressed = mappedInput.wasReleased(MappedInputManager::Button::Down);
  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;

  if (filePrevPressed) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / PAGE_ITEMS - 1) * PAGE_ITEMS + files.size()) % files.size();
    } else {
      selectorIndex = (selectorIndex + files.size() - 1) % files.size();
    }
    updateRequired = true;
  } else if (fileNextPressed) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / PAGE_ITEMS + 1) * PAGE_ITEMS) % files.size();
    } else {
      selectorIndex = (selectorIndex + 1) % files.size();
    }
    updateRequired = true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (basepath != "/") {
        size_t lastSlash = basepath.find_last_of('/');
        if (lastSlash == 0) {
          basepath = "/";
        } else {
          basepath = basepath.substr(0, lastSlash);
        }
        Serial.printf("[返回上一级] 新路径：%s\n", basepath.c_str());
        loadFiles();
        updateRequired = true;
      } else {
        onGoHome();
      }
    }
  }
}


// 原有函数（完全保留）
void FileSelectionActivity::displayTaskLoop() {
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

// ✅ 渲染函数（6个按钮+一行4个+文件列表下移）
void FileSelectionActivity::render() const {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  
  // 7个顶部选项（新增「剪切」）
  constexpr const char* topItems[7] = {"打开", "删除", "复制", "剪切", "粘贴", "倒序", "正序"};
  constexpr int margin = 10;
  constexpr int menuSpacing = 5;
  const int menuTileWidth = (pageWidth - 2 * margin - 3 * menuSpacing) / 4;
  constexpr int menuTileHeight = 30;
  constexpr int topMenuY = 15;

  // 分页显示（一行4个）
  int startIdx = ((int)topSelectorIndex / 4) * 4;
  if (startIdx + 4 > 7) startIdx = 3;

  for (int i = 0; i < 4; i++) {
    int btnIdx = startIdx + i;
    if (btnIdx >= 7) break;
    
    int tileX = margin + i * (menuTileWidth + menuSpacing);
    int tileY = topMenuY;
    bool selected = (TopOption)btnIdx == topSelectorIndex;

    if (selected) {
      renderer.fillRect(tileX, tileY, menuTileWidth, menuTileHeight);
    } else {
      renderer.drawRect(tileX, tileY, menuTileWidth, menuTileHeight);
    }

    int buttonCenterY = tileY;
    int textX = tileX + (menuTileWidth - renderer.getTextWidth(UI_10_FONT_ID, topItems[btnIdx])) / 2;
    renderer.drawText(UI_10_FONT_ID, textX, buttonCenterY, topItems[btnIdx], !selected, EpdFontFamily::BOLD);
  }

  const int fileListStartY = topMenuY + menuTileHeight + 40;
  if (files.empty()) {
    renderer.drawText(UI_10_FONT_ID, 20, fileListStartY, "No books found");
    renderer.displayBuffer();
    return;
  }

  const auto pageStartIndex = selectorIndex / PAGE_ITEMS * PAGE_ITEMS;
  renderer.fillRect(0, fileListStartY + (selectorIndex % PAGE_ITEMS) * 30 - 2, pageWidth - 1, 30);
  
  for (int i = pageStartIndex; i < files.size() && i < pageStartIndex + PAGE_ITEMS; i++) {
    auto item = renderer.truncatedText(UI_10_FONT_ID, files[i].c_str(), renderer.getScreenWidth() - 40);
    renderer.drawText(UI_10_FONT_ID, 20, fileListStartY + (i % PAGE_ITEMS) * 30, item.c_str(), i != selectorIndex);
  }
  drawDashedLine(renderer, 0, topMenuY+menuTileHeight+20, 480, 10);

  renderer.displayBuffer();
}

void FileSelectionActivity::drawDashedLine(GfxRenderer& renderer, int x1, int y, int x2, bool isDark) const {
  int startX = std::min(x1, x2);
  int endX = std::max(x1, x2);
  int currentX = startX;

  // 放大参数：段长=12（间隔×4），间隔=3（480px屏幕肉眼清晰）
  const int actualDash = 20;  
  const int actualGap = 10;    

  while (currentX < endX) {
    int segmentEndX = std::min(currentX + actualDash, endX);
    // 关键：先把!isDark改成true，强制画黑色实线段（排除颜色问题）
    renderer.drawLine(currentX, y, segmentEndX, y, true);
    currentX = segmentEndX + actualGap;
  }
}