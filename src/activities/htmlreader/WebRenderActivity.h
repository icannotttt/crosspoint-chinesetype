#pragma once
#include "../Activity.h"
#include "../network/WifiSelectionActivity.h"
#include <functional>



// 内存配置（保持最小）
#define MAX_WEB_CONTENT_LEN  1024
#define MAX_TEXT_LINE_LEN    64

class WebRenderActivity final : public Activity {
public:
    // 关键修复：引用成员必须在初始化列表中初始化
    explicit WebRenderActivity(GfxRenderer& renderer, InputManager& inputManager)
        : Activity("WebRender", renderer, inputManager), // 先初始化基类
          renderer(renderer), // 初始化子类的renderer引用
          inputManager(inputManager) {} // 初始化子类的inputManager引用

    void onEnter() override;

    // 公开成员（供回调访问）
    GfxRenderer& renderer;
    InputManager& inputManager;

private:
    // 空实现函数声明
    int fetchWebContent(const char* url, char* buffer, int maxLen);
    void cleanHtmlToText(const char* html, char* output, int maxOutLen);
    void renderTextToScreen(const char* text);
    void renderPopup(const char* message) const;
    void onWifiConnected(bool success);

    // 声明回调函数为友元
    friend void onWifiConnectComplete(WebRenderActivity* self, bool success);
    const std::function<void()> onGoBack;
};