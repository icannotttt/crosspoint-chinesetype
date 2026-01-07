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
    // 优化：用中文“中”计算宽度，适配UTF-8中文排版
    this->charWidth = renderer.getTextWidth(fontId, "中", REGULAR) + 2; // 32px（含2px字符间距）
    this->lineHeight = renderer.getFontAscenderSize(fontId) + 3; // 46px（含3px行间距）

    // 强制设置正确的屏幕尺寸（修复之前800×600的错误）
    screenWidth = 480;
    screenHeight = 800;
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
        }
    } else {
        // 英文按原逻辑保留
        words.push_back(std::move(word));
        wordStyles.push_back(fontStyle);
    }
}

// ========== 新增：拆分TXT内容为EPUB格式的words列表 ==========
/**
 * @brief 调用addWord拆分TXT内容，生成可直接渲染的words列表
 * @param pageContent 预处理后的TXT页面内容
 * @param defaultStyle 默认字体样式
 */
void Txt::splitTxtToWords(const std::string& pageContent, EpdFontStyle defaultStyle) {
    // 清空旧数据
    words.clear();
    wordStyles.clear();
    wordXpos.clear();

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
// ========== 改动：删减重复的UTF-8字符拆分逻辑，保留核心读取/预处理 ==========
std::string Txt::readPageFromFile(uint32_t pageIdx) {
    FsFile file;
    if (!SdMan.openFileForRead("TXT", filepath, file)) {
        Serial.printf("[%lu] [TXT] [ERROR] 打开文件失败: %s\n", millis(), filepath.c_str());
        return "";
    }

    uint32_t charsPerPage = charsPerLine * linesPerPage;
    uint32_t bytesPerPage = charsPerPage * 3; 
    uint32_t startByte = pageIdx * bytesPerPage;
    uint32_t endByte = startByte + bytesPerPage + 3; // 多读取3字节，避免截断中文

    // 跳转到起始位置
    if (startByte > 0) {
        file.seek(startByte);
        Serial.printf("[%lu] [TXT] [DEBUG2] 已跳转到起始字节：%lu\n", millis(), startByte);
    }

    // 读取字节（多读3字节）
    std::string pageBytes;
    char buffer[64];
    uint32_t readBytes = 0;
    while (file.available() && readBytes < endByte - startByte) {
        size_t len = file.readBytes(buffer, sizeof(buffer));
        if (len == 0) break;
        pageBytes.append(buffer, len);
        readBytes += len;
    }
    file.close();

    // ========== 关键新增：按UTF-8字符数截断，而非字节数 ==========
    std::string pageContent;
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(pageBytes.c_str());
    const uint8_t* startPtr = ptr;
    uint32_t charCount = 0;
    while (ptr < startPtr + pageBytes.size() && charCount < charsPerPage) {
        uint32_t cp = utf8NextCodepoint(&ptr);
        if (cp == 0) break;
        // 复制完整字符
        size_t charLen = ptr - startPtr;
        pageContent.append(reinterpret_cast<const char*>(startPtr), charLen);
        startPtr = ptr;
        charCount++;
    }

    // 调试打印（原始字节）
    //Serial.printf("[%lu] [TXT] [DEBUG3] 读取字节数：%lu | 字节内容前50字节（十六进制）：", millis(), pageBytes.size());
    for (size_t i=0; i<std::min(pageBytes.size(), (size_t)50); i++) {
        Serial.printf("%02X ", (uint8_t)pageBytes[i]);
    }
    //Serial.printf("\n");
    //Serial.printf("[%lu] [TXT] [DEBUG4] 字节内容前50字节（原始字符串）：%s\n", millis(), pageBytes.substr(0, 50).c_str());

    // 格式归一化（保留原有逻辑）
    std::string result = replaceHtmlEntities(pageContent.c_str());
    result = cleanTxtContent(result);       
    result = normalizeNewlines(result);    
    result = removeUtf8Bom(result);        

    //Serial.printf("[%lu] [TXT] [DEBUG11] 预处理后内容（交给addWord）前100字符：%s\n", millis(), result.substr(0, 100).c_str());

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

// 兼容旧方法：改用按页读取
std::string Txt::getPageContent(uint32_t pageIdx) {
    return readPageFromFile(pageIdx);
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
std::string Txt::getPage(uint32_t pageIdx) {
    if (!loaded || pageIdx >= totalPageCount) return "";
    
    // 优先读缓存
    std::string content = loadPageFromCache(pageIdx);
    if (!content.empty()) return content;

    // 按需读取当前页（按UTF-8字符解析）
    content = readPageFromFile(pageIdx);
    savePageToCache(pageIdx, content);
    return content;
}

// Txt.cpp 新增以下两个函数
bool Txt::saveProgress(uint32_t currentPage) {
    std::string progressPath = cachePath + "/progress.bin";
    

    FsFile file;
    if (!SdMan.openFileForWrite("TXT_PROGRESS", progressPath, file)) {
        Serial.printf("[%lu] [TXT] [ERROR] 保存进度失败：无法打开文件 %s\n", millis(), progressPath.c_str());
        return false;
    }

    // ========== 核心修改：按现有逻辑的二进制格式保存 ==========
    // 现有读取逻辑：data[0]+data[1]<<8 = currentChapterIndex，data[2]+data[3]<<8 = nextPageNumber
    uint8_t data[4] = {0};
    // currentChapterIndex 暂存0（如果需要保存章节号，可传参进来）
    data[0] = 0; 
    data[1] = 0;
    // nextPageNumber = 当前页码（拆分为2字节）
    data[2] = currentPage & 0xFF;        // 低8位
    data[3] = (currentPage >> 8) & 0xFF; // 高8位

    // 写入4字节二进制数据
    size_t written = file.write(data, 4);
    file.close();

    if (written == 4) {
        Serial.printf("[%lu] [TXT] 进度保存成功：%s -> 页码 %lu (二进制：0x%02X%02X)\n", 
                      millis(), progressPath.c_str(), currentPage, data[3], data[2]);
        return true;
    } else {
        Serial.printf("[%lu] [TXT] [ERROR] 进度保存失败：写入字节数异常 %zu/4\n", millis(), written);
        return false;
    }
}

uint32_t Txt::loadProgress() {
    std::string progressPath = cachePath + "/progress.bin";
    
    FsFile file;
    if (!SdMan.openFileForRead("TXT_PROGRESS", progressPath, file)) {
        Serial.printf("[%lu] [TXT] 无历史进度：%s\n", millis(), progressPath.c_str());
        return 0;
    }

    // ========== 核心修改：按现有逻辑的二进制格式读取 ==========
    uint8_t data[4];
    size_t read = file.read(data, 4);
    file.close();

    if (read != 4) {
        Serial.printf("[%lu] [TXT] 进度文件格式错误，读取字节数 %zu/4，返回第0页\n", millis(), read);
        return 0;
    }

    // 解析：nextPageNumber = data[2] + (data[3] << 8)
    uint32_t savedPage = data[2] + (data[3] << 8);
    // 校验页码有效性
    if (savedPage >= totalPageCount) {
        Serial.printf("[%lu] [TXT] 保存的页码 %lu 超出总页数 %lu，返回第0页\n", millis(), savedPage, totalPageCount);
        return 0;
    }

    Serial.printf("[%lu] [TXT] 恢复历史进度：%s -> 页码 %lu (二进制：0x%02X%02X)\n", 
                  millis(), progressPath.c_str(), savedPage, data[3], data[2]);
    return savedPage;
}