#include "ParsedText.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>
#include "Utf8.h"

constexpr int MAX_COST = std::numeric_limits<int>::max();

void ParsedText::addWord(std::string word, const EpdFontStyle fontStyle) {
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
      words.push_back(buf);
      wordStyles.push_back(fontStyle);
    }
  } else {
    // 英文按原逻辑保留
    words.push_back(std::move(word));
    wordStyles.push_back(fontStyle);
  }
}

// Consumes data to minimize memory usage
void ParsedText::layoutAndExtractLines(const GfxRenderer& renderer, const int fontId, const int horizontalMargin,
                                       const std::function<void(std::shared_ptr<TextBlock>)>& processLine) {
  if (words.empty()) {
    return;
  }

  const size_t totalWordCount = words.size();
  const int pageWidth = renderer.getScreenWidth() - horizontalMargin;
  const int spaceWidth = renderer.getSpaceWidth(fontId);

  std::vector<uint16_t> wordWidths;
  wordWidths.reserve(totalWordCount);

  // add em-space at the beginning of first word in paragraph to indent
  // 在段落首个单词开头添加全角空格以实现缩进
  if (!extraParagraphSpacing) {
    std::string& first_word = words.front();
    //first_word.insert(0, "\xe2\x80\x83");
        // 替换为2个中文全角空格（UTF-8编码），适配多数ESP32显示场景
    first_word.insert(0, "\xe3\x80\x80\xe3\x80\x80");
  }

  auto wordsIt = words.begin();
  auto wordStylesIt = wordStyles.begin();

  while (wordsIt != words.end()) {
    wordWidths.push_back(renderer.getTextWidth(fontId, wordsIt->c_str(), *wordStylesIt));

    std::advance(wordsIt, 1);
    std::advance(wordStylesIt, 1);
  }

  // DP table to store the minimum badness (cost) of lines starting at index i
  // 动态规划表，存储从索引 i 开始的行的最小损耗值（代价）
  std::vector<int> dp(totalWordCount);
  // 'ans[i]' stores the index 'j' of the *last word* in the optimal line starting at 'i'
  // 'ans[i]' 存储从索引 'i' 开始的最优行中最后一个单词的索引 'j'
  std::vector<size_t> ans(totalWordCount);

  // Base Case
  dp[totalWordCount - 1] = 0;
  ans[totalWordCount - 1] = totalWordCount - 1;

  for (int i = totalWordCount - 2; i >= 0; --i) {
    int currlen = -spaceWidth;
    dp[i] = MAX_COST;

    for (size_t j = i; j < totalWordCount; ++j) {
      // Current line length: previous width + space + current word width
      // 当前行长度：已占用宽度 + 空格宽度 + 当前单词宽度
      currlen += wordWidths[j] + spaceWidth;

      if (currlen > pageWidth) {
        break;
      }

      int cost;
      if (j == totalWordCount - 1) {
        cost = 0;  // Last line
      } else {
        const int remainingSpace = pageWidth - currlen;
        // Use long long for the square to prevent overflow
        // 使用长整型存储平方值以避免溢出
        const long long cost_ll = static_cast<long long>(remainingSpace) * remainingSpace + dp[j + 1];

        if (cost_ll > MAX_COST) {
          cost = MAX_COST;
        } else {
          cost = static_cast<int>(cost_ll);
        }
      }

      if (cost < dp[i]) {
        dp[i] = cost;
        ans[i] = j;  // j is the index of the last word in this optimal line
      }
    }
  }

  // Stores the index of the word that starts the next line (last_word_index + 1)
  // 存储下一行起始单词的索引（即最后一个单词的索引 + 1）
  std::vector<size_t> lineBreakIndices;
  size_t currentWordIndex = 0;
  constexpr size_t MAX_LINES = 1000;

  while (currentWordIndex < totalWordCount) {
    if (lineBreakIndices.size() >= MAX_LINES) {
      break;
    }

    size_t nextBreakIndex = ans[currentWordIndex] + 1;
    lineBreakIndices.push_back(nextBreakIndex);

    currentWordIndex = nextBreakIndex;
  }

  // Initialize iterators for consumption
  // 初始化
  auto wordStartIt = words.begin();
  auto wordStyleStartIt = wordStyles.begin();
  size_t wordWidthIndex = 0;

  size_t lastBreakAt = 0;
  for (const size_t lineBreak : lineBreakIndices) {
    const size_t lineWordCount = lineBreak - lastBreakAt;

    // Calculate end iterators for the range to splice
    // 计算待拼接区间的结束迭代器
    auto wordEndIt = wordStartIt;
    auto wordStyleEndIt = wordStyleStartIt;
    std::advance(wordEndIt, lineWordCount);
    std::advance(wordStyleEndIt, lineWordCount);

    // Calculate total word width for this line
    // 计算该行的单词总宽度
    int lineWordWidthSum = 0;
    for (size_t i = 0; i < lineWordCount; ++i) {
      lineWordWidthSum += wordWidths[wordWidthIndex + i];
    }

    // Calculate spacing
    // 计算间距
    int spareSpace = pageWidth - lineWordWidthSum;

    int spacing = spaceWidth;
    const bool isLastLine = lineBreak == totalWordCount;

    if (style == TextBlock::JUSTIFIED && !isLastLine && lineWordCount >= 2) {
      spacing = spareSpace / (lineWordCount - 1);
    }

    // Calculate initial x position
    // 计算初始横坐标
    uint16_t xpos = 0;
    if (style == TextBlock::RIGHT_ALIGN) {
      xpos = spareSpace - (lineWordCount - 1) * spaceWidth;
    } else if (style == TextBlock::CENTER_ALIGN) {
      xpos = (spareSpace - (lineWordCount - 1) * spaceWidth) / 2;
    }

    // Pre-calculate X positions for words
    // 预计算单词的横坐标
    std::list<uint16_t> lineXPos;
    for (size_t i = 0; i < lineWordCount; ++i) {
      const uint16_t currentWordWidth = wordWidths[wordWidthIndex + i];
      lineXPos.push_back(xpos);
      xpos += currentWordWidth + spacing;
    }

    // *** CRITICAL STEP: CONSUME DATA USING SPLICE ***
    // *** 关键步骤：通过拼接操作读取数据 ***
    std::list<std::string> lineWords;
    lineWords.splice(lineWords.begin(), words, wordStartIt, wordEndIt);
    std::list<EpdFontStyle> lineWordStyles;
    lineWordStyles.splice(lineWordStyles.begin(), wordStyles, wordStyleStartIt, wordStyleEndIt);

    processLine(
        std::make_shared<TextBlock>(std::move(lineWords), std::move(lineXPos), std::move(lineWordStyles), style));

    // Update pointers/indices for the next line
    // 更新下一行的指针/索引
    wordStartIt = wordEndIt;
    wordStyleStartIt = wordStyleEndIt;
    wordWidthIndex += lineWordCount;
    lastBreakAt = lineBreak;
  }
}
