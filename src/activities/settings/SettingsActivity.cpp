#include "SettingsActivity.h"

#include <EpdFontLoader.h>
#include <GfxRenderer.h>
#include <HardwareSerial.h>

#include <cstring>

#include "CalibreSettingsActivity.h"
#include "CrossPointSettings.h"
#include "FontSelectionActivity.h"
#include "MappedInputManager.h"
#include "OtaUpdateActivity.h"
#include "fontIds.h"

// 轻量级翻译函数：用if-else替代map，无容器开销
static const char* getChineseName(const char* englishName) {
  // 设置项名称翻译
  if (strcmp(englishName, "Sleep Screen") == 0) return "休眠屏幕";
  if (strcmp(englishName, "Sleep Screen Cover Mode") == 0) return "合盖息屏模式";
  if (strcmp(englishName, "Status Bar") == 0) return "状态栏";
  if (strcmp(englishName, "Hide Battery %") == 0) return "隐藏电池百分比";
  if (strcmp(englishName, "Extra Paragraph Spacing") == 0) return "额外段落间距";
  if (strcmp(englishName, "Text Anti-Aliasing") == 0) return "文字抗锯齿";
  if (strcmp(englishName, "Short Power Button Click") == 0) return "电源键短按";
  if (strcmp(englishName, "Reading Orientation") == 0) return "阅读方向";
  if (strcmp(englishName, "Front Button Layout") == 0) return "正面按键布局";
  if (strcmp(englishName, "Side Button Layout (reader)") == 0) return "侧边按键布局 (阅读器)";
  if (strcmp(englishName, "Long-press Chapter Skip") == 0) return "长按跳过章节";
  if (strcmp(englishName, "Reader Font Family") == 0) return "阅读器字体";
  if (strcmp(englishName, "Set Custom Font Family") == 0) return "设置自定义字体";
  if (strcmp(englishName, "Reader Font Size") == 0) return "阅读器字号";
  if (strcmp(englishName, "Reader Line Spacing") == 0) return "阅读器行间距";
  if (strcmp(englishName, "Reader Screen Margin") == 0) return "阅读器屏幕边距";
  if (strcmp(englishName, "Reader Paragraph Alignment") == 0) return "阅读器段落对齐方式";
  if (strcmp(englishName, "Time to Sleep") == 0) return "自动休眠时间";
  if (strcmp(englishName, "Refresh Frequency") == 0) return "刷新频率";
  if (strcmp(englishName, "返回界面") == 0) return "返回界面";
  if (strcmp(englishName, "Calibre Settings") == 0) return "Calibre 设置";
  if (strcmp(englishName, "Check for updates") == 0) return "检查更新";

  // 枚举值翻译
  if (strcmp(englishName, "Dark") == 0) return "深色";
  if (strcmp(englishName, "Light") == 0) return "浅色";
  if (strcmp(englishName, "Custom") == 0) return "自定义";
  if (strcmp(englishName, "Cover") == 0) return "封面";
  if (strcmp(englishName, "None") == 0) return "无";
  if (strcmp(englishName, "Fit") == 0) return "适配";
  if (strcmp(englishName, "Crop") == 0) return "裁剪";
  if (strcmp(englishName, "No Progress") == 0) return "无进度条";
  if (strcmp(englishName, "Full") == 0) return "完整";
  if (strcmp(englishName, "Never") == 0) return "从不";
  if (strcmp(englishName, "In Reader") == 0) return "阅读时";
  if (strcmp(englishName, "Always") == 0) return "始终";
  if (strcmp(englishName, "Ignore") == 0) return "忽略";
  if (strcmp(englishName, "Sleep") == 0) return "休眠";
  if (strcmp(englishName, "Page Turn") == 0) return "翻页";
  if (strcmp(englishName, "Portrait") == 0) return "竖屏";
  if (strcmp(englishName, "Landscape CW") == 0) return "顺时针横屏";
  if (strcmp(englishName, "Inverted") == 0) return "反转";
  if (strcmp(englishName, "Landscape CCW") == 0) return "逆时针横屏";
  if (strcmp(englishName, "Bck, Cnfrm, Lft, Rght") == 0) return "返回, 确认, 左, 右";
  if (strcmp(englishName, "Lft, Rght, Bck, Cnfrm") == 0) return "左, 右, 返回, 确认";
  if (strcmp(englishName, "Lft, Bck, Cnfrm, Rght") == 0) return "左, 返回, 确认, 右";
  if (strcmp(englishName, "Prev, Next") == 0) return "上一页, 下一页";
  if (strcmp(englishName, "Next, Prev") == 0) return "下一页, 上一页";
  if (strcmp(englishName, "Bookerly") == 0) return "Bookerly";
  if (strcmp(englishName, "Noto Sans") == 0) return "思源黑体";
  if (strcmp(englishName, "Open Dyslexic") == 0) return "Open Dyslexic";
  if (strcmp(englishName, "Small") == 0) return "小";
  if (strcmp(englishName, "Medium") == 0) return "中";
  if (strcmp(englishName, "Large") == 0) return "大";
  if (strcmp(englishName, "X Large") == 0) return "特大";
  if (strcmp(englishName, "Tight") == 0) return "紧凑";
  if (strcmp(englishName, "Normal") == 0) return "正常";
  if (strcmp(englishName, "Wide") == 0) return "宽松";
  if (strcmp(englishName, "Justify") == 0) return "两端对齐";
  if (strcmp(englishName, "Left") == 0) return "左对齐";
  if (strcmp(englishName, "Center") == 0) return "居中对齐";
  if (strcmp(englishName, "Right") == 0) return "右对齐";
  if (strcmp(englishName, "1 min") == 0) return "1分钟";
  if (strcmp(englishName, "5 min") == 0) return "5分钟";
  if (strcmp(englishName, "10 min") == 0) return "10分钟";
  if (strcmp(englishName, "15 min") == 0) return "15分钟";
  if (strcmp(englishName, "30 min") == 0) return "30分钟";
  if (strcmp(englishName, "1 page") == 0) return "1页";
  if (strcmp(englishName, "5 pages") == 0) return "5页";
  if (strcmp(englishName, "10 pages") == 0) return "10页";
  if (strcmp(englishName, "15 pages") == 0) return "15页";
  if (strcmp(englishName, "30 pages") == 0) return "30页";
  if (strcmp(englishName, "HOME") == 0) return "主页";
  if (strcmp(englishName, "READING") == 0) return "阅读页";
  if (strcmp(englishName, "ON") == 0) return "开启";
  if (strcmp(englishName, "OFF") == 0) return "关闭";

  // 未匹配到的返回原英文
  return englishName;
}

// Define the static settings list
namespace {
constexpr int settingsCount = 22;
const SettingInfo settingsList[settingsCount] = {
    // Should match with SLEEP_SCREEN_MODE
    SettingInfo::Enum("Sleep Screen", &CrossPointSettings::sleepScreen, {"Dark", "Light", "Custom", "Cover", "None"}),
    SettingInfo::Enum("Sleep Screen Cover Mode", &CrossPointSettings::sleepScreenCoverMode, {"Fit", "Crop"}),
    SettingInfo::Enum("Status Bar", &CrossPointSettings::statusBar, {"None", "No Progress", "Full"}),
    SettingInfo::Enum("Hide Battery %", &CrossPointSettings::hideBatteryPercentage, {"Never", "In Reader", "Always"}),
    SettingInfo::Toggle("Extra Paragraph Spacing", &CrossPointSettings::extraParagraphSpacing),
    SettingInfo::Toggle("Text Anti-Aliasing", &CrossPointSettings::textAntiAliasing),
    SettingInfo::Enum("Short Power Button Click", &CrossPointSettings::shortPwrBtn, {"Ignore", "Sleep", "Page Turn"}),
    SettingInfo::Enum("Reading Orientation", &CrossPointSettings::orientation,
                      {"Portrait", "Landscape CW", "Inverted", "Landscape CCW"}),
    SettingInfo::Enum("Front Button Layout", &CrossPointSettings::frontButtonLayout,
                      {"Bck, Cnfrm, Lft, Rght", "Lft, Rght, Bck, Cnfrm", "Lft, Bck, Cnfrm, Rght"}),
    SettingInfo::Enum("Side Button Layout (reader)", &CrossPointSettings::sideButtonLayout,
                      {"Prev, Next", "Next, Prev"}),
    SettingInfo::Toggle("Long-press Chapter Skip", &CrossPointSettings::longPressChapterSkip),
    SettingInfo::Enum("Reader Font Family", &CrossPointSettings::fontFamily,
                      {"Bookerly", "Noto Sans", "Open Dyslexic", "Custom"}),
    SettingInfo::Action("Set Custom Font Family"),
    SettingInfo::Enum("Reader Font Size", &CrossPointSettings::fontSize, {"Small", "Medium", "Large", "X Large"}),
    SettingInfo::Enum("Reader Line Spacing", &CrossPointSettings::lineSpacing, {"Tight", "Normal", "Wide"}),
    SettingInfo::Value("Reader Screen Margin", &CrossPointSettings::screenMargin, {5, 40, 5}),
    SettingInfo::Enum("Reader Paragraph Alignment", &CrossPointSettings::paragraphAlignment,
                      {"Justify", "Left", "Center", "Right"}),
    SettingInfo::Enum("Time to Sleep", &CrossPointSettings::sleepTimeout,
                      {"1 min", "5 min", "10 min", "15 min", "30 min"}),
    SettingInfo::Enum("Refresh Frequency", &CrossPointSettings::refreshFrequency,
                      {"1 page", "5 pages", "10 pages", "15 pages", "30 pages"}),
    SettingInfo::Enum("返回界面", &CrossPointSettings::gowhere,
                      {"HOME", "READING"}),
    SettingInfo::Action("Calibre Settings"),
    SettingInfo::Action("Check for updates")};
}  // namespace

void SettingsActivity::taskTrampoline(void* param) {
  auto* self = static_cast<SettingsActivity*>(param);
  self->displayTaskLoop();
}

void SettingsActivity::onEnter() {
  Activity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();

  // Reset selection to first item
  selectedSettingIndex = 0;

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&SettingsActivity::taskTrampoline, "SettingsActivityTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void SettingsActivity::onExit() {
  ActivityWithSubactivity::onExit();

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void SettingsActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  // Handle actions with early return
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    toggleCurrentSetting();
    updateRequired = true;
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    SETTINGS.saveToFile();
    onGoHome();
    return;
  }

  // Handle navigation
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    // Move selection up (with wrap-around)
    selectedSettingIndex = (selectedSettingIndex > 0) ? (selectedSettingIndex - 1) : (settingsCount - 1);
    updateRequired = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    // Move selection down (with wrap around)
    selectedSettingIndex = (selectedSettingIndex < settingsCount - 1) ? (selectedSettingIndex + 1) : 0;
    updateRequired = true;
  }

  if (updateRequired) {
    // Ensure selected item is in view
    if (selectedSettingIndex < scrollOffset) {
      scrollOffset = selectedSettingIndex;
    } else if (selectedSettingIndex >= scrollOffset + itemsPerPage) {
      scrollOffset = selectedSettingIndex - itemsPerPage + 1;
    }
  }
}

void SettingsActivity::toggleCurrentSetting() {
  // Validate index
  if (selectedSettingIndex < 0 || selectedSettingIndex >= settingsCount) {
    return;
  }

  const auto& setting = settingsList[selectedSettingIndex];
  Serial.printf("[Settings] Toggling: '%s' (Type: %d)\n", setting.name, (int)setting.type);

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    // Toggle the boolean value using the member pointer
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = (currentValue + 1) % static_cast<uint8_t>(setting.enumValues.size());

    if (strcmp(setting.name, "Reader Font Family") == 0 || strcmp(setting.name, "Reader Font Size") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      EpdFontLoader::loadFontsFromSd(renderer);
      xSemaphoreGive(renderingMutex);
    }
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    // Decreasing would also be nice for large ranges I think but oh well can't have everything
    const int8_t currentValue = SETTINGS.*(setting.valuePtr);
    // Wrap to minValue if exceeding setting value boundary
    if (currentValue + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = currentValue + setting.valueRange.step;
    }
  } else if (setting.type == SettingType::ACTION) {
    if (strcmp(setting.name, "Calibre Settings") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new CalibreSettingsActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(setting.name, "Check for updates") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new OtaUpdateActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(setting.name, "Set Custom Font Family") == 0) {
      Serial.println("[Settings] Launching FontSelectionActivity");
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      subActivity.reset(new FontSelectionActivity(renderer, mappedInput, [this] {
        subActivity.reset();
        updateRequired = true;
      }));
      subActivity->onEnter();
      xSemaphoreGive(renderingMutex);
    } else {
      Serial.printf("[Settings] Unknown action: %s\n", setting.name);
    }
  } else {
    // Only toggle if it's a toggle type and has a value pointer
    return;
  }

  // Save settings when they change
  SETTINGS.saveToFile();
}

void SettingsActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired && !subActivity) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void SettingsActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Draw header - 显示中文标题
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "设置", true, EpdFontFamily::BOLD);

  // Draw selection
  if (selectedSettingIndex >= scrollOffset && selectedSettingIndex < scrollOffset + itemsPerPage) {
    renderer.fillRect(0, 60 + (selectedSettingIndex - scrollOffset) * 30 - 2, pageWidth - 1, 30);
  }

  // Draw visible settings
  for (int i = 0; i < itemsPerPage; i++) {
    int index = scrollOffset + i;
    if (index >= settingsCount) break;

    const int settingY = 60 + i * 30;  // 30 pixels between settings

    // Draw setting name - 使用轻量函数获取中文名称
    const char* chineseName = getChineseName(settingsList[index].name);
    renderer.drawText(UI_10_FONT_ID, 20, settingY, chineseName, index != selectedSettingIndex);

    // Draw value based on setting type
    std::string valueText = "";
    if (settingsList[index].type == SettingType::TOGGLE && settingsList[index].valuePtr != nullptr) {
      const bool value = SETTINGS.*(settingsList[index].valuePtr);
      valueText = value ? "ON" : "OFF";
      // 翻译开关状态
      const char* chineseValue = getChineseName(valueText.c_str());
      valueText = chineseValue;
    } else if (settingsList[index].type == SettingType::ENUM && settingsList[index].valuePtr != nullptr) {
      const uint8_t value = SETTINGS.*(settingsList[index].valuePtr);
      const char* enumValueEng = settingsList[index].enumValues[value].c_str();
      // 翻译枚举值
      const char* enumValueCn = getChineseName(enumValueEng);
      valueText = enumValueCn;
    } else if (settingsList[index].type == SettingType::VALUE && settingsList[index].valuePtr != nullptr) {
      valueText = std::to_string(SETTINGS.*(settingsList[index].valuePtr));
    } else if (settingsList[index].type == SettingType::ACTION &&
               strcmp(settingsList[index].name, "Set Custom Font Family") == 0) {
      if (SETTINGS.fontFamily == CrossPointSettings::FONT_CUSTOM) {
        valueText = SETTINGS.customFontFamily;
      }
    }
    if (!valueText.empty()) {
      const auto width = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str());
      renderer.drawText(UI_10_FONT_ID, pageWidth - 20 - width, settingY, valueText.c_str(),
                        index != selectedSettingIndex);
    }
  }

  // Draw version text above button hints
  renderer.drawText(SMALL_FONT_ID, pageWidth - 20 - renderer.getTextWidth(SMALL_FONT_ID, CROSSPOINT_VERSION),
                    pageHeight - 60, CROSSPOINT_VERSION);

  // Draw help text - 按钮提示改为中文
  const auto labels = mappedInput.mapLabels("保存并退出", "切换", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}