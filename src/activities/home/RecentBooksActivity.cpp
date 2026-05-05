#include "RecentBooksActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <SDCardManager.h>

#include <algorithm>
#include <cstring>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "fontIds.h"
#include "util/StringUtils.h"

namespace {
constexpr int GRID_COLS = 3;
constexpr int GRID_ROWS = 3;
constexpr int ITEMS_PER_PAGE = GRID_COLS * GRID_ROWS;
constexpr int TILE_SPACING_X = 8;
constexpr int TILE_SPACING_Y = 10;
constexpr int TILE_PADDING = 6;
constexpr int TITLE_BOTTOM_PADDING = 4;
constexpr int SELECTION_RADIUS = 6;
constexpr unsigned long SKIP_PAGE_MS = 700;

int clampPercent(const int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

bool tryReadEpubBookProgressPercent(const std::string& bookPath, int& outPercent) {
  if (!StringUtils::checkFileExtension(bookPath, ".epub")) {
    return false;
  }

  Epub epub(bookPath, "/.crosspoint");
  if (!epub.load(false, true)) {
    return false;
  }

  const int spineCount = epub.getSpineItemsCount();
  if (spineCount <= 0) {
    return false;
  }

  FsFile progressFile;
  if (!SdMan.openFileForRead("RPR", epub.getCachePath() + "/progress.bin", progressFile)) {
    return false;
  }

  uint8_t data[6] = {0};
  const int dataSize = progressFile.read(data, sizeof(data));
  progressFile.close();
  if (dataSize != 4 && dataSize != 6) {
    return false;
  }

  const int currentSpineIndex = data[0] + (data[1] << 8);
  if (currentSpineIndex < 0 || currentSpineIndex >= spineCount) {
    return false;
  }

  const int currentPage = data[2] + (data[3] << 8);
  int pageCount = 0;
  if (dataSize == 6) {
    pageCount = data[4] + (data[5] << 8);
  }

  float chapterProgress = 0.0f;
  if (pageCount > 0) {
    chapterProgress = static_cast<float>(currentPage) / static_cast<float>(pageCount);
    if (chapterProgress < 0.0f) {
      chapterProgress = 0.0f;
    } else if (chapterProgress > 1.0f) {
      chapterProgress = 1.0f;
    }
  }

  const float bookProgress = epub.calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
  outPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
  return true;
}
}  // namespace

void RecentBooksActivity::taskTrampoline(void* param) {
  auto* self = static_cast<RecentBooksActivity*>(param);
  self->displayTaskLoop();
}

void RecentBooksActivity::loadRecentBooks() {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(books.size());

  for (const auto& book : books) {
    // Skip if file no longer exists
    if (!SdMan.exists(book.path.c_str())) {
      continue;
    }
    recentBooks.push_back(book);
    recentBooks.back().progressPercent = -1;
    int progressPercent = 0;
    if (tryReadEpubBookProgressPercent(recentBooks.back().path, progressPercent)) {
      recentBooks.back().progressPercent = progressPercent;
    }
  }
}

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  // Load data
  loadRecentBooks();
  pageBaseCache.clear();
  cachedPageStart = -1;
  cachedListSize = -1;
  pageBaseCacheValid = false;

  selectorIndex = 0;
  updateRequired = true;

  xTaskCreate(&RecentBooksActivity::taskTrampoline, "RecentBooksActivityTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void RecentBooksActivity::onExit() {
  Activity::onExit();

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;

  recentBooks.clear();
  pageBaseCache.clear();
  cachedPageStart = -1;
  cachedListSize = -1;
  pageBaseCacheValid = false;
}

void RecentBooksActivity::loop() {
  const bool upReleased = mappedInput.wasReleased(MappedInputManager::Button::Up);
  const bool downReleased = mappedInput.wasReleased(MappedInputManager::Button::Down);
  const bool leftReleased = mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool rightReleased = mappedInput.wasReleased(MappedInputManager::Button::Right);

  const bool prevReleased = upReleased || leftReleased;
  const bool nextReleased = downReleased || rightReleased;
  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!recentBooks.empty() && selectorIndex < static_cast<int>(recentBooks.size())) {
      Serial.printf("Selected recent book: %s\n", recentBooks[selectorIndex].path.c_str());
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
  }

  const int listSize = static_cast<int>(recentBooks.size());
  if (listSize <= 0) {
    return;
  }

  const int pageCount = (listSize + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE;
  int currentPage = static_cast<int>(selectorIndex) / ITEMS_PER_PAGE;
  int indexInPage = static_cast<int>(selectorIndex) % ITEMS_PER_PAGE;

  if (prevReleased) {
    if (skipPage && pageCount > 1) {
      currentPage = (currentPage + pageCount - 1) % pageCount;
      const int pageStart = currentPage * ITEMS_PER_PAGE;
      const int itemsOnPage = std::min(ITEMS_PER_PAGE, listSize - pageStart);
      if (indexInPage >= itemsOnPage) {
        indexInPage = itemsOnPage - 1;
      }
      selectorIndex = pageStart + indexInPage;
    } else {
      selectorIndex = (selectorIndex + listSize - 1) % listSize;
    }
    updateRequired = true;
  } else if (nextReleased) {
    if (skipPage && pageCount > 1) {
      currentPage = (currentPage + 1) % pageCount;
      const int pageStart = currentPage * ITEMS_PER_PAGE;
      const int itemsOnPage = std::min(ITEMS_PER_PAGE, listSize - pageStart);
      if (indexInPage >= itemsOnPage) {
        indexInPage = itemsOnPage - 1;
      }
      selectorIndex = pageStart + indexInPage;
    } else {
      selectorIndex = (selectorIndex + 1) % listSize;
    }
    updateRequired = true;
  }
}

void RecentBooksActivity::displayTaskLoop() {
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

void RecentBooksActivity::drawBookTile(const int bookIndex, const int gridX, const int gridY, const int tileWidth,
                                       const int tileHeight, const int titleLineHeight, const bool selected) const {
  if (bookIndex < 0 || bookIndex >= static_cast<int>(recentBooks.size())) {
    return;
  }

  const int slot = bookIndex % ITEMS_PER_PAGE;
  const int row = slot / GRID_COLS;
  const int col = slot % GRID_COLS;

  const int tileX = gridX + col * (tileWidth + TILE_SPACING_X);
  const int tileY = gridY + row * (tileHeight + TILE_SPACING_Y);

  if (selected) {
    renderer.fillRoundedRect(tileX, tileY, tileWidth, tileHeight, SELECTION_RADIUS, Color::LightGray);
  } else {
    renderer.fillRect(tileX, tileY, tileWidth, tileHeight, false);
    renderer.drawRect(tileX, tileY, tileWidth, tileHeight, true);
  }

  const int coverX = tileX + TILE_PADDING;
  const int coverY = tileY + TILE_PADDING;
  const int titleAreaHeight = titleLineHeight + TITLE_BOTTOM_PADDING + TILE_PADDING;
  const int coverWidth = tileWidth - TILE_PADDING * 2;
  const int coverHeight = tileHeight - titleAreaHeight - TILE_PADDING;

  bool drewCover = false;
  if (!recentBooks[bookIndex].coverBmpPath.empty()) {
    FsFile file;
    const std::string coverPathCurrent = UITheme::getCoverThumbPath(recentBooks[bookIndex].coverBmpPath, coverHeight);
    const std::string coverPathHome =
        UITheme::getCoverThumbPath(recentBooks[bookIndex].coverBmpPath, UITheme::getInstance().getMetrics().homeCoverHeight);

    if (SdMan.openFileForRead("RecentBooks", coverPathCurrent, file) ||
        (!drewCover && coverPathHome != coverPathCurrent && SdMan.openFileForRead("RecentBooks", coverPathHome, file))) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        float scale = 1.0f;
        if (bitmap.getWidth() > coverWidth || bitmap.getHeight() > coverHeight) {
          const float sx = static_cast<float>(coverWidth) / static_cast<float>(bitmap.getWidth());
          const float sy = static_cast<float>(coverHeight) / static_cast<float>(bitmap.getHeight());
          scale = std::min(sx, sy);
        }
        const int renderedWidth = static_cast<int>(bitmap.getWidth() * scale);
        const int drawCoverX = coverX + std::max(0, (coverWidth - renderedWidth) / 2);
        renderer.drawBitmap(bitmap, drawCoverX, coverY, coverWidth, coverHeight);
        drewCover = true;
      }
      file.close();
    }
  }

  if (!drewCover) {
    renderer.drawRect(coverX, coverY, coverWidth, coverHeight, true);
    renderer.drawIcon(CoverIcon, coverX + (coverWidth - 32) / 2, coverY + (coverHeight - 32) / 2, 32, 32);
  }

  if (StringUtils::checkFileExtension(recentBooks[bookIndex].path, ".epub") && recentBooks[bookIndex].progressPercent >= 0) {
    const int progressBarHeight = 4;
    const int progressBarPadding = 4;
    const int progressBarWidth = std::max(0, coverWidth - progressBarPadding * 2);
    const int progressBarX = coverX + progressBarPadding;
    const int progressBarY = coverY + coverHeight - progressBarHeight - 2;
    const int progressInnerHeight = std::max(0, progressBarHeight - 2);
    const int progressInnerWidth = std::max(0, progressBarWidth - 2);
    const int progressFillWidth = (progressInnerWidth * recentBooks[bookIndex].progressPercent) / 100;

    if (progressBarWidth > 0 && progressBarHeight > 0) {
      renderer.fillRect(progressBarX, progressBarY, progressBarWidth, progressBarHeight, false);
      renderer.drawRect(progressBarX, progressBarY, progressBarWidth, progressBarHeight, true);
      if (progressFillWidth > 0 && progressInnerHeight > 0) {
        renderer.fillRect(progressBarX + 1, progressBarY + 1, progressFillWidth, progressInnerHeight, true);
      }
    }
  }

  const std::string title = renderer.truncatedText(UI_10_FONT_ID, recentBooks[bookIndex].title.c_str(), coverWidth);
  renderer.drawText(UI_10_FONT_ID, coverX, tileY + tileHeight - titleLineHeight - TITLE_BOTTOM_PADDING, title.c_str(), true);
}

void RecentBooksActivity::render() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int gridX = metrics.contentSidePadding;
  const int gridY = contentTop;
  const int gridWidth = pageWidth - metrics.contentSidePadding * 2;
  const int gridHeight = contentHeight;
  const int tileWidth = (gridWidth - TILE_SPACING_X * (GRID_COLS - 1)) / GRID_COLS;
  const int tileHeight = (gridHeight - TILE_SPACING_Y * (GRID_ROWS - 1)) / GRID_ROWS;
  const int titleLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int listSize = static_cast<int>(recentBooks.size());
  const int pageStart = listSize > 0 ? (static_cast<int>(selectorIndex) / ITEMS_PER_PAGE) * ITEMS_PER_PAGE : 0;
  const int pageEnd = std::min(pageStart + ITEMS_PER_PAGE, listSize);

  const bool canUseBaseCache = pageBaseCacheValid && !pageBaseCache.empty() && cachedPageStart == pageStart &&
                               cachedListSize == listSize;

  if (canUseBaseCache) {
    std::memcpy(renderer.getFrameBuffer(), pageBaseCache.data(), pageBaseCache.size());
  } else {
    renderer.clearScreen();
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Recent Books");

    if (recentBooks.empty()) {
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, "No recent books");
    } else {
      for (int i = pageStart; i < pageEnd; i++) {
        drawBookTile(i, gridX, gridY, tileWidth, tileHeight, titleLineHeight, false);
      }
    }

    const auto labels = mappedInput.mapLabels("« 主页", "打开", "上一个", "下一个");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    const auto bufferSize = renderer.getBufferSize();
    pageBaseCache.resize(bufferSize);
    std::memcpy(pageBaseCache.data(), renderer.getFrameBuffer(), bufferSize);
    pageBaseCacheValid = true;
    cachedPageStart = pageStart;
    cachedListSize = listSize;
  }

  // Recent grid (3x3)
  if (!recentBooks.empty() && selectorIndex < static_cast<size_t>(pageEnd)) {
    drawBookTile(static_cast<int>(selectorIndex), gridX, gridY, tileWidth, tileHeight, titleLineHeight, true);
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
