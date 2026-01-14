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
// ========== 新增：声明EPUB依赖的外部函数（确保能调用） ==========
extern void calibrateUtf8Pointer(const unsigned char* ptr);
extern uint32_t utf8NextCodepoint(const uint8_t** ptr);

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
    this->charWidth = renderer.getTextWidth(fontId, "中", REGULAR) + 2; // 32px（含2px字符间距）
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
void Txt::addWord(std::string word, const EpdFontStyle fontStyle) {
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
            // ========== 新增：记录当前字符的字节长度 ==========
            wordByteLengths.push_back(buf.size()); // buf的长度就是该字符的UTF-8字节数
        }
    } else {
        // 英文按原逻辑保留
        words.push_back(std::move(word));
        wordStyles.push_back(fontStyle);
        // ========== 新增：记录英文单词/字符的字节长度 ==========
        wordByteLengths.push_back(word.size()); // 英文word的字节长度
    }
}

// ========== 新增：拆分TXT内容为EPUB格式的words列表 ==========
/**
 * @brief 调用addWord拆分TXT内容，生成可直接渲染的words列表
 * @param pageContent 预处理后的TXT页面内容
 * @param defaultStyle 默认字体样式
 */
// ========== 新增：拆分TXT内容为EPUB格式的words列表 ==========
void Txt::splitTxtToWords(const std::string& pageContent, EpdFontStyle defaultStyle) {
    // 清空旧数据（新增：同步清空字节长度记录）
    words.clear();
    wordStyles.clear();
    wordXpos.clear();
    wordByteLengths.clear(); // ========== 新增：清空字节长度记录 ==========

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

// 核心优化：1. 流式读取当前页字节 2. 简化UTF-8拆分（交给addWord） 3. 文本格式归一化（对齐EPUB）
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

    // ========== 修复2：修正endByte，避免越界 ==========
    // 最多读取到文件末尾，且多读3字节（避免截断中文）
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

    // 跳转到起始位置（增加seek失败防护）
    if (startByte > 0) {
        if (!file.seek(startByte)) {
            Serial.printf("[%lu] [TXT] [ERROR] 跳转到字节%lu失败\n", millis(), startByte);
            file.close();
            return "";
        }
        Serial.printf("[%lu] [TXT] [DEBUG2] 已跳转到起始字节：%lu\n", millis(), startByte);
    }

    // ========== 修复3：读取字节（限制最大读取量，增加空读取防护） ==========
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

    Serial.printf("[%lu] [TXT] [DEBUG11] 预处理后内容前100字符：%s\n", millis(), result.substr(0, 100).c_str());

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

    // 调试日志
    //Serial.printf("[%lu] [TXT] [DEBUG_TOTAL] 文件大小：%lu字节 | 每页字节数：%lu | 总页数：%lu\n", 
    //    millis(), fileSize, bytesPerPage, totalPageCount);
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
    // ========== 【替换map释放：解析前彻底清空结构体数组，零内存残留】 ==========
    chapterActualCount = 0;
    memset(chapterDataList, 0, sizeof(chapterDataList));
    Serial.printf("[Memory] ✅ 解析前：所有章节数据内存已彻底释放\n");

    FsFile file;
    if (!file.open(filepath.c_str(), FILE_READ)) {
        Serial.printf("[ChapterIndexParser] ❌ 打开文件失败：%s\n", filepath.c_str());
        return;
    }

    Serial.printf("[ChapterIndexParser] ✅ 开始解析章节序号+字节偏移+提取20字标题 | 文件：%s\n", filepath.c_str());
    Serial.printf("[ChapterIndexParser] ✅ 匹配规则：UTF8中文【第n章】格式 + 单行≤60字\n");
    Serial.printf("[ChapterIndexParser] ✅ 解析规则：从第%d条章节开始，最多存入【30条】+ 标题截取前20字\n", n+1);

    const int MAX_VALID_LEN = 60;
    const int TITLE_SUB_LEN = 20;  // 目录标题只保留前20个UTF8字符（核心配置）
    int chapterFoundCount = 0;     // 全局章节序号(从0开始)
    int currSaveCount = 0;         // 本次存入的数量，最多存30章，满了就退出
    bool skipBom = true;

    // ✅ 核心工具函数1：匹配UTF8中文【第n章】格式（原逻辑不变，无BUG）
    auto isHasChapterPattern = [](const std::string& s) -> bool {
        int len = s.length();
        for (int i = 0; i < len - 6; ) {
            if ( (uint8_t)s[i] == 0xE7 && (uint8_t)s[i+1] == 0xAC && (uint8_t)s[i+2] == 0xAC ) {
                int numPos = i + 3;
                if (numPos < len && s[numPos] >= '0' && s[numPos] <= '9') {
                    while (numPos < len && s[numPos] >= '0' && s[numPos] <= '9') numPos++;
                    if ( numPos +2 < len && (uint8_t)s[numPos] == 0xE7 && (uint8_t)s[numPos+1] == 0xAB && (uint8_t)s[numPos+2] == 0xA0 ) {
                        return true;
                    }
                }
            }
            if( (uint8_t)s[i] >= 0xE0 ) { i +=3; } else { i +=1; }
        }
        return false;
    };

    // ✅ 核心工具函数2：【精准截取UTF8字符串前20个字符】- 做目录专用，重中之重！
    // 特点：不会截断半个中文、中英文兼容、不足20字原样返回，完美适配小说标题
    auto subUTF8String = [](const std::string& str, int keepCount) -> std::string {
        std::string res;
        int charCount = 0;
        int len = str.length();
        for (int i = 0; i < len && charCount < keepCount; ) {
            res += str[i];
            // UTF8编码规则：中文占3字节(0xE0~0xEF)，英文/数字/符号占1字节
            if ((uint8_t)str[i] >= 0xE0) {
                if(i+1 < len) res += str[i+1];
                if(i+2 < len) res += str[i+2];
                i += 3;
            } else {
                i += 1;
            }
            charCount++;
        }
        return res;
    };

    // 循环解析，存满30章立刻退出，永不超量
    while (file.available() && currSaveCount < 30) {
        String readLine = file.readStringUntil('\n');
        std::string line = readLine.c_str();
        
        // 兼容Windows \r\n 换行符，去除行尾\r
        if (!line.empty() && line.back() == '\r') line.pop_back();
        
        // 跳过UTF8 BOM头（EF BB BF）
        if (skipBom && !line.empty() && (uint8_t)line[0] == 0xEF && (uint8_t)line[1] == 0xBB && (uint8_t)line[2] == 0xBF) {
            line = line.substr(3);
            skipBom = false;
        }

        // 判断当前行是否是合规的章节标题
        bool isChapter = false;
        if (line.length() > 0 && line.length() <= MAX_VALID_LEN) {
            isChapter = isHasChapterPattern(line);
        }

        // ✅ 核心逻辑：匹配成功+章节序号在区间内 → 存偏移量+存20字标题 (替换为结构体数组赋值)
        if (isChapter) {
            // 精准计算章节在文件中的绝对字节偏移量（无错误，原逻辑不变）
            uint32_t chapterStartByte = file.position() - readLine.length() - 1;
            
            // 只处理 n 开始的章节，存满30章自动停止
            if (chapterFoundCount >= n) {
                // ✅ 唯一改动：把map赋值 替换为 结构体数组赋值
                chapterDataList[currSaveCount].chapterIndex = chapterFoundCount;
                chapterDataList[currSaveCount].byteOffset = chapterStartByte;
                std::string shortTitle = subUTF8String(line, TITLE_SUB_LEN);
                strncpy(chapterDataList[currSaveCount].shortTitle, shortTitle.c_str(), TITLE_BUF_SIZE - 1);
                
                // 打印日志：和原来一模一样，不变
                Serial.printf("[ChapterIndexParser] ✅ 存储成功 ✅ 章节序号=%d | 完整标题=%s | 目录标题=%s | 字节偏移=0x%X(%u)\n", 
                              chapterFoundCount, line.c_str(), shortTitle.c_str(), chapterStartByte, chapterStartByte);
                currSaveCount++;
            }
            chapterFoundCount++;
        }
    }
    // ✅ 赋值实际存储的章节数量，供外部接口遍历使用
    chapterActualCount = currSaveCount;

    file.close();
    Serial.printf("[ChapterIndexParser] ======================\n");
    Serial.printf("[ChapterIndexParser] 📖 解析完成！本次共存储【%d】个章节的完整数据\n", currSaveCount);
    Serial.printf("[ChapterIndexParser] ======================\n");
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

// 保存页到缓存
bool Txt::savePageToCache(uint32_t pageIdx, const std::string& content) {
    setupCacheDir();
    std::string pagePath = cachePath + "/page_" + std::to_string(pageIdx) + ".txt";
    FsFile file;
    if (!SdMan.openFileForWrite("TXT", pagePath, file)) {
        Serial.printf("[%lu] [TXT] Failed to save page %lu to cache\n", millis(), pageIdx);
        return false;
    }
    file.write(content.c_str(), content.length());
    file.close();
    return true;
}

// 从缓存加载页
std::string Txt::loadPageFromCache(uint32_t pageIdx) {
    std::string pagePath = cachePath + "/page_" + std::to_string(pageIdx) + ".txt";
    if (!SdMan.exists(pagePath.c_str())) {
        return "";
    }
    FsFile file;
    if (!SdMan.openFileForRead("TXT", pagePath, file)) {
        return "";
    }
    std::string content;
    char buffer[128]; // 小缓冲区，减少内存分配
    while (file.available()) {
        size_t len = file.readBytes(buffer, sizeof(buffer));
        content.append(buffer, len);
    }
    file.close();
    return content;
}

// 核心加载方法（优化：不加载全量内容）
bool Txt::load() {
    Serial.printf("[%lu] [TXT] Loading TXT: %s\n", millis(), filepath.c_str());
    clearCache();

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
