#pragma once

#include <string>
#include <vector>
#include <memory>

#include <FsHelpers.h>
#include <SDCardManager.h>
#include <EpdFont.h>
// ========== 关键新增：引入EpdFontStyle枚举的头文件 ==========
// 替换为你项目中实际定义EpdFontStyle的头文件路径（比如EpdFontFamily.h）
#include <EpdFontFamily.h>

struct TxtChapterInfo {
    std::string title;
    uint32_t startLine;
    uint32_t pageCount;
};

class Txt {
private:
    std::string filepath;          
    std::string cachePath;         
    bool loaded = false;           
    
    std::vector<TxtChapterInfo> chapters;  
    std::shared_ptr<EpdFont> font = nullptr; // 初始化为空，避免空指针
    uint16_t screenWidth = 480;    
    uint16_t screenHeight = 800;   
    uint32_t charsPerLine = 0;     
    uint32_t linesPerPage = 0;     
    uint32_t totalPageCount = 0;   // 预计算的总页数（核心优化）
    int fontId = 0;                // 外部传入的字体ID

    // ========== 仅保留1份：内部工具函数（private） ==========
    void addWord(std::string word, const EpdFontStyle fontStyle);

    // ========== 新增：UTF-8排版核心成员变量（私有化） ==========
    uint16_t charWidth = 0;    // 单个UTF-8字符宽度（calculatePageLayout算出的32px）
    uint16_t lineHeight = 0;   // 单行高度（calculatePageLayout算出的46px）

    // 文本清理工具函数（private）
    std::string cleanTxtContent(const std::string& content);
    std::string normalizeNewlines(const std::string& content);
    std::string removeUtf8Bom(const std::string& content);

    // 核心优化：按页读取相关方法（private）
    std::string readPageFromFile(uint32_t pageIdx);
    void calculateTotalPages();

    // 兼容旧方法（空实现/简化实现，private）
    void setupCacheDir() const;    
    bool clearCache() const;       
    void calculatePageLayout();    
    std::string readTxtFile();     
    void splitChaptersByNewline(); 
    std::string getPageContent(uint32_t pageIdx);
    bool savePageToCache(uint32_t pageIdx, const std::string& content);
    std::string loadPageFromCache(uint32_t pageIdx);

public:
    // ========== 新增：对标EPUB的ParsedText成员（public，供Activity访问） ==========
    std::vector<std::string> words;               // 拆分后的UTF-8字符/单词列表
    std::vector<EpdFontStyle> wordStyles;         // 每个字符/单词的样式
    std::vector<int> wordXpos;                    // 每个字符/单词的X坐标

    // ========== 仅保留1份：对外暴露的拆分函数（public） ==========
    // 修复：默认参数用项目枚举的REGULAR，而非NORMAL（匹配项目实际值）
    void splitTxtToWords(const std::string& pageContent, EpdFontStyle defaultStyle = REGULAR);

    // 保留原有构造函数（兼容旧调用）
    explicit Txt(const std::string& path) : filepath(path) {
        // 生成路径哈希值（替代中文）
        uint32_t pathHash = 5381;
        for (char c : filepath) {
            pathHash = ((pathHash << 5) + pathHash) + static_cast<uint8_t>(c);
        }
        // 缓存路径：/txt_cache/哈希值（无中文）
        cachePath = "/txt_cache/" + std::to_string(pathHash);
        // 确保目录创建
        setupCacheDir();
    }

    // 新增：带缓存路径+fontId的构造函数
    Txt(const std::string& path, const std::string& cacheRoot, int fontId = 0) 
        : filepath(path), fontId(fontId) {
        // 生成路径哈希值（替代中文）
        uint32_t pathHash = 5381;
        for (char c : filepath) {
            pathHash = ((pathHash << 5) + pathHash) + static_cast<uint8_t>(c);
        }
        // 缓存路径：缓存根目录/txt_cache/哈希值（无中文）
        cachePath = cacheRoot + "/txt_cache/" + std::to_string(pathHash);
        // 确保目录创建
        setupCacheDir();
    }

    // 动态设置fontId
    void setFontId(int newFontId) {
        fontId = newFontId;
        calculatePageLayout();
    }

    // ========== 公开getter方法（访问私有成员，权限合规） ==========
    uint32_t getCharsPerLine() const { return charsPerLine; }
    uint32_t getLinesPerPage() const { return linesPerPage; }
    uint16_t getCharWidth() const { return charWidth; }
    uint16_t getLineHeight() const { return lineHeight; }
    uint16_t getScreenWidth() const { return screenWidth; }
    uint16_t getScreenHeight() const { return screenHeight; }

    // 对外接口
    bool load();                                   
    std::string getTitle() const;                   
    bool hasChapters() const { return !chapters.empty(); }
    const std::vector<TxtChapterInfo>& getChapters() const { return chapters; }
    uint32_t getPageCount() const;                  
    std::string getPage(uint32_t pageIdx);          
    uint16_t getPageWidth() const { return screenWidth; }
    uint16_t getPageHeight() const { return screenHeight; }
    std::string getPath() const { return filepath; }          
    std::string getCachePath() const { return cachePath; }    
    void setScreenSize(uint16_t w, uint16_t h) {
        screenWidth = w;
        screenHeight = h;
        calculatePageLayout();
    }

    // Txt.h -> public 区域
    // 保存阅读进度（当前页码）
    bool saveProgress(uint32_t currentPage);
    // 读取阅读进度（返回保存的页码，无则返回0）
    uint32_t loadProgress();
};