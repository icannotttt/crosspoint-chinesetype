#include "MyLibraryActivity.h"

#include <GfxRenderer.h>
#include <SDCardManager.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/StringUtils.h"
//加入搜索
#include "../util/KeyboardEntryActivity.h"


namespace {
constexpr int SKIP_PAGE_MS = 700;
constexpr unsigned long GO_HOME_MS = 1000;
//防止误删，把删除改为长按confirm
constexpr int COPY_BUF_SIZE = 256; // 256字节缓冲区，适配小运存
}  // namespace
//把原来几个函数加上
//删除
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
//复制
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
//复制文件夹
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

// 递归搜索含关键词文件
void searchFilesRecursive(const std::string& currentDir, const std::string& keyword, std::vector<std::string>& result) {
  auto root = SdMan.open(currentDir.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  char name[500];
  root.rewindDirectory();
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if (name[0] == '.' || strcmp(name, "System Volume Information") == 0 || strcmp(name, "fonts") == 0) {
      file.close();
      continue;
    }

    std::string fullPath = currentDir;
    if (fullPath.back() != '/') fullPath += "/";
    fullPath += name;

    if (file.isDirectory()) {
      searchFilesRecursive(fullPath + "/", keyword, result);
    } else {
      std::string fn = name;
      std::transform(fn.begin(), fn.end(), fn.begin(), ::tolower);
      std::string kw = keyword;
      std::transform(kw.begin(), kw.end(), kw.begin(), ::tolower);

      if (fn.find(kw) != std::string::npos) {
        if (StringUtils::checkFileExtension(fn, ".epub") ||
            StringUtils::checkFileExtension(fn, ".xtch") ||
            StringUtils::checkFileExtension(fn, ".xtc") ||
            StringUtils::checkFileExtension(fn, ".txt") ||
            StringUtils::checkFileExtension(fn, ".md")) {
          result.push_back(fullPath);
        }
      }
    }
    file.close();
  }
  root.close();
}
void MyLibraryActivity::sortFileList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    if (str1.back() == '/' && str2.back() != '/') return true;
    if (str1.back() != '/' && str2.back() == '/') return false;
    return lexicographical_compare(
        begin(str1), end(str1), begin(str2), end(str2),
        [](const char& char1, const char& char2) { return tolower(char1) < tolower(char2); });
  });
}
// 执行搜索（接收char*关键词，适配100字符限制）
void MyLibraryActivity::doSearch(const char* keyword) {
  // 安全拷贝关键词（防止超长，最多99字符+结束符）
  strncpy(SEARCH_KEYWORD, keyword, sizeof(SEARCH_KEYWORD)-1);
  SEARCH_KEYWORD[sizeof(SEARCH_KEYWORD)-1] = '\0'; // 确保字符串结束

  isSearchMode = true;
  originalBasePath = basepath;
  searchResults.clear();
  
  Serial.printf("[搜索] 开始搜索 %s 及其子目录中包含'%s'的文件\n", basepath.c_str(), SEARCH_KEYWORD);
  // 调用递归搜索（传char数组）
  searchFilesRecursive(basepath, SEARCH_KEYWORD, searchResults);
  
  sortFileList(searchResults);
  selectorIndex = 0;
  updateRequired = true;
  
  if (searchResults.empty()) {
    // 提示文字适配char数组
    char emptyHint[128];
    snprintf(emptyHint, sizeof(emptyHint), "未找到含'%s'的文件", SEARCH_KEYWORD);
    Serial.printf("[搜索] %s\n", emptyHint);
  } else {
    Serial.printf("[搜索] 共找到 %d 个匹配文件\n", searchResults.size());
  }
}
// 打开键盘输入Activity（核心修复）
void MyLibraryActivity::executeSearch() {
  // 清除现有子活动然后弹出输入框获取关键词
  exitActivity();
  updateRequired = true;
  enterNewActivity(new KeyboardEntryActivity(
      renderer, mappedInput, "输入搜索关键词", SEARCH_KEYWORD, 10,
      63,     // 最大长度63，与其它地方保持一致
      false,  // 非密码模式
      [this](const std::string& keyword) {
          // 保存关键词；不要立即删除键盘（会在键盘自身loop中导致对象自毁)
          std::string safeKeyword = keyword;
          // 确保不超过数组大小（100字节）
          if (safeKeyword.size() >= sizeof(SEARCH_KEYWORD)) {
              safeKeyword = safeKeyword.substr(0, sizeof(SEARCH_KEYWORD) - 1);
          }
          // ✅ 用memcpy替代strncpy，保证UTF-8完整
          memset(SEARCH_KEYWORD, 0, sizeof(SEARCH_KEYWORD)); // 先清空
          memcpy(SEARCH_KEYWORD, safeKeyword.c_str(), safeKeyword.size());
          
          // 隐藏键盘，使后续按键直接落到父活动
          if (subActivity) {
              static_cast<KeyboardEntryActivity*>(subActivity.get())->hide();
          }
          pendingSearch = true;
          pendingKeyword = SEARCH_KEYWORD;
          updateRequired = true;
      },
      [this]() {
          // 取消输入，仅请求退出子活动
          pendingSearch = false;
          updateRequired = true;
      }));
}

void MyLibraryActivity::cancelSearch() {
  isSearchMode = false;
  basepath = originalBasePath;
  loadFiles();
  selectorIndex = 0;
  updateRequired = true;
}






void MyLibraryActivity::taskTrampoline(void* param) {
  auto* self = static_cast<MyLibraryActivity*>(param);
  self->displayTaskLoop();
}

void MyLibraryActivity::loadFiles() {
  files.clear();

  // 修复：确保路径以/结尾，否则SdMan.open可能识别失败
  std::string realBasePath = basepath;
  if (realBasePath != "/" && realBasePath.back() != '/') {
    realBasePath += "/";
  }

  auto root = SdMan.open(realBasePath.c_str()); // 用修复后的路径打开
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  root.rewindDirectory();

  char name[500];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if (name[0] == '.' || strcmp(name, "System Volume Information") == 0 || strcmp(name, "fonts") == 0) {
      file.close();
      continue;
    }

    if (file.isDirectory()) {
      files.emplace_back(std::string(name) + "/"); // 目录名保留/结尾
    } else {
      auto filename = std::string(name);
      if (StringUtils::checkFileExtension(filename, ".epub") || StringUtils::checkFileExtension(filename, ".xtch") ||
          StringUtils::checkFileExtension(filename, ".xtc") || StringUtils::checkFileExtension(filename, ".txt") ||
          StringUtils::checkFileExtension(filename, ".md")) {
        files.emplace_back(filename);
      }
    }
    file.close();
  }
  root.close();
  sortFileList(files);
}

//enter也需要改
void MyLibraryActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  loadFiles();

  selectorIndex = 0;
  //新增
  topSelectorIndex = TopOption::OPEN;
  copySourcePath = "";
  hasCopyData = false;
  isCutMode = false; 
  isSearchMode = false;
  searchResults.clear();
  originalBasePath = "";
  //新增结束

  updateRequired = true;

  xTaskCreate(&MyLibraryActivity::taskTrampoline, "MyLibraryActivityTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void MyLibraryActivity::onExit() {
  ActivityWithSubactivity::onExit();

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;

  files.clear();
}

//核心：修改loop，匹配这几个我需要的操作
void MyLibraryActivity::loop() {
  if (subActivity) {
      subActivity->loop();
      // if a search was requested while the keyboard was running, close it now
      if (pendingSearch) {
          // perform exit after keyboard loop returns to avoid self-delete
          exitActivity();
          doSearch(pendingKeyword.c_str());
          pendingSearch = false;
          updateRequired = true;
      }
      return;
  }
  // if the keyboard has already been dismissed but search still pending (edge case)
  if (pendingSearch) {
      doSearch(pendingKeyword.c_str());
      pendingSearch = false;
      updateRequired = true;
  }
  // Long press BACK (1s+) goes to root folder
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= GO_HOME_MS &&
      basepath != "/") {
    basepath = "/";
    loadFiles();
    selectorIndex = 0;
    updateRequired = true;
    return;
  }
 //新增开始
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
    // ===== 新增：统一获取选中项的完整路径 =====
    std::string selectedItem;    // 选中的项（文件名/完整路径）
    std::string fullPath;        // 最终要操作的完整路径
    bool hasValidSelection = false;

    if (isSearchMode) {
      // 搜索模式：直接取searchResults的选中项（本身就是完整路径）
      if (!searchResults.empty() && selectorIndex < searchResults.size()) {
        selectedItem = searchResults[selectorIndex];
        fullPath = selectedItem; // 搜索结果本身是完整路径
        hasValidSelection = true;
      }
    } else {
      // 普通模式：原有逻辑
      if (!files.empty() && selectorIndex < files.size()) {
        selectedItem = files[selectorIndex];
        fullPath = basepath;
        if (fullPath.back() != '/') fullPath += "/";
        fullPath += selectedItem;
        hasValidSelection = true;
      }
    }

    // 无有效选中项，直接返回
    if (!hasValidSelection) return;

    // ===== 原有逻辑全部改用 fullPath/selectedItem，不再用 files[selectorIndex] =====
    switch (topSelectorIndex) {
      case TopOption::OPEN: 
        if (isSearchMode) {
          // 搜索模式：直接打开（selectedItem是完整路径，且只有文件）
          onSelectBook(fullPath);
        } else {
          // 普通模式：修复嵌套目录路径拼接
          if (selectedItem.back() == '/') {
            // 正确拼接嵌套路径：basepath + "/" + 子目录名（去掉末尾的/）
            if (basepath != "/") {
              basepath += "/"; // 确保basepath以/结尾
            }
            basepath += selectedItem.substr(0, selectedItem.length() - 1);
            loadFiles(); // 重新加载当前嵌套目录的文件
            selectorIndex = 0;
          } else {
            onSelectBook(fullPath);
          }
        }
        updateRequired = true;
        return;

      case TopOption::DELETE: 
        if (mappedInput.getHeldTime() >= 500) {
          deleteFileOrDir(fullPath); // 改用统一的fullPath
          // 删除后刷新列表（区分模式）
          if (isSearchMode) {
            executeSearch(); // 搜索模式：重新搜索
          } else {
            loadFiles();     // 普通模式：重新加载
          }
        } else {
          Serial.printf("[删除] 需长按Confirm确认删除\n");
        }
        break;

      case TopOption::COPY: 
        copySourcePath = fullPath; // 改用统一的fullPath
        hasCopyData = true;
        isCutMode = false;
        Serial.printf("[复制] 已选中：%s\n", copySourcePath.c_str());
        break;

      case TopOption::CUT: 
        copySourcePath = fullPath; // 改用统一的fullPath
        hasCopyData = true;
        isCutMode = true;
        Serial.printf("[剪切] 已选中：%s（粘贴后将删除源文件）\n", copySourcePath.c_str());
        break;

      case TopOption::PASTE: 
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

        if (pasteSuccess && isCutMode) {
          Serial.printf("[剪切] 粘贴成功，删除源文件：%s\n", copySourcePath.c_str());
          deleteFileOrDir(copySourcePath);
          isCutMode = false;
        }

        hasCopyData = false;
        copySourcePath = "";
        // 粘贴后刷新列表（区分模式）
        if (isSearchMode) {
          executeSearch();
        } else {
          loadFiles();
        }
        break;
      }

      // case TopOption::SEARCH:
      //   if (!isSearchMode) {
      //     executeSearch();
      //   }
      //   break;
      // case TopOption::CANCEL_SEARCH:
      //   cancelSearch();
      //   break;
    }
    updateRequired = true;
    return;
  }





  //新增结束
  const bool upReleased = mappedInput.wasReleased(MappedInputManager::Button::Up);
  ;
  const bool downReleased = mappedInput.wasReleased(MappedInputManager::Button::Down);

  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, false);

  //把文件打开的逻辑放上面了
  //这里去掉了
  //后面没动

if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
  // Short press: go up one directory, or go home if at root
  if (mappedInput.getHeldTime() < GO_HOME_MS) {
    if (basepath != "/") {
      const std::string oldPath = basepath;

      // 修复：正确截取上级目录（处理嵌套路径）
      size_t lastSlash = basepath.find_last_of('/');
      // 避免截取后为空（比如 /dir1 → 截取后是 ""，要改成 "/"）
      basepath = (lastSlash == 0) ? "/" : basepath.substr(0, lastSlash);
      
      loadFiles(); // 重新加载上级目录内容

      // 修复：返回上级后定位到之前的目录项
      const std::string dirName = oldPath.substr(lastSlash + 1) + "/";
      selectorIndex = findEntry(dirName);

      updateRequired = true;
    } else {
      onGoHome();
    }
  }
}

  const auto& displayList = isSearchMode ? searchResults : files;
  int listSize = static_cast<int>(displayList.size());
  if (upReleased) {
    if (skipPage) {
      selectorIndex = std::max(static_cast<int>((selectorIndex / pageItems - 1) * pageItems), 0);
    } else {
      selectorIndex = (selectorIndex + listSize - 1) % listSize;
    }
    updateRequired = true;
  } else if (downReleased) {
    if (skipPage) {
      selectorIndex = std::min(static_cast<int>((selectorIndex / pageItems + 1) * pageItems), listSize - 1);
    } else {
      selectorIndex = (selectorIndex + 1) % listSize;
    }
    updateRequired = true;
  }
}



void MyLibraryActivity::displayTaskLoop() {
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

//添加四个按钮：删除、复制、剪切、粘贴
//添加搜索和取消搜索
void MyLibraryActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();
  auto folderName = basepath == "/" ? "SD card" : basepath.substr(basepath.rfind('/') + 1).c_str();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, folderName);
  //开始添加
  //constexpr const char* topItems[7] = {"打开", "删除", "复制", "剪切", "粘贴", "搜索", "取消搜索"};
  constexpr const char* topItems[5] = {"打开", "删除", "复制", "剪切", "粘贴"};
  constexpr int margin = 10;
  constexpr int menuSpacing = 5;
  const int menuTileWidth = (pageWidth - 2 * margin - 3 * menuSpacing) / 4;
  constexpr int menuTileHeight = 30;
  constexpr int topMenuY = 15;
 // 分页显示（一行3个）防止挡电源
  int startIdx = ((int)topSelectorIndex / 3) * 3;
  if (startIdx + 3 > 5) startIdx = 3;

  for (int i = 0; i < 3; i++) {
    int btnIdx = startIdx + i;
    if (btnIdx >= 5) break;
    
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
  //添加结束



  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  // 核心：根据是否搜索模式，选择要显示的列表（files 或 searchResults）
  const auto& displayList = isSearchMode ? searchResults : files;

  // 显示空列表提示（区分普通模式和搜索模式）
  if (displayList.empty()) {
      // 先定义提示文本的基础部分
      char emptyHint[128];
      // 拼接 "未找到含'关键词'的文件"
      snprintf(emptyHint, sizeof(emptyHint), "未找到含'%s'的文件", SEARCH_KEYWORD);
      // 赋值给emptyText
      std::string emptyText = isSearchMode ? emptyHint : "No books found";
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, emptyText.c_str());
  } else {
      // 绘制列表时，用 displayList 替代原来的 files
      GUI.drawList(
          renderer, Rect{0, contentTop, pageWidth, contentHeight}, displayList.size(), selectorIndex,
          [this](int index) { 
              // 这里返回当前模式下的列表项
              return isSearchMode ? searchResults[index] : files[index]; 
          }, nullptr, nullptr, nullptr);
  }
  //侧边绘制，防止有的用户问
  GUI.drawSideButtonHints(renderer, "向上", "向下");
  // Help text
  const auto labels = mappedInput.mapLabels("« 返回", "选择", "左选", "右选");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}



size_t MyLibraryActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return 0;
}