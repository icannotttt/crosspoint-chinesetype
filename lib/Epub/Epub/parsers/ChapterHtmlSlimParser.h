#pragma once

#include <expat.h>

#include <climits>
#include <functional>
#include <memory>
#include <cstring>  // 新增：用于memset等内存操作

#include "../ParsedText.h"
#include "../blocks/TextBlock.h"
#include <string>  // 新增：用于字符串处理
#include <EpdFontFamily.h>

// 新增：声明EpdFontFamily命名空间（避免编译报错，与实现代码中的字体样式对应）

class Page;
class GfxRenderer;
class FsFile;  // 新增：声明FsFile类（用于资源释放函数）

#define MAX_WORD_SIZE 200

// 新增：UTF-8字符相关常量
constexpr int MAX_UTF8_CHAR_LEN = 4;  // UTF-8字符最大字节长度（中文3字节）

class ChapterHtmlSlimParser {
  // 原有成员变量（保留不变，补充部分默认初始化）
  const std::string& filepath;
  GfxRenderer& renderer;
  std::function<void(std::unique_ptr<Page>)> completePageFn;
  std::function<void(int)> progressFn;  // Progress callback (0-100)
  int depth = 0;
  int skipUntilDepth = INT_MAX;
  int boldUntilDepth = INT_MAX;
  int italicUntilDepth = INT_MAX;
  // buffer for building up words from characters, will auto break if longer than this
  // leave one char at end for null pointer
  char partWordBuffer[MAX_WORD_SIZE + 1] = {};
  int partWordBufferIndex = 0;
  // 新增：英文单词缓存（用于拼接英文单词，按空格拆分）
  std::string englishWordBuffer;

  std::unique_ptr<ParsedText> currentTextBlock = nullptr;
  std::unique_ptr<Page> currentPage = nullptr;
  int16_t currentPageNextY = 0;
  int fontId;
  float lineCompression;
  bool extraParagraphSpacing;
  int viewportWidth;
  int viewportHeight;

  // 补充：边距成员变量添加默认初始化（避免未初始化编译警告）
  int marginTop = 0;
  int marginRight = 0;
  int marginBottom = 0;
  int marginLeft = 0;

  // 新增：当前块样式（解决嵌套标签格式错乱，对应实现代码中的样式传递）
  TextBlock::BLOCK_STYLE currentBlockStyle = TextBlock::JUSTIFIED;

  // 原有成员函数声明（保留不变）
  void startNewTextBlock(TextBlock::BLOCK_STYLE style);
  void makePages();
  // XML callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  static void XMLCALL endElement(void* userData, const XML_Char* name);

  // 新增：<p>标签内容最大固定大小（可根据内存情况调整，建议先从300-500开始）
  // 1. <p>单段安全阈值
  constexpr static size_t P_SAFE_THRESHOLD = 200;

  // 2. 标记当前是否在<p>标签内（你已有的，保留）
  bool isInPTag = false;

  // 3. 累计当前<p>已处理的字符数（用于判断是否达到阈值）
  size_t currentPCharCount = 0;

  // 4. （可选）标记是否是拆分的虚拟<p>（避免无限递归拆分）
  bool isVirtualP = false;

  // 新增：字符类型判断/处理函数声
  /**
   * @brief 判断字符是否为空白符（空格/换行/制表符）
   */
  static bool isWhitespace(const char c);

  /**
   * @brief 获取UTF-8字符的字节长度（1=英文，3=中文）
   */
  static int getUtf8CharLength(const unsigned char* u8);

  /**
   * @brief 判断是否为中文字符（Unicode：0x4E00-0x9FFF）
   */
  static bool isChineseChar(const unsigned char* u8, int charLen);

  /**
   * @brief 清空英文单词缓存并添加为Word
   */
  void flushEnglishWordBuffer(const EpdFontFamily::Style fontStyle);

  // 新增：核心辅助函数声明（必须补充，对应实现中的标签匹配逻辑）
  /**
   * @brief 判断标签是否匹配目标标签数组
   */
  static bool matches(const char* tag_name, const char* possible_tags[], const int possible_tag_count);

  // 新增：统一资源释放函数声明（对应实现中的内存泄漏修复逻辑）
  /**
   * @brief 统一释放expat解析器和文件资源，避免内存泄漏
   */
  void cleanupResources(XML_Parser parser, FsFile& file);

 public:
  // 原有构造函数（保留不变）
  explicit ChapterHtmlSlimParser(const std::string& filepath, GfxRenderer& renderer, const int fontId,
                                 const float lineCompression, const bool extraParagraphSpacing, const int viewportWidth,
                                 const int viewportHeight,
                                 const std::function<void(std::unique_ptr<Page>)>& completePageFn,
                                 const std::function<void(int)>& progressFn = nullptr)
      : filepath(filepath),
        renderer(renderer),
        fontId(fontId),
        lineCompression(lineCompression),
        extraParagraphSpacing(extraParagraphSpacing),
        viewportWidth(viewportWidth),
        viewportHeight(viewportHeight),
        completePageFn(completePageFn),
        progressFn(progressFn) {}
  ~ChapterHtmlSlimParser() = default;
  bool parseAndBuildPages();
  void addLineToPage(std::shared_ptr<TextBlock> line);
};