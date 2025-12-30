#pragma once

#include <expat.h>

#include <climits>
#include <functional>
#include <memory>

#include "../ParsedText.h"
#include "../blocks/TextBlock.h"
#include <string>  // 新增：用于字符串处理

class Page;
class GfxRenderer;

#define MAX_WORD_SIZE 200
// 新增：UTF-8字符相关常量
constexpr int MAX_UTF8_CHAR_LEN = 4;  // UTF-8字符最大字节长度（中文3字节）

class ChapterHtmlSlimParser {
  const char* filepath;
  GfxRenderer& renderer;
  std::function<void(std::unique_ptr<Page>)> completePageFn;
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
  int marginTop;
  int marginRight;
  int marginBottom;
  int marginLeft;
  bool extraParagraphSpacing;

  void startNewTextBlock(TextBlock::BLOCK_STYLE style);
  void makePages();
  // XML callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  static void XMLCALL endElement(void* userData, const XML_Char* name);

   // 新增：字符类型判断/处理函数声明
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
  void flushEnglishWordBuffer(EpdFontStyle fontStyle);


 public:
  explicit ChapterHtmlSlimParser(const char* filepath, GfxRenderer& renderer, const int fontId,
                                 const float lineCompression, const int marginTop, const int marginRight,
                                 const int marginBottom, const int marginLeft, const bool extraParagraphSpacing,
                                 const std::function<void(std::unique_ptr<Page>)>& completePageFn)
      : filepath(filepath),
        renderer(renderer),
        fontId(fontId),
        lineCompression(lineCompression),
        marginTop(marginTop),
        marginRight(marginRight),
        marginBottom(marginBottom),
        marginLeft(marginLeft),
        extraParagraphSpacing(extraParagraphSpacing),
        completePageFn(completePageFn) {}
  ~ChapterHtmlSlimParser() = default;
  bool parseAndBuildPages();
  void addLineToPage(std::shared_ptr<TextBlock> line);
};
