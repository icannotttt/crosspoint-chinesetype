#pragma once

#include <SDCardManager.h>

#include <memory>
#include <string>

// 宏定义常量 
#define MAX_SAVE_CHAPTER  30    // 最多存30章
#define TITLE_KEEP_LENGTH 20    // 标题截取前20个UTF8字符
#define TITLE_BUF_SIZE    64    // 标题缓冲区64字节，完美匹配你的static char title[64]
#define MAX_SAVE_PAGE 100

// 目录结构体
struct ChapterData {
    int chapterIndex;        // 章节序号
    uint32_t byteOffset;     // 字节偏移量
    char shortTitle[TITLE_BUF_SIZE]; // 截取后的标题，char数组格式
    uint32_t endOffset;      // 章节结束的字节偏移（可选，预留字段）
};

class Txt {
  std::string filepath;
  std::string cacheBasePath;
  std::string cachePath;
  bool loaded = false;
  size_t fileSize = 0;

  //加目录

  ChapterData chapterDataList[MAX_SAVE_CHAPTER];
  int chapterActualCount = 0;
  void splitChaptersByNewline(); 
  void saveChapterToTxt(int startChapter);
  bool loadChapterFromTxt(int startChapter);
  
  bool m_isVolumeOnlyBook = false;



 public:
  explicit Txt(std::string path, std::string cacheBasePath);

  bool load();
  [[nodiscard]] const std::string& getPath() const { return filepath; }
  [[nodiscard]] const std::string& getCachePath() const { return cachePath; }
  [[nodiscard]] std::string getTitle() const;
  [[nodiscard]] size_t getFileSize() const { return fileSize; }

  void setupCacheDir() const;

  // Cover image support - looks for cover.bmp/jpg/jpeg/png in same folder as txt file
  [[nodiscard]] std::string getCoverBmpPath() const;
  [[nodiscard]] bool generateCoverBmp() const;
  [[nodiscard]] std::string findCoverImage() const;

  // Read content from file
  [[nodiscard]] bool readContent(uint8_t* buffer, size_t offset, size_t length) const;
  //加目录

  uint32_t getChapterOffsetByIndex(int chapterIndex) {
      for(int i = 0; i < chapterActualCount; i++) {
          if(chapterDataList[i].chapterIndex == chapterIndex) {
              return chapterDataList[i].byteOffset;
          }
      }
      return 0; // 无此章节返回0
  }
uint32_t getChapterendOffsetByIndex(int chapterIndex) {
      for(int i = 0; i < chapterActualCount; i++) {
          if(chapterDataList[i].chapterIndex == chapterIndex) {
              return chapterDataList[i].endOffset;
          }
      }
      return 0; // 无此章节返回0
  }
  std::string getChapterTitleByIndex(int chapterIndex) {
      for(int i = 0; i < chapterActualCount; i++) {
          if(chapterDataList[i].chapterIndex == chapterIndex) {
              return std::string(chapterDataList[i].shortTitle);
          }
      }
      return ""; // 无此章节返回空字符串
  }

  // ✅ 补充章节存在判断接口（和上面配套，之前漏掉了，已补全）
  bool isChapterExist(int chapterIndex) {
      for(int i = 0; i < chapterActualCount; i++) {
          if(chapterDataList[i].chapterIndex == chapterIndex) {
              return true;
          }
      }
      return false;
  }

  void parseChapterIndexAndOffset(int n);


};
