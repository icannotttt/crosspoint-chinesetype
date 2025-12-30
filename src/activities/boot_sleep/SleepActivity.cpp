#include "SleepActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <SD.h>

#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "config.h"
#include "images/CrossLarge.h"

void SleepActivity::onEnter() {
  Activity::onEnter();
  //renderPopup("Entering Sleep...");

  if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM) {
    return renderCustomSleepScreen();
  }

  if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::COVER) {
    return renderCoverSleepScreen();
  }

  renderDefaultSleepScreen();
}

void SleepActivity::renderPopup(const char* message) const {
  const int textWidth = renderer.getTextWidth(READER_FONT_ID, message);
  constexpr int margin = 20;
  const int x = (renderer.getScreenWidth() - textWidth - margin * 2) / 2;
  constexpr int y = 117;
  const int w = textWidth + margin * 2;
  const int h = renderer.getLineHeight(READER_FONT_ID) + margin * 2;
  // renderer.clearScreen();
  renderer.fillRect(x + 5, y + 5, w - 10, h - 10, false);
  renderer.drawText(READER_FONT_ID, x + margin, y + margin, message);
  renderer.drawRect(x + 5, y + 5, w - 10, h - 10);
  renderer.displayBuffer();
}

void SleepActivity::renderCustomSleepScreen() const {
  // Check if we have a /sleep directory
  auto dir = SD.open("/sleep");
  if (dir && dir.isDirectory()) {
    std::vector<std::string> files;
    // collect all valid BMP files
    for (File file = dir.openNextFile(); file; file = dir.openNextFile()) {
      if (file.isDirectory()) {
        file.close();
        continue;
      }
      auto filename = std::string(file.name());
      if (filename[0] == '.') {
        file.close();
        continue;
      }

      if (filename.substr(filename.length() - 4) != ".bmp") {
        Serial.printf("[%lu] [SLP] Skipping non-.bmp file name: %s\n", millis(), file.name());
        file.close();
        continue;
      }
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() != BmpReaderError::Ok) {
        Serial.printf("[%lu] [SLP] Skipping invalid BMP file: %s\n", millis(), file.name());
        file.close();
        continue;
      }
      files.emplace_back(filename);
      file.close();
    }
    const auto numFiles = files.size();
    if (numFiles > 0) {
      // Generate a random number between 1 and numFiles
      const auto randomFileIndex = random(numFiles);
      const auto filename = "/sleep/" + files[randomFileIndex];
      auto file = SD.open(filename.c_str());
      if (file) {
        Serial.printf("[%lu] [SLP] Randomly loading: /sleep/%s\n", millis(), files[randomFileIndex].c_str());
        delay(100);
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderBitmapSleepScreen(bitmap);
          dir.close();
          return;
        }
      }
    }
  }
  if (dir) dir.close();

  // Look for sleep.bmp on the root of the sd card to determine if we should
  // render a custom sleep screen instead of the default.
  auto file = SD.open("/sleep.bmp");
  if (file) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      Serial.printf("[%lu] [SLP] Loading: /sleep.bmp\n", millis());
      renderBitmapSleepScreen(bitmap);
      return;
    }
  }

  renderDefaultSleepScreen();
}

void SleepActivity::renderDefaultSleepScreen() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawImage(CrossLarge, (pageWidth - 128) / 2, (pageHeight - 128) / 2, 128, 128);
  renderer.drawCenteredText(UI_FONT_ID, pageHeight / 2 + 70, "CrossPoint", true, BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, "SLEEPING");

  // Make sleep screen dark unless light is selected in settings
  if (SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::LIGHT) {
    renderer.invertScreen();
  }

  renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap) const {
  int x, y;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // ========== 第一步：计算位图的居中/缩放位置 ==========
  // 若位图尺寸超过屏幕，按比例缩放并居中；否则直接居中
  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    const float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    if (ratio > screenRatio) {
      // 图片宽高比大于屏幕 → 垂直居中
      x = 0;
      y = (pageHeight - pageWidth / ratio) / 2;
    } else {
      // 图片宽高比小于屏幕 → 水平居中
      x = (pageWidth - pageHeight * ratio) / 2;
      y = 0;
    }
  } else {
    // 图片尺寸小于屏幕 → 整体居中
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  // ========== 第二步：绘制黑白（BW）基础层 ==========
  //renderer.clearScreen(); // 清空屏幕缓冲区（默认BW模式）
  // 绘制位图的黑白层到屏幕缓冲区
  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight);
  // 刷新黑白缓冲区到显示屏（快速半刷新）
  renderer.displayBuffer(EInkDisplay::HALF_REFRESH);

  // ========== 第三步：灰度显示核心逻辑（关键） ==========
  if (bitmap.hasGreyscale()) { // 判断位图是否包含灰度数据
    // 3.1 绘制灰度LSB层（最低有效位，对应低阶灰度）
    bitmap.rewindToData(); // 重置位图数据读取指针，从头读取灰度数据
    renderer.clearScreen(0x00); // 清空灰度缓冲区
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB); // 切换到灰度LSB渲染模式
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight); // 绘制LSB灰度层
    renderer.copyGrayscaleLsbBuffers(); // 将LSB灰度数据拷贝到显存

    // 3.2 绘制灰度MSB层（最高有效位，对应高阶灰度）
    bitmap.rewindToData(); // 再次重置指针，重新读取灰度数据
    renderer.clearScreen(0x00); // 清空缓冲区
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB); // 切换到灰度MSB渲染模式
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight); // 绘制MSB灰度层
    renderer.copyGrayscaleMsbBuffers(); // 将MSB灰度数据拷贝到显存

    // 3.3 刷新灰度缓冲区到显示屏（完成灰度显示）
    renderer.displayGrayBuffer();
    // 3.4 恢复为黑白渲染模式（避免影响后续绘制）
    renderer.setRenderMode(GfxRenderer::BW);
  }
}

void SleepActivity::renderCoverSleepScreen() const {
  if (APP_STATE.openEpubPath.empty()) {
    return renderDefaultSleepScreen();
  }

  Epub lastEpub(APP_STATE.openEpubPath, "/.crosspoint");
  if (!lastEpub.load()) {
    Serial.println("[SLP] Failed to load last epub");
    return renderDefaultSleepScreen();
  }

  if (!lastEpub.generateCoverBmp()) {
    Serial.println("[SLP] Failed to generate cover bmp");
    return renderDefaultSleepScreen();
  }

  auto file = SD.open(lastEpub.getCoverBmpPath().c_str(), FILE_READ);
  if (file) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      renderer.clearScreen();
      renderBitmapSleepScreen(bitmap);
      return;
    }
  }

  renderDefaultSleepScreen();
}
