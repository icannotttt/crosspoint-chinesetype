#include "Txt.h"

#include <FsHelpers.h>
#include <JpegToBmpConverter.h>
#include <Serialization.h>

Txt::Txt(std::string path, std::string cacheBasePath)
    : filepath(std::move(path)), cacheBasePath(std::move(cacheBasePath)) {
  // Generate cache path from file path hash
  const size_t hash = std::hash<std::string>{}(filepath);
  cachePath = this->cacheBasePath + "/txt_" + std::to_string(hash);
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

  FsFile file;
  if (!SdMan.openFileForRead("TXT", filepath, file)) {
    return false;
  }

  if (!file.seek(offset)) {
    file.close();
    return false;
  }

  size_t bytesRead = file.read(buffer, length);
  file.close();

  return bytesRead > 0;
}


//加目录
void Txt::parseChapterIndexAndOffset(int n) {
    char readBuffer[128] = {0};
    int bufferLen = 0;

    // 配置参数（保持不变）
    const int CHAPTER_START = n;
    const int CHAPTER_END = n + 24;
    const uint32_t VOLUME_PAGE_SIZE = 7680;
    const char* VOLUME_TITLE_PREFIX = "分卷阅读";
    const uint64_t CHAPTER_CHECK_THRESHOLD = VOLUME_PAGE_SIZE;
    const int MAX_BACK_SEARCH_LEN = 1024;
    // 向后探测的最大范围：覆盖下一批起始，避免无限扫描
    const uint64_t MAX_NEXT_SEARCH = 2 * VOLUME_PAGE_SIZE;

    Serial.printf("[ChapterRange] ✅ 本次加载范围：%d ~ %d\n", CHAPTER_START, CHAPTER_END);

    // ========== 1. 优先读缓存（保持） ==========
    bool loadSuccess = loadChapterFromTxt(n);
    if (loadSuccess) {
        Serial.printf("[ChapterLoader] ✅ 缓存命中，直接返回\n");
        return;
    }

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

    // ========== 3. 分卷/章节模式检测（保持） ==========
    if (!m_isVolumeOnlyBook) {
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
        }
    }

    // ========== 4. 纯分卷模式（核心修改：向后探测下一分卷） ==========
    if (m_isVolumeOnlyBook) {
        FsFile file;
        if (!file.open(filepath.c_str(), FILE_READ)) {
            Serial.printf("[VolumeMode] ❌ 打开文件失败\n");
            goto save_and_exit;
        }

        int volCount = 0;
        uint64_t volOffsets[25] = {0}; // 存储当前批次偏移
        int volIndexes[25] = {0};      // 存储当前批次分卷号

        // 步骤1：解析当前批次25个分卷（保持）
        for (int i = 0; i < 25; ++i) {
            int volIdx = CHAPTER_START + i;
            uint64_t theoryOffset = (uint64_t)volIdx * VOLUME_PAGE_SIZE;
            uint64_t actualOffset = theoryOffset;

            if (volIdx > 0 && theoryOffset < fileSize) {
                if (file.seek(theoryOffset)) {
                    uint64_t backSearchStart = (theoryOffset >= MAX_BACK_SEARCH_LEN) ? (theoryOffset - MAX_BACK_SEARCH_LEN) : 0;
                    uint64_t currentSearchPos = theoryOffset;
                    bool foundNewLine = false;

                    while (currentSearchPos > backSearchStart) {
                        currentSearchPos--;
                        if (!file.seek(currentSearchPos)) break;
                        char c = file.read();
                        if (c == '\n') {
                            actualOffset = currentSearchPos + 1;
                            foundNewLine = true;
                            break;
                        }
                    }

                    if (foundNewLine) {
                        Serial.printf("[Volume] ✅ 分卷%d 找到\\n，理论%llu → 实际%llu\n", volIdx, (unsigned long long)theoryOffset, (unsigned long long)actualOffset);
                    } else {
                        Serial.printf("[Volume] ⚠️ 分卷%d 未找到\\n，使用理论%llu\n", volIdx, (unsigned long long)theoryOffset);
                    }
                } else {
                    Serial.printf("[Volume] ❌ 分卷%d 定位失败，使用理论%llu\n", volIdx, (unsigned long long)theoryOffset);
                }
            }

            if (actualOffset >= fileSize) {
                if (volCount == 0) break;
                else continue;
            }

            volOffsets[volCount] = actualOffset;
            volIndexes[volCount] = volIdx;
            chapterDataList[volCount].chapterIndex = volIdx;
            chapterDataList[volCount].byteOffset = actualOffset;
            snprintf(chapterDataList[volCount].shortTitle, TITLE_BUF_SIZE - 1, "%s%d", VOLUME_TITLE_PREFIX, volIdx + 1);
            chapterDataList[volCount].shortTitle[TITLE_BUF_SIZE - 1] = '\0';

            Serial.printf("[Volume] ✅ 分卷%d 已生成，实际偏移%llu\n", volIdx, (unsigned long long)actualOffset);
            volCount++;
        }

        // 步骤2：为每个分卷计算endOffset（核心：向后探测）
        for (int i = 0; i < volCount; i++) {
            if (i < volCount - 1) {
                // 非批次最后一个：用下一分卷的偏移
                chapterDataList[i].endOffset = volOffsets[i + 1];
            } else {
                // 批次最后一个：探测下一分卷（CHAPTER_START + volCount）
                int nextVolIdx = CHAPTER_START + volCount;
                uint64_t nextTheoryOffset = (uint64_t)nextVolIdx * VOLUME_PAGE_SIZE;
                uint64_t nextActualOffset = 0;
                bool hasNextVol = false;

                // 仅在理论偏移未超出文件且探测范围内时执行
                if (nextTheoryOffset < fileSize && nextTheoryOffset <= volOffsets[i] + MAX_NEXT_SEARCH) {
                    if (file.seek(nextTheoryOffset)) {
                        uint64_t backSearchStart = (nextTheoryOffset >= MAX_BACK_SEARCH_LEN) ? (nextTheoryOffset - MAX_BACK_SEARCH_LEN) : 0;
                        uint64_t currentSearchPos = nextTheoryOffset;
                        bool foundNewLine = false;

                        while (currentSearchPos > backSearchStart) {
                            currentSearchPos--;
                            if (!file.seek(currentSearchPos)) break;
                            char c = file.read();
                            if (c == '\n') {
                                nextActualOffset = currentSearchPos + 1;
                                foundNewLine = true;
                                break;
                            }
                        }

                        // 验证下一分卷偏移有效性
                        if (foundNewLine && nextActualOffset < fileSize && nextActualOffset > volOffsets[i]) {
                            hasNextVol = true;
                            Serial.printf("[Volume] ✅ 探测到下一分卷%d，偏移%llu\n", nextVolIdx, (unsigned long long)nextActualOffset);
                        }
                    }
                }

                // 赋值endOffset：有下一卷则用其偏移，否则用文件大小
                chapterDataList[i].endOffset = hasNextVol ? nextActualOffset : fileSize;
                Serial.printf("[Volume] ✅ 分卷%d endOffset：%llu（%s）\n",
                    volIndexes[i], (unsigned long long)chapterDataList[i].endOffset,
                    hasNextVol ? "下一分卷" : "文件末尾");
            }
        }

        file.close();
        chapterActualCount = volCount;
        goto save_and_exit;
    }

    // ========== 5. 有章节模式（核心修改：向后探测下一章节） ==========
    {
        FsFile file;
        if (!file.open(filepath.c_str(), FILE_READ)) {
            Serial.printf("[ChapterMode] ❌ 打开文件失败\n");
            goto save_and_exit;
        }

        const int MAX_VALID_LEN = 60;
        const int TITLE_SUB_LEN = 20;
        int chapterFoundCount = 0;
        int currSaveCount = 0;
        bool skipBom = true;
        uint64_t currentReadOffset = 0;
        uint64_t chapOffsets[25] = {0}; // 当前批次章节偏移
        int chapIndexes[25] = {0};      // 当前批次章节号

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

        // 步骤1：解析当前批次25个章节（保持）
        while (file.available() && currSaveCount < 25) {
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
                                    Serial.printf("[Chapter] ✅ 探测到下一章节%d，偏移%llu\n", chapterFoundCount, (unsigned long long)nextChapOffset);
                                    break; // 找到即退出，避免多余扫描
                                }
                            }
                        }
                        memset(innerBuffer, 0, sizeof(innerBuffer)); // 清理临时缓冲区
                    }
                }

                // 赋值endOffset：有下一章节则用其偏移，否则用文件大小
                chapterDataList[i].endOffset = hasNextChap ? nextChapOffset : fileSize;
                Serial.printf("[Chapter] ✅ 章节%d endOffset：%llu（%s）\n",
                    chapIndexes[i], (unsigned long long)chapterDataList[i].endOffset,
                    hasNextChap ? "下一章节" : "文件末尾");
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
    saveChapterToTxt(n);
    memset(readBuffer, 0, sizeof(readBuffer));
}


// 保存25章到单个TXT（纯C风格，无String）
// 先确保必要的宏/类型定义（如果未定义）
#ifndef CACHE_MAGIC
#define CACHE_MAGIC 0x43484150  // "CHAP" ASCII码，自定义魔数
#endif

#ifndef CACHE_VERSION
#define CACHE_VERSION 1          // 缓存版本号
#endif

// 保存25章到单个BIN文件（使用serialization::writePod/writeString规范）
void Txt::saveChapterToTxt(int startChapter) {
    FsFile f;
    char savePath[128] = {0};
    // 文件名格式：chapters_起始章n_25.bin
    snprintf(savePath, sizeof(savePath), "%s/chapters_%d_25.bin", getCachePath().c_str(), startChapter);

    // 打开文件（失败则直接返回并打印日志）
    if (!SdMan.openFileForWrite("TRA", savePath, f)) {
        Serial.printf("[ChapterSaver] ❌ %d~%d章合并保存失败 → %s\n", 
                      startChapter, startChapter+24, savePath);
        return;
    }

    // ========== 1. 写入缓存头部（和index.bin格式保持一致） ==========
    serialization::writePod(f, CACHE_MAGIC);                // 魔数（验证文件合法性）
    serialization::writePod(f, CACHE_VERSION);              // 版本号（兼容升级）
    serialization::writePod(f, static_cast<uint32_t>(startChapter));  // 起始章节号
    serialization::writePod(f, static_cast<uint32_t>(chapterActualCount));  // 实际保存章节数

    // ========== 2. 写入章节数据主体（使用writeString存储标题） ==========
    for (int i = 0; i < chapterActualCount && i < 25; i++) {
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
                  startChapter, startChapter+24, savePath, chapterActualCount, CACHE_MAGIC, CACHE_VERSION);
}

// 加载25章从单个TXT（纯C风格，无String）
bool Txt::loadChapterFromTxt(int startChapter) {
    // ========== 1. 初始化/清理数据（保留原loadChapterFromTxt的清理逻辑） ==========
    chapterActualCount = 0;
    memset(chapterDataList, 0, sizeof(chapterDataList));
    bool loadOk = false;

    FsFile f;
    char loadPath[128] = {0};
    snprintf(loadPath, sizeof(loadPath), "%s/chapters_%d_25.bin", getCachePath().c_str(), startChapter);

    // 打开文件失败（对齐参考示例的日志风格）
    if (!SdMan.openFileForRead("TRA", loadPath, f)) {
        Serial.printf("[%lu] [TRA] No chapter cache found for %d~%d → %s\n", millis(), startChapter, startChapter+24, loadPath);
        return false;
    }

    // ========== 2. 读取并验证头部（完全对齐loadPageIndexCache风格） ==========
    // 2.1 读取魔数并验证
    uint32_t magic;
    serialization::readPod(f, magic);
    if (magic != CACHE_MAGIC) {
        Serial.printf("[%lu] [TRA] Chapter cache magic mismatch (0x%X != 0x%X), rebuilding\n", 
                      millis(), magic, CACHE_MAGIC);
        f.close();
        return false;
    }

    // 2.2 读取版本号并验证
    uint32_t version; // 对齐参考示例用uint32_t，若原版本是uint8_t可调整
    serialization::readPod(f, version);
    if (version != CACHE_VERSION) {
        Serial.printf("[%lu] [TRA] Chapter cache version mismatch (%d != %d), rebuilding\n", 
                      millis(), version, CACHE_VERSION);
        f.close();
        return false;
    }

    // 2.3 读取起始章号并验证（确保缓存文件和要加载的章节匹配）
    uint32_t cacheStartChapter;
    serialization::readPod(f, cacheStartChapter);
    if (cacheStartChapter != static_cast<uint32_t>(startChapter)) {
        Serial.printf("[%lu] [TRA] Chapter cache start mismatch (%d != %d), rebuilding\n", 
                      millis(), cacheStartChapter, startChapter);
        f.close();
        return false;
    }

    // 2.4 读取缓存的章节总数
    uint32_t cacheChapterCount;
    serialization::readPod(f, cacheChapterCount);
    if (cacheChapterCount > 25) { // 最多只存25章，超出则无效
        Serial.printf("[%lu] [TRA] Chapter cache count invalid (%d > 25), rebuilding\n", 
                      millis(), cacheChapterCount);
        f.close();
        return false;
    }

    // ========== 3. 读取章节数据主体（逐字段+验证） ==========
     int chapterNum = 0;
    while (chapterNum < 25 && chapterNum < cacheChapterCount && f.available()) {
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

        // 清理string（可选，加速内存释放）
        titleStr.clear();
        titleStr.shrink_to_fit();

        chapterNum++;
        loadOk = true;
    }

    // ========== 5. 收尾处理（对齐参考示例） ==========
    f.close();
    chapterActualCount = chapterNum;

    // 日志输出（融合参考示例+业务逻辑）
    if (loadOk) {
        Serial.printf("[%lu] [TRA] Loaded chapter cache: %d~%d → %s | %d chapters\n", 
                      millis(), startChapter, startChapter+24, loadPath, chapterActualCount);
    }

    return loadOk;
}