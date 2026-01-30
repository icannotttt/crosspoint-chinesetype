#include "ReaderActivity.h"

// 1. 新增TXT头文件依赖（核心修改）
#include "Txt.h"                  // 新增的Txt类头文件
#include "Epub.h"
#include "EpubReaderActivity.h"
#include "FileSelectionActivity.h"
#include "Xtc.h"
#include "XtcReaderActivity.h"
// 新增TXTReaderActivity头文件（关键）
#include "TXTReaderActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "CrossPointSettings.h"
//
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "fontIds.h"
#include <GfxRenderer.h>
#include <MappedInputManager.h>




std::string ReaderActivity::extractFolderPath(const std::string& filePath) {
  const auto lastSlash = filePath.find_last_of('/');
  if (lastSlash == std::string::npos || lastSlash == 0) {
    return "/";
  }
  return filePath.substr(0, lastSlash);
}

bool ReaderActivity::isXtcFile(const std::string& path) {
  if (path.length() < 4) return false;
  std::string ext4 = path.substr(path.length() - 4);
  if (ext4 == ".xtc") return true;
  if (path.length() >= 5) {
    std::string ext5 = path.substr(path.length() - 5);
    if (ext5 == ".xtch") return true;
  }
  return false;
}

// 2. 修正TXT文件识别函数命名（规范：isTxtFile，首字母大写）
bool ReaderActivity::isTxtFile(const std::string& path) {
  if (path.length() < 4) return false;
  std::string ext = path.substr(path.length() - 4);
  // 统一转小写（兼容大写后缀如.TXT）
  for (auto& c : ext) c = tolower(c);
  if (ext == ".txt") return true;
  return false;
}
//新增bmp
bool ReaderActivity::isBmpFile(const std::string& path) {
  if (path.length() < 4) return false;
  std::string ext = path.substr(path.length() - 4);
  for (auto& c : ext) c = tolower(c);
  return ext == ".bmp";
}

//新增jpg
bool ReaderActivity::isjpgFile(const std::string& path) {
  if (path.length() < 4) return false;
  std::string ext = path.substr(path.length() - 4);
  for (auto& c : ext) c = tolower(c);
  return ext == ".jpg";
}

//新增Png
bool ReaderActivity::isPngFile(const std::string& path) {
  if (path.length() < 7) return false;
  std::string ext = path.substr(path.length() - 7);
  for (auto& c : ext) c = tolower(c);
  return ext == ".pngtxt";
}

// 3. 重写loadtxt函数：返回Txt类型而非Epub（核心修改）
std::unique_ptr<Txt> ReaderActivity::loadTxt(const std::string& path) {
  if (!SdMan.exists(path.c_str())) {
    Serial.printf("[%lu] [TXT] File does not exist: %s\n", millis(), path.c_str());
    return nullptr;
  }

  // 创建Txt实例（替换Epub）
  auto txt = std::unique_ptr<Txt>(new Txt(path, "/.crosspoint",SETTINGS.getReaderFontId()));
  // 配置屏幕尺寸（需和阅读器保持一致，此处示例800x600，可从SETTINGS读取）
  //txt->setScreenSize(renderer.getScreenWidth(), renderer.getScreenHeight());
  if (txt->load()) {
    Serial.printf("[%lu] [TXT] Loaded successfully: %s\n", millis(), path.c_str());
    return txt;
  }

  Serial.printf("[%lu] [TXT] Failed to load TXT\n", millis());
  return nullptr;
}



std::unique_ptr<Epub> ReaderActivity::loadEpub(const std::string& path) {
  if (!SdMan.exists(path.c_str())) {
    Serial.printf("[%lu] [EPUB] File does not exist: %s\n", millis(), path.c_str());
    return nullptr;
  }

  auto epub = std::unique_ptr<Epub>(new Epub(path, "/.crosspoint"));
  if (epub->load()) {
    return epub;
  }

  Serial.printf("[%lu] [EPUB] Failed to load epub\n", millis());
  return nullptr;
}

std::unique_ptr<Xtc> ReaderActivity::loadXtc(const std::string& path) {
  if (!SdMan.exists(path.c_str())) {
    Serial.printf("[%lu] [XTC] File does not exist: %s\n", millis(), path.c_str());
    return nullptr;
  }

  auto xtc = std::unique_ptr<Xtc>(new Xtc(path, "/.crosspoint"));
  if (xtc->load()) {
    return xtc;
  }

  Serial.printf("[%lu] [XTC] Failed to load XTC\n", millis());
  return nullptr;
}



void ReaderActivity::onSelectBookFile(const std::string& path) {
  currentBookPath = path;  // Track current book path
  exitActivity();
  enterNewActivity(new FullScreenMessageActivity(renderer, mappedInput, "Loading..."));

  if (isXtcFile(path)) {
    // Load XTC file
    auto xtc = loadXtc(path);
    if (xtc) {
      onGoToXtcReader(std::move(xtc));
    } else {
      exitActivity();
      enterNewActivity(new FullScreenMessageActivity(renderer, mappedInput, "Failed to load XTC", EpdFontFamily::REGULAR,
                                                     EInkDisplay::HALF_REFRESH));
      delay(2000);
      onGoToFileSelection();
    }
  } 
  // 4. 修正TXT分支逻辑（核心修改）
  else if(isTxtFile(path)) {
    auto txt = loadTxt(path);  // 调用修正后的loadTxt函数
    if (txt) {
      onGoToTxtReader(std::move(txt));  // 调用TXT专属的跳转函数
    } else {
      exitActivity();
      enterNewActivity(new FullScreenMessageActivity(renderer, mappedInput, "Failed to load TXT", EpdFontFamily::REGULAR,
                                                     EInkDisplay::HALF_REFRESH));
      delay(2000);
      onGoToFileSelection();
    }
  }
  else {
    auto epub = loadEpub(path);
    if (epub) {
      onGoToEpubReader(std::move(epub));
    } else {
      exitActivity();
      enterNewActivity(new FullScreenMessageActivity(renderer, mappedInput, "Failed to load epub", EpdFontFamily::REGULAR,
                                                     EInkDisplay::HALF_REFRESH));
      delay(2000);
      onGoToFileSelection();
    }
  }
}

void ReaderActivity::onGoToFileSelection(const std::string& fromBookPath) {
  exitActivity();
  // If coming from a book, start in that book's folder; otherwise start from root
  const auto initialPath = fromBookPath.empty() ? "/" : extractFolderPath(fromBookPath);
  enterNewActivity(new FileSelectionActivity(
      renderer, mappedInput, [this](const std::string& path) { onSelectBookFile(path); }, onGoBack, initialPath));
}

void ReaderActivity::onGoToEpubReader(std::unique_ptr<Epub> epub) {
  const auto epubPath = epub->getPath();
  currentBookPath = epubPath;
  exitActivity();
  enterNewActivity(new EpubReaderActivity(
      renderer, mappedInput, std::move(epub), [this, epubPath] { onGoToFileSelection(epubPath); },
      [this] { onGoBack(); }));
}

void ReaderActivity::onGoToXtcReader(std::unique_ptr<Xtc> xtc) {
  const auto xtcPath = xtc->getPath();
  currentBookPath = xtcPath;
  exitActivity();
  enterNewActivity(new XtcReaderActivity(
      renderer, mappedInput, std::move(xtc), [this, xtcPath] { onGoToFileSelection(xtcPath); },
      [this] { onGoBack(); }));
}



// 5. 重写onGoToTxtReader函数（核心修改）
void ReaderActivity::onGoToTxtReader(std::unique_ptr<Txt> txt) {
  const auto txtPath = txt->getPath();
  currentBookPath = txtPath;
  exitActivity();
  // 创建TXTReaderActivity（替换EpubReaderActivity）
  enterNewActivity(new TXTReaderActivity(
      renderer, mappedInput, std::move(txt), 
      [this, txtPath] { onGoToFileSelection(txtPath); },  // 返回文件选择的回调
      [this] { onGoBack(); }));                           // 返回首页的回调
}

void ReaderActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  if (initialBookPath.empty()) {
    onGoToFileSelection();  // Start from root when entering via Browse
    return;
  }

  currentBookPath = initialBookPath;

  // 6. 补充onEnter中的TXT分支（核心修改）
  if (isXtcFile(initialBookPath)) {
    auto xtc = loadXtc(initialBookPath);
    if (!xtc) {
      onGoBack();
      return;
    }
    onGoToXtcReader(std::move(xtc));
  } else if (isTxtFile(initialBookPath)) {  // 新增TXT判断
    auto txt = loadTxt(initialBookPath);
    if (!txt) {
      onGoBack();
      return;
    }
    onGoToTxtReader(std::move(txt));
  }
  else {
    auto epub = loadEpub(initialBookPath);
    if (!epub) {
      onGoBack();
      return;
    }
    onGoToEpubReader(std::move(epub));
  }
}