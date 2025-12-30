#pragma once
#include <memory>

#include "../ActivityWithSubactivity.h"

class Epub;
class Bitmap;

class ReaderActivity final : public ActivityWithSubactivity {
  std::string initialEpubPath;
  const std::function<void()> onGoBack;
  static std::unique_ptr<Epub> loadEpub(const std::string& path);
  bool loadBmp(const std::string& path, Bitmap& bitmap);

  void onSelectEpubFile(const std::string& path);
  void onGoToFileSelection();
  void onGoToEpubReader(std::unique_ptr<Epub> epub);
  void renderBitmap(const Bitmap& bitmap) const;

 public:
  explicit ReaderActivity(GfxRenderer& renderer, InputManager& inputManager, std::string initialEpubPath,
                          const std::function<void()>& onGoBack)
      : ActivityWithSubactivity("Reader", renderer, inputManager),
        initialEpubPath(std::move(initialEpubPath)),
        onGoBack(onGoBack) {}
  void onEnter() override;
};
