/**
 * Txt.cpp
 * 核心优化：1. 按页读取文件，不加载全量内容，解决内存溢出
 *           2. 适配UTF-8中文解析，按字符数（非字节数）分页，修复一行显示过少问题
 *           3. 统一文本格式（清理隐形字符/换行符/BOM），和EPUB渲染逻辑对齐，修复问号显示问题
 *           4. 新增：复用EPUB的addWord逻辑拆分UTF-8字符，和EPUB渲染完全对齐
 */
#include "Txt.h"
#include <HardwareSerial.h>
#include <../Utf8/Utf8.h>          
#include <../Epub/Epub/htmlEntities.h>  
#include "../../src/fontIds.h"
#include <../GfxRenderer/GfxRenderer.h>
#include "../../src/CrossPointSettings.h"

namespace {
// 墨水屏刷新阈值：每渲染15页执行一次全刷新（解决残影），15页内执行半刷新（更快）
constexpr int pagesPerRefresh = 15;
// 跳章长按阈值：长按翻页键超过700ms，直接跳转到上/下一章（短按仅翻页）
constexpr unsigned long skipChapterMs = 700;
// 返回主页长按阈值：长按返回键超过1000ms，直接返回主界面（短按仅退出章节选择）
constexpr unsigned long goHomeMs = 1000;
// 顶部内边距：文本区域顶部到屏幕顶部的距离（控制文本顶部留白）
constexpr int topPadding = 5;
// 左右内边距：文本区域左右到屏幕边缘的距离（控制文本左右留白）
constexpr int horizontalPadding = 5;
// 状态栏底部边距：状态栏到屏幕底部的距离（防止文本与状态栏重叠）
constexpr int statusBarMargin = 19;
// 文本行高：每行文本的垂直间距（数值越大，行间距越宽）
constexpr int LINE_HEIGHT = 24;    
// 单行最大宽度：单行文本的最大像素宽度（超过该宽度自动换行）
constexpr int MAX_LINE_WIDTH = 500;
}  // namespace

// 初始化缓存目录
void Txt::setupCacheDir() const {
    if (SdMan.exists(cachePath.c_str())) {
        return;
    }
    for (size_t i = 1; i < cachePath.length(); i++) {
        if (cachePath[i] == '/') {
            SdMan.mkdir(cachePath.substr(0, i).c_str());
        }
    }
    SdMan.mkdir(cachePath.c_str());
}

// 清理缓存
bool Txt::clearCache() const {
    if (!SdMan.exists(cachePath.c_str())) {
        Serial.printf("[%lu] [TXT] Cache does not exist\n", millis());
        return true;
    }
    if (!SdMan.removeDir(cachePath.c_str())) {
        Serial.printf("[%lu] [TXT] Failed to clear cache\n", millis());
        return false;
    }
    Serial.printf("[%lu] [TXT] Cache cleared\n", millis());
    return true;
}

// 计算分页布局（保留fontId逻辑，兼容你的项目）
void Txt::calculatePageLayout() {
    extern GfxRenderer renderer; 
    // 计算并保存到成员变量（关键！）
    int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
    renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                    &orientedMarginLeft);
    orientedMarginTop += topPadding;
    orientedMarginLeft += horizontalPadding;
    orientedMarginRight += horizontalPadding;
    orientedMarginBottom += statusBarMargin;
    // 优化：用中文“中”计算宽度，适配UTF-8中文排版
    this->charWidth = renderer.getTextWidth(fontId, "中", EpdFontFamily::REGULAR) + 2; // 32px（含2px字符间距）
    this->lineHeight = renderer.getFontAscenderSize(fontId) + 3; // 46px（含3px行间距）

    // 强制设置正确的屏幕尺寸（修复之前800×600的错误）
    const auto screenHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
    const auto screenWidth = renderer.getScreenWidth();   
    // 按“UTF-8字符数”计算分页（不是字节数）
    charsPerLine = screenWidth / this->charWidth; // 15字/行（480/32）
    linesPerPage = screenHeight / this->lineHeight; // 17行/页（800/46）

    // 原有打印保留，新增总字符数打印
    Serial.printf("[%lu] [TXT] 分页参数计算过程：\n", millis());
    Serial.printf("  - 屏幕尺寸：width=%lu, height=%lu\n", screenWidth, screenHeight);
    Serial.printf("  - 单个中文字符宽度（含2px间距）：%d px\n", this->charWidth);
    Serial.printf("  - 单行高度（含3px间距）：%d px\n", this->lineHeight);
    Serial.printf("[%lu] [TXT] Layout (fontId=%d): %lu 字/行, %lu 行/页, %lu 字/页\n", 
        millis(), fontId, charsPerLine, linesPerPage, charsPerLine * linesPerPage);
}

// ========== 新增：文本清理工具函数（和EPUB渲染逻辑对齐） ==========
/**
 * @brief 清理TXT中的隐形干扰字符（EPUB解析时会自动过滤）
 * @param content 原始文本内容
 * @return 清理后的纯可见字符内容
 */
// Txt.cpp 中替换 cleanTxtContent 函数
std::string Txt::cleanTxtContent(const std::string& content) {
    std::string cleaned;
    for (size_t i = 0; i < content.size();) {
        uint8_t uc = static_cast<uint8_t>(content[i]);
        
        // 1. 处理UTF-8多字节字符（中文是3字节，E0-EF开头）
        if (uc >= 0xE0 && uc <= 0xEF) { // 中文UTF-8首字节（3字节）
            // 确保有足够的字节（防止截断）
            if (i + 2 < content.size()) {
                cleaned += content[i];
                cleaned += content[i+1];
                cleaned += content[i+2];
                i += 3; // 跳过已处理的3字节
                continue;
            }
        } 
        // 2. 处理UTF-8双字节字符（如中文标点）
        else if (uc >= 0xC0 && uc <= 0xDF) {
            if (i + 1 < content.size()) {
                cleaned += content[i];
                cleaned += content[i+1];
                i += 2;
                continue;
            }
        } 
        // 3. 处理ASCII字符（可见字符+换行+空格）
        else if ((uc >= 0x20 && uc <= 0x7E) || uc == '\n' || uc == ' ') {
            cleaned += content[i];
            i += 1;
            continue;
        }
        
        // 跳过无效/不可见字符
        i += 1;
    }
    return cleaned;
}

/**
 * @brief 统一换行符格式（和EPUB保持一致，只保留\n，去重多余换行）
 * @param content 原始文本内容
 * @return 换行符归一化后的内容
 */
std::string Txt::normalizeNewlines(const std::string& content) {
    std::string normalized;
    bool lastWasNewline = false;
    for (char c : content) {
        if (c == '\r') { // 把\r转为\n（Windows换行符）
            c = '\n';
        }
        if (c == '\n') {
            if (!lastWasNewline) { // 去重连续换行
                normalized += '\n';
                lastWasNewline = true;
            }
        } else {
            normalized += c;
            lastWasNewline = false;
        }
    }
    return normalized;
}

/**
 * @brief 移除UTF-8 BOM标记（部分TXT文件开头会有，EPUB解析时会自动移除）
 * @param content 原始文本内容
 * @return 移除BOM后的纯UTF-8内容
 */
std::string Txt::removeUtf8Bom(const std::string& content) {
    if (content.size() >= 3) {
        uint8_t b1 = static_cast<uint8_t>(content[0]);
        uint8_t b2 = static_cast<uint8_t>(content[1]);
        uint8_t b3 = static_cast<uint8_t>(content[2]);
        if (b1 == 0xEF && b2 == 0xBB && b3 == 0xBF) { // UTF-8 BOM标记
            return content.substr(3);
        }
    }
    return content;
}

// ========== 新增：EPUB的addWord逻辑（复用，最小改动适配） ==========
/**
 * @brief 复用EPUB的addWord逻辑，拆分UTF-8字符（中文拆单个，英文保留整词）
 * @param word 待拆分的字符串
 * @param fontStyle 字体样式（默认NORMAL）
 */
void Txt::addWord(std::string word, const EpdFontFamily::Style fontStyle) {
    if (word.empty()) return;

    // 判断是否包含中文字符（非ASCII字符）
    bool hasChinese = false;
    for (char c : word) {
        if ((static_cast<uint8_t>(c) & 0x80) != 0) {
            hasChinese = true;
            break;
        }
    }

    if (hasChinese) {
        // 先把word.c_str()转成校准函数需要的类型，再调用校准
        const unsigned char* calibrate_ptr = reinterpret_cast<const unsigned char*>(word.c_str());
        calibrateUtf8Pointer(calibrate_ptr);
        // 按UTF-8字符拆分（每个中文字符为一个“单词”）
        const uint8_t* p = reinterpret_cast<const uint8_t*>(word.c_str());
        uint32_t cp;
        while ((cp = utf8NextCodepoint(&p))) {
            std::string buf;
            // 直接在当前函数中实现Unicode转UTF-8
            if (cp < 0x80) {
                buf += static_cast<char>(cp);
            } else if (cp < 0x800) {
                buf += static_cast<char>(0xC0 | (cp >> 6));
                buf += static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                buf += static_cast<char>(0xE0 | (cp >> 12));
                buf += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                buf += static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp < 0x200000) {
                buf += static_cast<char>(0xF0 | (cp >> 18));
                buf += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                buf += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                buf += static_cast<char>(0x80 | (cp & 0x3F));
            }
            words.push_back(buf);          // 存入拆分后的字符
            wordStyles.push_back(fontStyle);// 存入样式
            // 记录当前字符的字节长度
            wordByteLengths.push_back(buf.size()); // buf的长度就是该字符的UTF-8字节数
        }
    } else {
        // 英文按原逻辑保留
        words.push_back(std::move(word));
        wordStyles.push_back(fontStyle);
        //记录英文单词/字符的字节长度 
        wordByteLengths.push_back(word.size()); // 英文word的字节长度
    }
}


/**
 * @brief 调用addWord拆分TXT内容，生成可直接渲染的words列表
 * @param pageContent 预处理后的TXT页面内容
 * @param defaultStyle 默认字体样式
 */

void Txt::splitTxtToWords(const std::string& pageContent, EpdFontFamily::Style defaultStyle) {
    // 清空旧数据（新增：同步清空字节长度记录）
    words.clear();
    wordStyles.clear();
    wordXpos.clear();
    wordByteLengths.clear(); 

    // 调用EPUB的addWord拆分内容
    addWord(pageContent, defaultStyle);

    // 计算每个字符的X坐标（对标EPUB的wordXpos）
    int currentX = 0;
    uint32_t charsPerPage = charsPerLine * linesPerPage;
    (void)charsPerPage; // 避免未使用警告

    for (size_t i = 0; i < words.size(); i++) {
        wordXpos.push_back(currentX); // 记录X坐标

        // 分行逻辑：到行尾重置X坐标
        if ((i + 1) % charsPerLine == 0) {
            currentX = 0;
        } else {
            currentX += charWidth;
        }
    }
}


std::string Txt::readPageFromFile(uint32_t beginbype) {
    FsFile file;
    if (!SdMan.openFileForRead("TXT", filepath, file)) {
        Serial.printf("[%lu] [TXT] [ERROR] 打开文件失败: %s\n", millis(), filepath.c_str());
        return "";
    }

    // ========== 修复1：获取文件总长度（关键） ==========
    uint32_t fileTotalBytes = file.size();
    if (fileTotalBytes == 0) {
        Serial.printf("[%lu] [TXT] [ERROR] 文件为空: %s\n", millis(), filepath.c_str());
        file.close();
        return "";
    }

    uint32_t charsPerPage = charsPerLine * linesPerPage;
    uint32_t bytesPerPage = charsPerPage * 3; // 预估每字符3字节（中文）
    uint32_t startByte = beginbype;


    uint32_t maxReadBytes = bytesPerPage + 3;
    uint32_t endByte = startByte + maxReadBytes;
    Serial.printf("[%lu] [TXT] startByte: %d,endByte:%d\n", millis(), startByte,endByte);
    if (endByte > fileTotalBytes) {
        endByte = fileTotalBytes;
        maxReadBytes = endByte - startByte; // 实际可读取的字节数
    }
    if (maxReadBytes <= 0) {
        Serial.printf("[%lu] [TXT] [WARN] 起始字节%lu超过文件总长度%lu\n", millis(), startByte, fileTotalBytes);
        file.close();
        return "";
    }

    // 跳转到起始位置
    if (startByte > 0) {
        if (!file.seek(startByte)) {
            Serial.printf("[%lu] [TXT] [ERROR] 跳转到字节%lu失败\n", millis(), startByte);
            file.close();
            return "";
        }
        Serial.printf("[%lu] [TXT] [DEBUG2] 已跳转到起始字节：%lu\n", millis(), startByte);
    }


    std::string pageBytes;
    char buffer[64];
    uint32_t readBytes = 0;
    while (file.available() && readBytes < maxReadBytes) {
        size_t len = file.readBytes(buffer, sizeof(buffer));
        if (len == 0) break;
        pageBytes.append(buffer, len);
        readBytes += len;
    }
    file.close();

    // 防护：读取字节为空
    if (pageBytes.empty()) {
        Serial.printf("[%lu] [TXT] [WARN] 从字节%lu读取到0字节\n", millis(), startByte);
        return "";
    }
    Serial.printf("[%lu] [TXT] [DEBUG] 实际读取字节数：%lu（预估%lu）\n", millis(), readBytes, maxReadBytes);

    // ========== 修复4：修正UTF-8字符截断逻辑（核心） ==========
    std::string pageContent;
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(pageBytes.c_str());
    const uint8_t* endPtr = ptr + pageBytes.size();
    uint32_t charCount = 0;

    while (ptr < endPtr && charCount < charsPerPage) {
        const uint8_t* charStart = ptr; // 记录当前字符起始位置
        uint32_t cp = utf8NextCodepoint(&ptr); // ptr会自动后移
        if (cp == 0) break; // 无效UTF-8字符，停止截断

        // 复制完整的UTF-8字符（修复：用charStart到ptr的长度）
        size_t charLen = ptr - charStart;
        pageContent.append(reinterpret_cast<const char*>(charStart), charLen);
        charCount++;
    }

    // 防护：截断后内容为空
    if (pageContent.empty()) {
        Serial.printf("[%lu] [TXT] [WARN] UTF-8截断后内容为空\n", millis());
        return "";
    }

    // 调试打印（可选）
    for (size_t i=0; i<std::min(pageBytes.size(), (size_t)50); i++) {
        Serial.printf("%02X ", (uint8_t)pageBytes[i]);
    }

    // ========== 保留原有格式归一化逻辑 ==========
    std::string result = replaceHtmlEntities(pageContent.c_str());
    result = cleanTxtContent(result);       
    result = normalizeNewlines(result);    
    result = removeUtf8Bom(result);        

    //Serial.printf("[%lu] [TXT] [DEBUG11] 预处理后内容前100字符：%s\n", millis(), result.substr(0, 100).c_str());

    return result;
}



// 预计算总页数（优化：按文件大小估算，不读全文件）
void Txt::calculateTotalPages() {
    FsFile file;
    if (!SdMan.openFileForRead("TXT", filepath, file)) {
        totalPageCount = 0;
        //Serial.printf("[%lu] [TXT] [ERROR] 计算总页数时打开文件失败\n", millis());
        return;
    }
    uint32_t fileSize = file.size();
    file.close();

    uint32_t charsPerPage = charsPerLine * linesPerPage;
    uint32_t bytesPerPage = charsPerPage * 3;
    totalPageCount = (fileSize + bytesPerPage - 1) / bytesPerPage;


}

// 兼容旧方法：空实现（避免编译错误）
std::string Txt::readTxtFile() { return ""; }
void Txt::splitChaptersByNewline() {
    // 简化章节：只创建1章，避免内存占用
    TxtChapterInfo chapter;
    chapter.title = getTitle();
    chapter.startLine = 0;
    chapter.pageCount = totalPageCount;
    chapters.push_back(chapter);
}


// ✅✅✅ 全新独立函数 - 专属解析章节序号+字节偏移，只存入chapterIndex2Offset，不碰chapters任何内容
void Txt::parseChapterIndexAndOffset(int n) {
    char readBuffer[128] = {0};
    int bufferLen = 0;

    // 配置参数（贴合你的需求，检测阈值=分卷大小）
    const int CHAPTER_START = n;
    const int CHAPTER_END = n + 24;
    const uint32_t VOLUME_PAGE_SIZE = 7680;
    const char* VOLUME_TITLE_PREFIX = "分卷阅读";
    const uint64_t CHAPTER_CHECK_THRESHOLD = VOLUME_PAGE_SIZE; // 直接用分卷大小作为检测边界
    const int MAX_BACK_SEARCH_LEN = 1024; // 往前找\n的最大回溯长度，避免死循环

    Serial.printf("[ChapterRange] ✅ 本次加载范围：%d ~ %d\n", CHAPTER_START, CHAPTER_END);

    // ========== 1. 优先读缓存（保持原有逻辑，提升效率） ==========
    bool loadSuccess = loadChapterFromTxt(n);
    if (loadSuccess) {
        Serial.printf("[ChapterLoader] ✅ 缓存命中，直接返回\n");
        return;
    }

    // ========== 2. 初始化清空 ==========
    chapterActualCount = 0;
    memset(chapterDataList, 0, sizeof(chapterDataList));

    // ========== 3. 核心：先判断是否为纯分卷书籍（无章则走分卷，有章走原逻辑） ==========
    // 第一步：检测书籍是否有有效章节（首次调用或未标记时执行）
    if (!m_isVolumeOnlyBook) {
        FsFile checkFile;
        bool hasValidChapter = false;
        int chapterFoundCount = 0;

        if (checkFile.open(filepath.c_str(), FILE_READ)) {
            Serial.printf("[Parser] ✅ 开始在 %lu 字节内检测是否有章节\n", VOLUME_PAGE_SIZE);
            bool skipBom = true;
            uint64_t currentReadOffset = 0;

            // 章节匹配lambda（保持原有逻辑）
            auto isHasChapterPattern = [](const char* s, int len) -> bool {
                if (len < 6) return false;
                bool hasDi = false, hasZhang = false;
                for (int i = 0; i < len - 2; i++) {
                    if (s[i] == 0xE7 && s[i+1] == 0xAC && s[i+2] == 0xAC) hasDi = true; // 第
                    if (s[i] == 0xE7 && s[i+1] == 0xAB && s[i+2] == 0xA0) hasZhang = true; // 章
                    if (hasDi && hasZhang) return true;
                }
                return false;
            };

            // 只在 CHAPTER_CHECK_THRESHOLD（=VOLUME_PAGE_SIZE）范围内检测
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

                // 跳过BOM头
                if (skipBom && bufferLen >= 3) {
                    if ((uint8_t)readBuffer[0] == 0xEF && (uint8_t)readBuffer[1] == 0xBB && (uint8_t)readBuffer[2] == 0xBF) {
                        memmove(readBuffer, readBuffer + 3, bufferLen - 3);
                        bufferLen -= 3;
                        skipBom = false;
                    }
                }

                // 检测是否为有效章节
                bool isChapter = (bufferLen > 0 && bufferLen <= 60) && isHasChapterPattern(readBuffer, bufferLen);
                if (isChapter) {
                    hasValidChapter = true;
                    chapterFoundCount++;
                    break; // 找到章节即可退出，无需继续检测
                }
            }
            checkFile.close();
        } else {
            Serial.printf("[Parser] ❌ 打开文件失败，默认按分卷处理\n");
        }

        // 第二步：标记是否为纯分卷书籍（无有效章节则标记）
        if (!hasValidChapter) {
            Serial.printf("[VolumeMode] ⚠️ %lu 字节内无章节，标记为纯分卷书籍\n", VOLUME_PAGE_SIZE);
            m_isVolumeOnlyBook = true;
        } else {
            Serial.printf("[ChapterMode] ✅ 检测到有效章节，走原章节解析逻辑\n");
        }
    }

    // ========== 4. 纯分卷模式 ==========
    if (m_isVolumeOnlyBook) {
        FsFile file;
        if (!file.open(filepath.c_str(), FILE_READ)) {
            Serial.printf("[VolumeMode] ❌ 打开文件失败\n");
            goto save_and_exit;
        }
        uint64_t fileSize = file.size();
        int volCount = 0;

        for (int i = 0; i < 25; ++i) {
            int volIdx = CHAPTER_START + i;
            uint64_t theoryOffset = (uint64_t)volIdx * VOLUME_PAGE_SIZE; // 理论偏移：n*VOLUME_PAGE_SIZE
            uint64_t actualOffset = theoryOffset; // 实际偏移（最终取\n后）

            // ========== 核心：除第0卷外，往前找\n，取\n后作为起始 ==========
            if (volIdx > 0 && theoryOffset < fileSize) {
                // 1. 定位到理论偏移位置
                if (file.seek(theoryOffset)) {
                    uint64_t backSearchStart = (theoryOffset >= MAX_BACK_SEARCH_LEN) ? (theoryOffset - MAX_BACK_SEARCH_LEN) : 0;
                    uint64_t currentSearchPos = theoryOffset;
                    bool foundNewLine = false;

                    // 2. 从理论偏移往回找\n
                    while (currentSearchPos > backSearchStart) {
                        currentSearchPos--;
                        if (!file.seek(currentSearchPos)) break;

                        char c = file.read();
                        if (c == '\n') {
                            // 3. 找到\n，实际偏移为\n的下一个字符
                            actualOffset = currentSearchPos + 1;
                            foundNewLine = true;
                            break;
                        }
                    }

                    // 日志输出查找结果
                    if (foundNewLine) {
                        Serial.printf("[Volume] ✅ 分卷%d 找到\\n，理论偏移%llu → 实际偏移%llu\n", 
                            volIdx, (unsigned long long)theoryOffset, (unsigned long long)actualOffset);
                    } else {
                        Serial.printf("[Volume] ⚠️ 分卷%d 未找到\\n，使用理论偏移%llu\n", 
                            volIdx, (unsigned long long)theoryOffset);
                    }
                } else {
                    Serial.printf("[Volume] ❌ 分卷%d 定位失败，使用理论偏移%llu\n", 
                        volIdx, (unsigned long long)theoryOffset);
                }
            }

            // 边界检查：超出文件大小则停止
            if (actualOffset >= fileSize) {
                if (volCount == 0) break;
                else continue;
            }

            // 填充分卷数据
            chapterDataList[volCount].chapterIndex = volIdx;
            chapterDataList[volCount].byteOffset = actualOffset;
            snprintf(chapterDataList[volCount].shortTitle, TITLE_BUF_SIZE - 1, "%s%d", VOLUME_TITLE_PREFIX, volIdx + 1);
            chapterDataList[volCount].shortTitle[TITLE_BUF_SIZE - 1] = '\0';

            Serial.printf("[Volume] ✅ 分卷%d 已生成，实际偏移%llu\n", volIdx, (unsigned long long)actualOffset);
            volCount++;
        }

        file.close();
        chapterActualCount = volCount;
        goto save_and_exit;
    }

    // ========== 5. 有章节模式 ==========
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

                    chapterDataList[currSaveCount].chapterIndex = chapterFoundCount;
                    chapterDataList[currSaveCount].byteOffset = pos;
                    subUTF8String(chapterDataList[currSaveCount].shortTitle, readBuffer, bufferLen, TITLE_SUB_LEN);
                    currSaveCount++;
                }
                chapterFoundCount++;
            }
        }

        file.close();
        chapterActualCount = currSaveCount;
    }

    // ========== 6. 保存缓存并退出（统一逻辑，无冗余） ==========
save_and_exit:
    if (chapterActualCount > 0) {
        Serial.printf("[Result] ✅ 本次生成 %d 个有效条目\n", chapterActualCount);
    } else {
        Serial.printf("[Result] ⚠️ 本次无有效条目\n");
    }
    saveChapterToTxt(n);
    memset(readBuffer, 0, sizeof(readBuffer));
}


// 保存25章到单个TXT（纯C风格，无String）
void Txt::saveChapterToTxt(int startChapter) {
    FsFile f;
    char savePath[128] = {0};
    // 文件名格式：chapters_起始章n_25.txt（例：chapters_0_25.txt、chapters_25_25.txt）
    snprintf(savePath, sizeof(savePath), "%s/chapters_%d_25.txt", getCachePath().c_str(), startChapter);

    if (SdMan.openFileForWrite("TRA", savePath, f)) {
        // 遍历0~24索引，保存n~n+24章数据
        for (int i = 0; i < chapterActualCount && i < 25; i++) {
            f.printf("%d|%lu|%s\n",
                     chapterDataList[i].chapterIndex,  // 实际章节号（n~n+24）
                     chapterDataList[i].byteOffset,
                     chapterDataList[i].shortTitle);
        }
        f.sync();
        f.close();
        Serial.printf("[ChapterSaver] ✅ %d~%d章合并保存成功 → %s | 实际保存%d章\n", 
                      startChapter, startChapter+24, savePath, chapterActualCount);
    } else {
        Serial.printf("[ChapterSaver] ❌ %d~%d章合并保存失败 → %s\n", 
                      startChapter, startChapter+24, savePath);
    }
}

// 加载25章从单个TXT（纯C风格，无String）
bool Txt::loadChapterFromTxt(int startChapter) {
    chapterActualCount = 0;
    memset(chapterDataList, 0, sizeof(chapterDataList));
    bool loadOk = false;

    FsFile f;
    char loadPath[128] = {0};
    // 匹配对应起始章的缓存文件：chapters_n_25.txt
    snprintf(loadPath, sizeof(loadPath), "%s/chapters_%d_25.txt", getCachePath().c_str(), startChapter);
    if (!SdMan.openFileForRead("TRA", loadPath, f)) {
        Serial.printf("[ChapterLoader] ⚠️ 无%d~%d章缓存文件 → %s\n", startChapter, startChapter+24, loadPath);
        return false;
    }

    // 固定缓冲区读取，无动态String
    char lineBuffer[128] = {0};
    int lineLen = 0;
    int chapterNum = 0; // 0~24对应n~n+24章

    while (f.available() && chapterNum < 25) {
        // 逐行读取（纯C）
        lineLen = 0;
        memset(lineBuffer, 0, sizeof(lineBuffer));
        while (f.available()) {
            char c = f.read();
            if (c == '\n' || c == '\r' || lineLen >= 127) break;
            lineBuffer[lineLen++] = c;
        }
        if (lineLen == 0) break;

        // 拆分竖线分隔符（纯C strchr）
        char* idx1 = strchr(lineBuffer, '|');
        char* idx2 = (idx1 != NULL) ? strchr(idx1 + 1, '|') : NULL;
        if (idx1 == NULL || idx2 == NULL) continue;

        // 解析字段（纯C函数，无String::toInt）
        int actualChap = atoi(lineBuffer);       // 实际章节号（n~n+24）
        uint32_t offset = strtoul(idx1 + 1, NULL, 10);
        char* title = idx2 + 1;

        // 填充数组
        chapterDataList[chapterNum].chapterIndex = actualChap;
        chapterDataList[chapterNum].byteOffset = offset;
        memset(chapterDataList[chapterNum].shortTitle, 0, TITLE_BUF_SIZE);
        strncpy(chapterDataList[chapterNum].shortTitle, title, TITLE_BUF_SIZE - 1);
        
        chapterNum++;
        loadOk = true;
    }

    f.close();
    chapterActualCount = chapterNum;
    // 清理缓冲区
    memset(lineBuffer, 0, sizeof(lineBuffer));

    if (loadOk) {
        Serial.printf("[ChapterLoader] ✅ %d~%d章合并加载成功 → %s | 加载%d章\n", 
                      startChapter, startChapter+24, loadPath, chapterActualCount);
    }
    return loadOk;
}

void Txt::SectionLayout(uint32_t beginbype,uint32_t endbype){
    while (beginbype<endbype){
    //获取需要分页的文本；
    std::string content;
    content = readPageFromFile(beginbype);
    splitTxtToWords(content, EpdFontFamily::REGULAR);
    // 空数组防护
    if (words.empty()) {
        Serial.printf("[TXT] 警告：words数组为空，跳过渲染！\n");
        return;
    }
    
    //单页排版
      // 初始化渲染统计变量
    size_t renderEndIdx = 0;         // 精准指向屏幕最后一个字的索引
    size_t renderedTotalBytes = 0;   // 最后一个字的总字节数
    int currentLineIdx = 0;          // 屏幕行号（0=第一行，1=第二行...）
    int currentCharIdx = 0;          
    bool isRenderComplete = false;   // 标记是否渲染到屏幕底部

    extern GfxRenderer renderer; 
    // 计算并保存到成员变量（关键！）
    int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
    renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                    &orientedMarginLeft);
    orientedMarginTop += topPadding;
    orientedMarginLeft += horizontalPadding;
    orientedMarginRight += horizontalPadding;
    orientedMarginBottom += statusBarMargin;

    const auto renderableHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
    const auto screenWidth = renderer.getScreenWidth();   
    const auto renderableWidth = screenWidth - orientedMarginLeft - orientedMarginRight;
    // 优化：用中文“中”计算宽度，适配UTF-8中文排版
    this->charWidth = renderer.getTextWidth(fontId, "中", EpdFontStyles::REGULAR) + 2; // 32px（含2px字符间距）
    this->lineHeight = renderer.getFontAscenderSize(fontId) + 3; // 46px（含3px行间距）

    // ========== 核心修正：仅用段落第一行标记，屏幕第一行通过currentLineIdx判断 ==========
    bool isFirstLineOfParagraph = true; // 段落第一行标记
    const int FIRST_LINE_OFFSET = 2 * this->charWidth; // 段落第一行偏移量

    // 遍历words列表渲染（宽度判断+偏移控制）
    for (size_t i = 0; i < words.size() && !isRenderComplete; i++) {
        const std::string& utf8Char = words[i];
        
        // 1. 处理手动换行符\n → 重置行索引，标记段落第一行（但屏幕第一行仍不偏移）
        if (utf8Char == "\n") {
            renderEndIdx = i + 1; // 换行符也算字节，索引+1
            currentCharIdx = 0;   // 重置当前行字符索引
            currentLineIdx++;     // 屏幕行号+1
            isFirstLineOfParagraph = true; // 换行后进入新段落第一行
            continue; // 跳过换行符的绘制
        }

        // ========== 计算当前字符宽度 + 宽度边界判断 ==========
        int currCharWidth = this->charWidth;
        // 核心修正：
        // - 屏幕第一行（currentLineIdx=0）：整行不偏移（baseXOffset=0）
        // - 非屏幕第一行+段落第一行：偏移2*charWidth
        // - 非屏幕第一行+非段落第一行：不偏移
        int baseXOffset = 0;
        if (currentLineIdx > 0 && isFirstLineOfParagraph) {
            baseXOffset = FIRST_LINE_OFFSET;
        }

        // 预期X坐标 = 左内边距 + 基础偏移 + 字符索引*字符宽度
        int expectedXPos = orientedMarginLeft + baseXOffset + (currentCharIdx * charWidth);
        int charRightBound = expectedXPos + currCharWidth;

        // ========== 屏幕宽度判断（超出则自动换行） ==========
        if (charRightBound > orientedMarginLeft + renderableWidth) {
            currentCharIdx = 0;   // 重置行内字符索引
            currentLineIdx++;     // 屏幕行号+1
            isFirstLineOfParagraph = false; // 自动换行后，不再是段落第一行
            // 重新计算换行后的X坐标（非段落第一行，基础偏移=0）
            expectedXPos = orientedMarginLeft + 0 + (currentCharIdx * charWidth);
            // 边界防护：换行后检查高度是否超出
            int newYPos = orientedMarginTop + (currentLineIdx * lineHeight);
            if (newYPos + lineHeight > renderableHeight) {
                isRenderComplete = true;
                break;
            }
        }

        // 2. 计算最终渲染坐标
        int xPos = expectedXPos;
        int yPos = orientedMarginTop + (currentLineIdx * lineHeight);

        // 3. 高度边界判断：字符底部超出可渲染高度 → 停止渲染
        if (yPos + lineHeight > renderableHeight) {
            isRenderComplete = true;
            break;
        }


        // 5. 更新索引
        renderEndIdx = i + 1;

        // 6. 原有水平字符数满换行（兜底）
        currentCharIdx++;

    }

    // 统计最后一个字的总字节数
    PageDataList[pageCount].PageIndex=pageCount;
    PageDataList[pageCount].byteOffset=beginbype;
    renderedTotalBytes = getTotalBytesByWordRange(0, renderEndIdx);
    beginbype += renderedTotalBytes;
    pageCount++;
    Serial.printf("[TXT] 已渲染第%d页\n",pageCount);
    }
}


//用完释放目录内存
void Txt::releaseAllChapterMemory() {
    chapterActualCount = 0;
    memset(chapterDataList, 0, sizeof(chapterDataList));
    Serial.printf("[Memory] ✅ 章节数据内存已彻底清空\n");
}

// 兼容旧方法：改用按页读取
std::string Txt::getPageContent(uint32_t beginbype) {
    return readPageFromFile(beginbype);
}



// 核心加载方法（优化：不加载全量内容）
bool Txt::load() {
    Serial.printf("[%lu] [TXT] Loading TXT: %s\n", millis(), filepath.c_str());
    //clearCache();

    // 计算布局+总页数（按UTF-8字符数计算）
    calculatePageLayout();
    calculateTotalPages();
    if (totalPageCount == 0) return false;

    // 创建章节（简化版）
    splitChaptersByNewline();
    parseChapterIndexAndOffset(0);

    loaded = true;
    Serial.printf("[%lu] [TXT] Loaded: %lu total pages (no full content in memory)\n", 
        millis(), totalPageCount);
    return true;
}

// 获取标题
std::string Txt::getTitle() const {
    size_t lastSlash = filepath.find_last_of('/');
    size_t lastDot = filepath.find_last_of('.');
    if (lastSlash == std::string::npos) lastSlash = 0;
    else lastSlash++;
    if (lastDot == std::string::npos || lastDot <= lastSlash) {
        return filepath.substr(lastSlash);
    }
    return filepath.substr(lastSlash, lastDot - lastSlash);
}

// 获取总页数（返回预计算的totalPageCount）
uint32_t Txt::getPageCount() const {
    return totalPageCount;
}

// 获取指定页内容（优先缓存，按需读取）
std::string Txt::getPage(uint32_t beginbype) {

    std::string content;


    // 按需读取当前页（按UTF-8字符解析）
    content = readPageFromFile(beginbype);
    return content;
}
uint32_t Txt::getFileTotalBytes(const std::string& filePath) {
    // 关键：用 "rb" 二进制模式打开，避免Windows下\n\r转换导致字节数错误
    FILE* file = fopen(filePath.c_str(), "rb");
    if (file == nullptr) {
        Serial.printf("[TXT] 打开文件失败：%s\n", filePath.c_str());
        return 0;
    }

    // 步骤1：将文件指针移到末尾
    int seekResult = fseek(file, 0, SEEK_END);
    if (seekResult != 0) {
        Serial.printf("[TXT] 移动文件指针失败\n");
        fclose(file);
        return 0;
    }

    // 步骤2：获取文件总字节数（ftell返回long，转成uint64_t适配大文件）
    long totalBytesLong = ftell(file);
    if (totalBytesLong < 0) {
        Serial.printf("[TXT] 获取文件字节数失败\n");
        fclose(file);
        return 0;
    }
    totalBytes = static_cast<uint32_t>(totalBytesLong);

    // 步骤3：恢复文件指针到开头（如果后续要继续读取文件，必须做！）
    fseek(file, 0, SEEK_SET);

    // 步骤4：关闭文件（如果你的阅读器已持有文件句柄，可注释此步）
    fclose(file);

    Serial.printf("[TXT] 文件总字节数：%llu\n", totalBytes);
    return totalBytes;
}
