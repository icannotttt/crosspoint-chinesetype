#include "ChapterHtmlSlimParser.h"

#include <GfxRenderer.h>
#include <HardwareSerial.h>
#include <SDCardManager.h>
#include <expat.h>

#include "../Page.h"
#include "../htmlEntities.h"

const char* HEADER_TAGS[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
constexpr int NUM_HEADER_TAGS = sizeof(HEADER_TAGS) / sizeof(HEADER_TAGS[0]);

// Minimum file size (in bytes) to show progress bar - smaller chapters don't benefit from it
constexpr size_t MIN_SIZE_FOR_PROGRESS = 50 * 1024;  // 50KB

const char* BLOCK_TAGS[] = {"p", "li", "div", "br", "blockquote"};
constexpr int NUM_BLOCK_TAGS = sizeof(BLOCK_TAGS) / sizeof(BLOCK_TAGS[0]);

const char* BOLD_TAGS[] = {"b", "strong"};
constexpr int NUM_BOLD_TAGS = sizeof(BOLD_TAGS) / sizeof(BOLD_TAGS[0]);

const char* ITALIC_TAGS[] = {"i", "em"};
constexpr int NUM_ITALIC_TAGS = sizeof(ITALIC_TAGS) / sizeof(ITALIC_TAGS[0]);

const char* IMAGE_TAGS[] = {"img"};
constexpr int NUM_IMAGE_TAGS = sizeof(IMAGE_TAGS) / sizeof(IMAGE_TAGS[0]);

const char* SKIP_TAGS[] = {"head", "table"};
constexpr int NUM_SKIP_TAGS = sizeof(SKIP_TAGS) / sizeof(SKIP_TAGS[0]);

bool isWhitespace(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

// given the start and end of a tag, check to see if it matches a known tag
bool matches(const char* tag_name, const char* possible_tags[], const int possible_tag_count) {
  for (int i = 0; i < possible_tag_count; i++) {
    if (strcmp(tag_name, possible_tags[i]) == 0) {
      return true;
    }
  }
  return false;
}

// start a new text block if needed
void ChapterHtmlSlimParser::startNewTextBlock(const TextBlock::BLOCK_STYLE style) {
  if (currentTextBlock) {
    // already have a text block running and it is empty - just reuse it
    if (currentTextBlock->isEmpty()) {
      currentTextBlock->setStyle(style);
      return;
    }

    makePages();
  }
  currentTextBlock.reset(new ParsedText(style, extraParagraphSpacing));
}

void XMLCALL ChapterHtmlSlimParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    self->depth += 1;
    return;
  }

  if (matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS)) {
    // TODO: Start processing image tags
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  if (matches(name, SKIP_TAGS, NUM_SKIP_TAGS)) {
    // start skip
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  // Skip blocks with role="doc-pagebreak" and epub:type="pagebreak"
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "role") == 0 && strcmp(atts[i + 1], "doc-pagebreak") == 0 ||
          strcmp(atts[i], "epub:type") == 0 && strcmp(atts[i + 1], "pagebreak") == 0) {
        self->skipUntilDepth = self->depth;
        self->depth += 1;
        return;
      }
    }
  }

  if (matches(name, HEADER_TAGS, NUM_HEADER_TAGS)) {
    self->startNewTextBlock(TextBlock::CENTER_ALIGN);
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
  } else if (matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS)) {
    if (strcmp(name, "br") == 0) {
      self->startNewTextBlock(self->currentTextBlock->getStyle());
    } else {
      self->startNewTextBlock(TextBlock::JUSTIFIED);
    }
  } else if (matches(name, BOLD_TAGS, NUM_BOLD_TAGS)) {
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
  } else if (matches(name, ITALIC_TAGS, NUM_ITALIC_TAGS)) {
    self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
  }

  self->depth += 1;
}

//中文分割支持
// ========== 静态函数实现（必须加类名限定） ==========
bool ChapterHtmlSlimParser::isWhitespace(const char c) {
  return c == ' ' || c == '\r' || c == '\n' || c == '\t';
}

int ChapterHtmlSlimParser::getUtf8CharLength(const unsigned char* u8) {
  if (u8 == nullptr) return 0;
  if ((u8[0] & 0x80) == 0) return 1;        // 1字节（ASCII/英文）
  if ((u8[0] & 0xE0) == 0xC0) return 2;     // 2字节UTF-8
  if ((u8[0] & 0xF0) == 0xE0) return 3;     // 3字节UTF-8（中文）
  if ((u8[0] & 0xF8) == 0xF0) return 4;     // 4字节UTF-8
  return 0; // 无效UTF-8编码
}

bool ChapterHtmlSlimParser::isChineseChar(const unsigned char* u8, int charLen) {
  if (u8 == nullptr || charLen != 3) return false;
  
  // UTF-8 3字节转Unicode
  uint32_t unicode = ((u8[0] & 0x0F) << 12) | 
                     ((u8[1] & 0x3F) << 6) | 
                     (u8[2] & 0x3F);
  
  // 常用中文字符Unicode范围：0x4E00 - 0x9FFF
  return (unicode >= 0x4E00 && unicode <= 0x9FFF);
}

void XMLCALL ChapterHtmlSlimParser::characterData(void* userData, const XML_Char* s, int len) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  if (self->skipUntilDepth < self->depth) return;

  // 1. 确定当前字体样式
  EpdFontStyle fontStyle = REGULAR;
  if (self->boldUntilDepth < self->depth && self->italicUntilDepth < self->depth) {
    fontStyle = BOLD_ITALIC;
  } else if (self->boldUntilDepth < self->depth) {
    fontStyle = BOLD;
  } else if (self->italicUntilDepth < self->depth) {
    fontStyle = ITALIC;
  }

  // 2. 逐字节解析UTF-8字符
  int i = 0;
  while (i < len) {
    const unsigned char* u8 = (const unsigned char*)&s[i];
    // 调用静态函数（必须通过类名限定，或直接调用）
    int charLen = ChapterHtmlSlimParser::getUtf8CharLength(u8);
    
    // 跳过无效编码/越界字符
    if (charLen == 0 || i + charLen > len) {
      i++;
      continue;
    }

    // 3. 提取单个UTF-8字符
    std::string currentChar(reinterpret_cast<const char*>(u8), charLen);
    i += charLen;

    // 4. 跳过空白符（调用静态函数）
    if (ChapterHtmlSlimParser::isWhitespace(currentChar[0])) {
      // 空白符触发：清空英文缓存
      if (self->partWordBufferIndex > 0) {
        self->partWordBuffer[self->partWordBufferIndex] = '\0';
        self->currentTextBlock->addWord(std::string(self->partWordBuffer), fontStyle);
        self->partWordBufferIndex = 0;
      }
      continue;
    }

    // 5. 判断字符类型：中文/英文（调用静态函数）
    if (ChapterHtmlSlimParser::isChineseChar(u8, charLen)) {
      // 中文：先清空英文缓存，再单个字符添加
      if (self->partWordBufferIndex > 0) {
        self->partWordBuffer[self->partWordBufferIndex] = '\0';
        self->currentTextBlock->addWord(std::string(self->partWordBuffer), fontStyle);
        self->partWordBufferIndex = 0;
      }
      self->currentTextBlock->addWord(currentChar, fontStyle);
    } else {
      // 英文：拼接到字符数组缓存
      if (self->partWordBufferIndex + charLen >= MAX_WORD_SIZE) {
        // 缓存溢出：先添加现有内容，再重置
        self->partWordBuffer[self->partWordBufferIndex] = '\0';
        self->currentTextBlock->addWord(std::string(self->partWordBuffer), fontStyle);
        self->partWordBufferIndex = 0;
      }
      // 逐字节拷贝到字符数组
      memcpy(&self->partWordBuffer[self->partWordBufferIndex], u8, charLen);
      self->partWordBufferIndex += charLen;
    }
  }

  // 6. 处理剩余的英文单词
  if (self->partWordBufferIndex > 0) {
    self->partWordBuffer[self->partWordBufferIndex] = '\0';
    self->currentTextBlock->addWord(std::string(self->partWordBuffer), fontStyle);
    self->partWordBufferIndex = 0;
  }

}


void XMLCALL ChapterHtmlSlimParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  if (self->partWordBufferIndex > 0) {
    // Only flush out part word buffer if we're closing a block tag or are at the top of the HTML file.
    // We don't want to flush out content when closing inline tags like <span>.
    // Currently this also flushes out on closing <b> and <i> tags, but they are line tags so that shouldn't happen,
    // text styling needs to be overhauled to fix it.
    const bool shouldBreakText =
        matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS) || matches(name, HEADER_TAGS, NUM_HEADER_TAGS) ||
        matches(name, BOLD_TAGS, NUM_BOLD_TAGS) || matches(name, ITALIC_TAGS, NUM_ITALIC_TAGS) || self->depth == 1;

    if (shouldBreakText) {
      EpdFontStyle fontStyle = REGULAR;
      if (self->boldUntilDepth < self->depth && self->italicUntilDepth < self->depth) {
        fontStyle = BOLD_ITALIC;
      } else if (self->boldUntilDepth < self->depth) {
        fontStyle = BOLD;
      } else if (self->italicUntilDepth < self->depth) {
        fontStyle = ITALIC;
      }

      self->partWordBuffer[self->partWordBufferIndex] = '\0';
      self->currentTextBlock->addWord(std::move(replaceHtmlEntities(self->partWordBuffer)), fontStyle);
      self->partWordBufferIndex = 0;
    }
  }

  self->depth -= 1;

  // Leaving skip
  if (self->skipUntilDepth == self->depth) {
    self->skipUntilDepth = INT_MAX;
  }

  // Leaving bold
  if (self->boldUntilDepth == self->depth) {
    self->boldUntilDepth = INT_MAX;
  }

  // Leaving italic
  if (self->italicUntilDepth == self->depth) {
    self->italicUntilDepth = INT_MAX;
  }
}

bool ChapterHtmlSlimParser::parseAndBuildPages() {
  startNewTextBlock(TextBlock::JUSTIFIED);

  const XML_Parser parser = XML_ParserCreate(nullptr);
  int done;

  if (!parser) {
    Serial.printf("[%lu] [EHP] Couldn't allocate memory for parser\n", millis());
    return false;
  }

  FsFile file;
  if (!SdMan.openFileForRead("EHP", filepath, file)) {
    XML_ParserFree(parser);
    return false;
  }

  // Get file size for progress calculation
  const size_t totalSize = file.size();
  size_t bytesRead = 0;
  int lastProgress = -1;

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);

  do {
    void* const buf = XML_GetBuffer(parser, 1024);
    if (!buf) {
      Serial.printf("[%lu] [EHP] Couldn't allocate memory for buffer\n", millis());
      XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      file.close();
      return false;
    }

    const size_t len = file.read(buf, 1024);

    if (len == 0 && file.available() > 0) {
      Serial.printf("[%lu] [EHP] File read error\n", millis());
      XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      file.close();
      return false;
    }

    // Update progress (call every 10% change to avoid too frequent updates)
    // Only show progress for larger chapters where rendering overhead is worth it
    bytesRead += len;
    if (progressFn && totalSize >= MIN_SIZE_FOR_PROGRESS) {
      const int progress = static_cast<int>((bytesRead * 100) / totalSize);
      if (lastProgress / 10 != progress / 10) {
        lastProgress = progress;
        progressFn(progress);
      }
    }

    done = file.available() == 0;

    if (XML_ParseBuffer(parser, static_cast<int>(len), done) == XML_STATUS_ERROR) {
      Serial.printf("[%lu] [EHP] Parse error at line %lu:\n%s\n", millis(), XML_GetCurrentLineNumber(parser),
                    XML_ErrorString(XML_GetErrorCode(parser)));
      XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      file.close();
      return false;
    }
  } while (!done);

  XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
  XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
  XML_SetCharacterDataHandler(parser, nullptr);
  XML_ParserFree(parser);
  file.close();

  // Process last page if there is still text
  if (currentTextBlock) {
    makePages();
    completePageFn(std::move(currentPage));
    currentPage.reset();
    currentTextBlock.reset();
  }

  return true;
}

void ChapterHtmlSlimParser::addLineToPage(std::shared_ptr<TextBlock> line) {
  const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;

  if (currentPageNextY + lineHeight > viewportHeight) {
    completePageFn(std::move(currentPage));
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  currentPage->elements.push_back(std::make_shared<PageLine>(line, 0, currentPageNextY));
  currentPageNextY += lineHeight;
}

void ChapterHtmlSlimParser::makePages() {
  if (!currentTextBlock) {
    Serial.printf("[%lu] [EHP] !! No text block to make pages for !!\n", millis());
    return;
  }

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;
  currentTextBlock->layoutAndExtractLines(
      renderer, fontId, viewportWidth,
      [this](const std::shared_ptr<TextBlock>& textBlock) { addLineToPage(textBlock); });
  // Extra paragraph spacing if enabled
  if (extraParagraphSpacing) {
    currentPageNextY += lineHeight / 2;
  }
}
