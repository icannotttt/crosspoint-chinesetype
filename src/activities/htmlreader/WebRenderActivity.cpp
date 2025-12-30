#include "WebRenderActivity.h"
#include "../network/WifiSelectionActivity.h"
#include "config.h"
#include <WiFi.h>
#include <string.h>
#include <../../lib/GfxRenderer/GfxRenderer.h>
//第二步需要的
#include <HTTPClient.h>


#include "WebRenderActivity.h"
#include "../network/WifiSelectionActivity.h"
#include "config.h"
#include <WiFi.h>
#include <string.h>
#include <../../lib/GfxRenderer/GfxRenderer.h>
//第二步需要的
#include <HTTPClient.h>

// 前置声明
class Activity;
void enterNewActivity(Activity* activity);
void exitActivity();

// 全局配置：要访问的测试网站（选小文本网站，避免内存溢出）
#define TEST_WEBSITE "https://blog.gdcba.cyou/test/"
#define MAX_HTTP_BUF 1024  // 限制网页内容大小，小内存优先

// 核心：WiFi连接成功后 → 显示IP → 访问网站 → 显示内容
void onWifiConnectComplete(WebRenderActivity* self, bool success) {
    if (!self || !success) {
        if (self) exitActivity();
        return;
    }

    GfxRenderer& renderer = self->renderer;
    renderer.clearScreen();
    renderer.displayBuffer();

    // 第一步：显示IP地址
    char ipText[128] = {0};
    IPAddress localIp = WiFi.localIP();
    snprintf(ipText, sizeof(ipText),
             "正在访问网站...",
             localIp[0], localIp[1], localIp[2], localIp[3]);
    int textX = (renderer.getScreenWidth() - renderer.getTextWidth(READER_FONT_ID, ipText)) / 2;
    renderer.drawText(READER_FONT_ID, textX, 20, ipText);
    renderer.displayBuffer();
    delay(1500);

    // 第二步：访问网站（极简HTTP请求，堆内存存储内容）
    char* webContent = (char*)malloc(MAX_HTTP_BUF);
    if (!webContent) {
        renderer.drawCenteredText(UI_FONT_ID, renderer.getScreenHeight()/2, "内存不足！");
        renderer.displayBuffer();
        delay(2000);
        free(webContent);
        exitActivity();
        return;
    }
    memset(webContent, 0, MAX_HTTP_BUF);

    // 发起HTTP GET请求
    HTTPClient http;
    http.begin(TEST_WEBSITE);
    http.setTimeout(5000); // 超时5秒
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
        // 修复：toCharArray无返回值，拆分两步获取内容
        String response = http.getString();
        response.toCharArray(webContent, MAX_HTTP_BUF - 1);
    } else {
        // 修复：sizeof(webContent) → MAX_HTTP_BUF（指针sizeof是4，会越界）
        snprintf(webContent, MAX_HTTP_BUF, "访问失败！\n错误码：%d", httpCode);
    }
    http.end(); // 释放HTTP资源

    // 第三步：显示网页内容（分行显示，适配中文+英文）
    renderer.clearScreen();
    // 标题（保留原有样式）
    renderer.drawText(READER_FONT_ID, 10, 10, "网页内容：");
    renderer.clearScreen();

    // ========== 新增：UTF-8字符解析函数（适配中文） ==========
    auto getUtf8Char = [&](const char* str, int offset, char* outChar, int& charWidth) -> int {
        if (offset >= strlen(str)) return 0; // 超出字符串长度
        
        unsigned char c = (unsigned char)str[offset];
        int bytes = 0;
        // 判断UTF-8字符字节数（适配中文3字节/英文1字节）
        if ((c & 0x80) == 0) {
            bytes = 1; // 单字节：英文/数字
        } else if ((c & 0xF0) == 0xE0) {
            bytes = 3; // 三字节：中文核心适配
        } else {
            bytes = 1; // 其他情况默认单字节（容错）
        }
        // 复制字符到缓冲区（保证中文完整）
        memcpy(outChar, str + offset, bytes);
        outChar[bytes] = '\0';
        // 获取字符实际显示宽度（适配中英文不同宽度）
        charWidth = 1.3*renderer.getTextWidth(READER_FONT_ID, outChar);
        return bytes;
    };

    // ========== 中文适配的内容绘制逻辑 ==========
    int lineY = 30;                  // 内容起始Y坐标（保留原有值）
    int currentX = 10;               // 当前绘制X坐标（左对齐）
    int maxLineWidth = renderer.getScreenWidth() - 20; // 一行最大宽度（左右留边）
    int charWidth = 0;               // 单个字符的显示宽度
    char utf8Char[5] = {0};          // 存储单个UTF-8字符（最多4字节+结束符）
    int i = 0;
    int strLen = strlen(webContent); // 网页内容总长度

    while (i < strLen && lineY < renderer.getScreenHeight() - 20) {
        // 提取单个UTF-8字符（自动区分中英文）
        int bytes = getUtf8Char(webContent, i, utf8Char, charWidth);
        if (bytes == 0) break;

        // 处理换行符（保留原有换行逻辑）
        if (strcmp(utf8Char, "\n") == 0) {
            lineY += 40; // 中文行高（比原20大，避免重叠）
            currentX = 10;
            i += bytes;
            continue;
        }

        // 超出一行宽度则换行（按实际宽度判断，而非字符数）
        if (currentX + charWidth > maxLineWidth) {
            lineY += 40;
            currentX = 10;
            // 超出屏幕高度则停止绘制
            if (lineY >= renderer.getScreenHeight() - 20) break;
        }

        // 绘制单个字符（支持中文，无需固定8像素偏移）
        renderer.drawText(READER_FONT_ID, currentX, lineY, utf8Char);
        currentX += charWidth; // 累加实际字符宽度
        i += bytes;            // 跳过当前字符的字节数（中文3字节/英文1字节）
    }

    // 刷新显示（保留原有逻辑）
    renderer.displayBuffer();

    // 第四步：显示5秒后返回主页
    delay(5000);
    free(webContent); // 释放堆内存
    exitActivity();
}

// 核心逻辑：先跳转到WiFi选择页面
void WebRenderActivity::onEnter() {
    Activity::onEnter();

    // 清屏+提示
    renderer.clearScreen();
    renderer.drawCenteredText(READER_FONT_ID, renderer.getScreenHeight()/2, "正在进入WiFi配置页面...");
    renderer.displayBuffer();
    delay(1000);

    // 跳转到WiFi选择页面
    auto* wifiActivity = new WifiSelectionActivity(
        renderer,
        inputManager,
        std::bind(&onWifiConnectComplete, this, std::placeholders::_1)
    );
    enterNewActivity(wifiActivity);
}

// 空实现（避免编译报错）
int WebRenderActivity::fetchWebContent(const char* url, char* buffer, int maxLen) {
    if (!buffer || maxLen < 128) return -1;
    
    HTTPClient http;
    http.begin(url);
    int httpCode = http.GET();
    int readLen = 0;
    if (httpCode == HTTP_CODE_OK) {
        // 修复：toCharArray无返回值，拆分两步
        String response = http.getString();
        response.toCharArray(buffer, maxLen - 1);
        readLen = response.length(); // 正确获取长度
        buffer[readLen] = '\0';
    }
    http.end();
    return readLen;
}

void WebRenderActivity::cleanHtmlToText(const char* html, char* output, int maxOutLen) {
    if (!html || !output || maxOutLen < 64) return;
    
    // 极简HTML清洗：只保留文本，去掉标签（小内存版）
    int outIdx = 0;
    bool inTag = false;
    for (int i = 0; html[i] != '\0' && outIdx < maxOutLen - 1; i++) {
        if (html[i] == '<') {
            inTag = true;
            continue;
        }
        if (html[i] == '>') {
            inTag = false;
            continue;
        }
        if (!inTag) {
            output[outIdx++] = html[i];
        }
    }
    output[outIdx] = '\0';
}

void WebRenderActivity::renderTextToScreen(const char* text) {
    if (!text) return;
    
    renderer.clearScreen();
    int lineY = 20;
    int lineCharCount = 0;
    for (int i = 0; text[i] != '\0' && lineY < renderer.getScreenHeight() - 20; i++) {
        if (lineCharCount >= 30 || text[i] == '\n') {
            lineY += 20;
            lineCharCount = 0;
            if (text[i] == '\n') continue;
        }
        char singleChar[2] = {text[i], '\0'};
        renderer.drawText(UI_FONT_ID, 10 + (lineCharCount * 8), lineY, singleChar);
        lineCharCount++;
    }
    renderer.displayBuffer();
}

void WebRenderActivity::renderPopup(const char* message) const {
    if (!message) return;
    renderer.clearScreen();
    const int x = (renderer.getScreenWidth() - renderer.getTextWidth(READER_FONT_ID, message)) / 2;
    const int y = renderer.getScreenHeight() / 2;
    renderer.drawText(READER_FONT_ID, x, y, message);
    renderer.displayBuffer();
}

void WebRenderActivity::onWifiConnected(bool success) {
    return;
}