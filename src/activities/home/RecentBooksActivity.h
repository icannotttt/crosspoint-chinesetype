#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <cstdint>
#include <string>
#include <vector>

#include "../Activity.h"
#include "RecentBooksStore.h"

class RecentBooksActivity final : public Activity {
 private:
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;

  size_t selectorIndex = 0;
  bool updateRequired = false;
  int cachedPageStart = -1;
  int cachedListSize = -1;
  bool pageBaseCacheValid = false;
  std::vector<uint8_t> pageBaseCache;

  // Recent tab state
  std::vector<RecentBook> recentBooks;

  // Callbacks
  const std::function<void(const std::string& path)> onSelectBook;
  const std::function<void()> onGoHome;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render();
  void drawBookTile(int bookIndex, int gridX, int gridY, int tileWidth, int tileHeight, int titleLineHeight,
                    bool selected) const;

  // Data loading
  void loadRecentBooks();

 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                               const std::function<void()>& onGoHome,
                               const std::function<void(const std::string& path)>& onSelectBook)
      : Activity("RecentBooks", renderer, mappedInput), onSelectBook(onSelectBook), onGoHome(onGoHome) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
