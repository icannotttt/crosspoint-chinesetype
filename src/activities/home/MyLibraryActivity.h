#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "../ActivityWithSubactivity.h"

class MyLibraryActivity final : public ActivityWithSubactivity {
 private:

  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;

  size_t selectorIndex = 0;
  bool updateRequired = false;

  // Files state
  std::string basepath = "/";
  std::vector<std::string> files;

  // Callbacks
  const std::function<void(const std::string& path)> onSelectBook;
  const std::function<void()> onGoHome;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;

  // Data loading
  void loadFiles();
  size_t findEntry(const std::string& name) const;

  //文件管理
  std::string copySourcePath;  // 待复制的源路径
  bool hasCopyData = false;    // 是否有待复制内容
  bool isCutMode = false;  
  //搜索模式
  bool isSearchMode = false;
  std::vector<std::string> searchResults; //搜索结果
  std::string originalBasePath; // 进入搜索前的路径

  // pending search request from keyboard callback; handled in loop()
  bool pendingSearch = false;
  std::string pendingKeyword;

  void executeSearch();
  void cancelSearch();
  static void sortFileList(std::vector<std::string>& strs); // 新增静态成员函数声明
  void doSearch(const char* keyword); // 执行搜索（接收char*)

  // ✅ 6个顶部选项枚举
  enum class TopOption { 
      OPEN = 0,        // 打开
      DELETE = 1,      // 删除
      COPY = 2,        // 复制
      CUT = 3,         // 剪切
      PASTE = 4
      // ,        // 粘贴
      // SEARCH = 5,        // 搜索
      // CANCEL_SEARCH = 6   // 取消搜索
  };
  TopOption topSelectorIndex = TopOption::OPEN;
  const int topOptionCount = 7;
  char SEARCH_KEYWORD[100] = "赛博"; // 搜索关键词（示例：包含“赛博”的文件）


 public:
  explicit MyLibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                             const std::function<void()>& onGoHome,
                             const std::function<void(const std::string& path)>& onSelectBook,
                             std::string initialPath = "/")
      : ActivityWithSubactivity("MyLibrary", renderer, mappedInput),
        basepath(initialPath.empty() ? "/" : std::move(initialPath)),
        onSelectBook(onSelectBook),
        onGoHome(onGoHome) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
