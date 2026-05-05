#pragma once

#include <functional>
#include <string>
#include <vector>

#include <Bitmap.h>
#include <JpegToBmpConverter.h>
#include <HalDisplay.h>

#include "../Activity.h"
#include "../../../lib/Epub/Epub/converters/PngToFramebufferConverter.h"

class ImgReaderActivity final : public Activity {
 public:
  enum class ImageType { PNG, JPEG, BMP };
  enum class ActionMenuItem { SET_READING_BG = 0, SET_CUSTOM_SLEEP = 1, SET_TRANSPARENT_WALLPAPER = 2, ROTATE_180 = 3, FLIP_HORIZONTAL = 4 };

 private:
  std::string imagePath;
  ImageType imageType;
  std::vector<std::string> folderImagePaths;
  int currentImageIndex = -1;
  int currentFolderImageIndex = -1;
  int windowStartImageIndex = 0;
  bool rotate180Enabled = false;
  bool flipHorizontalEnabled = false;
  bool menuVisible = false;
  int menuIndex = 0;
  const std::function<void(const std::string&)> onGoBack;
  const std::function<void()> onGoHome;

  void renderMenuOverlay() const;
  void showOperationResult(const char* message);
  bool saveFrameBufferAsPxc(const std::string& pxcPath) const;
  bool saveTransparentOverlayPxaFromPng(const std::string& pngPath, const std::string& pxaPath);
  bool saveFrameBufferAs1BitBmp(const std::string& bmpPath) const;
  bool setAsReadingBackground();
  bool setAsCustomSleepScreen();
  bool setAsTransparentWallpaper();
  void executeMenuAction();
  bool inferImageType(const std::string& path, ImageType& outType) const;
  std::string extractFolderPath() const;
  bool locateCurrentImageInFolder(const std::string& folderPath);
  void rebuildImageWindow(const std::string& folderPath);
  bool resolveImageAtFolderIndex(const std::string& folderPath, int targetIndex, std::string& outPath,
                                 ImageType& outType) const;
  void buildFolderImageList();
  bool switchImageInFolder(int delta);
  void applyCurrentRenderModeToFramebuffer() const;
  void applyRotate180ToFramebuffer() const;
  void applyHorizontalFlipToFramebuffer() const;

  bool renderImage();
  void renderBitmapFit(const Bitmap& bitmap);
  bool renderPng();
  bool renderBmp(const std::string& bmpPath);
  bool renderJpeg();

 public:
  explicit ImgReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string imagePath,
                             const ImageType imageType, const std::function<void(const std::string&)>& onGoBack,
                             const std::function<void()>& onGoHome)
      : Activity("ImgReader", renderer, mappedInput),
        imagePath(std::move(imagePath)),
        imageType(imageType),
        onGoBack(onGoBack),
        onGoHome(onGoHome) {}

  void onEnter() override;
  void loop() override;
};
