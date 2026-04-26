#pragma once
#include <Epub.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <memory>

#include "KOReaderSyncClient.h"
#include "ProgressMapper.h"
#include "activities/ActivityWithSubactivity.h"

class JianGuoSyncActivity final : public ActivityWithSubactivity {
private:
  const std::shared_ptr<Epub> epub;
  const std::string epubPath;
  const int currentSpineIndex;
  const std::function<void()> onGoBack;
  const std::function<void(uint32_t newChapter)> onSelectChapter;
  
  // 新增：同步状态
  enum class BrowserState {
      CHECK_WIFI,
      WIFI_SELECTION,
      DOWNLOADING,
      SHOWING_RESULT,  // 新增：显示选项
      UPLOADING,       // 新增：上传中
      COMPLETE,
      ERROR
  };

  // 新增：成员变量
  int selectedOption = 0;          // 0=下载，1=上传
  std::string matchedFileName = ""; // 匹配的文件名
  std::string matchedFileUrl = "";  // 匹配的文件完整 URL（已编码）

  void displayTaskLoop();
  void render() const;
  
  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(const bool connected);
  
  void downloadProgressFile();
  void uploadProgressFile();  // 新增：上传函数
  void parseAndCleanup();

  static void taskTrampoline(void* param);

  // 静态成员
  static SemaphoreHandle_t renderingMutex;
  static TaskHandle_t displayTaskHandle;
  static bool updateRequired;
  static BrowserState state;
  static std::string errorMessage;
  static std::string statusMessage;
  static size_t downloadProgress;
  static size_t downloadTotal;
  static int lastExtractedChapterIndex;
  static std::string lastExtractedBookName;
  static std::string lastExtractedChapterTitle; 
  bool skipFirstButtonCheck = false;  // 新增：跳过初始残留按键

  // 极简版：Spine → TOC 校准
int getTocIndexFromSpine(int spineIndex) {
    return epub->getTocIndexForSpineIndex(spineIndex); // 直接复用KOReader同款方法
}

// 极简版：TOC → Spine 校准
int getSpineIndexFromToc(int tocIndex) {
    if (tocIndex >= 0 && tocIndex < epub->getTocItemsCount()) {
        return epub->getTocItem(tocIndex).spineIndex; // 和KOReader映射逻辑完全一致
    }
    return -1;
}
 
 public:
  using OnCancelCallback = std::function<void()>;
  using OnSyncCompleteCallback = std::function<void(int newSpineIndex, int newPageNumber)>;

  explicit JianGuoSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                const std::shared_ptr<Epub>& epub, const std::string& epubPath,
                                int currentSpineIndex,
                                const std::function<void()>& onGoBack, 
                                const std::function<void(uint32_t newChapter)>& onSelectChapter)
      : ActivityWithSubactivity("JianGuoSync", renderer, mappedInput), 
      epub(epub),
      epubPath(epubPath), 
      currentSpineIndex(currentSpineIndex), 
      onGoBack(onGoBack),
      onSelectChapter(onSelectChapter),
      selectedOption(0),
      matchedFileName("") {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
};