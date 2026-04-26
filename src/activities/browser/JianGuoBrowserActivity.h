#ifndef JIANGUOBROWSERACTIVITY_H
#define JIANGUOBROWSERACTIVITY_H

#include "../ActivityWithSubactivity.h"
#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include <vector>
#include <string>
#include <WiFiClientSecure.h>

// 提前声明WebDAVEntry结构体
struct WebDAVEntry {
    std::string title;    // 文件名/文件夹名
    std::string path;     // 相对路径
    enum Type { FOLDER, BOOK_FILE } type; // 简化类型定义
    std::string sourceFolder;  // 新增：标记来源文件夹 ("jg" 或 "legado")
};

class JianGuoBrowserActivity : public ActivityWithSubactivity {
public:
    enum class BrowserState {
        CHECK_WIFI,
        WIFI_SELECTION,
        LOADING,
        BROWSING,
        DOWNLOADING,
        ERROR
    };

    // 修复：构造函数添加name参数（适配ActivityWithSubactivity）
    JianGuoBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                   const std::function<void()>& onGoHome)
      : ActivityWithSubactivity("JianGuoBrowser", renderer, mappedInput), onGoHome(onGoHome) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

private:
    static void taskTrampoline(void* param);
    void displayTaskLoop();
    void render() const;
    void fetchFeed(const std::string& path);
    void navigateToEntry(const WebDAVEntry& entry);
    void navigateBack();
    void downloadBook(const WebDAVEntry& book);
    void checkAndConnectWifi();
    void launchWifiSelection();
    void onWifiSelectionComplete(const bool connected);
    void parseXmlEntries(const String& xmlResult, 
                                              const std::string& basePath,
                                              const std::string& sourceFolder,
                                              std::vector<WebDAVEntry>& outEntries);

    static bool endsWith(const std::string& str, const std::string& suffix); 

    const std::function<void()> onGoHome;

    // 成员变量声明
    static std::vector<WebDAVEntry> entries;
    static std::string currentPath;
    static std::vector<std::string> navigationHistory;
    static int selectorIndex;
    static std::string errorMessage;
    static std::string statusMessage;
    size_t downloadProgress;
    size_t downloadTotal;
    static BrowserState state;
    static SemaphoreHandle_t renderingMutex;
    static TaskHandle_t displayTaskHandle;
    static bool updateRequired;



};

#endif // JIANGUOBROWSERACTIVITY_H