#include "BookmarkStore.h"

#include <SDCardManager.h>

#include <algorithm>
#include <cctype>

namespace {
std::string toLowerCopy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}
}  // namespace

BookmarkStore::BookType BookmarkStore::detectBookType(const std::string& path) {
  const std::string lower = toLowerCopy(path);
  if (lower.size() >= 5 && lower.substr(lower.size() - 5) == ".epub") {
    return BookType::EPUB;
  }
  if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".xtc") {
    return BookType::XTC;
  }
  if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".txt") {
    return BookType::TXT;
  }
  return BookType::UNKNOWN;
}

bool BookmarkStore::append(const std::string& cachePath, const std::string& bookPath, const int progressPercent,
                           const int32_t pos1, const int32_t pos2, const int32_t pos3) {
  const BookType type = detectBookType(bookPath);
  if (type == BookType::UNKNOWN || cachePath.empty()) {
    return false;
  }

  SdMan.mkdir(cachePath.c_str(), true);
  const std::string bookmarkPath = cachePath + "/bookmark.bin";
  FsFile file = SdMan.open(bookmarkPath.c_str(), O_RDWR | O_CREAT);
  if (!file) {
    return false;
  }

  const uint32_t fileSize = file.fileSize();
  if (!file.seek(fileSize)) {
    file.close();
    return false;
  }

  BookmarkRecord record{};
  record.magic = BOOKMARK_MAGIC;
  record.version = BOOKMARK_VERSION;
  record.bookType = static_cast<uint8_t>(type);
  record.reserved = 0;
  record.savedAtMs = millis();
  record.progressPercent = static_cast<int16_t>(std::max(0, std::min(100, progressPercent)));
  record.pos1 = pos1;
  record.pos2 = pos2;
  record.pos3 = pos3;

  const size_t written = file.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record));
  file.flush();
  file.close();
  return written == sizeof(record);
}

std::vector<BookmarkStore::BookmarkRecord> BookmarkStore::load(const std::string& cachePath,
                                                               const std::string& bookPath) {
  std::vector<BookmarkRecord> result;
  if (cachePath.empty()) {
    return result;
  }

  const BookType type = detectBookType(bookPath);
  if (type == BookType::UNKNOWN) {
    return result;
  }

  FsFile file;
  if (!SdMan.openFileForRead("BMK", cachePath + "/bookmark.bin", file)) {
    return result;
  }

  while (file.available() >= static_cast<int>(sizeof(BookmarkRecord))) {
    BookmarkRecord record{};
    if (file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) != static_cast<int>(sizeof(record))) {
      break;
    }
    if (record.magic != BOOKMARK_MAGIC || record.version != BOOKMARK_VERSION) {
      continue;
    }
    if (record.bookType != static_cast<uint8_t>(type)) {
      continue;
    }
    result.push_back(record);
  }

  file.close();
  return result;
}
