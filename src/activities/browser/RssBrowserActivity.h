#ifndef RSSBROWSERACTIVITY_H
#define RSSBROWSERACTIVITY_H

#include "../ActivityWithSubactivity.h"
#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include <vector>
#include <string>
#include <WiFiClientSecure.h>

struct RssItem {
    std::string title;
    std::string description;
    std::string link;
    std::string journal;
    std::string volume;
    std::string number;
};

class RssBrowserActivity : public ActivityWithSubactivity {
public:
    enum class BrowserState {
        CHECK_WIFI,
        WIFI_SELECTION,
        LOADING,
        BROWSING,
        DETAIL,
        ERROR
    };

    RssBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                       const std::function<void()>& onGoHome)
        : ActivityWithSubactivity("RssBrowser", renderer, mappedInput),
          onGoHome(onGoHome) {}

    void onEnter() override;
    void onExit() override;
    void loop() override;

private:
    static void taskTrampoline(void* param);
    void displayTaskLoop();
    void render() const;
    void fetchRss();
    void checkAndConnectWifi();
    void launchWifiSelection();
    void onWifiSelectionComplete(const bool connected);
    void showArticleDetail(const RssItem& item);

    static bool endsWith(const std::string& str, const std::string& suffix);

    const std::function<void()> onGoHome;

    static std::vector<RssItem> items;
    static int selectorIndex;
    static std::string errorMessage;
    static std::string statusMessage;
    static BrowserState state;
    static SemaphoreHandle_t renderingMutex;
    static TaskHandle_t displayTaskHandle;
    static bool updateRequired;
};

#endif