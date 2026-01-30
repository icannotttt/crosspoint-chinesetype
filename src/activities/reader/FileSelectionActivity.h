#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>



#include "../Activity.h"

class FileSelectionActivity final : public Activity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  std::string basepath = "/";
  std::vector<std::string> files;
  int selectorIndex = 0;
  bool updateRequired = false;
  const std::function<void(const std::string&)> onSelect;
  const std::function<void()> onGoHome;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  void loadFiles();

  // ✅ 新增：复制粘贴状态（仅占几十字节内存）
  std::string copySourcePath;  // 待复制的源路径
  bool hasCopyData = false;    // 是否有待复制内容
      // ✅ 新增：剪切标记
  bool isCutMode = false;  

  // ✅ 6个顶部选项枚举
  enum class TopOption { 
      OPEN = 0,        // 打开
      DELETE = 1,      // 删除
      COPY = 2,        // 复制
      CUT = 3,         // 剪切
      PASTE = 4,       // 粘贴
      SORT_DESC = 5,   // 倒序
      SORT_ASC = 6     // 正序
  };
  TopOption topSelectorIndex = TopOption::OPEN;
  const int topOptionCount = 7; // 按钮总数改为7

  // 新增按名称排序
  void sortFileListByName(std::vector<std::string>& strs);
  void drawDashedLine(GfxRenderer& renderer, int x1, int y, int x2, bool isDark) const;




 public:
  explicit FileSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 const std::function<void(const std::string&)>& onSelect,
                                 const std::function<void()>& onGoHome, std::string initialPath = "/")
      : Activity("FileSelection", renderer, mappedInput),
        basepath(initialPath.empty() ? "/" : std::move(initialPath)),
        onSelect(onSelect),
        onGoHome(onGoHome) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
