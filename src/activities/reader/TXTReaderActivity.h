#pragma once
#include <memory>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// 修正头文件路径（根据项目实际结构调整，若ActivityWithSubactivity.h在activities目录下）
#include "../ActivityWithSubactivity.h"
// 确保Txt.h路径正确（假设在lib/TXT/下）
#include "../lib/TXT/TXT.h"          

// 1. 提前定义命名空间 + Section类（避免重定义）
namespace TXTReaderNS {
class Section {
public:
  int currentPage = 0;
  uint32_t pageCount = 0;
  
  std::shared_ptr<Txt> txt;

  Section(std::shared_ptr<Txt> txtPtr) : txt(txtPtr) {
    pageCount = txt->getPageCount();
  }


  

  void clearCache() {}
};
} // namespace TXTReaderNS

class TXTReaderActivity final : public ActivityWithSubactivity {
  // 成员变量（全部保留，仅修正Section类型）
  std::shared_ptr<Txt> txt;                         
  // 3. 使用命名空间的Section（核心修复）
  std::unique_ptr<TXTReaderNS::Section> section = nullptr;        
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  int currentChapterIndex = 0;                      
  int nextPageNumber = 0;
  int pagesUntilFullRefresh = 0;
  uint32_t beginbype=0; //动态分配
    // 2. 从txt读取
  std::unique_ptr<std::string> loadPageFromSectionFile() {
    // 
    std::string* content = new std::string(txt->getPage(beginbype));
    return std::unique_ptr<std::string>(content);
  }
  bool updateRequired = false;
  const std::function<void()> onGoBack;
  const std::function<void()> onGoHome;
  bool saveScreenToBMP(const char* filename); 

  // 函数声明（保留）
  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();
  void renderContents(std::unique_ptr<std::string> pageContent, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar(int orientedMarginRight, int orientedMarginBottom, int orientedMarginLeft) const;
  void renderpage(std::unique_ptr<std::string>& pageContent,  // 改为引用，避免所有权转移
                                    const int orientedMarginTop,
                                    const int orientedMarginRight,
                                    const int orientedMarginBottom,
                                    const int orientedMarginLeft,
                                    bool isForGrayscale /* 新增：标记是否为灰度渲染，避免累加beginbype */) ;



 public:
  
  // 构造函数（保留）
  explicit TXTReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt,
                              const std::function<void()>& onGoBack, const std::function<void()>& onGoHome)
      : ActivityWithSubactivity("TXTReader", renderer, mappedInput),
        txt(std::move(txt)),
        onGoBack(onGoBack),
        onGoHome(onGoHome) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void saveProgress();
  void loadProgress();
  void savePage();
  void loadPage();
};