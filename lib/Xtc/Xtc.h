/**
 * Xtc.h
 *
 * Main XTC ebook class for CrossPoint Reader
 * Provides EPUB-like interface for XTC file handling
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Xtc/XtcParser.h"
#include "Xtc/XtcTypes.h"

/**
 * XTC Ebook Handler
 *
 * Handles XTC file loading, page access, and cover image generation.
 * Interface is designed to be similar to Epub class for easy integration.
 */
class Xtc {
  std::string filepath;
  std::string cachePath;
  std::unique_ptr<xtc::XtcParser> parser;
  bool loaded;
  bool scaleCoverToThumb(int height) const;

 public:
  explicit Xtc(std::string filepath, const std::string& cacheDir) : filepath(std::move(filepath)), loaded(false) {
    // Create cache key based on filepath (same as Epub)
    cachePath = cacheDir + "/xtc_" + std::to_string(std::hash<std::string>{}(this->filepath));
  }
  ~Xtc() = default;

  /**
   * Load XTC file
   * @return true on success
   */
  bool load();

  /**
   * Clear cached data
   * @return true on success
   */
  bool clearCache() const;

  /**
   * Setup cache directory
   */
  void setupCacheDir() const;

  // Path accessors
  const std::string& getCachePath() const { return cachePath; }
  const std::string& getPath() const { return filepath; }

  // Metadata
  std::string getTitle() const;
  std::string getAuthor() const;
  bool hasChapters() const;
  const std::vector<xtc::ChapterInfo>& getChapters() const;

      // ================ ✅ 新增：暴露动态加载的3个核心对外接口 ================
  /**
   * 动态加载下一批页码（内部调用 XtcParser 的 loadNextPageBatch）
   */
  xtc::XtcError loadNextPageBatch() const {
      return parser ? parser->loadNextPageBatch() : xtc::XtcError::FILE_NOT_FOUND;
  }

  xtc::XtcError loadPageBatchByStart(uint16_t startPage) const {
      return parser ? parser->loadPageBatchByStart(startPage): xtc::XtcError::FILE_NOT_FOUND;
  }

  /**
   * 获取当前已加载的最大页码
   */
  uint16_t getLoadedMaxPage() const {
      return parser ? parser->getLoadedMaxPage() : 0;
  }

  uint16_t getchapter(uint16_t pageNum) {
      return parser ? parser->getChapterIndexByPage(pageNum) : 0;
  }

  /**
   * 获取每次加载的批次页数（默认10）
   */
  uint16_t getPageBatchSize() const {
      return parser ? parser->getPageBatchSize() : 10;
  }
  xtc::XtcError readChapters_gd(uint16_t chapterStart) const {
      return parser ? parser->readChapters_gd(chapterStart) : xtc::XtcError::FILE_NOT_FOUND;
  }
uint32_t getChapterstartpage(int chapterIndex) {
  return parser ? parser->getChapterstartpage(chapterIndex) : 0;
}
std::string getChapterTitleByIndex(int chapterIndex) {
    return parser ? parser->getChapterTitleByIndex(chapterIndex) : "";
}

void releasePageBatchByStart(uint16_t startPage){    
    if (parser) {
        parser->releasePageBatchByStart(startPage);
    }
}
    bool getPageInfo(uint32_t pageIndex, xtc::PageInfo& info) {
        // 修正2：空指针校验 + 正确调用 parser 的 getPageInfo
        if (parser) {
            // 修正3：返回 parser->getPageInfo 的结果（bool 类型）
            return parser->getPageInfo(pageIndex, info);
        }
        // 修正4：无 parser 时返回 false（补全返回值）
        Serial.printf("[%lu] [XTC] getPageInfo失败：parser 为空\n", millis());
        return false;
    } 
size_t getmaxchapter(){    
    if (parser) {
        return parser->maxChapterCount;
    }
};


  
  // ======================================================================

 

  // Cover image support (for sleep screen)
  std::string getCoverBmpPath() const;
  bool generateCoverBmp() const;
  // Thumbnail support (for Continue Reading card)
  std::string getThumbBmpPath() const;
  std::string getThumbBmpPath(int height) const;
  bool generateThumbBmp(int height) const;

  // Page access
  uint32_t getPageCount() const;
  uint16_t getPageWidth() const;
  uint16_t getPageHeight() const;
  uint8_t getBitDepth() const;  // 1 = XTC (1-bit), 2 = XTCH (2-bit)

  /**
   * Load page bitmap data
   * @param pageIndex Page index (0-based)
   * @param buffer Output buffer
   * @param bufferSize Buffer size
   * @return Number of bytes read
   */
  size_t loadPage(uint32_t pageIndex, uint8_t* buffer, size_t bufferSize) const;

  /**
   * Load page with streaming callback
   * @param pageIndex Page index
   * @param callback Callback for each chunk
   * @param chunkSize Chunk size
   * @return Error code
   */
  xtc::XtcError loadPageStreaming(uint32_t pageIndex,
                                  std::function<void(const uint8_t* data, size_t size, size_t offset)> callback,
                                  size_t chunkSize = 1024) const;

  // Progress calculation
  uint8_t calculateProgress(uint32_t currentPage) const;

  // Check if file is loaded
  bool isLoaded() const { return loaded; }

  // Error information
  xtc::XtcError getLastError() const;
};
