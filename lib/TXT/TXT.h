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

// 宏定义常量 (按需修改数值即可，和你需求一致)
#define MAX_SAVE_CHAPTER  30    // 最多存30章
#define TITLE_KEEP_LENGTH 20    // 标题截取前20个UTF8字符
#define TITLE_BUF_SIZE    64    // 标题缓冲区64字节，完美匹配你的static char title[64]
#define MAX_SAVE_PAGE 100

// ✅ 你指定的结构体，一字不改！
struct ChapterData {
    int chapterIndex;        // 章节序号
    uint32_t byteOffset;     // 字节偏移量
    char shortTitle[TITLE_BUF_SIZE]; // 截取后的标题，char数组格式
};

struct PageData {
    int PageIndex;        // 章节序号
    uint32_t byteOffset;     // 字节偏移量
};

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

    // ✅ 核心替换：删掉std::map，换成结构体数组 + 实际存储计数
    ChapterData chapterDataList[MAX_SAVE_CHAPTER];
    PageData PageDataList[MAX_SAVE_PAGE];
    int chapterActualCount = 0;
    int pageCount =0;

    // ========== 仅保留1份：内部工具函数（private） ==========
    void addWord(std::string word, const EpdFontFamily::Style fontStyle);

    // ========== 新增：UTF-8排版核心成员变量（私有化） ==========
    uint16_t charWidth = 0;    // 单个UTF-8字符宽度（calculatePageLayout算出的32px）
    uint16_t lineHeight = 0;   // 单行高度（calculatePageLayout算出的46px）
    uint32_t totalBytes =0 ;

    // 文本清理工具函数（private）
    std::string cleanTxtContent(const std::string& content);
    std::string normalizeNewlines(const std::string& content);
    std::string removeUtf8Bom(const std::string& content);

    // 核心优化：按页读取相关方法（private）
    std::string readPageFromFile(uint32_t beginbype);
    void calculateTotalPages();

    // 兼容旧方法（空实现/简化实现，private）
    void setupCacheDir() const;    
    bool clearCache() const;       
    void calculatePageLayout();    
    std::string readTxtFile();     
    void splitChaptersByNewline(); 
    void saveChapterToTxt(int startChapter);
    bool loadChapterFromTxt(int startChapter);

    std::string getPageContent(uint32_t beginbype);
    uint32_t getFileTotalBytes(const std::string& filePath);
    bool m_isVolumeOnlyBook = false;
 

public:
    // ========== 新增：对标EPUB的ParsedText成员（public，供Activity访问） ==========
    std::vector<std::string> words;               // 拆分后的UTF-8字符/单词列表
    std::vector<EpdFontFamily::Style> wordStyles;         // 每个字符/单词的样式
    std::vector<int> wordXpos;                    // 每个字符/单词的X坐标

    // ========== 仅保留1份：对外暴露的拆分函数（public） ==========
    // 修复：默认参数用项目枚举的REGULAR，而非NORMAL（匹配项目实际值）
    void splitTxtToWords(const std::string& pageContent, EpdFontFamily::Style fontStyle = EpdFontFamily::REGULAR);
    void parseChapterIndexAndOffset(int n);
    void releaseAllChapterMemory(); // 释放目录内存

    std::vector<size_t> wordByteLengths; // 每个word对应的字节长度（和words一一对应）
    
    // 获取指定word索引范围内的总字节数
    size_t getTotalBytesByWordRange(size_t startIdx, size_t endIdx) {
        size_t total = 0;
        for (size_t i = startIdx; i < endIdx && i < wordByteLengths.size(); i++) {
            total += wordByteLengths[i];
        }
        return total;
    }

    // 清空字节长度记录
    void resetWordByteLengths() {
        wordByteLengths.clear();
    }

    // 保留原有构造函数（兼容旧调用）
    // 保留原有构造函数（兼容旧调用）
    explicit Txt(const std::string& path) : filepath(path) {
        // ✅ 修改：复用Epub的哈希方式，路径拼接为 /TXT/txt_哈希值 （单层目录）
        cachePath = "/TXT/txt_" + std::to_string(std::hash<std::string>{}(this->filepath));
        setupCacheDir();
        Serial.printf("[%lu] 进入该界面1: \n", millis());
    }

    // 新增：带缓存路径+fontId的构造函数（保留，修改哈希方式）
    Txt(const std::string& path, const std::string& cacheRoot, int fontId = 0) 
        : filepath(path), fontId(fontId) {
        // ✅ 修改：复用Epub的哈希方式，路径拼接为 cacheRoot/txt_哈希值 （单层目录）
        cachePath = cacheRoot + "/txt_" + std::to_string(std::hash<std::string>{}(this->filepath));
        setupCacheDir();
        Serial.printf("[%lu] 进入该界面2: \n", millis());
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
    uint32_t gettotalbytes() const { return totalBytes; }

    // ✅ 适配结构体数组 - 接口声明不变、调用不变、返回值不变，仅改内部实现
    uint32_t getChapterOffsetByIndex(int chapterIndex) {
        for(int i = 0; i < chapterActualCount; i++) {
            if(chapterDataList[i].chapterIndex == chapterIndex) {
                return chapterDataList[i].byteOffset;
            }
        }
        return 0; // 无此章节返回0
    }

    // ✅ 适配结构体数组 - 和上面接口风格完全一致，无任何变化
    std::string getChapterTitleByIndex(int chapterIndex) {
        for(int i = 0; i < chapterActualCount; i++) {
            if(chapterDataList[i].chapterIndex == chapterIndex) {
                return std::string(chapterDataList[i].shortTitle);
            }
        }
        return ""; // 无此章节返回空字符串
    }

    // ========== 你额外要的 char数组版标题接口 (补充加好，按需调用) ==========
    void getChapterTitleByIndex(int chapterIndex, char* outTitleBuf) {
        memset(outTitleBuf, 0, TITLE_BUF_SIZE);
        for(int i = 0; i < chapterActualCount; i++) {
            if(chapterDataList[i].chapterIndex == chapterIndex) {
                strncpy(outTitleBuf, chapterDataList[i].shortTitle, TITLE_BUF_SIZE - 1);
                break;
            }
        }
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

    // 对外接口
    bool load();                                   
    std::string getTitle() const;                   
    bool hasChapters() const { return !chapters.empty(); }
    const std::vector<TxtChapterInfo>& getChapters() const { return chapters; }
    uint32_t getPageCount() const;                  
    std::string getPage(uint32_t beginbype);          
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

        // 对外公共函数：初始化缓存目录（原三参构造的完整逻辑抽离，你在OnEnter里直接调用这个！）
    void initCache() {
        this->setupCacheDir();
        // 5. 你的日志打印，能正常输出，确认函数执行成功
        Serial.printf("[%lu] [Txt] initCache执行成功 ✅ 缓存路径：%s \n", millis(), this->cachePath.c_str());
    }


        // ========== 新增：Section相关核心函数声明 ==========
    /**
     * @brief 预计算指定章节（section）的每页beginbype（核心排版函数）
     * @param chapterIndex 要计算的章节索引（对应目录中的章节号）
     * @param sectionNum 输出的section编号（用于命名bin文件，如section_0.bin）
     * @return 该section内每页的beginbype列表（字节偏移）
     */
    void SectionLayout(uint32_t beginbype,uint32_t endbype);

};