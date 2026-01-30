#pragma once
#include <memory>

#include "../ActivityWithSubactivity.h"

// 1. 新增Txt前向声明（核心修改）
class Epub;
class Xtc;
class Txt;  // 声明Txt类，避免编译错误

class ReaderActivity final : public ActivityWithSubactivity {
  std::string initialBookPath;
  std::string currentBookPath;  // Track current book path for navigation
  const std::function<void()> onGoBack;
  
  // 原有EPUB/XTC函数声明（保留）
  static std::unique_ptr<Epub> loadEpub(const std::string& path);
  static std::unique_ptr<Xtc> loadXtc(const std::string& path);

  
  // 2. 修正TXT相关函数声明（核心修改）
  static std::unique_ptr<Txt> loadTxt(const std::string& path);  // 替换原loadtxt，返回Txt类型
  static bool isXtcFile(const std::string& path);
  static bool isTxtFile(const std::string& path);  // 修正命名：istxtFile -> isTxtFile
  bool isBmpFile(const std::string& path);
  bool isjpgFile(const std::string& path);
  bool isPngFile(const std::string& path);

  // 原有工具函数（保留）
  static std::string extractFolderPath(const std::string& filePath);
  
  // 原有回调函数（保留）
  void onSelectBookFile(const std::string& path);
  void onGoToFileSelection(const std::string& fromBookPath = "");

  // 原有阅读器跳转函数（保留）
  void onGoToEpubReader(std::unique_ptr<Epub> epub);
  void onGoToXtcReader(std::unique_ptr<Xtc> xtc);
 

  // 3. 修正TXT阅读器跳转函数（核心修改）
  void onGoToTxtReader(std::unique_ptr<Txt> txt);  // 替换原onGoTotxtReader，参数为Txt类型


 public:
  explicit ReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialBookPath,
                          const std::function<void()>& onGoBack)
      : ActivityWithSubactivity("Reader", renderer, mappedInput),
        initialBookPath(std::move(initialBookPath)),
        onGoBack(onGoBack) {}
  void onEnter() override;
};