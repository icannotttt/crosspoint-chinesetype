#pragma once

#include <functional>
#include <string>

#include "activities/Activity.h"
#include "BookmarkStore.h"

class BookmarkActivity final : public Activity {
 public:
  explicit BookmarkActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                            std::string cachePath, int progressPercent, int32_t pos1, int32_t pos2, int32_t pos3,
                            const std::function<void(bool)>& onDone)
      : Activity("Bookmark", renderer, mappedInput),
        bookPath(std::move(bookPath)),
        cachePath(std::move(cachePath)),
        progressPercent(progressPercent),
        pos1(pos1),
        pos2(pos2),
        pos3(pos3),
        onDone(onDone) {}

  void onEnter() override;

 private:
  std::string bookPath;
  std::string cachePath;
  int progressPercent;
  int32_t pos1;
  int32_t pos2;
  int32_t pos3;
  std::function<void(bool)> onDone;
};
