#include "ImgReaderActivity.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>

#include <GfxRenderer.h>
#include <SDCardManager.h>
#include <Serialization.h>

#include "fontIds.h"

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "MappedInputManager.h"
#include "util/StringUtils.h"

namespace {
constexpr uint32_t WALLPAPER_PXC_MAGIC = 0x31584350;   // "PXC1"
constexpr uint16_t WALLPAPER_PXC_VERSION = 1;
constexpr uint32_t TRANSPARENT_PXA_MAGIC = 0x31415850;  // "PXA1"
constexpr uint16_t TRANSPARENT_PXA_VERSION = 1;
constexpr uint8_t FIXED_CACHE_ORIENTATION = CrossPointSettings::ORIENTATION::PORTRAIT;
constexpr char READING_BG_PXC[] = "/.crosspoint/wallpaper_bg.pxc";
constexpr char TRANSPARENT_BG_PXA[] = "/.crosspoint/transparent_wallpaper2.pxa";
constexpr char TRANSPARENT_WHITE_TMP[] = "/.crosspoint/.transparent_white.tmp";
constexpr char CUSTOM_SLEEP_BMP[] = "/sleep.bmp";
constexpr char CUSTOM_SLEEP_PXC[] = "/.crosspoint/custom_sleep.pxc";
constexpr int IMAGE_NEARBY_WINDOW = 2;

bool caseInsensitiveLess(const std::string& lhs, const std::string& rhs) {
  return std::lexicographical_compare(
      lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
      [](const char a, const char b) { return std::tolower(static_cast<unsigned char>(a)) < std::tolower(static_cast<unsigned char>(b)); });
}

std::vector<std::string> collectSortedImagePaths(const std::string& folderPath) {
  std::vector<std::string> imagePaths;

  auto dir = SdMan.open(folderPath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) {
      dir.close();
    }
    return imagePaths;
  }

  char name[256];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) {
      file.close();
      continue;
    }

    file.getName(name, sizeof(name));
    const std::string fileName = name;
    if (fileName.empty() || fileName[0] == '.') {
      file.close();
      continue;
    }

    if (StringUtils::checkFileExtension(fileName, ".png") || StringUtils::checkFileExtension(fileName, ".jpg") ||
        StringUtils::checkFileExtension(fileName, ".jpeg") || StringUtils::checkFileExtension(fileName, ".bmp")) {
      const std::string fullPath = (folderPath == "/") ? ("/" + fileName) : (folderPath + "/" + fileName);
      imagePaths.push_back(fullPath);
    }

    file.close();
  }

  dir.close();
  std::sort(imagePaths.begin(), imagePaths.end(), caseInsensitiveLess);
  return imagePaths;
}

void logTransparentWallpaperMem(const char* stage) {
  Serial.printf("[%lu] [IMG] [TPAPER] %s | freeHeap=%u minFreeHeap=%u\n", millis(), stage, ESP.getFreeHeap(),
                ESP.getMinFreeHeap());
}

void logicalToPhysical(const GfxRenderer::Orientation orientation, const int x, const int y, int* phyX, int* phyY) {
  switch (orientation) {
    case GfxRenderer::Portrait:
      *phyX = y;
      *phyY = HalDisplay::DISPLAY_HEIGHT - 1 - x;
      break;
    case GfxRenderer::LandscapeClockwise:
      *phyX = HalDisplay::DISPLAY_WIDTH - 1 - x;
      *phyY = HalDisplay::DISPLAY_HEIGHT - 1 - y;
      break;
    case GfxRenderer::PortraitInverted:
      *phyX = HalDisplay::DISPLAY_WIDTH - 1 - y;
      *phyY = x;
      break;
    case GfxRenderer::LandscapeCounterClockwise:
      *phyX = x;
      *phyY = y;
      break;
  }
}

bool readFramebufferPixelLogical(const uint8_t* frameBuffer, const GfxRenderer::Orientation orientation, const int x,
                                 const int y) {
  int phyX = 0;
  int phyY = 0;
  logicalToPhysical(orientation, x, y, &phyX, &phyY);

  const uint32_t byteIndex = static_cast<uint32_t>(phyY) * HalDisplay::DISPLAY_WIDTH_BYTES + (phyX / 8);
  const uint8_t bitPosition = static_cast<uint8_t>(7 - (phyX % 8));
  return (frameBuffer[byteIndex] & (1U << bitPosition)) == 0;
}

void writeFramebufferPixelLogical(uint8_t* frameBuffer, const GfxRenderer::Orientation orientation, const int x,
                                  const int y, const bool black) {
  int phyX = 0;
  int phyY = 0;
  logicalToPhysical(orientation, x, y, &phyX, &phyY);

  const uint32_t byteIndex = static_cast<uint32_t>(phyY) * HalDisplay::DISPLAY_WIDTH_BYTES + (phyX / 8);
  const uint8_t bitPosition = static_cast<uint8_t>(7 - (phyX % 8));
  if (black) {
    frameBuffer[byteIndex] &= static_cast<uint8_t>(~(1U << bitPosition));
  } else {
    frameBuffer[byteIndex] |= static_cast<uint8_t>(1U << bitPosition);
  }
}

void writeByte(FsFile& file, const uint8_t value) {
  file.write(&value, 1);
}

void write16(FsFile& file, const uint16_t value) {
  writeByte(file, static_cast<uint8_t>(value & 0xFF));
  writeByte(file, static_cast<uint8_t>((value >> 8) & 0xFF));
}

void write32(FsFile& file, const uint32_t value) {
  writeByte(file, static_cast<uint8_t>(value & 0xFF));
  writeByte(file, static_cast<uint8_t>((value >> 8) & 0xFF));
  writeByte(file, static_cast<uint8_t>((value >> 16) & 0xFF));
  writeByte(file, static_cast<uint8_t>((value >> 24) & 0xFF));
}

}  // namespace

void ImgReaderActivity::onEnter() {
  Activity::onEnter();
  buildFolderImageList();
  renderImage();
}

void ImgReaderActivity::loop() {
  constexpr unsigned long goHomeMs = 1000;

  if (menuVisible) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      menuVisible = false;
      renderImage();
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Left)|| mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      menuIndex = (menuIndex + 4) % 5;
      renderMenuOverlay();
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Right)|| mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      menuIndex = (menuIndex + 1) % 5;
      renderMenuOverlay();
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      executeMenuAction();
      return;
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    menuVisible = true;
    menuIndex = 0;
    renderMenuOverlay();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left) || mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    switchImageInFolder(-1);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right)|| mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    switchImageInFolder(1);
    return;
  }

  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= goHomeMs) {
    onGoHome();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && mappedInput.getHeldTime() < goHomeMs) {
    onGoBack(imagePath);
    return;
  }
}

bool ImgReaderActivity::renderImage() {
  bool ok = false;
  switch (imageType) {
    case ImageType::PNG:
      ok = renderPng();
      break;
    case ImageType::JPEG:
      ok = renderJpeg();
      break;
    case ImageType::BMP:
      ok = renderBmp(imagePath);
      break;
  }

  if (ok) {
    applyCurrentRenderModeToFramebuffer();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  } else {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "Image load failed", true, EpdFontFamily::BOLD);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }

  return ok;
}

void ImgReaderActivity::renderMenuOverlay() const {
  renderer.fillRect(20, 250, renderer.getScreenWidth() - 40, 300, false);
  renderer.drawRect(20, 250, renderer.getScreenWidth() - 40, 300, true);
  renderer.drawCenteredText(UI_12_FONT_ID, 275, "图片操作", true, EpdFontFamily::BOLD);

  const char* item0 = (menuIndex == 0) ? "> 设为阅读背景" : "  设为阅读背景";
  const char* item1 = (menuIndex == 1) ? "> 设为自定义睡眠屏" : "  设为自定义睡眠屏";
  const char* item2 = (menuIndex == 2) ? "> 设为透明壁纸" : "  设为透明壁纸";
  const char* item3 = (menuIndex == 3) ? "> 旋转180度" : "  旋转180度";
  const char* item4 = (menuIndex == 4) ? "> 左右翻转" : "  左右翻转";
  renderer.drawCenteredText(SMALL_FONT_ID, 325, item0);
  renderer.drawCenteredText(SMALL_FONT_ID, 355, item1);
  renderer.drawCenteredText(SMALL_FONT_ID, 385, item2);
  renderer.drawCenteredText(SMALL_FONT_ID, 415, item3);
  renderer.drawCenteredText(SMALL_FONT_ID, 445, item4);

  const auto labels = mappedInput.mapLabels("返回", "应用", "上一项", "下一项");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void ImgReaderActivity::showOperationResult(const char* message) {
  renderImage();
  GUI.drawPopup(renderer, message);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  delay(600);
}

bool ImgReaderActivity::saveFrameBufferAsPxc(const std::string& pxcPath) const {
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return false;
  }

  SdMan.mkdir("/.crosspoint");

  FsFile output;
  if (!SdMan.openFileForWrite("IMG", pxcPath, output)) {
    return false;
  }

  const uint32_t payloadSize = static_cast<uint32_t>(renderer.getBufferSize());
  const uint8_t reserved = 0;
  serialization::writePod(output, WALLPAPER_PXC_MAGIC);
  serialization::writePod(output, WALLPAPER_PXC_VERSION);
  serialization::writePod(output, FIXED_CACHE_ORIENTATION);
  serialization::writePod(output, reserved);
  serialization::writePod(output, payloadSize);

  size_t written = 0;
  while (written < payloadSize) {
    const size_t toWrite = std::min(static_cast<size_t>(1024), static_cast<size_t>(payloadSize - written));
    const size_t bytesWritten = output.write(frameBuffer + written, toWrite);
    if (bytesWritten != toWrite) {
      output.close();
      SdMan.remove(pxcPath.c_str());
      return false;
    }
    written += bytesWritten;
  }

  output.sync();
  output.close();
  return true;
}

bool ImgReaderActivity::saveTransparentOverlayPxaFromPng(const std::string& pngPath, const std::string& pxaPath) {
  if (imageType != ImageType::PNG) {
    return false;
  }

  RenderConfig renderConfig;
  renderConfig.x = 0;
  renderConfig.y = 0;
  renderConfig.maxWidth = renderer.getScreenWidth();
  renderConfig.maxHeight = renderer.getScreenHeight();
  renderConfig.useDithering = true;
  renderConfig.cachePath = "";

  PngToFramebufferConverter pngConverter;
  logTransparentWallpaperMem("start");

  renderer.clearScreen(0xFF);
  logTransparentWallpaperMem("before first decode");
  if (!pngConverter.decodeToFramebuffer(pngPath, renderer, renderConfig)) {
    Serial.printf("Failed to decode PNG for transparent wallpaper (first pass): %s\n", pngPath.c_str());
    logTransparentWallpaperMem("first decode failed");
    return false;
  }
  applyCurrentRenderModeToFramebuffer();
  logTransparentWallpaperMem("after first decode");

  const uint32_t payloadSize = static_cast<uint32_t>(renderer.getBufferSize());
  Serial.printf("[%lu] [IMG] [TPAPER] payloadSize=%lu\n", millis(), static_cast<unsigned long>(payloadSize));
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    Serial.printf("Failed to get framebuffer for transparent wallpaper\n");
    return false;
  }

  SdMan.mkdir("/.crosspoint");

  if (SdMan.exists(TRANSPARENT_WHITE_TMP)) {
    SdMan.remove(TRANSPARENT_WHITE_TMP);
  }

  FsFile whiteTmpOutput;
  if (!SdMan.openFileForWrite("IMG", TRANSPARENT_WHITE_TMP, whiteTmpOutput)) {
    Serial.printf("Failed to open temp file for transparent wallpaper: %s\n", TRANSPARENT_WHITE_TMP);
    return false;
  }

  size_t copied = 0;
  while (copied < payloadSize) {
    const size_t toWrite = std::min(static_cast<size_t>(1024), static_cast<size_t>(payloadSize - copied));
    const size_t written = whiteTmpOutput.write(frameBuffer + copied, toWrite);
    if (written != toWrite) {
      Serial.printf("Failed to write temp white payload at offset=%u\n", static_cast<unsigned>(copied));
      whiteTmpOutput.close();
      SdMan.remove(TRANSPARENT_WHITE_TMP);
      return false;
    }
    copied += written;
  }

  whiteTmpOutput.sync();
  whiteTmpOutput.close();
  logTransparentWallpaperMem("after white temp persisted");

  renderer.clearScreen(0x00);
  logTransparentWallpaperMem("before second decode");
  if (!pngConverter.decodeToFramebuffer(pngPath, renderer, renderConfig)) {
    Serial.printf("Failed to decode PNG for transparent wallpaper (second pass): %s\n", pngPath.c_str());
    logTransparentWallpaperMem("second decode failed");
    SdMan.remove(TRANSPARENT_WHITE_TMP);
    return false;
  }
  applyCurrentRenderModeToFramebuffer();
  logTransparentWallpaperMem("after second decode");

  frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    Serial.printf("Failed to get framebuffer for transparent wallpaper (second pass)\n");
    SdMan.remove(TRANSPARENT_WHITE_TMP);
    return false;
  }

  FsFile whiteTmpInput;
  if (!SdMan.openFileForRead("IMG", TRANSPARENT_WHITE_TMP, whiteTmpInput)) {
    Serial.printf("Failed to open temp white payload for reading: %s\n", TRANSPARENT_WHITE_TMP);
    SdMan.remove(TRANSPARENT_WHITE_TMP);
    return false;
  }

  FsFile output;
  if (!SdMan.openFileForWrite("IMG", pxaPath, output)) {
    Serial.printf("Failed to open output file for transparent wallpaper: %s\n", pxaPath.c_str());
    whiteTmpInput.close();
    SdMan.remove(TRANSPARENT_WHITE_TMP);
    return false;
  }

  const uint8_t reserved = 0;
  const uint32_t maskSize = payloadSize;
  serialization::writePod(output, TRANSPARENT_PXA_MAGIC);
  serialization::writePod(output, TRANSPARENT_PXA_VERSION);
  serialization::writePod(output, SETTINGS.orientation);
  serialization::writePod(output, reserved);
  serialization::writePod(output, payloadSize);
  serialization::writePod(output, maskSize);

  std::array<uint8_t, 1024> whiteChunk{};
  uint32_t offset = 0;

  while (offset < payloadSize) {
    const size_t toRead = std::min(static_cast<size_t>(whiteChunk.size()), static_cast<size_t>(payloadSize - offset));
    const size_t read = whiteTmpInput.read(whiteChunk.data(), toRead);
    if (read != toRead) {
      Serial.printf("Failed to read temp white payload at offset=%u\n", static_cast<unsigned>(offset));
      output.close();
      whiteTmpInput.close();
      SdMan.remove(TRANSPARENT_WHITE_TMP);
      SdMan.remove(pxaPath.c_str());
      return false;
    }

    const size_t written = output.write(whiteChunk.data(), toRead);
    if (written != toRead) {
      Serial.printf("Failed to write white payload to pxa at offset=%u\n", static_cast<unsigned>(offset));
      output.close();
      whiteTmpInput.close();
      SdMan.remove(TRANSPARENT_WHITE_TMP);
      SdMan.remove(pxaPath.c_str());
      return false;
    }

    offset += static_cast<uint32_t>(toRead);
  }

  if (!whiteTmpInput.seek(0)) {
    Serial.printf("Failed to seek temp white payload to start\n");
    output.close();
    whiteTmpInput.close();
    SdMan.remove(TRANSPARENT_WHITE_TMP);
    SdMan.remove(pxaPath.c_str());
    return false;
  }

  offset = 0;
  while (offset < payloadSize) {
    const size_t toRead = std::min(static_cast<size_t>(whiteChunk.size()), static_cast<size_t>(payloadSize - offset));
    const size_t read = whiteTmpInput.read(whiteChunk.data(), toRead);
    if (read != toRead) {
      Serial.printf("Failed to read temp white payload for mask at offset=%u\n", static_cast<unsigned>(offset));
      output.close();
      whiteTmpInput.close();
      SdMan.remove(TRANSPARENT_WHITE_TMP);
      SdMan.remove(pxaPath.c_str());
      return false;
    }

    for (size_t i = 0; i < toRead; ++i) {
      const uint8_t transparentMask = static_cast<uint8_t>(whiteChunk[i] ^ frameBuffer[offset + i]);
      whiteChunk[i] = static_cast<uint8_t>(~transparentMask);
    }

    const size_t written = output.write(whiteChunk.data(), toRead);
    if (written != toRead) {
      Serial.printf("Failed to write mask payload to pxa at offset=%u\n", static_cast<unsigned>(offset));
      output.close();
      whiteTmpInput.close();
      SdMan.remove(TRANSPARENT_WHITE_TMP);
      SdMan.remove(pxaPath.c_str());
      return false;
    }

    offset += static_cast<uint32_t>(toRead);
  }

  output.sync();
  output.close();
  whiteTmpInput.close();
  SdMan.remove(TRANSPARENT_WHITE_TMP);
  logTransparentWallpaperMem("done");
  return true;
}

bool ImgReaderActivity::saveFrameBufferAs1BitBmp(const std::string& bmpPath) const {
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return false;
  }

  if (SdMan.exists(bmpPath.c_str())) {
    SdMan.remove(bmpPath.c_str());
  }

  FsFile output;
  if (!SdMan.openFileForWrite("IMG", bmpPath, output)) {
    return false;
  }

  const int logicalWidth = renderer.getScreenWidth();
  const int logicalHeight = renderer.getScreenHeight();
  const int width = std::min(logicalWidth, logicalHeight);
  const int height = std::max(logicalWidth, logicalHeight);
  const uint32_t rowSize = static_cast<uint32_t>((width + 31) / 32) * 4;
  const uint32_t imageSize = rowSize * static_cast<uint32_t>(height);
  const uint32_t fileSize = 14 + 40 + 8 + imageSize;

  writeByte(output, static_cast<uint8_t>('B'));
  writeByte(output, static_cast<uint8_t>('M'));
  write32(output, fileSize);
  write32(output, 0);
  write32(output, 62);

  write32(output, 40);
  write32(output, static_cast<uint32_t>(width));
  write32(output, static_cast<uint32_t>(height));
  write16(output, 1);
  write16(output, 1);
  write32(output, 0);
  write32(output, imageSize);
  write32(output, 2835);
  write32(output, 2835);
  write32(output, 2);
  write32(output, 2);

  writeByte(output, static_cast<uint8_t>(0x00));
  writeByte(output, static_cast<uint8_t>(0x00));
  writeByte(output, static_cast<uint8_t>(0x00));
  writeByte(output, static_cast<uint8_t>(0x00));
  writeByte(output, static_cast<uint8_t>(0xFF));
  writeByte(output, static_cast<uint8_t>(0xFF));
  writeByte(output, static_cast<uint8_t>(0xFF));
  writeByte(output, static_cast<uint8_t>(0x00));

  const int bytesPerRow = (width + 7) / 8;
  const int padding = static_cast<int>(rowSize) - bytesPerRow;
  const uint8_t pad[3] = {0, 0, 0};

  for (int y = height - 1; y >= 0; --y) {
    const int srcOffset = y * bytesPerRow;
    output.write(frameBuffer + srcOffset, bytesPerRow);
    if (padding > 0) {
      output.write(pad, padding);
    }
  }

  output.sync();
  output.close();
  return true;
}

bool ImgReaderActivity::setAsReadingBackground() {
  if (imageType != ImageType::PNG && imageType != ImageType::JPEG && imageType != ImageType::BMP) {
    return false;
  }

  const auto originalOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  const bool rendered = renderImage();

  const bool pxcSaved = rendered && saveFrameBufferAsPxc(READING_BG_PXC);
  renderer.setOrientation(originalOrientation);

  if (pxcSaved) {
    SETTINGS.ReadingScreenEnabled = 1;
    SETTINGS.saveToFile();
    return true;
  }
  return false;
}

bool ImgReaderActivity::setAsCustomSleepScreen() {
  const auto originalOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  const bool rendered = renderImage();

  bool ok = false;
  if (!rendered) {
    renderer.setOrientation(originalOrientation);
    return false;
  }

  if (imageType == ImageType::BMP) {
    ok = saveFrameBufferAs1BitBmp(CUSTOM_SLEEP_BMP);
    if (ok) {
      SETTINGS.customSleepUsePxc = 0;
    }
  } else {
    ok = saveFrameBufferAsPxc(CUSTOM_SLEEP_PXC);
    if (ok) {
      SETTINGS.customSleepUsePxc = 1;
    }
  }

  renderer.setOrientation(originalOrientation);

  if (ok) {
    SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM;
    SETTINGS.saveToFile();
  }
  return ok;
}

bool ImgReaderActivity::setAsTransparentWallpaper() {
  if (imageType != ImageType::PNG) {
    Serial.printf("Unsupported image type for transparent wallpaper: %d\n", static_cast<int>(imageType));
    return false;
  }

  const bool pxaSaved = saveTransparentOverlayPxaFromPng(imagePath, TRANSPARENT_BG_PXA);
  if (pxaSaved) {
    SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::MARSK2;
    SETTINGS.saveToFile();
  }

  return pxaSaved;
}

void ImgReaderActivity::executeMenuAction() {
  const auto item = static_cast<ActionMenuItem>(menuIndex);
  bool ok = false;

  switch (item) {
    case ActionMenuItem::SET_READING_BG:
      ok = setAsReadingBackground();
      showOperationResult(ok ? "阅读背景设置完成" : "仅支持 PNG/JPG/BMP / 设置失败");
      break;
    case ActionMenuItem::SET_CUSTOM_SLEEP:
      ok = setAsCustomSleepScreen();
      showOperationResult(ok ? "自定义睡眠屏幕保存成功" : "保存失败");
      break;
    case ActionMenuItem::SET_TRANSPARENT_WALLPAPER:
      ok = setAsTransparentWallpaper();
      showOperationResult(ok ? "透明壁纸保存成功" : "仅支持 PNG / 保存失败");
      break;
    case ActionMenuItem::ROTATE_180:
      rotate180Enabled = !rotate180Enabled;
      ok = true;
      showOperationResult(ok ? "旋转成功" : "旋转失败");
      break;
    case ActionMenuItem::FLIP_HORIZONTAL:
      flipHorizontalEnabled = !flipHorizontalEnabled;
      ok = true;
      showOperationResult(ok ? "翻转成功" : "翻转失败");
      break;
  }

  menuVisible = false;
}

bool ImgReaderActivity::inferImageType(const std::string& path, ImageType& outType) const {
  if (StringUtils::checkFileExtension(path, ".png")) {
    outType = ImageType::PNG;
    return true;
  }
  if (StringUtils::checkFileExtension(path, ".jpg") || StringUtils::checkFileExtension(path, ".jpeg")) {
    outType = ImageType::JPEG;
    return true;
  }
  if (StringUtils::checkFileExtension(path, ".bmp")) {
    outType = ImageType::BMP;
    return true;
  }
  return false;
}

std::string ImgReaderActivity::extractFolderPath() const {
  const auto slashPos = imagePath.find_last_of('/');
  if (slashPos == std::string::npos || slashPos == 0) {
    return "/";
  }
  return imagePath.substr(0, slashPos);
}

bool ImgReaderActivity::locateCurrentImageInFolder(const std::string& folderPath) {
  currentFolderImageIndex = -1;
  const auto sortedImages = collectSortedImagePaths(folderPath);
  for (size_t i = 0; i < sortedImages.size(); ++i) {
    if (sortedImages[i] == imagePath) {
      currentFolderImageIndex = static_cast<int>(i);
      break;
    }
  }
  return currentFolderImageIndex >= 0;
}

void ImgReaderActivity::rebuildImageWindow(const std::string& folderPath) {
  folderImagePaths.clear();
  currentImageIndex = -1;
  windowStartImageIndex = 0;

  if (currentFolderImageIndex < 0) {
    return;
  }

  const int minIndex = std::max(0, currentFolderImageIndex - IMAGE_NEARBY_WINDOW);
  const int maxIndex = currentFolderImageIndex + IMAGE_NEARBY_WINDOW;
  windowStartImageIndex = minIndex;
  const auto sortedImages = collectSortedImagePaths(folderPath);
  const int endIndex = std::min(maxIndex, static_cast<int>(sortedImages.size()) - 1);
  for (int i = minIndex; i <= endIndex; ++i) {
    folderImagePaths.push_back(sortedImages[static_cast<size_t>(i)]);
  }

  const int localIndex = currentFolderImageIndex - windowStartImageIndex;
  if (localIndex >= 0 && localIndex < static_cast<int>(folderImagePaths.size())) {
    currentImageIndex = localIndex;
  }
}

bool ImgReaderActivity::resolveImageAtFolderIndex(const std::string& folderPath, int targetIndex, std::string& outPath,
                                                  ImageType& outType) const {
  if (targetIndex < 0) {
    return false;
  }

  const auto sortedImages = collectSortedImagePaths(folderPath);
  if (targetIndex >= static_cast<int>(sortedImages.size())) {
    return false;
  }

  outPath = sortedImages[static_cast<size_t>(targetIndex)];
  return inferImageType(outPath, outType);
}

void ImgReaderActivity::buildFolderImageList() {
  const std::string folderPath = extractFolderPath();
  if (!locateCurrentImageInFolder(folderPath)) {
    return;
  }
  rebuildImageWindow(folderPath);
}

bool ImgReaderActivity::switchImageInFolder(int delta) {
  if (currentFolderImageIndex < 0 || delta == 0) {
    return false;
  }

  const std::string folderPath = extractFolderPath();
  const int targetIndex = currentFolderImageIndex + delta;
  std::string nextPath;
  ImageType nextType = imageType;
  if (!resolveImageAtFolderIndex(folderPath, targetIndex, nextPath, nextType)) {
    return false;
  }

  imagePath = nextPath;
  imageType = nextType;
  currentFolderImageIndex = targetIndex;
  rebuildImageWindow(folderPath);
  renderImage();
  return true;
}

bool ImgReaderActivity::renderPng() {
  RenderConfig renderConfig;
  renderConfig.x = 0;
  renderConfig.y = 0;
  renderConfig.maxWidth = renderer.getScreenWidth();
  renderConfig.maxHeight = renderer.getScreenHeight();
  renderConfig.useDithering = true;
  renderConfig.cachePath = "";

  renderer.clearScreen();
  PngToFramebufferConverter pngConverter;
  if (!pngConverter.decodeToFramebuffer(imagePath, renderer, renderConfig)) {
    return false;
  }
  return true;
}

bool ImgReaderActivity::renderBmp(const std::string& bmpPath) {
  FsFile file;
  if (!SdMan.openFileForRead("IMG", bmpPath, file)) {
    return false;
  }

  Bitmap bitmap(file, true);
  const auto error = bitmap.parseHeaders();
  if (error != BmpReaderError::Ok) {
    file.close();
    return false;
  }

  renderBitmapFit(bitmap);
  file.close();
  return true;
}

bool ImgReaderActivity::renderJpeg() {
  const std::string tempBmpPath = "/.crosspoint/.img_viewer_tmp.bmp";
  SdMan.mkdir("/.crosspoint");

  FsFile jpgFile;
  if (!SdMan.openFileForRead("IMG", imagePath, jpgFile)) {
    return false;
  }

  FsFile bmpFile;
  if (!SdMan.openFileForWrite("IMG", tempBmpPath, bmpFile)) {
    jpgFile.close();
    return false;
  }

  const bool converted = JpegToBmpConverter::jpegFileToBmpStream(jpgFile, bmpFile);
  jpgFile.close();
  bmpFile.close();

  if (!converted) {
    SdMan.remove(tempBmpPath.c_str());
    return false;
  }

  const bool rendered = renderBmp(tempBmpPath);
  SdMan.remove(tempBmpPath.c_str());
  return rendered;
}

void ImgReaderActivity::applyRotate180ToFramebuffer() const {
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return;
  }

  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const int tx = width - 1 - x;
      const int ty = height - 1 - y;
      if (y > ty || (y == ty && x >= tx)) {
        continue;
      }

      const bool src = readFramebufferPixelLogical(frameBuffer, orientation, x, y);
      const bool dst = readFramebufferPixelLogical(frameBuffer, orientation, tx, ty);
      writeFramebufferPixelLogical(frameBuffer, orientation, x, y, dst);
      writeFramebufferPixelLogical(frameBuffer, orientation, tx, ty, src);
    }
  }
}

void ImgReaderActivity::applyCurrentRenderModeToFramebuffer() const {
  if (rotate180Enabled) {
    applyRotate180ToFramebuffer();
  }
  if (flipHorizontalEnabled) {
    applyHorizontalFlipToFramebuffer();
  }
}

void ImgReaderActivity::applyHorizontalFlipToFramebuffer() const {
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return;
  }

  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width / 2; ++x) {
      const int tx = width - 1 - x;
      const bool left = readFramebufferPixelLogical(frameBuffer, orientation, x, y);
      const bool right = readFramebufferPixelLogical(frameBuffer, orientation, tx, y);
      writeFramebufferPixelLogical(frameBuffer, orientation, x, y, right);
      writeFramebufferPixelLogical(frameBuffer, orientation, tx, y, left);
    }
  }
}

void ImgReaderActivity::renderBitmapFit(const Bitmap& bitmap) {
  int x = 0;
  int y = 0;
  float cropX = 0.0f;
  float cropY = 0.0f;

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    if (ratio > screenRatio) {
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropX = 1.0f - (screenRatio / ratio);
        ratio = (1.0f - cropX) * static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
      }
      x = 0;
      y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
    } else {
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropY = 1.0f - (ratio / screenRatio);
        ratio = static_cast<float>(bitmap.getWidth()) / ((1.0f - cropY) * static_cast<float>(bitmap.getHeight()));
      }
      x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
      y = 0;
    }
  } else {
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  renderer.clearScreen();
  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
}
