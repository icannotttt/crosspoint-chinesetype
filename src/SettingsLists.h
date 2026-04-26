#pragma once

#include <vector>

#include "CrossPointSettings.h"
#include "KOReaderCredentialStore.h"
#include "activities/settings/SettingsActivity.h"

// Shared settings list used by both the device settings UI and the web settings API.
// Each entry has a key (for JSON API) and category (for grouping).
// ACTION-type entries and entries without a key are device-only.

inline std::vector<SettingInfo> getSettingsList() {
  return {
      // --- Display ---
      SettingInfo::Enum("休眠屏", &CrossPointSettings::sleepScreen,
                      {"默认黑", "默认白", "自定义", "书籍封面", "透明壁纸","透明壁纸2","无", "封面 + 自定义"}, "sleepScreen", "Display"),
    SettingInfo::Enum("休眠屏封面模式", &CrossPointSettings::sleepScreenCoverMode, {"适配", "裁剪"},
                      "sleepScreenCoverMode", "Display"),
    SettingInfo::Enum("休眠屏封面滤镜", &CrossPointSettings::sleepScreenCoverFilter,
                      {"无", "增强对比度", "反色"}, "sleepScreenCoverFilter", "Display"),
    SettingInfo::Enum(
        "状态栏", &CrossPointSettings::statusBar,
        {"无", "不显示进度", "完整+百分比", "完整+书籍条", "仅书籍条", "完整+章节条"}, "statusBar", "Display"),
    SettingInfo::Enum("隐藏电池百分比", &CrossPointSettings::hideBatteryPercentage, {"从不", "仅阅读", "总是"},
                      "hideBatteryPercentage", "Display"),
    SettingInfo::Enum("刷新频率", &CrossPointSettings::refreshFrequency,
                      {"1 page", "5 pages", "10 pages", "15 pages", "30 pages"},"refreshFrequency","Display"),
    SettingInfo::Enum("UI 主题", &CrossPointSettings::uiTheme, {"经典", "Lyra"},"UI Theme","Display"),
    SettingInfo::Toggle("抗阳光褪色", &CrossPointSettings::fadingFix,"Sunlight Fading Compensation","Display"),

      // --- Reader ---
      SettingInfo::Enum("字体", &CrossPointSettings::fontFamily, {"汉仪空山楷","汉仪空山楷", "汉仪空山楷", "自定义"}, "字体", "Reader"),
      SettingInfo::Enum("字号", &CrossPointSettings::fontSize, {"小", "中", "大", "特大"}, "字号", "Reader"),
    SettingInfo::Enum("行间距", &CrossPointSettings::lineSpacing,  {"Tight", "Normal", "Wide"}, "行间距", "Reader"),
    SettingInfo::Toggle("首行缩进", &CrossPointSettings::firstlineintented, "首行缩进","Reader"),
    SettingInfo::Value("字间距", &CrossPointSettings::wordSpacing, 0,10,2, "字间距", "Reader"),
    SettingInfo::Value("上边距", &CrossPointSettings::screenMargin_Top, 0,80,5, "上边距", "Reader"),
    SettingInfo::Value("下边距", &CrossPointSettings::screenMargin_Bottom, 0,80,5, "下边距", "Reader"),
    SettingInfo::Value("左边距", &CrossPointSettings::screenMargin_Left, 0,40,5,"左边距", "Reader"),
    SettingInfo::Value("右边距", &CrossPointSettings::screenMargin_Right, 0,40,5, "右边距", "Reader"),
    SettingInfo::Toggle("阅读背景", &CrossPointSettings::ReadingScreenEnabled,"阅读背景","Reader"),
    SettingInfo::Toggle("划线", &CrossPointSettings::extraline,"划线","Reader"),
    SettingInfo::Enum("对齐方式", &CrossPointSettings::paragraphAlignment,
                      {"两边对齐", "左对齐", "居中", "右对齐", "书本样式"}, "对齐方式", "Reader"),
    SettingInfo::Toggle("是否使用书籍内嵌样式", &CrossPointSettings::embeddedStyle,"是否使用书籍内嵌样式","Reader"),
    SettingInfo::Toggle("连字符", &CrossPointSettings::hyphenationEnabled,"连字符","Reader"),
    SettingInfo::Enum("阅读方向", &CrossPointSettings::orientation,
                      {"默认方向", "按钮在左边", "按钮在上边", "按钮在右边"},"阅读方向","Reader"),
    SettingInfo::Toggle("额外段间距", &CrossPointSettings::extraParagraphSpacing,"额外段间距","Reader"),
    SettingInfo::Toggle("抗锯齿", &CrossPointSettings::textAntiAliasing,"抗锯齿","Reader"),

      // --- Controls ---
      SettingInfo::Enum("侧边按钮设置（仅阅读）", &CrossPointSettings::sideButtonLayout,
                        {"上, 下", "下, 上"}, "sideButtonLayout", "Controls"),
      SettingInfo::Toggle("长按跳章节", &CrossPointSettings::longPressChapterSkip, "longPressChapterSkip",
                          "Controls"),
      SettingInfo::Enum("短按电源键", &CrossPointSettings::shortPwrBtn, {"忽略", "休眠", "翻页"},
                        "shortPwrBtn", "Controls"),

      // --- System ---
      SettingInfo::Enum("休眠时间", &CrossPointSettings::sleepTimeout,
                        {"1 min", "5 min", "10 min", "15 min", "30 min"}, "sleepTimeout", "System"),
      SettingInfo::Toggle("bluetoothEnabled", &CrossPointSettings::bluetoothEnabled, "bluetoothEnabled", "System"),

      // --- KOReader Sync (web-only, uses KOReaderCredentialStore) ---
      SettingInfo::DynamicString(
          "KOReader Username", [] { return KOREADER_STORE.getUsername(); },
          [](const std::string& v) {
            KOREADER_STORE.setCredentials(v, KOREADER_STORE.getPassword());
            KOREADER_STORE.saveToFile();
          },
          "koUsername", "KOReader Sync"),
      SettingInfo::DynamicString(
          "KOReader Password", [] { return KOREADER_STORE.getPassword(); },
          [](const std::string& v) {
            KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), v);
            KOREADER_STORE.saveToFile();
          },
          "koPassword", "KOReader Sync"),
      SettingInfo::DynamicString(
          "Sync Server URL", [] { return KOREADER_STORE.getServerUrl(); },
          [](const std::string& v) {
            KOREADER_STORE.setServerUrl(v);
            KOREADER_STORE.saveToFile();
          },
          "koServerUrl", "KOReader Sync"),
      SettingInfo::DynamicEnum(
          "Document Matching", {"Filename", "Binary"},
          [] { return static_cast<uint8_t>(KOREADER_STORE.getMatchMethod()); },
          [](uint8_t v) {
            KOREADER_STORE.setMatchMethod(static_cast<DocumentMatchMethod>(v));
            KOREADER_STORE.saveToFile();
          },
          "koMatchMethod", "KOReader Sync"),

      // --- OPDS Browser (web-only, uses CrossPointSettings char arrays) ---
      SettingInfo::String("OPDS Server URL", SETTINGS.opdsServerUrl, sizeof(SETTINGS.opdsServerUrl), "opdsServerUrl",
                          "OPDS Browser"),
      SettingInfo::String("OPDS Username", SETTINGS.opdsUsername, sizeof(SETTINGS.opdsUsername), "opdsUsername",
                          "OPDS Browser"),
      SettingInfo::String("OPDS Password", SETTINGS.opdsPassword, sizeof(SETTINGS.opdsPassword), "opdsPassword",
                          "OPDS Browser"),

      // --- 坚果云配置 (web-only, uses CrossPointSettings char arrays) ---
      SettingInfo::String("坚果云账号", SETTINGS.jgUsername, sizeof(SETTINGS.jgUsername), "jgUsername",
                          "坚果云配置"),
      SettingInfo::String("坚果云应用密码", SETTINGS.jgAppPassword, sizeof(SETTINGS.jgAppPassword), "jgAppPassword",
                          "坚果云配置"),
      SettingInfo::String("坚果云读取目录", SETTINGS.jgBookFolder, sizeof(SETTINGS.jgBookFolder), "jgBookFolder",
                          "坚果云配置"),
  };
}