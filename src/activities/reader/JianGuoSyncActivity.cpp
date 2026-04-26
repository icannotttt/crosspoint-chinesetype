#include "JianGuoSyncActivity.h"

#include <GfxRenderer.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <base64.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

#include <HTTPClient.h>
#include "network/HttpDownloader.h"
#include "util/StringUtils.h"
#include <SPIFFS.h>

#include <cctype>
#include <iomanip>
#include <sstream>

namespace {
constexpr int SKIP_PAGE_MS = 700;
int matchOffset = 0; // 新增：记录匹配到的偏移差值（核心）
}  // namespace

// 静态成员变量初始化
SemaphoreHandle_t JianGuoSyncActivity::renderingMutex = nullptr;
TaskHandle_t JianGuoSyncActivity::displayTaskHandle = nullptr;
bool JianGuoSyncActivity::updateRequired = false;
JianGuoSyncActivity::BrowserState JianGuoSyncActivity::state = JianGuoSyncActivity::BrowserState::CHECK_WIFI;
std::string JianGuoSyncActivity::errorMessage = "";
std::string JianGuoSyncActivity::statusMessage = "";
size_t JianGuoSyncActivity::downloadProgress = 0;
size_t JianGuoSyncActivity::downloadTotal = 0;
int JianGuoSyncActivity::lastExtractedChapterIndex = -1;
std::string JianGuoSyncActivity::lastExtractedBookName = "";
std::string JianGuoSyncActivity::lastExtractedChapterTitle = "";

void JianGuoSyncActivity::taskTrampoline(void* param) {
  auto* self = static_cast<JianGuoSyncActivity*>(param);
  self->displayTaskLoop();
}

// 在文件开头添加URL编码函数
static std::string urlEncode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::setw(2) << static_cast<int>(c);
        }
    }
    return escaped.str();
}

void JianGuoSyncActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();
  state = BrowserState::CHECK_WIFI;
  selectedOption = 0;           // 重置选项
  matchedFileName = "";         // 清空文件名
  matchedFileUrl = "";          // 清空文件 URL
  errorMessage.clear();
  statusMessage = "检查 WiFi...";
  updateRequired = true;
  downloadProgress = 0;
  downloadTotal = 0;
  lastExtractedChapterIndex = -1;
  lastExtractedBookName = "";

  SPIFFS.end();
  if (!SPIFFS.begin(true)) {
    Serial.println("[JG] SPIFFS 初始化失败");
    state = BrowserState::ERROR;
    errorMessage = "存储初始化失败";
    updateRequired = true;
  }

  xTaskCreate(&JianGuoSyncActivity::taskTrampoline, "JianGuoSyncTask",
              4096, this, 1, &displayTaskHandle);

  checkAndConnectWifi();
}

void JianGuoSyncActivity::onExit() {
  ActivityWithSubactivity::onExit();

  WiFi.mode(WIFI_OFF);

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void JianGuoSyncActivity::loop() {
  // WiFi 选择子页面
  if (state == BrowserState::WIFI_SELECTION) {
    ActivityWithSubactivity::loop();
    return;
  }

  // 错误状态
  if (state == BrowserState::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      state = BrowserState::DOWNLOADING;
      statusMessage = "重试中...";
      updateRequired = true;
      downloadProgressFile();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoBack();
    }
    return;
  }

  // 检查 WiFi/下载中/上传中：只允许返回
  if (state == BrowserState::CHECK_WIFI || 
      state == BrowserState::DOWNLOADING ||
      state == BrowserState::UPLOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoBack();
    }
    return;
  }

  // 显示结果状态：左右选择，确认执行
// 显示结果状态：左右选择，确认执行
if (state == BrowserState::SHOWING_RESULT) {
  // 新增：先清空残留的按键事件，必须等用户重新操作
  if (skipFirstButtonCheck) {
    // 检查 Confirm/Back 按键是否都已释放，且没有残留的释放事件
    const bool confirmCleared = !mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
                                !mappedInput.wasReleased(MappedInputManager::Button::Confirm);
    const bool backCleared = !mappedInput.isPressed(MappedInputManager::Button::Back) &&
                            !mappedInput.wasReleased(MappedInputManager::Button::Back);
    const bool leftRightCleared = !mappedInput.isPressed(MappedInputManager::Button::Left) &&
                                  !mappedInput.wasReleased(MappedInputManager::Button::Left) &&
                                  !mappedInput.isPressed(MappedInputManager::Button::Right) &&
                                  !mappedInput.wasReleased(MappedInputManager::Button::Right);
    
    if (confirmCleared && backCleared && leftRightCleared) {
      skipFirstButtonCheck = false;  // 隔离完成，开始响应新按键
    }
    return;  // 隔离期间不处理任何按键
  }

  //左右切换选项
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    selectedOption = 0;  // 下载
    updateRequired = true;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    selectedOption = 1;  // 上传
    updateRequired = true;
  }

  // 确认执行
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedOption == 0) {
      // 下载：应用云端进度
      Serial.printf("[%lu] [JG] 选择下载，章节：%d\n", millis(), lastExtractedChapterIndex);
      onSelectChapter(lastExtractedChapterIndex);
      onGoBack();
    } else {
      // 上传：上传当前进度
      Serial.printf("[%lu] [JG] 选择上传，当前章节：%d\n", millis(), currentSpineIndex);
      state = BrowserState::UPLOADING;
      statusMessage = "上传中...";
      updateRequired = true;
      uploadProgressFile();
    }
  }

  // 原有逻辑：返回取消
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
  }
  return;
}

  // 完成状态
  if (state == BrowserState::COMPLETE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoBack();
    }
    return;
  }
}

void JianGuoSyncActivity::displayTaskLoop() {
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

void JianGuoSyncActivity::render() const {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, "阅读进度同步", true, EpdFontFamily::BOLD);

  // 检查 WiFi
  if (state == BrowserState::CHECK_WIFI) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "连接 WiFi 中...");
    const auto labels = mappedInput.mapLabels("返回", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // 下载中
  if (state == BrowserState::DOWNLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 30, "下载进度文件...");
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, statusMessage.c_str());
    if (downloadTotal > 0) {
        const int barWidth = pageWidth - 100;
        const int barX = 50;
        const int barY = pageHeight / 2 + 30;
        GUI.drawProgressBar(renderer, Rect{barX, barY, barWidth, 20}, 
                           downloadProgress, downloadTotal);
    }
    const auto labels = mappedInput.mapLabels("返回", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // 上传中
  if (state == BrowserState::UPLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 30, "上传进度文件...");
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels("返回", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // 显示结果（选择下载/上传）
  if (state == BrowserState::SHOWING_RESULT) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 50, "✓ 找到进度文件");
    
    char infoBuf[64];
    snprintf(infoBuf, sizeof(infoBuf), "云端章节：%d", lastExtractedChapterIndex);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, infoBuf);
    
    snprintf(infoBuf, sizeof(infoBuf), "本地章节：%d", currentSpineIndex);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 5, infoBuf);

    if (!lastExtractedBookName.empty()) {
        renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 35, lastExtractedBookName.c_str());
    }

    // 选项
    const int optionY = pageHeight / 2 + 70;
    const int optionHeight = 30;

    // 下载选项
    if (selectedOption == 0) {
      renderer.fillRect(0, optionY - 2, pageWidth - 1, optionHeight);
      renderer.drawText(UI_10_FONT_ID, 20, optionY, "← 下载云端进度", false);
    } else {
      renderer.drawText(UI_10_FONT_ID, 20, optionY, "  下载云端进度", true);
    }

    // 上传选项
    if (selectedOption == 1) {
      renderer.fillRect(0, optionY + optionHeight - 2, pageWidth - 1, optionHeight);
      renderer.drawText(UI_10_FONT_ID, 20, optionY + optionHeight, "→ 上传本地进度", false);
    } else {
      renderer.drawText(UI_10_FONT_ID, 20, optionY + optionHeight, "  上传本地进度", true);
    }

    const auto labels = mappedInput.mapLabels("返回", "选择", "左", "右");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // 完成
  if (state == BrowserState::COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 30, "✓ 同步完成");
    char infoBuf[64];
    snprintf(infoBuf, sizeof(infoBuf), "章节：%d", lastExtractedChapterIndex);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, infoBuf);
    if (!lastExtractedBookName.empty()) {
        renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 25, lastExtractedBookName.c_str());
    }
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight - 30, "按返回键退出");
    renderer.displayBuffer();
    return;
  }

  // 错误
  if (state == BrowserState::ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, "✗ 失败");
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, errorMessage.c_str());
    const auto labels = mappedInput.mapLabels("返回", "重试", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}


// WiFi检查逻辑
void JianGuoSyncActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    state = BrowserState::DOWNLOADING;
    statusMessage = "准备下载...";
    updateRequired = true;
    downloadProgressFile();
    return;
  }
  launchWifiSelection();
}

// 启动WiFi选择页面
void JianGuoSyncActivity::launchWifiSelection() {
  state = BrowserState::WIFI_SELECTION;
  updateRequired = true;
  enterNewActivity(new WifiSelectionActivity(renderer, mappedInput,
                                             [this](const bool connected) { 
                                               onWifiSelectionComplete(connected); 
                                             }));
}

// WiFi选择完成回调
void JianGuoSyncActivity::onWifiSelectionComplete(const bool connected) {
  exitActivity();
  if (connected) {
    Serial.printf("[%lu] [JG] WiFi已连接，开始下载进度文件\n", millis());
    state = BrowserState::DOWNLOADING;
    statusMessage = "准备下载...";
    updateRequired = true;
    downloadProgressFile();
  } else {
    Serial.printf("[%lu] [JG] WiFi连接失败\n", millis());
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    state = BrowserState::ERROR;
    errorMessage = "WiFi连接失败";
    updateRequired = true;
  }
}

// 自动下载进度文件
void JianGuoSyncActivity::downloadProgressFile() {
    std::string username = SETTINGS.jgUsername;
    std::string appPwd = SETTINGS.jgAppPassword;
    
    if (username.empty() || appPwd.empty()) {
        state = BrowserState::ERROR;
        errorMessage = "请配置坚果云账号/应用密码";
        updateRequired = true;
        return;
    }

    // 目标文件路径（相对路径）
    // 目标书名
    //debug std::string targetName = "十万个氪金的理由";  // ← 可改为参数传入
    std::string targetName = epub->getTitle();
    Serial.printf("[JG] 标题为：%s\n", targetName.c_str());
    std::string matchedFileName = "";

    // === PROPFIND 列目录查找文件 ===
    WiFiClientSecure client;
    client.setInsecure();

    String authStr = String(username.c_str()) + ":" + String(appPwd.c_str());
    String base64Auth = base64::encode(authStr);
    base64Auth.trim();

    if (!client.connect("dav.jianguoyun.com", 443)) {
        state = BrowserState::ERROR;
        errorMessage = "连接坚果云失败";
        updateRequired = true;
        return;
    }

    String propfindPath = "/dav/";
    propfindPath += "legado/bookProgress/";

    client.print("PROPFIND ");
    client.print(propfindPath);
    client.println(" HTTP/1.1");
    client.println("Host: dav.jianguoyun.com");
    client.println("Authorization: Basic " + base64Auth);
    client.println("Depth: 1");
    client.println("Content-Length: 0");
    client.println("Connection: close");
    client.println();

    // 读取响应
    String response = "";
    unsigned long timeout = millis() + 8000;
    while (millis() < timeout && client.connected()) {
        if (client.available()) {
            response += client.readString();
        } else {
            delay(10);
        }
    }
    client.stop();

    // 解析 XML，查找匹配的文件
    int xmlPos = 0;
    while (true) {
        int nameStart = response.indexOf("<d:displayname>", xmlPos);
        if (nameStart == -1) break;
        nameStart += 15;
        int nameEnd = response.indexOf("</d:displayname>", nameStart);
        if (nameEnd == -1) break;
        String fileName = response.substring(nameStart, nameEnd);
        xmlPos = nameEnd;
        
        Serial.printf("[JG] 找到文件：%s\n", fileName.c_str());
        
        // 匹配：以 targetName 开头 且 以 .json 结尾
        // 匹配成功后
        if (fileName.startsWith(targetName.c_str()) && fileName.endsWith(".json")) {
            matchedFileName = fileName.c_str();
            this->matchedFileName = matchedFileName;
            
            // ⬇️ 保存完整 URL（已编码）
            std::string remotePath = "legado/bookProgress/" + matchedFileName;
            this->matchedFileUrl = "https://dav.jianguoyun.com/dav/" + urlEncode(remotePath);
            
            Serial.printf("[JG] 匹配成功：%s\n", matchedFileName.c_str());
            Serial.printf("[JG] 保存 URL: %s\n", this->matchedFileUrl.c_str());
            break;
        }
    }

    if (matchedFileName.empty()) {
        state = BrowserState::ERROR;
        errorMessage = "未找到进度文件";
        updateRequired = true;
        return;
    }

    // 组合 remotePath
    std::string remotePath = "legado/bookProgress/" + matchedFileName;
    
    // 构建完整URL
    std::string downloadUrl = "https://dav.jianguoyun.com/dav/";
  
    
    // 添加文件相对路径（URL编码）
    downloadUrl += urlEncode(remotePath);
    
    // 本地临时文件路径
    std::string localPath = "/temp.json";

    Serial.printf("[%lu] [JG] 下载进度文件\n", millis());
    Serial.printf("[JG] URL: %s\n", downloadUrl.c_str());
    Serial.printf("[JG] 用户: %s\n", username.c_str());
    statusMessage = "正在下载...";
    updateRequired = true;

    // 执行下载（需要传递认证信息）
    const auto result = HttpDownloader::downloadToFile_jg(
        downloadUrl,
        localPath,
        [this](size_t downloaded, size_t total) {
            downloadProgress = downloaded;
            downloadTotal = total;
            updateRequired = true;
        }
    );

    if (result == HttpDownloader::OK) {
        Serial.printf("[%lu] [JG] 下载完成，开始解析...\n", millis());
        statusMessage = "解析中...";
        updateRequired = true;
        parseAndCleanup();
    } else {
        state = BrowserState::ERROR;
        char codeStr[16];
        snprintf(codeStr, sizeof(codeStr), "HTTP %d", (int)result);
        errorMessage = "下载失败: ";
        errorMessage += codeStr;
        Serial.printf("[%lu] [JG] 下载失败: %d\n", millis(), (int)result);
        updateRequired = true;
    }
}

// 解析JSON并删除临时文件
void JianGuoSyncActivity::parseAndCleanup() {
    // 打开临时文件
    FsFile tempFile;
    if (!SdMan.openFileForRead("JGsys", "/temp.json", tempFile)) {
        state = BrowserState::ERROR;
        errorMessage = "无法打开临时文件";
        updateRequired = true;
        return;
    }

    // 读取文件内容
    String jsonContent = "";
    while (tempFile.available()) {
        jsonContent += (char)tempFile.read();
    }
    tempFile.close();

    // === 解析 durChapterIndex ===
    int chapterIndex = -1;
    String keyIndex = "\"durChapterIndex\":";
    int idxPos = jsonContent.indexOf(keyIndex);
    if (idxPos != -1) {
        int startVal = idxPos + keyIndex.length();
        while (startVal < jsonContent.length() && isspace(jsonContent[startVal])) startVal++;
        String numStr = "";
        while (startVal < jsonContent.length() && isdigit(jsonContent[startVal])) {
            numStr += jsonContent[startVal];
            startVal++;
        }
        if (numStr.length() > 0) {
            chapterIndex = numStr.toInt();
        }
    }

  // === 解析 name ===
  String bookName = "";
  String keyName = "\"name\":";
  int namePos = jsonContent.indexOf(keyName);
  if (namePos != -1) {
      int startVal = namePos + keyName.length();
      while (startVal < jsonContent.length() && jsonContent[startVal] == ' ') startVal++;
      if (startVal < jsonContent.length() && jsonContent[startVal] == '"') startVal++;
      int endVal = jsonContent.indexOf("\"", startVal);
      if (endVal != -1 && endVal > startVal) {
          bookName = jsonContent.substring(startVal, endVal);
      }
  }

  // === 解析 durChapterTitle ===
  String durChapterTitle = "";
  String keyDurChapterTitle = "\"durChapterTitle\":";
  int durChapterTitlePos = jsonContent.indexOf(keyDurChapterTitle);
  if (durChapterTitlePos != -1) {
      int startVal = durChapterTitlePos + keyDurChapterTitle.length();
      while (startVal < jsonContent.length() && jsonContent[startVal] == ' ') startVal++;
      if (startVal < jsonContent.length() && jsonContent[startVal] == '"') startVal++;
      int endVal = jsonContent.indexOf("\"", startVal);
      if (endVal != -1 && endVal > startVal) {
          durChapterTitle = jsonContent.substring(startVal, endVal);
      }
  }

  Serial.printf("[JG] 云端章节标题：%s\n", durChapterTitle.c_str());
  Serial.printf("[JG] 初始章节索引：%d\n", chapterIndex);

 // === 通过标题匹配校准章节索引（新增差值记录）===
if (!durChapterTitle.isEmpty() && chapterIndex >= 0) {
    int tocCount = epub->getTocItemsCount();  // 获取目录总数
    int maxIndex = tocCount - 1;
    
    // 新增：记录原始索引（用于计算差值）
    int originalChapterIndex = chapterIndex;
    bool found = false;
    
    
    // 先在附近搜索（±5 章）
    for (int offset = 0; offset <= 5; offset++) {
        // 先检查 +offset
        if (chapterIndex + offset <= maxIndex) {
            String localTitle = epub->getTocItem(chapterIndex + offset).title.c_str();
            if (localTitle == durChapterTitle) {
                matchOffset = offset;  // 记录+偏移值
                chapterIndex = chapterIndex + offset;
                found = true;
                Serial.printf("[JG] 标题匹配成功（+%d）：原始索引%d → 匹配索引%d，差值：+%d\n", 
                             offset, originalChapterIndex, chapterIndex, matchOffset);
                break;
            }
        }
        // 再检查 -offset
        if (offset > 0 && chapterIndex - offset >= 0) {
            String localTitle = epub->getTocItem(chapterIndex - offset).title.c_str();
            if (localTitle == durChapterTitle) {
                matchOffset = -offset; // 记录-偏移值
                chapterIndex = chapterIndex - offset;
                found = true;
                Serial.printf("[JG] 标题匹配成功（-%d）：原始索引%d → 匹配索引%d，差值：%d\n", 
                             offset, originalChapterIndex, chapterIndex, matchOffset);
                break;
            }
        }
    }
    
    // 如果附近没找到，遍历整个目录
    if (!found) {
        Serial.printf("[JG] 附近未找到，遍历整个目录...\n");
        for (int i = 0; i < tocCount; i++) {
            String localTitle = epub->getTocItem(i).title.c_str();
            if (localTitle == durChapterTitle) {
                matchOffset = i - originalChapterIndex; // 计算遍历匹配的差值
                chapterIndex = i;
                found = true;
                Serial.printf("[JG] 标题匹配成功（遍历）：原始索引%d → 匹配索引%d，差值：%d\n", 
                             originalChapterIndex, chapterIndex, matchOffset);
                break;
            }
        }
    }
    
    if (!found) {
        matchOffset = 0; // 未匹配到，差值为0
        Serial.printf("[JG] 警告：未找到匹配的章节标题，使用原始索引：%d，差值：%d\n", 
                     chapterIndex, matchOffset);
    } else {
        // 新增：全局记录匹配差值（可根据需要保存到类成员变量）
        Serial.printf("[JG] 【最终差值记录】原始索引%d → 匹配索引%d，偏移差值：%d\n", 
                     originalChapterIndex, chapterIndex, matchOffset);
    }
} else {
    Serial.printf("[JG] 跳过标题匹配（标题为空或索引无效）\n");
}
  Serial.printf("[JG] 最终章节索引：%d\n", chapterIndex);
    // === 删除临时文件 ===
    SdMan.remove("/temp.json");

    // === 存储提取结果 ===
    lastExtractedChapterIndex = chapterIndex;
    lastExtractedBookName = bookName.c_str();
    lastExtractedChapterTitle = durChapterTitle.c_str();

    Serial.printf("[PROGRESS] 章节索引：%d, 书名：%s, 章节标题：%s\n", chapterIndex, bookName.c_str(), durChapterTitle.c_str());

    // === 显示选项界面（而不是直接完成）===
    state = BrowserState::SHOWING_RESULT;
    selectedOption = 0;  // 默认选中下载
    skipFirstButtonCheck = true;  // 新增：开启按键隔离
    updateRequired = true;
}

void JianGuoSyncActivity::uploadProgressFile() {
    std::string username = SETTINGS.jgUsername;
    std::string appPwd = SETTINGS.jgAppPassword;
    
    if (username.empty() || appPwd.empty()) {
        state = BrowserState::ERROR;
        errorMessage = "请配置坚果云账号/应用密码";
        updateRequired = true;
        return;
    }

    Serial.printf("[JG] 上传文件名：%s\n", this->matchedFileName.c_str());
    Serial.printf("[JG] 上传 URL: %s\n", this->matchedFileUrl.c_str());  // ⬇️ 使用保存的 URL
    
    if (this->matchedFileUrl.empty()) {
        state = BrowserState::ERROR;
        errorMessage = "文件 URL 为空";
        updateRequired = true;
        return;
    }

    // 重新下载文件
    std::string localPath = "/temp_upload.json";

    Serial.printf("[%lu] [JG] 重新下载文件用于修改...\n", millis());
    
    const auto downloadResult = HttpDownloader::downloadToFile_jg(
        this->matchedFileUrl.c_str(),  // ⬇️ 使用保存的 URL
        localPath,
        nullptr
    );

    if (downloadResult != HttpDownloader::OK) {
        state = BrowserState::ERROR;
        char codeStr[16];
        snprintf(codeStr, sizeof(codeStr), "HTTP %d", (int)downloadResult);
        errorMessage = "下载文件失败：";
        errorMessage += codeStr;
        updateRequired = true;
        return;
    }

    // 读取 JSON
    FsFile tempFile;
    if (!SdMan.openFileForRead("JGsys", "/temp_upload.json", tempFile)) {
        state = BrowserState::ERROR;
        errorMessage = "无法打开临时文件";
        updateRequired = true;
        return;
    }

    String jsonContent = "";
    while (tempFile.available()) {
        jsonContent += (char)tempFile.read();
    }
    tempFile.close();

    // === 修改 durChapterIndex（章节索引）===
    String keyIndex = "\"durChapterIndex\":";
    int idxPos = jsonContent.indexOf(keyIndex);
    int oldChapterIndex = -1;
    if (idxPos != -1) {
        int startVal = idxPos + keyIndex.length();
        while (startVal < jsonContent.length() && isspace(jsonContent[startVal])) startVal++;
        int endVal = startVal;
        while (endVal < jsonContent.length() && isdigit(jsonContent[endVal])) endVal++;
        
        oldChapterIndex = jsonContent.substring(startVal, endVal).toInt();
        
        // 设置为当前章节
        //int currentTocIndex = getTocIndexFromSpine(currentSpineIndex);  // 更新当前章节索引
        String newJson = jsonContent.substring(0, startVal) + 
                        String(currentSpineIndex-matchOffset) + 
                        jsonContent.substring(endVal);
        jsonContent = newJson;

        Serial.printf("[JG] Spline索引：%d,Toc索引:%d,\n", currentSpineIndex, getTocIndexFromSpine(currentSpineIndex));
        
        Serial.printf("[%lu] [JG] 修改 durChapterIndex: %d → %d\n", millis(), 
                      oldChapterIndex, currentSpineIndex);
    }

    // === 修改 durChapterPos（章节内位置）===
    String keyPos = "\"durChapterPos\":";
    int posPos = jsonContent.indexOf(keyPos);
    int oldChapterPos = -1;
    if (posPos != -1) {
        int startVal = posPos + keyPos.length();
        while (startVal < jsonContent.length() && isspace(jsonContent[startVal])) startVal++;
        int endVal = startVal;
        while (endVal < jsonContent.length() && isdigit(jsonContent[endVal])) endVal++;
        
        oldChapterPos = jsonContent.substring(startVal, endVal).toInt();
        
        // 设置为 0（章节开头）
        String newJson = jsonContent.substring(0, startVal) + 
                        String(0) + 
                        jsonContent.substring(endVal);
        jsonContent = newJson;
        
        Serial.printf("[%lu] [JG] 修改 durChapterPos: %d → %d\n", millis(), 
                      oldChapterPos, 0);
    }

    // === 修改 durChapterTitle（章节标题）===
      String durChapterTitle = "";
      String keyDurChapterTitle = "\"durChapterTitle\":";
      int durChapterTitlePos = jsonContent.indexOf(keyDurChapterTitle);
      if (durChapterTitlePos != -1) {
          int startVal = durChapterTitlePos + keyDurChapterTitle.length();
          while (startVal < jsonContent.length() && jsonContent[startVal] == ' ') startVal++;
          if (startVal < jsonContent.length() && jsonContent[startVal] == '"') startVal++;
          int endVal = jsonContent.indexOf("\"", startVal);
          if (endVal != -1 && endVal > startVal) {
              durChapterTitle = jsonContent.substring(startVal, endVal);

        
       
        String newJson = jsonContent.substring(0, startVal) + 
                        epub->getTocItem(currentSpineIndex).title.c_str() + 
                        jsonContent.substring(endVal);
        jsonContent = newJson;
        
        Serial.printf("[%lu] [JG] 修改 durChapterTitle: %s → %s\n", millis(), 
                      durChapterTitle, epub->getTocItem(currentSpineIndex).title.c_str());
          }
      }

    // 保存修改后的 JSON
    FsFile writeFile;
    if (!SdMan.openFileForWrite("JGsys", "/temp_upload.json", writeFile)) {
        state = BrowserState::ERROR;
        errorMessage = "无法写入临时文件";
        updateRequired = true;
        return;
    }
    writeFile.write((const uint8_t*)jsonContent.c_str(), jsonContent.length());
    writeFile.close();

    Serial.printf("[%lu] [JG] JSON 修改完成，准备上传...\n", millis());
    Serial.printf("[JG] JSON 长度：%d\n", jsonContent.length());

    // === 使用 WiFiClientSecure 手动 PUT（和下载保持一致）===
    WiFiClientSecure client;
    client.setInsecure();

    String authStr = String(username.c_str()) + ":" + String(appPwd.c_str());
    String base64Auth = base64::encode(authStr);
    base64Auth.trim();

    if (!client.connect("dav.jianguoyun.com", 443)) {
        state = BrowserState::ERROR;
        errorMessage = "连接坚果云失败";
        updateRequired = true;
        return;
    }

    // 提取 URL 中的路径部分（去掉 https://dav.jianguoyun.com）
    String urlPath = this->matchedFileUrl.c_str();
    int pathStart = urlPath.indexOf("/", 8);  // 跳过 "https://"
    if (pathStart == -1) pathStart = 0;
    String path = urlPath.substring(pathStart);

    Serial.printf("[JG] PUT 路径：%s\n", path.c_str());

    // 发送 PUT 请求
    client.print("PUT ");
    client.print(path);
    client.println(" HTTP/1.1");
    client.println("Host: dav.jianguoyun.com");
    client.println("Authorization: Basic " + base64Auth);
    client.println("Content-Type: application/json");
    client.println("Content-Length: " + String(jsonContent.length()));
    client.println("Connection: close");
    client.println();
    client.print(jsonContent);

    // 读取响应
    unsigned long timeout = millis() + 8000;
    String responseLine = "";
    while (millis() < timeout && client.connected()) {
        if (client.available()) {
            responseLine = client.readStringUntil('\n');
            break;
        }
        delay(1);
    }

    int httpCode = 0;
    if (responseLine.startsWith("HTTP/1.1 ")) {
        httpCode = responseLine.substring(9, 12).toInt();
    }

    client.stop();
    SdMan.remove("/temp_upload.json");

    Serial.printf("[JG] HTTP 响应码：%d\n", httpCode);

    if (httpCode == 200 || httpCode == 201 || httpCode == 204) {
        Serial.printf("[%lu] [JG] ✓ 上传成功！\n", millis());
        Serial.printf("[JG] 章节：%d → %d\n", oldChapterIndex, currentSpineIndex);
        state = BrowserState::COMPLETE;
        updateRequired = true;
    } else {
        Serial.printf("[%lu] [JG] ✗ 上传失败，HTTP 码：%d\n", millis(), httpCode);
        state = BrowserState::ERROR;
        char codeStr[16];
        snprintf(codeStr, sizeof(codeStr), "HTTP %d", httpCode);
        errorMessage = "上传失败：";
        errorMessage += codeStr;
        updateRequired = true;
    }
}