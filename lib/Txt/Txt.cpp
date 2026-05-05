#include "Txt.h"

#include <FsHelpers.h>
#include <JpegToBmpConverter.h>
#include <Serialization.h>

namespace {
constexpr int CHAPTER_BATCH_SIZE = 15;
constexpr int CHAPTER_CACHE_SPAN = CHAPTER_BATCH_SIZE + 1;
constexpr int CHAPTER_CACHE_LAST_OFFSET = CHAPTER_CACHE_SPAN - 1;
constexpr uint32_t TYPE_CACHE_MAGIC = 0x54595045; // "TYPE"
constexpr uint32_t TYPE_CACHE_VERSION = 1;
}

Txt::Txt(std::string path, std::string cacheBasePath)
    : filepath(std::move(path)), cacheBasePath(std::move(cacheBasePath)) {
  // Generate cache path from file path hash
  const size_t hash = std::hash<std::string>{}(filepath);
  cachePath = this->cacheBasePath + "/txt_" + std::to_string(hash);
}

Txt::~Txt() {
    if (streamingReadFileOpen) {
        streamingReadFile.close();
        streamingReadFileOpen = false;
    }
}

bool Txt::load() {
  if (loaded) {
    return true;
  }

  if (!SdMan.exists(filepath.c_str())) {
    Serial.printf("[%lu] [TXT] File does not exist: %s\n", millis(), filepath.c_str());
    return false;
  }

  FsFile file;
  if (!SdMan.openFileForRead("TXT", filepath, file)) {
    Serial.printf("[%lu] [TXT] Failed to open file: %s\n", millis(), filepath.c_str());
    return false;
  }

  fileSize = file.size();
  file.close();

  loaded = true;
  Serial.printf("[%lu] [TXT] Loaded TXT file: %s (%zu bytes)\n", millis(), filepath.c_str(), fileSize);
  return true;
}

std::string Txt::getTitle() const {
  // Extract filename without path and extension
  size_t lastSlash = filepath.find_last_of('/');
  std::string filename = (lastSlash != std::string::npos) ? filepath.substr(lastSlash + 1) : filepath;

  // Remove .txt extension
  if (filename.length() >= 4 && filename.substr(filename.length() - 4) == ".txt") {
    filename = filename.substr(0, filename.length() - 4);
  }

  return filename;
}

void Txt::setupCacheDir() const {
  if (!SdMan.exists(cacheBasePath.c_str())) {
    SdMan.mkdir(cacheBasePath.c_str());
  }
  if (!SdMan.exists(cachePath.c_str())) {
    SdMan.mkdir(cachePath.c_str());
  }
}

std::string Txt::findCoverImage() const {
  // Get the folder containing the txt file
  size_t lastSlash = filepath.find_last_of('/');
  std::string folder = (lastSlash != std::string::npos) ? filepath.substr(0, lastSlash) : "";
  if (folder.empty()) {
    folder = "/";
  }

  // Get the base filename without extension (e.g., "mybook" from "/books/mybook.txt")
  std::string baseName = getTitle();

  // Image extensions to try
  const char* extensions[] = {".bmp", ".jpg", ".jpeg", ".png", ".BMP", ".JPG", ".JPEG", ".PNG"};

  // First priority: look for image with same name as txt file (e.g., mybook.jpg)
  for (const auto& ext : extensions) {
    std::string coverPath = folder + "/" + baseName + ext;
    if (SdMan.exists(coverPath.c_str())) {
      Serial.printf("[%lu] [TXT] Found matching cover image: %s\n", millis(), coverPath.c_str());
      return coverPath;
    }
  }

  // Fallback: look for cover image files
  const char* coverNames[] = {"cover", "Cover", "COVER"};
  for (const auto& name : coverNames) {
    for (const auto& ext : extensions) {
      std::string coverPath = folder + "/" + std::string(name) + ext;
      if (SdMan.exists(coverPath.c_str())) {
        Serial.printf("[%lu] [TXT] Found fallback cover image: %s\n", millis(), coverPath.c_str());
        return coverPath;
      }
    }
  }

  return "";
}

std::string Txt::getCoverBmpPath() const { return cachePath + "/cover.bmp"; }

std::string Txt::getChapterCacheFilePath(int startChapter) const {
    char cacheFile[128] = {0};
    if (m_isVolumeOnlyBook) {
        snprintf(cacheFile, sizeof(cacheFile), "%s/sentence_chapters_%d_%d.bin", getCachePath().c_str(), startChapter,
                 CHAPTER_BATCH_SIZE);
    } else {
        snprintf(cacheFile, sizeof(cacheFile), "%s/chapters_%d_%d.bin", getCachePath().c_str(), startChapter,
                 CHAPTER_BATCH_SIZE);
    }
    return cacheFile;
}

bool Txt::generateCoverBmp() const {
  // Already generated, return true
  if (SdMan.exists(getCoverBmpPath().c_str())) {
    return true;
  }

  std::string coverImagePath = findCoverImage();
  if (coverImagePath.empty()) {
    Serial.printf("[%lu] [TXT] No cover image found for TXT file\n", millis());
    return false;
  }

  // Setup cache directory
  setupCacheDir();

  // Get file extension
  const size_t len = coverImagePath.length();
  const bool isJpg =
      (len >= 4 && (coverImagePath.substr(len - 4) == ".jpg" || coverImagePath.substr(len - 4) == ".JPG")) ||
      (len >= 5 && (coverImagePath.substr(len - 5) == ".jpeg" || coverImagePath.substr(len - 5) == ".JPEG"));
  const bool isBmp = len >= 4 && (coverImagePath.substr(len - 4) == ".bmp" || coverImagePath.substr(len - 4) == ".BMP");

  if (isBmp) {
    // Copy BMP file to cache
    Serial.printf("[%lu] [TXT] Copying BMP cover image to cache\n", millis());
    FsFile src, dst;
    if (!SdMan.openFileForRead("TXT", coverImagePath, src)) {
      return false;
    }
    if (!SdMan.openFileForWrite("TXT", getCoverBmpPath(), dst)) {
      src.close();
      return false;
    }
    uint8_t buffer[1024];
    while (src.available()) {
      size_t bytesRead = src.read(buffer, sizeof(buffer));
      dst.write(buffer, bytesRead);
    }
    src.close();
    dst.close();
    Serial.printf("[%lu] [TXT] Copied BMP cover to cache\n", millis());
    return true;
  }

  if (isJpg) {
    // Convert JPG/JPEG to BMP (same approach as Epub)
    Serial.printf("[%lu] [TXT] Generating BMP from JPG cover image\n", millis());
    FsFile coverJpg, coverBmp;
    if (!SdMan.openFileForRead("TXT", coverImagePath, coverJpg)) {
      return false;
    }
    if (!SdMan.openFileForWrite("TXT", getCoverBmpPath(), coverBmp)) {
      coverJpg.close();
      return false;
    }
    const bool success = JpegToBmpConverter::jpegFileToBmpStream(coverJpg, coverBmp);
    coverJpg.close();
    coverBmp.close();

    if (!success) {
      Serial.printf("[%lu] [TXT] Failed to generate BMP from JPG cover image\n", millis());
      SdMan.remove(getCoverBmpPath().c_str());
    } else {
      Serial.printf("[%lu] [TXT] Generated BMP from JPG cover image\n", millis());
    }
    return success;
  }

  // PNG files are not supported (would need a PNG decoder)
  Serial.printf("[%lu] [TXT] Cover image format not supported (only BMP/JPG/JPEG)\n", millis());
  return false;
}




bool Txt::readContent(uint8_t* buffer, size_t offset, size_t length) const {
  if (!loaded) {
    return false;
  }

    if (!streamingReadFileOpen) {
        if (!SdMan.openFileForRead("TXT", filepath, streamingReadFile)) {
            return false;
        }
        streamingReadFileOpen = true;
    }

    if (!streamingReadFile.seek(offset)) {
        return false;
    }

    size_t bytesRead = streamingReadFile.read(buffer, length);

  return bytesRead > 0;
}


//加目录
void Txt::parseChapterIndexAndOffset(int n) {
    const unsigned long parseStartMs = millis();
    char readBuffer[128] = {0};
    int bufferLen = 0;
    constexpr bool VERBOSE_CHAPTER_PARSE_LOG = false;

    // 配置参数（保持不变）
    const int CHAPTER_START = n / CHAPTER_BATCH_SIZE * CHAPTER_BATCH_SIZE;
    const int CHAPTER_END = CHAPTER_START + CHAPTER_CACHE_LAST_OFFSET;
    const uint32_t VOLUME_PAGE_SIZE = 2880;
    const uint64_t CHAPTER_CHECK_THRESHOLD = VOLUME_PAGE_SIZE;
    const uint64_t MAX_NEXT_SEARCH = 2 * VOLUME_PAGE_SIZE;

    uint64_t chapterScanStartOffset = 0;
    int chapterScanStartIndex = 0;

    Serial.printf("[ChapterRange] ✅ 本次加载范围：%d ~ %d\n", CHAPTER_START, CHAPTER_END);

    // ========== 2. 初始化 + 获取文件总大小（保持） ==========
    chapterActualCount = 0;
    memset(chapterDataList, 0, sizeof(chapterDataList));
    uint64_t fileSize = 0;
    FsFile sizeFile;
    if (sizeFile.open(filepath.c_str(), FILE_READ)) {
        fileSize = sizeFile.size();
        sizeFile.close();
    } else {
        Serial.printf("[Parser] ❌ 无法获取文件大小，endOffset将设为0\n");
        return;
    }

    // ========== 3. 分卷/章节模式检测（优先读取type缓存） ==========
    bool typeCacheLoaded = false;
    setupCacheDir();
    const std::string typeCachePath = getCachePath() + "/type.bin";

    {
        FsFile typeFile;
        if (SdMan.openFileForRead("TXT", typeCachePath, typeFile)) {
            uint32_t magic = 0;
            uint32_t version = 0;
            uint8_t mode = 0;
            serialization::readPod(typeFile, magic);
            serialization::readPod(typeFile, version);
            serialization::readPod(typeFile, mode);
            typeFile.close();

            if (magic == TYPE_CACHE_MAGIC && version == TYPE_CACHE_VERSION && (mode == 0 || mode == 1)) {
                m_isVolumeOnlyBook = (mode == 1);
                typeCacheLoaded = true;
                Serial.printf("[%lu] [TXT] Loaded type cache: %s (%s)\n", millis(), typeCachePath.c_str(),
                              m_isVolumeOnlyBook ? "volume-only" : "chapter");
            } else {
                Serial.printf("[%lu] [TXT] Type cache invalid, will rescan: %s\n", millis(), typeCachePath.c_str());
            }
        }
    }

    if (!typeCacheLoaded) {
        FsFile checkFile;
        bool hasValidChapter = false;
        int chapterFoundCount = 0;

        if (checkFile.open(filepath.c_str(), FILE_READ)) {
            Serial.printf("[Parser] ✅ 开始在 %lu 字节内检测是否有章节\n", VOLUME_PAGE_SIZE);
            bool skipBom = true;
            uint64_t currentReadOffset = 0;

            auto isHasChapterPattern = [](const char* s, int len) -> bool {
                if (len < 6) return false;
                bool hasDi = false, hasZhang = false;
                for (int i = 0; i < len - 2; i++) {
                    if (s[i] == 0xE7 && s[i+1] == 0xAC && s[i+2] == 0xAC) hasDi = true;
                    if (s[i] == 0xE7 && s[i+1] == 0xAB && s[i+2] == 0xA0) hasZhang = true;
                    if (hasDi && hasZhang) return true;
                }
                return false;
            };

            while (checkFile.available() && currentReadOffset < CHAPTER_CHECK_THRESHOLD) {
                bufferLen = 0;
                memset(readBuffer, 0, sizeof(readBuffer));

                while (checkFile.available() && currentReadOffset < CHAPTER_CHECK_THRESHOLD) {
                    char c = checkFile.read();
                    currentReadOffset++;
                    if (c == '\n' || c == '\r' || bufferLen >= 127) break;
                    readBuffer[bufferLen++] = c;
                }

                if (bufferLen == 0) continue;

                if (skipBom && bufferLen >= 3) {
                    if ((uint8_t)readBuffer[0] == 0xEF && (uint8_t)readBuffer[1] == 0xBB && (uint8_t)readBuffer[2] == 0xBF) {
                        memmove(readBuffer, readBuffer + 3, bufferLen - 3);
                        bufferLen -= 3;
                        skipBom = false;
                    }
                }

                bool isChapter = (bufferLen > 0 && bufferLen <= 60) && isHasChapterPattern(readBuffer, bufferLen);
                if (isChapter) {
                    hasValidChapter = true;
                    chapterFoundCount++;
                    break;
                }
            }
            checkFile.close();
        } else {
            Serial.printf("[Parser] ❌ 打开文件失败，默认按分卷处理\n");
        }

        if (!hasValidChapter) {
            Serial.printf("[VolumeMode] ⚠️ %lu 字节内无章节，标记为纯分卷书籍\n", VOLUME_PAGE_SIZE);
            m_isVolumeOnlyBook = true;
        } else {
            Serial.printf("[ChapterMode] ✅ 检测到有效章节，走原章节解析逻辑\n");
            m_isVolumeOnlyBook = false;
        }

        FsFile typeFile;
        if (SdMan.openFileForWrite("TXT", typeCachePath, typeFile)) {
            serialization::writePod(typeFile, TYPE_CACHE_MAGIC);
            serialization::writePod(typeFile, TYPE_CACHE_VERSION);
            const uint8_t mode = m_isVolumeOnlyBook ? 1 : 0;
            serialization::writePod(typeFile, mode);
            typeFile.sync();
            typeFile.close();
            Serial.printf("[%lu] [TXT] Saved type cache: %s (%s)\n", millis(), typeCachePath.c_str(),
                          m_isVolumeOnlyBook ? "volume-only" : "chapter");
        } else {
            Serial.printf("[%lu] [TXT] Failed to save type cache: %s\n", millis(), typeCachePath.c_str());
        }
    }

    // ========== 4. 优先读缓存（保持） ==========
    bool loadSuccess = loadChapterFromTxt(CHAPTER_START);
    if (loadSuccess) {
        Serial.printf("[ChapterLoader] ✅ 缓存命中，直接返回\n");
        return;
    }

    // ========== 3.5 章节模式扫描起点优化：尝试复用上一批缓存 ==========
    if (CHAPTER_START >= CHAPTER_BATCH_SIZE) {
        bool hasChapterHint = false;
        const int prevBatchStart = CHAPTER_START - CHAPTER_BATCH_SIZE;
        if (loadChapterFromTxt(prevBatchStart) && chapterActualCount > 0) {
            const int safeCount = (chapterActualCount > CHAPTER_CACHE_SPAN) ? CHAPTER_CACHE_SPAN : chapterActualCount;
            const int last = safeCount - 1;

            // 优先用“边界章”起点做下一批起扫，回退再用上一章的endOffset。
            uint64_t hintOffset = chapterDataList[last].byteOffset;
            int hintIndex = chapterDataList[last].chapterIndex;
            if (hintIndex < CHAPTER_START || hintOffset >= fileSize) {
                hintOffset = chapterDataList[last].endOffset;
                hintIndex = chapterDataList[last].chapterIndex + 1;
            }

            if (hintOffset < fileSize && hintIndex <= CHAPTER_START) {
                chapterScanStartOffset = hintOffset;
                chapterScanStartIndex = hintIndex;
                hasChapterHint = true;
                Serial.printf("[ChapterHint] ✅ 使用上一批缓存起扫：chapter=%d, offset=%llu\n",
                              chapterScanStartIndex, (unsigned long long)chapterScanStartOffset);
            }
        }

        if (!hasChapterHint) {
            chapterScanStartOffset = 0;
            chapterScanStartIndex = 0;
            if (VERBOSE_CHAPTER_PARSE_LOG) {
                Serial.printf("[ChapterHint] ⚠️ 上一批缓存不可用，回退文件头扫描\n");
            }
        }

        // 清理上一批缓存数据，避免污染本次结果
        chapterActualCount = 0;
        memset(chapterDataList, 0, sizeof(chapterDataList));
    }

    // ========== 5. 纯分卷模式 ==========
    if (m_isVolumeOnlyBook) {
        FsFile file;
        if (!file.open(filepath.c_str(), FILE_READ)) {
            Serial.printf("[VolumeMode] ❌ 打开文件失败\n");
            goto save_and_exit;
        }

        const int PARAGRAPHS_PER_CHAPTER = 20 +random(5); // 增加随机性
        const size_t VOLUME_SCAN_CHUNK_SIZE = 1024;

        int chapterIndex = chapterScanStartIndex;
        int currSaveCount = 0;
        int paragraphCount = 0;
        bool lineHasContent = false;
        uint64_t chapterStartOffset = chapterScanStartOffset;
        uint64_t currentReadOffset = chapterScanStartOffset;
        char chunk[VOLUME_SCAN_CHUNK_SIZE];

        if (chapterScanStartOffset > 0 && !file.seek(chapterScanStartOffset)) {
            Serial.printf("[VolumeHint] ⚠️ 起扫偏移定位失败，回退文件头\n");
            chapterIndex = 0;
            currSaveCount = 0;
            paragraphCount = 0;
            lineHasContent = false;
            chapterStartOffset = 0;
            currentReadOffset = 0;
            file.seek(0);
        }

        auto storeVolumeChapter = [&](int index, uint64_t startOffset) {
            if (index < CHAPTER_START || index > CHAPTER_END || currSaveCount >= CHAPTER_CACHE_SPAN) {
                return;
            }

            chapterDataList[currSaveCount].chapterIndex = index;
            chapterDataList[currSaveCount].byteOffset = startOffset;
            chapterDataList[currSaveCount].endOffset = fileSize;
            snprintf(chapterDataList[currSaveCount].shortTitle, TITLE_BUF_SIZE - 1, "第%d章", index + 1);
            chapterDataList[currSaveCount].shortTitle[TITLE_BUF_SIZE - 1] = '\0';
            currSaveCount++;

            if (VERBOSE_CHAPTER_PARSE_LOG) {
                Serial.printf("[Volume] ✅ 虚拟章节%d 已生成，起始偏移%llu\n", index, (unsigned long long)startOffset);
            }
        };

        storeVolumeChapter(chapterIndex, chapterStartOffset);

        while (file.available()) {
            const size_t bytesToRead = (file.available() > static_cast<int>(VOLUME_SCAN_CHUNK_SIZE))
                                           ? VOLUME_SCAN_CHUNK_SIZE
                                           : static_cast<size_t>(file.available());
            const int bytesRead = file.read(chunk, bytesToRead);
            if (bytesRead <= 0) {
                break;
            }

            for (int i = 0; i < bytesRead; ++i) {
                const uint8_t currentByte = static_cast<uint8_t>(chunk[i]);
                currentReadOffset++;

                if (currentByte == '\n') {
                    if (lineHasContent) {
                        paragraphCount++;
                        lineHasContent = false;
                    }

                    if (paragraphCount >= PARAGRAPHS_PER_CHAPTER) {
                        const uint64_t chapterEndOffset = currentReadOffset;
                        if (currSaveCount > 0) {
                            chapterDataList[currSaveCount - 1].endOffset = chapterEndOffset;
                        }

                        if (chapterEndOffset >= fileSize) {
                            goto volume_parse_done;
                        }

                        chapterIndex++;
                        chapterStartOffset = chapterEndOffset;
                        paragraphCount = 0;
                        storeVolumeChapter(chapterIndex, chapterStartOffset);

                        if (currSaveCount > 0 && chapterIndex > CHAPTER_END) {
                            goto volume_parse_done;
                        }
                    }
                } else if (currentByte != '\r' && currentByte != ' ' && currentByte != '\t') {
                    lineHasContent = true;
                }
            }
        }

volume_parse_done:
        file.close();
        chapterActualCount = currSaveCount;
        goto save_and_exit;
    }

    // ========== 6. 有章节模式（核心修改：向后探测下一章节） ==========
    {
        FsFile file;
        if (!file.open(filepath.c_str(), FILE_READ)) {
            Serial.printf("[ChapterMode] ❌ 打开文件失败\n");
            goto save_and_exit;
        }

        const int MAX_VALID_LEN = 60;
        const int TITLE_SUB_LEN = 20;
        int chapterFoundCount = chapterScanStartIndex;
        int currSaveCount = 0;
        bool skipBom = true;
        uint64_t currentReadOffset = chapterScanStartOffset;
        uint64_t chapOffsets[CHAPTER_CACHE_SPAN] = {0}; // 当前批次章节偏移
        int chapIndexes[CHAPTER_CACHE_SPAN] = {0};      // 当前批次章节号

        if (chapterScanStartOffset > 0 && !file.seek(chapterScanStartOffset)) {
            Serial.printf("[ChapterHint] ⚠️ 起扫偏移定位失败，回退文件头\n");
            chapterFoundCount = 0;
            currentReadOffset = 0;
            file.seek(0);
        }

        auto isHasChapterPattern = [](const char* s, int len) -> bool {
            if (len < 6) return false;
            bool hasDi = false, hasZhang = false;
            for (int i = 0; i < len - 2; i++) {
                if (s[i] == 0xE7 && s[i+1] == 0xAC && s[i+2] == 0xAC) hasDi = true;
                if (s[i] == 0xE7 && s[i+1] == 0xAB && s[i+2] == 0xA0) hasZhang = true;
                if (hasDi && hasZhang) return true;
            }
            return false;
        };

        auto subUTF8String = [](char* dst, const char* src, int len, int keepCount) {
            int charCount = 0, i = 0;
            memset(dst, 0, TITLE_BUF_SIZE);
            while (i < len && charCount < keepCount) {
                dst[i] = src[i];
                if ((uint8_t)src[i] >= 0xE0) {
                    dst[i+1] = (i+1 < len) ? src[i+1] : 0;
                    dst[i+2] = (i+2 < len) ? src[i+2] : 0;
                    i += 3;
                } else {
                    i += 1;
                }
                charCount++;
            }
            dst[TITLE_BUF_SIZE - 1] = '\0';
        };

        // 步骤1：解析当前批次章节（保持）
        while (file.available() && currSaveCount < CHAPTER_CACHE_SPAN) {
            bufferLen = 0;
            memset(readBuffer, 0, sizeof(readBuffer));

            while (file.available()) {
                char c = file.read();
                currentReadOffset++;
                if (c == '\n' || c == '\r' || bufferLen >= 127) break;
                readBuffer[bufferLen++] = c;
            }

            if (bufferLen == 0) continue;

            if (skipBom && bufferLen >= 3) {
                if ((uint8_t)readBuffer[0] == 0xEF && (uint8_t)readBuffer[1] == 0xBB && (uint8_t)readBuffer[2] == 0xBF) {
                    memmove(readBuffer, readBuffer + 3, bufferLen - 3);
                    bufferLen -= 3;
                    skipBom = false;
                }
            }

            bool isChapter = (bufferLen > 0 && bufferLen <= MAX_VALID_LEN) && isHasChapterPattern(readBuffer, bufferLen);
            if (isChapter) {
                if (chapterFoundCount >= CHAPTER_START && chapterFoundCount <= CHAPTER_END) {
                    uint64_t pos = currentReadOffset - bufferLen - 1;
                    if (pos < 0) pos = 0;

                    chapOffsets[currSaveCount] = pos;
                    chapIndexes[currSaveCount] = chapterFoundCount;
                    chapterDataList[currSaveCount].chapterIndex = chapterFoundCount;
                    chapterDataList[currSaveCount].byteOffset = pos;
                    subUTF8String(chapterDataList[currSaveCount].shortTitle, readBuffer, bufferLen, TITLE_SUB_LEN);
                    currSaveCount++;
                }
                chapterFoundCount++;
            }
        }

        // 步骤2：为每个章节计算endOffset（核心：向后探测）
        for (int i = 0; i < currSaveCount; i++) {
            if (i < currSaveCount - 1) {
                // 非批次最后一个：用下一章节的偏移
                chapterDataList[i].endOffset = chapOffsets[i + 1];
            } else {
                // 批次最后一个：探测下一章节（chapterFoundCount）
                uint64_t searchStart = chapOffsets[i] + 1;
                uint64_t searchEnd = searchStart + MAX_NEXT_SEARCH;
                if (searchEnd > fileSize) searchEnd = fileSize;
                uint64_t nextChapOffset = 0;
                bool hasNextChap = false;

                // 仅在搜索范围有效时执行
                if (searchStart < fileSize) {
                    if (file.seek(searchStart)) {
                        bool innerSkipBom = false; // 内部BOM已在主解析中处理
                        uint64_t innerReadOffset = searchStart;
                        char innerBuffer[128] = {0};
                        int innerBufLen = 0;

                        while (file.available() && innerReadOffset < searchEnd) {
                            innerBufLen = 0;
                            memset(innerBuffer, 0, sizeof(innerBuffer));

                            while (file.available() && innerReadOffset < searchEnd) {
                                char c = file.read();
                                innerReadOffset++;
                                if (c == '\n' || c == '\r' || innerBufLen >= 127) break;
                                innerBuffer[innerBufLen++] = c;
                            }

                            if (innerBufLen == 0) continue;

                            bool isNextChapter = (innerBufLen > 0 && innerBufLen <= MAX_VALID_LEN) && isHasChapterPattern(innerBuffer, innerBufLen);
                            if (isNextChapter) {
                                // 计算下一章节的起始偏移
                                nextChapOffset = innerReadOffset - innerBufLen - 1;
                                if (nextChapOffset < 0) nextChapOffset = 0;
                                if (nextChapOffset > chapOffsets[i] && nextChapOffset < fileSize) {
                                    hasNextChap = true;
                                    if (VERBOSE_CHAPTER_PARSE_LOG) {
                                        Serial.printf("[Chapter] ✅ 探测到下一章节%d，偏移%llu\n", chapterFoundCount,
                                                      (unsigned long long)nextChapOffset);
                                    }
                                    break; // 找到即退出，避免多余扫描
                                }
                            }
                        }
                        memset(innerBuffer, 0, sizeof(innerBuffer)); // 清理临时缓冲区
                    }
                }

                // 赋值endOffset：有下一章节则用其偏移，否则用文件大小
                chapterDataList[i].endOffset = hasNextChap ? nextChapOffset : fileSize;
                if (VERBOSE_CHAPTER_PARSE_LOG) {
                    Serial.printf("[Chapter] ✅ 章节%d endOffset：%llu（%s）\n", chapIndexes[i],
                                  (unsigned long long)chapterDataList[i].endOffset,
                                  hasNextChap ? "下一章节" : "文件末尾");
                }
            }
        }

        file.close();
        chapterActualCount = currSaveCount;
    }

    // ========== 6. 保存缓存并退出（保持） ==========
save_and_exit:
    if (chapterActualCount > 0) {
        Serial.printf("[Result] ✅ 本次生成 %d 个有效条目，endOffset已按文件实际末尾校准\n", chapterActualCount);
    } else {
        Serial.printf("[Result] ⚠️ 本次无有效条目\n");
    }
    saveChapterToTxt(CHAPTER_START);
    Serial.printf("[ChapterTime] chapterStart=%d mode=%s total=%lums\n", CHAPTER_START,
                  m_isVolumeOnlyBook ? "sentence" : "chapter",
                  millis() - parseStartMs);
    memset(readBuffer, 0, sizeof(readBuffer));
}


// 保存章节批次到单个TXT（纯C风格，无String）
// 先确保必要的宏/类型定义（如果未定义）
#ifndef CACHE_MAGIC
#define CACHE_MAGIC 0x43484150  // "CHAP" ASCII码，自定义魔数
#endif

#ifndef CACHE_VERSION
#define CACHE_VERSION 2          // 缓存版本号（批次规则变更后提升）
#endif

// 保存章节批次到单个BIN文件（使用serialization::writePod/writeString规范）
void Txt::saveChapterToTxt(int startChapter) {
    FsFile f;
    char savePath[128] = {0};
    const std::string cacheFilePath = getChapterCacheFilePath(startChapter);
    snprintf(savePath, sizeof(savePath), "%s", cacheFilePath.c_str());

    // 打开文件（失败则直接返回并打印日志）
    if (!SdMan.openFileForWrite("TRA", savePath, f)) {
        Serial.printf("[ChapterSaver] ❌ %d~%d章合并保存失败 → %s\n", 
                      startChapter, startChapter + CHAPTER_CACHE_LAST_OFFSET, savePath);
        return;
    }

    // ========== 1. 写入缓存头部（和index.bin格式保持一致） ==========
    serialization::writePod(f, CACHE_MAGIC);                // 魔数（验证文件合法性）
    serialization::writePod(f, CACHE_VERSION);              // 版本号（兼容升级）
    serialization::writePod(f, static_cast<uint32_t>(startChapter));  // 起始章节号
    serialization::writePod(f, static_cast<uint32_t>(chapterActualCount));  // 实际保存章节数

    // ========== 2. 写入章节数据主体（使用writeString存储标题） ==========
    for (int i = 0; i < chapterActualCount && i < CHAPTER_CACHE_SPAN; i++) {
        // 1. 章节序号（int → int32_t 保证长度统一）
        serialization::writePod(f, static_cast<int32_t>(chapterDataList[i].chapterIndex));
        // 2. 字节偏移量（uint32_t 直接写入）
        serialization::writePod(f, chapterDataList[i].byteOffset);
        // 3. 短标题：char数组 → 用writeString序列化（自动处理长度+内容）
        // 核心调整：替换writePod为writeString，适配字符串存储规范
        serialization::writeString(f, chapterDataList[i].shortTitle);
        // 4. 章节结束偏移（uint32_t 直接写入）
        serialization::writePod(f, chapterDataList[i].endOffset);
    }

    // ========== 3. 完成写入 ==========
    f.sync();  // 同步到磁盘，防止数据丢失
    f.close();

    Serial.printf("[ChapterSaver] ✅ %d~%d章合并保存成功 → %s | 实际保存%d章 | 魔数：0x%X 版本：%d\n", 
                  startChapter, startChapter + CHAPTER_CACHE_LAST_OFFSET, savePath,
                  chapterActualCount, CACHE_MAGIC, CACHE_VERSION);
}

// 加载章节批次从单个TXT（纯C风格，无String）
bool Txt::loadChapterFromTxt(int startChapter) {
    // ========== 1. 初始化/清理数据（保留原loadChapterFromTxt的清理逻辑） ==========
    chapterActualCount = 0;
    memset(chapterDataList, 0, sizeof(chapterDataList));
    bool loadOk = false;

    FsFile f;
    char loadPath[128] = {0};
    const std::string cacheFilePath = getChapterCacheFilePath(startChapter);
    snprintf(loadPath, sizeof(loadPath), "%s", cacheFilePath.c_str());

    // 打开文件失败（对齐参考示例的日志风格）
    if (!SdMan.openFileForRead("TRA", loadPath, f)) {
        Serial.printf("[%lu] [%s] No chapter cache found for %d~%d → %s\n", millis(), m_isVolumeOnlyBook ? "VOL" : "TRA",
                      startChapter, startChapter + CHAPTER_CACHE_LAST_OFFSET, loadPath);
        return false;
    }

    // ========== 2. 读取并验证头部（完全对齐loadPageIndexCache风格） ==========
    // 2.1 读取魔数并验证
    uint32_t magic;
    serialization::readPod(f, magic);
    if (magic != CACHE_MAGIC) {
        Serial.printf("[%lu] [%s] Chapter cache magic mismatch (0x%X != 0x%X), rebuilding\n", 
                      millis(), m_isVolumeOnlyBook ? "VOL" : "TRA", magic, CACHE_MAGIC);
        f.close();
        return false;
    }

    // 2.2 读取版本号并验证
    uint32_t version; // 对齐参考示例用uint32_t，若原版本是uint8_t可调整
    serialization::readPod(f, version);
    if (version != CACHE_VERSION) {
        Serial.printf("[%lu] [%s] Chapter cache version mismatch (%d != %d), rebuilding\n", 
                      millis(), m_isVolumeOnlyBook ? "VOL" : "TRA", version, CACHE_VERSION);
        f.close();
        return false;
    }

    // 2.3 读取起始章号并验证（确保缓存文件和要加载的章节匹配）
    uint32_t cacheStartChapter;
    serialization::readPod(f, cacheStartChapter);
    if (cacheStartChapter != static_cast<uint32_t>(startChapter)) {
        Serial.printf("[%lu] [%s] Chapter cache start mismatch (%d != %d), rebuilding\n", 
                      millis(), m_isVolumeOnlyBook ? "VOL" : "TRA", cacheStartChapter, startChapter);
        f.close();
        return false;
    }

    // 2.4 读取缓存的章节总数
    uint32_t cacheChapterCount;
    serialization::readPod(f, cacheChapterCount);
    if (cacheChapterCount > CHAPTER_CACHE_SPAN) { // 最多只存一个批次（含边界章），超出则无效
        Serial.printf("[%lu] [%s] Chapter cache count invalid (%d > %d), rebuilding\n", 
                  millis(), m_isVolumeOnlyBook ? "VOL" : "TRA", cacheChapterCount,
                  CHAPTER_CACHE_SPAN);
        f.close();
        return false;
    }

    // ========== 3. 读取章节数据主体（逐字段+验证） ==========
     int chapterNum = 0;
    while (chapterNum < CHAPTER_CACHE_SPAN && chapterNum < cacheChapterCount && f.available()) {
        // 3.1 读取章节序号
        int32_t actualChap;
        serialization::readPod(f, actualChap);

        // 3.2 读取字节偏移量
        uint32_t byteOffset;
        serialization::readPod(f, byteOffset);

        // 3.3 读取短标题：核心调整为std::string类型
        std::string titleStr; // 必须使用std::string
        serialization::readString(f, titleStr); // 直接读取到string，无需缓冲区

        // 3.4 读取结束偏移量
        uint32_t endOffset;
        serialization::readPod(f, endOffset);

        // ========== 4. 填充数据（string转char数组，保证结构体兼容） ==========
        chapterDataList[chapterNum].chapterIndex = actualChap;
        chapterDataList[chapterNum].byteOffset = byteOffset;
        chapterDataList[chapterNum].endOffset = endOffset;

        // 清空标题数组 + string安全拷贝到char数组（防止越界）
        memset(chapterDataList[chapterNum].shortTitle, 0, TITLE_BUF_SIZE);
        strncpy(chapterDataList[chapterNum].shortTitle, titleStr.c_str(), TITLE_BUF_SIZE - 1);

        // 不做shrink_to_fit，避免频繁堆内存收缩造成额外耗时
        titleStr.clear();

        chapterNum++;
        loadOk = true;
    }

    // ========== 5. 收尾处理（对齐参考示例） ==========
    f.close();
    chapterActualCount = chapterNum;

    // 日志输出（融合参考示例+业务逻辑）
    if (loadOk) {
        Serial.printf("[%lu] [%s] Loaded chapter cache: %d~%d → %s | %d chapters\n", 
                      millis(), m_isVolumeOnlyBook ? "VOL" : "TRA", startChapter,
                      startChapter + CHAPTER_CACHE_LAST_OFFSET, loadPath,
                      chapterActualCount);
    }

    return loadOk;
}