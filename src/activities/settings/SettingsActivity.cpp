#include "SettingsActivity.h"

#include <GfxRenderer.h>
#include <HardwareSerial.h>

#include "ButtonRemapActivity.h"
#include "CalibreSettingsActivity.h"
#include "ClearCacheActivity.h"
#include "CrossPointSettings.h"
#include "KOReaderSettingsActivity.h"
#include "MappedInputManager.h"
#include "OtaUpdateActivity.h"
#include "components/UITheme.h"
#include <EpdFontLoader.h>
#include "FontSelectionActivity.h"
#include "fontIds.h"
#include "JianGuoYunSettingsActivity.h"
#include "languageMapper.h"
#include "BluetoothSettingsActivity.h"

#include "SettingsLists.h"



const char* SettingsActivity::categoryNames[categoryCount] = {"Display", "Reader", "Controls", "System"};

namespace {
constexpr int changeTabsMs = 700;

}  // namespace

void SettingsActivity::taskTrampoline(void* param) {
  auto* self = static_cast<SettingsActivity*>(param);
  self->displayTaskLoop();
}

void SettingsActivity::onEnter() {
  Activity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();

  // Build per-category vectors from the shared settings list
  displaySettings.clear();
  readerSettings.clear();
  controlsSettings.clear();
  systemSettings.clear();

  for (auto& setting : getSettingsList()) {
    if (!setting.category) continue;
    if (strcmp(setting.category, "Display") == 0) {
      displaySettings.push_back(std::move(setting));
    } else if (strcmp(setting.category, "Reader") == 0) {
      readerSettings.push_back(std::move(setting));
    } else if (strcmp(setting.category, "Controls") == 0) {
      controlsSettings.push_back(std::move(setting));
    } else if (strcmp(setting.category, "System") == 0) {
      systemSettings.push_back(std::move(setting));
    }
    // Web-only categories (KOReader Sync, OPDS Browser) are skipped for device UI
  }

  // Append device-only ACTION items
  controlsSettings.insert(controlsSettings.begin(), SettingInfo::Action("Remap Front Buttons"));
  systemSettings.push_back(SettingInfo::Action("bluetooth"));
  systemSettings.push_back(SettingInfo::Action("KOReader Sync"));
  systemSettings.push_back(SettingInfo::Action("OPDS Browser"));
  systemSettings.push_back(SettingInfo::Action("坚果云信息配置"));
  systemSettings.push_back(SettingInfo::Action("Clear Cache"));
  systemSettings.push_back(SettingInfo::Action("Check for updates"));
  systemSettings.push_back(SettingInfo::Action("Set Custom Font Family"));


  // Reset selection to first category
  selectedCategoryIndex = 0;
  selectedSettingIndex = 0;

  // Initialize with first category (Display)
  currentSettings = &displaySettings;
  settingsCount = static_cast<int>(displaySettings.size());

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

  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

void SettingsActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }
  bool hasChangedCategory = false;

  // Handle actions with early return
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedSettingIndex == 0) {
      selectedCategoryIndex = (selectedCategoryIndex < categoryCount - 1) ? (selectedCategoryIndex + 1) : 0;
      hasChangedCategory = true;
      updateRequired = true;
    } else {
      toggleCurrentSetting();
      updateRequired = true;
      return;
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    SETTINGS.saveToFile();
    EpdFontLoader::loadFontsFromSd(renderer);
    onGoHome();
    return;
  }

  const bool upReleased = mappedInput.wasReleased(MappedInputManager::Button::Up);
  const bool downReleased = mappedInput.wasReleased(MappedInputManager::Button::Down);
  const bool leftReleased = mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool rightReleased = mappedInput.wasReleased(MappedInputManager::Button::Right);
  const bool changeTab = mappedInput.getHeldTime() > changeTabsMs;

  // Handle navigation
  if (upReleased && changeTab) {
    hasChangedCategory = true;
    selectedCategoryIndex = (selectedCategoryIndex > 0) ? (selectedCategoryIndex - 1) : (categoryCount - 1);
    updateRequired = true;
  } else if (downReleased && changeTab) {
    hasChangedCategory = true;
    selectedCategoryIndex = (selectedCategoryIndex < categoryCount - 1) ? (selectedCategoryIndex + 1) : 0;
    updateRequired = true;
  } else if (upReleased || leftReleased) {
    selectedSettingIndex = (selectedSettingIndex > 0) ? (selectedSettingIndex - 1) : (settingsCount);
    updateRequired = true;
  } else if (rightReleased || downReleased) {
    selectedSettingIndex = (selectedSettingIndex < settingsCount) ? (selectedSettingIndex + 1) : 0;
    updateRequired = true;
  }

  if (hasChangedCategory) {
    selectedSettingIndex = (selectedSettingIndex == 0) ? 0 : 1;
    switch (selectedCategoryIndex) {
      case 0:  // Display
        currentSettings = &displaySettings;
        break;
      case 1:  // Reader
        currentSettings = &readerSettings;
        break;
      case 2:  // Controls
        currentSettings = &controlsSettings;
        break;
      case 3:  // System
        currentSettings = &systemSettings;
        break;
    }
     settingsCount = static_cast<int>(currentSettings->size());
  }
}

void SettingsActivity::toggleCurrentSetting() {
  int selectedSetting = selectedSettingIndex - 1;
  if (selectedSetting < 0 || selectedSetting >= settingsCount) {
    return;
  }

  const auto& setting = (*currentSettings)[selectedSetting];

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    // Toggle the boolean value using the member pointer
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = (currentValue + 1) % static_cast<uint8_t>(setting.enumValues.size());
} else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    // ========== 匹配 ValueRange 结构体的 VALUE 逻辑 ==========
    // 1. 读取当前值（类型匹配：uint8_t，避免有符号/无符号错误）
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr); 
    // 2. 计算新值：当前值 + 步长（改用 valueRange.step）
    uint8_t newValue = currentValue + setting.valueRange.step;
    // 3. 循环逻辑：超过最大值则回到最小值（改用 valueRange.min/max）
    // 比如 40 + 5 = 45 > 40 → 重置为 0；35 + 5 = 40 ≤40 → 保留40
    if (newValue > setting.valueRange.max) {
        newValue = setting.valueRange.min;
    }   
    // 4. 写回新值（这一步是真正改变数值的核心）
    SETTINGS.*(setting.valuePtr) = newValue;
} else if (setting.type == SettingType::ACTION) {
    if (strcmp(setting.name, "Remap Front Buttons") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new ButtonRemapActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(setting.name, "KOReader Sync") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new KOReaderSettingsActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(setting.name, "OPDS Browser") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new CalibreSettingsActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
            }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(setting.name, "坚果云信息配置") == 0) {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    exitActivity();
    enterNewActivity(new JianGuoYunSettingsActivity(renderer, mappedInput, [this] {
      exitActivity();
      updateRequired = true;
    }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(setting.name, "Clear Cache") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new ClearCacheActivity(renderer, mappedInput, [this] {
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
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new FontSelectionActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
      } else if (strcmp(setting.name, "bluetooth") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new BluetoothSettingsActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    }



  } else {
    return;
  }

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

  auto metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Settings");

  std::vector<TabInfo> tabs;
  tabs.reserve(categoryCount);
  for (int i = 0; i < categoryCount; i++) {
    tabs.push_back({getChineseName(categoryNames[i]), selectedCategoryIndex == i});
  }
  GUI.drawTabBar(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight}, tabs,
                 selectedSettingIndex == 0);


  const auto& settings = *currentSettings;        
  GUI.drawList(
      renderer,
      Rect{0, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing, pageWidth,
          pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.buttonHintsHeight +
                        metrics.verticalSpacing * 2)},
      settingsCount, selectedSettingIndex - 1, 
      // 第一个回调：设置项名称（英文转中文）
      [&settings](int index) { 
          // 核心修改：调用getChineseName转换名称
          const char* englishName = settings[index].name;
          const char* chineseName = getChineseName(englishName);
          return std::string(chineseName); 
      },
      nullptr, nullptr,
      // 第二个回调：设置项值（英文转中文）
      [&settings](int i) {
        std::string valueText = "";
        if (settings[i].type == SettingType::TOGGLE && settings[i].valuePtr != nullptr) {
          const bool value = SETTINGS.*(settings[i].valuePtr);
          // 核心修改：ON/OFF 转 开启/关闭
          valueText = value ? getChineseName("ON") : getChineseName("OFF");
          // 如果你没给ON/OFF加映射，也可以直接写：valueText = value ? "开启" : "关闭";
        } else if (settings[i].type == SettingType::ENUM && settings[i].valuePtr != nullptr) {
          const uint8_t value = SETTINGS.*(settings[i].valuePtr);
          // 核心修改：枚举值（如Tight/Normal）转中文
          const char* englishValue = settings[i].enumValues[value].c_str();
          const char* chineseValue = getChineseName(englishValue);
          valueText = chineseValue;
        } else if (settings[i].type == SettingType::VALUE && settings[i].valuePtr != nullptr) {
          // 数值型（如5/10）无需转换，直接显示
          valueText = std::to_string(SETTINGS.*(settings[i].valuePtr));
        } else if (settings[i].type == SettingType::ACTION &&
            strcmp(settings[i].name, "Set Custom Font Family") == 0) {
          if (SETTINGS.fontFamily == CrossPointSettings::FONT_CUSTOM) {
            // 自定义字体名称保留原字符串（无需转换）
            valueText = SETTINGS.customFontFamily;
          }
        }
        return valueText;
      });

  // Draw version text
  renderer.drawText(SMALL_FONT_ID,
                    pageWidth - metrics.versionTextRightX - renderer.getTextWidth(SMALL_FONT_ID, CROSSPOINT_VERSION),
                    metrics.versionTextY, CROSSPOINT_VERSION);

  // Draw help text
  const auto labels = mappedInput.mapLabels(getChineseName("« Back"), getChineseName("Toggle"), getChineseName("Up"), getChineseName("Down"));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
