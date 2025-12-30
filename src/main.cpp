// 引入Arduino核心库（提供pinMode、millis、delay等基础函数）
#include <Arduino.h>
// 电子纸显示驱动核心库（控制电子纸显示的底层逻辑）
#include <EInkDisplay.h>
// EPUB电子书解析库（用于读取和解析电子书文件）
#include <Epub.h>
// 图形渲染库（负责在电子纸上绘制文字、图形、界面）
#include <GfxRenderer.h>
// 输入管理库（处理按键的按下、释放、长按等事件）
#include <InputManager.h>
// SD卡操作库（读取SD卡中的电子书、配置文件等）
#include <SD.h>
// SPI通信库（电子纸、SD卡均基于SPI协议通信）
#include <SPI.h>
// 内置字体文件（bookerly字体，用于电子书正文显示）
#include <builtinFonts/bookerly_2b.h>
// 内置字体文件（ubuntu 10号字体，用于UI界面文字显示）
#include <builtinFonts/ubuntu_10.h>

// 电池管理模块（读取电池电压、电量等）
#include "Battery.h"
// 设备配置管理（存储/读取用户设置，如电源键长按时长、字体大小等）
#include "CrossPointSettings.h"
// 应用状态管理（存储/读取应用运行状态，如上次打开的电子书路径）
#include "CrossPointState.h"
// 开机页面模块（设备启动时显示的页面）
#include "activities/boot_sleep/BootActivity.h"
// 休眠页面模块（设备进入休眠时显示的页面）
#include "activities/boot_sleep/SleepActivity.h"
// 新增：网页渲染页面模块（用于显示HTML网页内容）
#include "activities/yuedu/yueduActivity.h"
// 主页模块（阅读器的主界面，包含功能入口）
#include "activities/home/HomeActivity.h"
// 网络服务器页面模块（用于文件传输的Web服务器界面）
#include "activities/network/CrossPointWebServerActivity.h"
// 电子书阅读页面模块（核心阅读功能，翻页、字体调整等）
#include "activities/reader/ReaderActivity.h"
// 设置页面模块（设备参数设置，如休眠时间、按键校准等）
#include "activities/settings/SettingsActivity.h"
// 全屏提示页面模块（显示错误、提示等全屏文字信息）
#include "activities/util/FullScreenMessageActivity.h"
// 全局配置文件（包含版本号、引脚定义等常量）
#include "config.h"

// SPI通信频率定义（40MHz，即40000000赫兹，控制SPI数据传输速度）
#define SPI_FQ 40000000

// 电子纸SPI引脚定义（XteinkX4设备的自定义引脚，非硬件默认SPI引脚）
#define EPD_SCLK 8   // SPI时钟引脚（数据传输的节拍器）
#define EPD_MOSI 10  // SPI主机发数据引脚（单片机→电子纸传输数据）
#define EPD_CS 21    // 电子纸片选引脚（选中电子纸设备，避免SPI总线冲突）
#define EPD_DC 4     // 数据/命令选择引脚（区分传输的是命令还是显示数据）
#define EPD_RST 5    // 电子纸复位引脚（重启电子纸，解决显示异常）
#define EPD_BUSY 6   // 电子纸忙状态引脚（电子纸告知单片机是否正在处理数据）

#define UART0_RXD 20  // UART0接收引脚（专门用于检测USB是否连接）

// SD卡SPI引脚定义
#define SD_SPI_CS 12  // SD卡片选引脚（选中SD卡设备）
#define SD_SPI_MISO 7 // SD卡主机收数据引脚（SD卡→单片机传输数据）

// 实例化电子纸显示对象（绑定上述定义的电子纸引脚）
EInkDisplay einkDisplay(EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY);
// 实例化输入管理对象（处理按键输入）
InputManager inputManager;
// 实例化图形渲染对象（绑定电子纸对象，用于在电子纸上绘制内容）
GfxRenderer renderer(einkDisplay);
// 声明当前活动页面指针（指向当前显示的页面，如主页、阅读页等）
Activity* currentActivity;

// 字体对象初始化（bookerly基础字体）
EpdFont bookerlyFont(&bookerly_2b);
// bookerly粗体（暂用同个字体文件，可根据需求替换）
EpdFont bookerlyBoldFont(&bookerly_2b);
// bookerly斜体（暂用同个字体文件，可根据需求替换）
EpdFont bookerlyItalicFont(&bookerly_2b);
// bookerly粗斜体（暂用同个字体文件，可根据需求替换）
EpdFont bookerlyBoldItalicFont(&bookerly_2b);
// 封装bookerly字体族（整合常规/粗体/斜体/粗斜体，供渲染器调用）
EpdFontFamily bookerlyFontFamily(&bookerlyFont, &bookerlyBoldFont, &bookerlyItalicFont, &bookerlyBoldItalicFont);

// 小字体对象（ubuntu 10号字体，用于UI提示）
EpdFont smallFont(&ubuntu_10);
// 封装小字体族（仅常规字体）
EpdFontFamily smallFontFamily(&smallFont);

// ubuntu 10号字体对象
EpdFont ubuntu10Font(&ubuntu_10);
// ubuntu 10号粗体（暂用同个字体文件，可根据需求替换）
EpdFont ubuntuBold10Font(&ubuntu_10);
// 封装ubuntu字体族（常规+粗体）
EpdFontFamily ubuntuFontFamily(&ubuntu10Font, &ubuntuBold10Font);


// 自动休眠超时时间（10分钟，单位毫秒：10*60秒*1000毫秒/秒）
constexpr unsigned long AUTO_SLEEP_TIMEOUT_MS = 10 * 60 * 1000;
// 电源键长按时长校准变量（记录按键按下/释放时间）
unsigned long t1 = 0;
unsigned long t2 = 0;

/**
 * @brief 退出当前活动页面
 * @details 执行页面退出逻辑，释放页面内存，清空当前页面指针
 */
void exitActivity() {
  if (currentActivity) {
    currentActivity->onExit();  // 执行当前页面的退出回调（如保存进度）
    delete currentActivity;     // 释放页面占用的内存
    currentActivity = nullptr;  // 清空指针，避免野指针
  }
}

/**
 * @brief 进入新的活动页面
 * @param activity 指向新页面的指针
 * @details 切换到新页面，并执行页面的进入初始化逻辑
 */
void enterNewActivity(Activity* activity) {
  currentActivity = activity;   // 将当前页面指针指向新页面
  currentActivity->onEnter();   // 执行新页面的进入回调（如绘制UI）
}



/**
 * @brief 唤醒时验证电源键长按是否有效
 * @details 校准硬件延迟，确保用户确实长按电源键唤醒，避免误触发
 */
void verifyWakeupLongPress() {
  // 记录唤醒开始时间，给用户1000ms时间开始长按电源键
  const auto start = millis();
  bool abort = false;
  // 硬件唤醒有延迟，需校准（减去25ms）
  uint16_t calibration = 25;
  uint16_t calibratedPressDuration =
      (calibration < SETTINGS.getPowerButtonDuration()) ? SETTINGS.getPowerButtonDuration() - calibration : 1;

  inputManager.update();  // 刷新输入状态
  // 等待用户按下电源键，超时1000ms则判定为误唤醒
  while (!inputManager.isPressed(InputManager::BTN_POWER) && millis() - start < 1000) {
    delay(10);  // 短延迟，避免CPU空转
    inputManager.update();
  }

  t2 = millis();  // 记录按键按下后的时间
  // 如果电源键被按下，检测长按时长是否达标
  if (inputManager.isPressed(InputManager::BTN_POWER)) {
    do {
      delay(10);
      inputManager.update();
    } while (inputManager.isPressed(InputManager::BTN_POWER) && inputManager.getHeldTime() < calibratedPressDuration);
    // 长按时长不足则判定为无效唤醒
    abort = inputManager.getHeldTime() < calibratedPressDuration;
  } else {
    // 未按下电源键则判定为无效唤醒
    abort = true;
  }

  if (abort) {
    // 无效唤醒，重新配置GPIO唤醒并返回深度睡眠
    esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_deep_sleep_start();
  }
}

/**
 * @brief 等待电源键释放
 * @details 确保电源键完全释放，避免休眠后立即被唤醒
 */
void waitForPowerRelease() {
  inputManager.update();
  // 循环检测，直到电源键释放
  while (inputManager.isPressed(InputManager::BTN_POWER)) {
    delay(50);
    inputManager.update();
  }
}

/**
 * @brief 进入深度睡眠模式
 * @details 退出当前页面，显示休眠页，配置唤醒GPIO，进入ESP32深度睡眠
 */
void enterDeepSleep() {
  exitActivity();  // 退出当前页面
  // 进入休眠页面
  enterNewActivity(new SleepActivity(renderer, inputManager));

  einkDisplay.deepSleep();  // 电子纸进入深度休眠
  // 打印电源键校准时间（调试用）
  Serial.printf("[%lu] [   ] Power button press calibration value: %lu ms\n", millis(), t2 - t1);
  Serial.printf("[%lu] [   ] Entering deep sleep.\n", millis());
  // 配置GPIO唤醒（电源键低电平唤醒）
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  // 等待电源键释放
  waitForPowerRelease();
  // 启动ESP32深度睡眠（CPU停止运行，仅GPIO唤醒有效）
  esp_deep_sleep_start();
}

// 前置声明：跳转到主页的函数（解决函数调用顺序问题）
void onGoHome();

/**
 * @brief 跳转到电子书阅读页面
 * @param initialEpubPath 初始打开的电子书路径
 * @details 退出当前页面，创建并进入ReaderActivity
 */
void onGoToReader(const std::string& initialEpubPath) {
  exitActivity();
  // 进入阅读页面，绑定返回主页的回调
  enterNewActivity(new ReaderActivity(renderer, inputManager, initialEpubPath, onGoHome));
}

/**
 * @brief 跳转到阅读页面（无初始路径）
 * @details 重载onGoToReader，默认打开空路径
 */
void onGoToReaderHome() { onGoToReader(std::string()); }

/**
 * @brief 跳转到文件传输页面（Web服务器）
 * @details 退出当前页面，创建并进入CrossPointWebServerActivity
 */
void onGoToFileTransfer() {
  exitActivity();
  // 进入文件传输页面，绑定返回主页的回调
  enterNewActivity(new CrossPointWebServerActivity(renderer, inputManager, onGoHome));
}

/**
 * @brief 跳转到阅读页面
 * @details 退出当前页面，创建并进入yueduActivity
 */
void onyuedu() { 
  exitActivity();
  // 进入阅读页面，绑定返回主页的回调
  enterNewActivity(new yueduActivity(renderer, inputManager, onGoHome)); }

/**
 * @brief 跳转到设置页面
 * @details 退出当前页面，创建并进入SettingsActivity
 */
void onGoToSettings() {
  exitActivity();
  // 进入设置页面，绑定返回主页的回调
  enterNewActivity(new SettingsActivity(renderer, inputManager, onGoHome));
}

/**
 * @brief 跳转到主页
 * @details 退出当前页面，创建并进入HomeActivity，绑定各功能入口回调
 */
void onGoHome() {
  exitActivity();
  // 进入主页，绑定阅读/设置/文件传输/网页渲染的回调
  enterNewActivity(new HomeActivity(renderer, inputManager, onGoToReaderHome, onGoToSettings, onGoToFileTransfer,onyuedu));
}

/**
 * @brief Arduino初始化函数（仅运行一次）
 * @details 初始化硬件、加载配置、校准唤醒、启动初始页面
 */
void setup() {
  t1 = millis();  // 记录程序启动时间（用于电源键校准）
  Serial.begin(115200);  // 初始化串口（波特率115200，用于调试输出）

  Serial.printf("[%lu] [   ] Starting CrossPoint version " CROSSPOINT_VERSION "\n", millis());

  inputManager.begin();  // 初始化输入管理器（按键检测）
  // 初始化电池检测引脚为输入模式
  pinMode(BAT_GPIO0, INPUT);

  // 初始化SPI总线（绑定自定义引脚：SCLK/MISO/MOSI/CS）
  SPI.begin(EPD_SCLK, SD_SPI_MISO, EPD_MOSI, EPD_CS);

  // 初始化SD卡（指定片选引脚、SPI对象、通信频率）
  if (!SD.begin(SD_SPI_CS, SPI, SPI_FQ)) {
    Serial.printf("[%lu] [   ] SD card initialization failed\n", millis());
    exitActivity();
    // SD卡初始化失败，显示全屏错误提示
    enterNewActivity(new FullScreenMessageActivity(renderer, inputManager, "SD card error", BOLD));
    return;
  }

  SETTINGS.loadFromFile();  // 从SD卡加载设备配置

  // 唤醒后校准电源键长按（需在加载配置后执行）
  verifyWakeupLongPress();

  einkDisplay.begin();  // 初始化电子纸显示
  Serial.printf("[%lu] [   ] Display initialized\n", millis());

  // 向渲染器注册字体（绑定字体ID和字体族）
  renderer.insertFont(READER_FONT_ID, bookerlyFontFamily);  // 阅读页字体
  renderer.insertFont(UI_FONT_ID, ubuntuFontFamily);        // UI界面字体
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);      // 小字体（提示文字）
  Serial.printf("[%lu] [   ] Fonts setup\n", millis());

  exitActivity();
  // 启动开机页面
  enterNewActivity(new BootActivity(renderer, inputManager));

  APP_STATE.loadFromFile();  // 加载应用状态（上次打开的电子书路径）
  if (APP_STATE.openEpubPath.empty()) {
    // 无上次阅读记录，跳转到主页
    onGoHome();
  } else {
    // 有上次阅读记录，清空状态避免启动循环，跳转到阅读页
    //const auto path = APP_STATE.openEpubPath;
    //APP_STATE.openEpubPath = "";
    //APP_STATE.saveToFile();
    //onGoToReader(path);
    onGoHome();
  }

  // 确保电源键释放后再进入主循环
  waitForPowerRelease();
}

/**
 * @brief Arduino主循环函数（一直重复运行）
 * @details 处理输入、检测自动休眠、刷新当前页面、管理循环延迟
 */
void loop() {
  static unsigned long maxLoopDuration = 0;  // 记录最大循环耗时（调试用）
  const unsigned long loopStartTime = millis();  // 记录本次循环开始时间
  static unsigned long lastMemPrint = 0;  // 上次打印内存信息的时间

  inputManager.update();  // 刷新输入状态（检测按键事件）

  // 每10秒打印一次内存信息（调试用，监控内存使用）
  if (Serial && millis() - lastMemPrint >= 10000) {
    Serial.printf("[%lu] [MEM] Free: %d bytes, Total: %d bytes, Min Free: %d bytes\n", millis(), ESP.getFreeHeap(),
                  ESP.getHeapSize(), ESP.getMinFreeHeap());
    lastMemPrint = millis();
  }

  // 自动休眠检测逻辑
  static unsigned long lastActivityTime = millis();  // 上次用户活动时间
  // 检测到按键按下/释放，重置活动计时器
  if (inputManager.wasAnyPressed() || inputManager.wasAnyReleased()) {
    lastActivityTime = millis();
  }

  // 无活动时间达到10分钟，触发自动休眠
  if (millis() - lastActivityTime >= AUTO_SLEEP_TIMEOUT_MS) {
    Serial.printf("[%lu] [SLP] Auto-sleep triggered after %lu ms of inactivity\n", millis(), AUTO_SLEEP_TIMEOUT_MS);
    enterDeepSleep();
    return;  // 休眠后无需继续执行循环
  }

  // 电源键长按检测（超过设置时长则休眠）
  if (inputManager.isPressed(InputManager::BTN_POWER) &&
      inputManager.getHeldTime() > SETTINGS.getPowerButtonDuration()) {
    enterDeepSleep();
    return;
  }

  // 执行当前页面的循环逻辑（如刷新UI、处理触摸事件）
  const unsigned long activityStartTime = millis();
  if (currentActivity) {
    currentActivity->loop();
  }
  const unsigned long activityDuration = millis() - activityStartTime;

  // 记录并打印最大循环耗时（超过50ms则提示，优化性能）
  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      Serial.printf("[%lu] [LOOP] New max loop duration: %lu ms (activity: %lu ms)\n", millis(), maxLoopDuration,
                    activityDuration);
    }
  }

  // 循环延迟控制（平衡响应速度和功耗）
  if (currentActivity && currentActivity->skipLoopDelay()) {
    yield();  // 无延迟，仅让出CPU（如Web服务器运行时需快速响应）
  } else {
    delay(10);  // 常规延迟10ms（降低CPU占用，节省功耗）
  }
}