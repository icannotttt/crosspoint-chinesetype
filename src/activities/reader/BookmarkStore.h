#pragma once

#include <cstdint>
#include <string>
#include <vector>

class BookmarkStore {
 public:
  enum class BookType : uint8_t {
    EPUB = 1,
    XTC = 2,
    TXT = 3,
    UNKNOWN = 255,
  };

  struct BookmarkRecord {
    uint32_t magic;
    uint16_t version;
    uint8_t bookType;
    uint8_t reserved;
    uint32_t savedAtMs;
    int16_t progressPercent;
    int32_t pos1;
    int32_t pos2;
    int32_t pos3;
  };

  static constexpr uint32_t BOOKMARK_MAGIC = 0x314B4D42;  // "BMK1"
  static constexpr uint16_t BOOKMARK_VERSION = 1;

  static BookType detectBookType(const std::string& path);
  static bool append(const std::string& cachePath, const std::string& bookPath, int progressPercent, int32_t pos1,
                     int32_t pos2, int32_t pos3);
  static std::vector<BookmarkRecord> load(const std::string& cachePath, const std::string& bookPath);
};
