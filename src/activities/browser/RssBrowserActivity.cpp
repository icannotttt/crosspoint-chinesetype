#include "RssBrowserActivity.h"

#include <GfxRenderer.h>
#include <HardwareSerial.h>
#include <WiFi.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

#include <HTTPClient.h>
#include <expat.h>
#include "util/StringUtils.h"

#include <cstring>
#include <vector>
#include <string>

namespace {
constexpr unsigned long goHomeMs = 500;
constexpr int PAGE_ITEMS = 50;
constexpr int SKIP_PAGE_MS = 700;

static std::vector<RssItem> rssItems;
static bool inItem = false;
static std::string currentTag;
static std::string currentText;

// 替换为兼容版本：不用 StringUtils::replaceAll
void replaceHtmlEntities(std::string& s) {
    size_t pos;
    while ((pos = s.find("&lt;")) != std::string::npos) s.replace(pos, 4, "<");
    while ((pos = s.find("&gt;")) != std::string::npos) s.replace(pos, 4, ">");
    while ((pos = s.find("&amp;")) != std::string::npos) s.replace(pos, 5, "&");
}

std::string stripHtml(const std::string& html) {
    std::string res;
    bool inTag = false;
    for (char c : html) {
        if (c == '<') { inTag = true; }
        else if (c == '>') { inTag = false; }
        else if (!inTag) { res += c; }
    }
    replaceHtmlEntities(res);
    return res;
}

static void XMLCALL startElement(void*, const char* el, const char**) {
    currentTag = el;
    currentText.clear();
    if (!strcmp(el, "item")) {
        inItem = true;
        rssItems.emplace_back();
    }
}

static void XMLCALL endElement(void*, const char* el) {
    if (!inItem || rssItems.empty()) return;
    auto& item = rssItems.back();
    std::string t = stripHtml(currentText);

    if (!strcmp(el, "title")) item.title = t;
    else if (!strcmp(el, "description")) item.description = t;
    else if (!strcmp(el, "link")) item.link = t;
    else if (!strcmp(el, "prism:publicationName")) item.journal = t;
    else if (!strcmp(el, "prism:volume")) item.volume = t;
    else if (!strcmp(el, "prism:number")) item.number = t;
    else if (!strcmp(el, "item")) inItem = false;
}

static void XMLCALL charData(void*, const char* text, int len) {
    if (inItem && !currentTag.empty()) currentText.append(text, len);
}
}

std::vector<RssItem> RssBrowserActivity::items;
int RssBrowserActivity::selectorIndex = 0;
std::string RssBrowserActivity::errorMessage;
std::string RssBrowserActivity::statusMessage;
RssBrowserActivity::BrowserState RssBrowserActivity::state;
SemaphoreHandle_t RssBrowserActivity::renderingMutex = nullptr;
TaskHandle_t RssBrowserActivity::displayTaskHandle = nullptr;
bool RssBrowserActivity::updateRequired = false;

bool RssBrowserActivity::endsWith(const std::string& str, const std::string& suffix) {
    if (str.size() < suffix.size()) return false;
    return str.substr(str.size() - suffix.size()) == suffix;
}

void RssBrowserActivity::taskTrampoline(void* param) {
    auto* self = (RssBrowserActivity*)param;
    self->displayTaskLoop();
}

void RssBrowserActivity::onEnter() {
    ActivityWithSubactivity::onEnter();
    renderingMutex = xSemaphoreCreateMutex();
    state = BrowserState::CHECK_WIFI;
    items.clear();
    selectorIndex = 0;
    errorMessage.clear();
    statusMessage = "检查WiFi...";
    updateRequired = true;

    xTaskCreate(taskTrampoline, "RssBrowserTask", 4096, this, 1, &displayTaskHandle);
    checkAndConnectWifi();
}

void RssBrowserActivity::onExit() {
    ActivityWithSubactivity::onExit();
    WiFi.mode(WIFI_OFF);

    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    if (displayTaskHandle) {
        vTaskDelete(displayTaskHandle);
        displayTaskHandle = nullptr;
    }
    vSemaphoreDelete(renderingMutex);
    renderingMutex = nullptr;
    items.clear();
}

void RssBrowserActivity::loop() {
    if (state == BrowserState::WIFI_SELECTION) {
        ActivityWithSubactivity::loop();
        return;
    }

    if (state == BrowserState::ERROR) {
        if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= goHomeMs) {
            if (WiFi.status() == WL_CONNECTED) {
                state = BrowserState::LOADING;
                statusMessage = "加载中...";
                updateRequired = true;
                fetchRss();
            } else launchWifiSelection();
        } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
            onGoHome();
        }
        return;
    }

    if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
        if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
            onGoHome();
        }
        return;
    }

    if (state == BrowserState::DETAIL) {
        if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
            state = BrowserState::BROWSING;
            updateRequired = true;
        }
        return;
    }

    if (state == BrowserState::BROWSING) {
        bool prev = mappedInput.wasReleased(MappedInputManager::Button::Up) || mappedInput.wasReleased(MappedInputManager::Button::Left);
        bool next = mappedInput.wasReleased(MappedInputManager::Button::Down) || mappedInput.wasReleased(MappedInputManager::Button::Right);

        if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
            if (!items.empty()) {
                state = BrowserState::DETAIL;
                updateRequired = true;
            }
        } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
            onGoHome();
        } else if (prev && !items.empty()) {
            // 上一个（循环）
            selectorIndex = (selectorIndex - 1 + items.size()) % items.size();
            updateRequired = true;
        } else if (next && !items.empty()) {
            // 下一个（循环）
            selectorIndex = (selectorIndex + 1) % items.size();
            updateRequired = true;
        }
    }
}

void RssBrowserActivity::displayTaskLoop() {
    while (true) {
        if (updateRequired) {
            updateRequired = false;
            xSemaphoreTake(renderingMutex, portMAX_DELAY);
            render();
            xSemaphoreGive(renderingMutex);
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void RssBrowserActivity::render() const {
    renderer.clearScreen();
    int w = renderer.getScreenWidth();
    int h = renderer.getScreenHeight();

    renderer.drawCenteredText(UI_12_FONT_ID, 15, "物理学报 RSS", true, EpdFontFamily::BOLD);

    if (state == BrowserState::CHECK_WIFI) {
        renderer.drawCenteredText(UI_10_FONT_ID, h/2, statusMessage.c_str());
        auto l = mappedInput.mapLabels("返回", "", "", "");
        GUI.drawButtonHints(renderer, l.btn1, l.btn2, l.btn3, l.btn4);
        renderer.displayBuffer();
        return;
    }

    if (state == BrowserState::LOADING) {
        renderer.drawCenteredText(UI_10_FONT_ID, h/2, statusMessage.c_str());
        auto l = mappedInput.mapLabels("返回", "", "", "");
        GUI.drawButtonHints(renderer, l.btn1, l.btn2, l.btn3, l.btn4);
        renderer.displayBuffer();
        return;
    }

    if (state == BrowserState::ERROR) {
        renderer.drawCenteredText(UI_10_FONT_ID, h/2-20, "加载失败");
        renderer.drawCenteredText(UI_10_FONT_ID, h/2+10, errorMessage.c_str());
        auto l = mappedInput.mapLabels("返回", "重试", "", "");
        GUI.drawButtonHints(renderer, l.btn1, l.btn2, l.btn3, l.btn4);
        renderer.displayBuffer();
        return;
    }

    if (state == BrowserState::DETAIL) {
        if (!items.empty() && selectorIndex < items.size()) {
            const auto& it = items[selectorIndex];
            int y = 40;

            renderer.drawText(UI_10_FONT_ID, 20, y, "标题:", false);
            y += 20;
            renderer.drawText(UI_10_FONT_ID, 20, y, it.title.c_str(), false);
            y += 22;

            if (!it.journal.empty()) {
                std::string j = it.journal + " " + it.volume + "卷" + it.number + "期";
                renderer.drawText(UI_10_FONT_ID, 20, y, j.c_str(), false);
                y += 22;
            }

            renderer.drawText(UI_10_FONT_ID, 20, y, "摘要:", false);
            y += 20;
            std::string desc = it.description;
            if (desc.size() > 180) desc = desc.substr(0, 180) + "...";
            renderer.drawText(UI_10_FONT_ID, 20, y, desc.c_str(), false);
        }

        auto l = mappedInput.mapLabels("返回", "", "", "");
        GUI.drawButtonHints(renderer, l.btn1, l.btn2, l.btn3, l.btn4);
        renderer.displayBuffer();
        return;
    }

    auto labels = mappedInput.mapLabels("返回", "查看", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    if (items.empty()) {
        renderer.drawCenteredText(UI_10_FONT_ID, h/2, "暂无内容");
        renderer.displayBuffer();
        return;
    }

    size_t st = selectorIndex / PAGE_ITEMS * PAGE_ITEMS;
    renderer.fillRect(0, 60 + (selectorIndex % PAGE_ITEMS)*30 - 2, w-1, 30);

    for (size_t i = st; i < items.size() && i < st + PAGE_ITEMS; i++) {
        std::string s = items[i].title;
        if (s.size() > 42) s = s.substr(0,42) + "...";
        renderer.drawText(UI_10_FONT_ID, 20, 60 + (i%PAGE_ITEMS)*30, s.c_str(), i != selectorIndex);
    }

    renderer.displayBuffer();
}

void RssBrowserActivity::fetchRss() {
    items.clear();
    rssItems.clear();
    inItem = false;
    currentTag.clear();
    currentText.clear();

    std::string url = SETTINGS.rssUrl;
    if (url.empty()) url = "https://wulixb.iphy.ac.cn/rss/latest.xml";

    HTTPClient http;
    http.begin(url.c_str());
    int code = http.GET();
    if (code != 200) {
        state = BrowserState::ERROR;
        errorMessage = "HTTP " + std::to_string(code);
        updateRequired = true;
        http.end();
        return;
    }

    String xml = http.getString();
    http.end();

    XML_Parser p = XML_ParserCreate(nullptr);
    XML_SetElementHandler(p, startElement, endElement);
    XML_SetCharacterDataHandler(p, charData);
    XML_Parse(p, xml.c_str(), xml.length(), true);
    XML_ParserFree(p);

    items = rssItems;
    state = BrowserState::BROWSING;
    updateRequired = true;
}

void RssBrowserActivity::checkAndConnectWifi() {
    if (WiFi.status() == WL_CONNECTED) {
        state = BrowserState::LOADING;
        statusMessage = "加载中...";
        updateRequired = true;
        fetchRss();
        return;
    }
    launchWifiSelection();
}

void RssBrowserActivity::launchWifiSelection() {
    state = BrowserState::WIFI_SELECTION;
    updateRequired = true;
    enterNewActivity(new WifiSelectionActivity(renderer, mappedInput,
        [this](bool c){ onWifiSelectionComplete(c); }));
}

void RssBrowserActivity::onWifiSelectionComplete(const bool connected) {
    exitActivity();
    if (connected) {
        state = BrowserState::LOADING;
        statusMessage = "加载中...";
        updateRequired = true;
        fetchRss();
    } else {
        state = BrowserState::ERROR;
        errorMessage = "WiFi 连接失败";
        updateRequired = true;
    }
}

void RssBrowserActivity::showArticleDetail(const RssItem&) {
    state = BrowserState::DETAIL;
    updateRequired = true;
}