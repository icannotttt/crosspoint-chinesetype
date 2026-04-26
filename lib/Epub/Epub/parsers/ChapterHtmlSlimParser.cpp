#include "ChapterHtmlSlimParser.h"


#include<FsHelpers.h>
#include <GfxRenderer.h>
#include <HardwareSerial.h>
#include <SDCardManager.h>
#include <expat.h>

#include "../Page.h"

#include"../../Epub.h"
#include "../converters/ImageDecoderFactory.h"
#include "../converters/ImageToFramebufferDecoder.h"

const char* HEADER_TAGS[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
constexpr int NUM_HEADER_TAGS = sizeof(HEADER_TAGS) / sizeof(HEADER_TAGS[0]);

// Minimum file size (in bytes) to show indexing popup - smaller chapters don't benefit from it
constexpr size_t MIN_SIZE_FOR_POPUP = 50 * 1024;  // 50KB

const char* BLOCK_TAGS[] = {"p", "li", "div", "br", "blockquote"};
constexpr int NUM_BLOCK_TAGS = sizeof(BLOCK_TAGS) / sizeof(BLOCK_TAGS[0]);

const char* BOLD_TAGS[] = {"b", "strong"};
constexpr int NUM_BOLD_TAGS = sizeof(BOLD_TAGS) / sizeof(BOLD_TAGS[0]);

const char* ITALIC_TAGS[] = {"i", "em"};
constexpr int NUM_ITALIC_TAGS = sizeof(ITALIC_TAGS) / sizeof(ITALIC_TAGS[0]);

const char* UNDERLINE_TAGS[] = {"u", "ins"};
constexpr int NUM_UNDERLINE_TAGS = sizeof(UNDERLINE_TAGS) / sizeof(UNDERLINE_TAGS[0]);

const char* IMAGE_TAGS[] = {"img"};
constexpr int NUM_IMAGE_TAGS = sizeof(IMAGE_TAGS) / sizeof(IMAGE_TAGS[0]);

const char* SKIP_TAGS[] = {"head"};
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

bool isHeaderOrBlock(const char* name) {
  return matches(name, HEADER_TAGS, NUM_HEADER_TAGS) || matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS);
}

// Update effective bold/italic/underline based on block style and inline style stack
void ChapterHtmlSlimParser::updateEffectiveInlineStyle() {
  // Start with block-level styles
  effectiveBold = currentCssStyle.hasFontWeight() && currentCssStyle.fontWeight == CssFontWeight::Bold;
  effectiveItalic = currentCssStyle.hasFontStyle() && currentCssStyle.fontStyle == CssFontStyle::Italic;
  effectiveUnderline =
      currentCssStyle.hasTextDecoration() && currentCssStyle.textDecoration == CssTextDecoration::Underline;

  // Apply inline style stack in order
  for (const auto& entry : inlineStyleStack) {
    if (entry.hasBold) {
      effectiveBold = entry.bold;
    }
    if (entry.hasItalic) {
      effectiveItalic = entry.italic;
    }
    if (entry.hasUnderline) {
      effectiveUnderline = entry.underline;
    }
  }
}

// flush the contents of partWordBuffer to currentTextBlock
void ChapterHtmlSlimParser::flushPartWordBuffer() {
  // Determine font style from depth-based tracking and CSS effective style
  const bool isBold = boldUntilDepth < depth || effectiveBold;
  const bool isItalic = italicUntilDepth < depth || effectiveItalic;
  const bool isUnderline = underlineUntilDepth < depth || effectiveUnderline;

  // Combine style flags using bitwise OR
  EpdFontFamily::Style fontStyle = EpdFontFamily::REGULAR;
  if (isBold) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::BOLD);
  }
  if (isItalic) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::ITALIC);
  }
  if (isUnderline) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::UNDERLINE);
  }

  // flush the buffer
  partWordBuffer[partWordBufferIndex] = '\0';
  currentTextBlock->addWord(partWordBuffer, fontStyle, false, nextWordContinues);
  partWordBufferIndex = 0;
  nextWordContinues = false;
}

// start a new text block if needed
void ChapterHtmlSlimParser::startNewTextBlock(const BlockStyle& blockStyle) {
  nextWordContinues = false;  // New block = new paragraph, no continuation
  if (currentTextBlock) {
    // already have a text block running and it is empty - just reuse it
    if (currentTextBlock->isEmpty()) {
      // Merge with existing block style to accumulate CSS styling from parent block elements.
      // This handles cases like <div style="margin-bottom:2em"><h1>text</h1></div> where the
      // div's margin should be preserved, even though it has no direct text content.
      currentTextBlock->setBlockStyle(currentTextBlock->getBlockStyle().getCombinedBlockStyle(blockStyle));
      return;
    }

    makePages();
  }
  currentTextBlock.reset(new ParsedText(extraParagraphSpacing, hyphenationEnabled,wordSpacing,firstlineintented, blockStyle));
}

void XMLCALL ChapterHtmlSlimParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    self->depth += 1;
    return;
  }

  // Extract class and style attributes for CSS processing
  std::string classAttr;
  std::string styleAttr;
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "class") == 0) {
        classAttr = atts[i + 1];
      } else if (strcmp(atts[i], "style") == 0) {
        styleAttr = atts[i + 1];
      }
    }
  }

  auto centeredBlockStyle = BlockStyle();
  centeredBlockStyle.textAlignDefined = true;
  centeredBlockStyle.alignment = CssTextAlign::Center;

  // Special handling for tables - show placeholder text instead of dropping silently
  if (strcmp(name, "table") == 0) {
    // Add placeholder text
    self->startNewTextBlock(centeredBlockStyle);

    self->italicUntilDepth = min(self->italicUntilDepth, self->depth);
    // Advance depth before processing character data (like you would for an element with text)
    self->depth += 1;
    self->characterData(userData, "[Table omitted]", strlen("[Table omitted]"));

    // Skip table contents (skip until parent as we pre-advanced depth above)
    self->skipUntilDepth = self->depth - 1;
    return;
  }

  if (matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS)) {
    std::string src;
    std::string alt = "[Image]";

    if (atts != nullptr) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "src") == 0) {
          src = atts[i + 1];
        } else if (strcmp(atts[i], "alt") == 0) {
          alt = atts[i + 1];
        }
      }
    if(!src.empty()){
      // Add placeholder text for image - could be improved in the future to support inline images or at least page-level images instead of just a placeholder
      Serial.printf("[%lu] [HTML Parser] Image tag with src=\"%s\" - showing placeholder text\n", millis(), src.c_str());

    {
      std::string resolvedPath=FsHelpers::normalisePath(self->contentBase+src);
                // Create a unique filename for the cached image
          std::string ext;
          size_t extPos = resolvedPath.rfind('.');
          if (extPos != std::string::npos) {
            ext = resolvedPath.substr(extPos);
          }
          std::string cachedImagePath = self->imageBasePath + std::to_string(self->imageCounter++) + ext;

          // Extract image to cache file
          FsFile cachedImageFile;
          bool extractSuccess = false;
          if (SdMan.openFileForWrite("EHP", cachedImagePath, cachedImageFile)) {
            extractSuccess = self->epub->readItemContentsToStream(resolvedPath, cachedImageFile, 4096);
            cachedImageFile.flush();
            cachedImageFile.close();
            delay(50);  // Give SD card time to sync
          }

          if (extractSuccess) {
            // Get image dimensions
            ImageDimensions dims = {0, 0};
            ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(cachedImagePath);
            if (decoder && decoder->getDimensions(cachedImagePath, dims)) {
              Serial.printf("EHP", "Image dimensions: %dx%d", dims.width, dims.height);

              // Scale to fit viewport while maintaining aspect ratio
              int maxWidth = self->viewportWidth;
              int maxHeight = self->viewportHeight;
              float scaleX = (dims.width > maxWidth) ? (float)maxWidth / dims.width : 1.0f;
              float scaleY = (dims.height > maxHeight) ? (float)maxHeight / dims.height : 1.0f;
              float scale = (scaleX < scaleY) ? scaleX : scaleY;
              if (scale > 1.0f) scale = 1.0f;

              int displayWidth = (int)(dims.width * scale);
              int displayHeight = (int)(dims.height * scale);

              Serial.printf("EHP", "Display size: %dx%d (scale %.2f)", displayWidth, displayHeight, scale);

              // Create page for image - only break if image won't fit remaining space
              if (self->currentPage && !self->currentPage->elements.empty() &&
                  (self->currentPageNextY + displayHeight > self->viewportHeight)) {
                self->completePageFn(std::move(self->currentPage));
                self->currentPage.reset(new Page());
                if (!self->currentPage) {
                  Serial.printf("EHP", "Failed to create new page");
                  return;
                }
                self->currentPageNextY = 0;
              } else if (!self->currentPage) {
                self->currentPage.reset(new Page());
                if (!self->currentPage) {
                  Serial.printf("EHP", "Failed to create initial page");
                  return;
                }
                self->currentPageNextY = 0;
              }

              // Create ImageBlock and add to page
              auto imageBlock = std::make_shared<ImageBlock>(cachedImagePath, displayWidth, displayHeight);
              if (!imageBlock) {
                Serial.printf("EHP", "Failed to create ImageBlock");
                return;
              }
              int xPos = (self->viewportWidth - displayWidth) / 2;
              auto pageImage = std::make_shared<PageImage>(imageBlock, xPos, self->currentPageNextY);
              if (!pageImage) {
                Serial.printf("EHP", "Failed to create PageImage");
                return;
              }
              self->currentPage->elements.push_back(pageImage);
              self->currentPageNextY += displayHeight;

              self->depth += 1;
              return;
            } else {
              Serial.printf("EHP", "Failed to get image dimensions");
              SdMan.remove(cachedImagePath.c_str());
            }
          } else {
            Serial.printf("EHP", "Failed to extract image");
          }
        }
      }

      // Fallback to alt text if image processing fails
      if (!alt.empty()) {
        alt = "[Image: " + alt + "]";
        self->startNewTextBlock(centeredBlockStyle);
        self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
        self->depth += 1;
        self->characterData(userData, alt.c_str(), alt.length());
        // Skip any child content (skip until parent as we pre-advanced depth above)
        self->skipUntilDepth = self->depth - 1;
        return;
      }

      // No alt text, skip
      self->skipUntilDepth = self->depth;
      self->depth += 1;
      return;
    }
 
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

  // Compute CSS style for this element
  CssStyle cssStyle;
  if (self->cssParser) {
    // Get combined tag + class styles
    cssStyle = self->cssParser->resolveStyle(name, classAttr);
    // Merge inline style (highest priority)
    if (!styleAttr.empty()) {
      CssStyle inlineStyle = CssParser::parseInlineStyle(styleAttr);
      cssStyle.applyOver(inlineStyle);
    }
  }

  const float emSize = static_cast<float>(self->renderer.getLineHeight(self->fontId)) * self->lineCompression;
  const auto userAlignmentBlockStyle = BlockStyle::fromCssStyle(
      cssStyle, emSize, static_cast<CssTextAlign>(self->paragraphAlignment), self->viewportWidth);

  if (matches(name, HEADER_TAGS, NUM_HEADER_TAGS)) {
    self->currentCssStyle = cssStyle;
    auto headerBlockStyle = BlockStyle::fromCssStyle(cssStyle, emSize, CssTextAlign::Center, self->viewportWidth);
    headerBlockStyle.textAlignDefined = true;
    if (self->embeddedStyle && cssStyle.hasTextAlign()) {
      headerBlockStyle.alignment = cssStyle.textAlign;
    }
    self->startNewTextBlock(headerBlockStyle);
    //稍微排下版，标题前加小黑点
    self->currentTextBlock->addWord("\xe2\x97\x8f ", EpdFontFamily::REGULAR);
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS)) {
    if (strcmp(name, "br") == 0) {
      if (self->partWordBufferIndex > 0) {
        // flush word preceding <br/> to currentTextBlock before calling startNewTextBlock
        self->flushPartWordBuffer();
      }
      self->startNewTextBlock(self->currentTextBlock->getBlockStyle());
    } else {
      self->currentCssStyle = cssStyle;
      self->startNewTextBlock(userAlignmentBlockStyle);
      self->updateEffectiveInlineStyle();

      if (strcmp(name, "li") == 0) {
        self->currentTextBlock->addWord("\xe2\x80\xa2", EpdFontFamily::REGULAR);
      }
    }
  } else if (matches(name, UNDERLINE_TAGS, NUM_UNDERLINE_TAGS)) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->underlineUntilDepth = std::min(self->underlineUntilDepth, self->depth);
    // Push inline style entry for underline tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasUnderline = true;
    entry.underline = true;
    if (cssStyle.hasFontWeight()) {
      entry.hasBold = true;
      entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
    }
    if (cssStyle.hasFontStyle()) {
      entry.hasItalic = true;
      entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
    }
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, BOLD_TAGS, NUM_BOLD_TAGS)) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    // Push inline style entry for bold tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasBold = true;
    entry.bold = true;
    if (cssStyle.hasFontStyle()) {
      entry.hasItalic = true;
      entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
    }
    if (cssStyle.hasTextDecoration()) {
      entry.hasUnderline = true;
      entry.underline = cssStyle.textDecoration == CssTextDecoration::Underline;
    }
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, ITALIC_TAGS, NUM_ITALIC_TAGS)) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
    // Push inline style entry for italic tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasItalic = true;
    entry.italic = true;
    if (cssStyle.hasFontWeight()) {
      entry.hasBold = true;
      entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
    }
    if (cssStyle.hasTextDecoration()) {
      entry.hasUnderline = true;
      entry.underline = cssStyle.textDecoration == CssTextDecoration::Underline;
    }
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (strcmp(name, "span") == 0 || !isHeaderOrBlock(name)) {
    // Handle span and other inline elements for CSS styling
    if (cssStyle.hasFontWeight() || cssStyle.hasFontStyle() || cssStyle.hasTextDecoration()) {
      // Flush buffer before style change so preceding text gets current style
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
      StyleStackEntry entry;
      entry.depth = self->depth;  // Track depth for matching pop
      if (cssStyle.hasFontWeight()) {
        entry.hasBold = true;
        entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
      }
      if (cssStyle.hasFontStyle()) {
        entry.hasItalic = true;
        entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
      }
      if (cssStyle.hasTextDecoration()) {
        entry.hasUnderline = true;
        entry.underline = cssStyle.textDecoration == CssTextDecoration::Underline;
      }
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();
    }
  }

  // Unprocessed tag, just increasing depth and continue forward
  self->depth += 1;
}

// 以下为自加
bool ChapterHtmlSlimParser::isEnglishPunctuation(unsigned char c) {
    return c == '.' || c == ',' || c == '!' || c == '?' || c == ';' || c == ':' || 
           c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' ||
           c == '"' || c == '\'' || c == '-' || c == '~' || c == '`' || c == '@' ||
           c == '#' || c == '$' || c == '%' || c == '^' || c == '&' || c == '*' ||
           c == '+' || c == '=' || c == '|' || c == '\\' || c == '/' || c == '<' ||
           c == '>' || c == '_';
}

// 修复：改为静态函数，适配静态的characterData调用
bool ChapterHtmlSlimParser::isOnlyWhitespace(const char* buf, int len) {
    if (len <= 0) return true;
    for (int k = 0; k < len; k++) {
        unsigned char c = static_cast<unsigned char>(buf[k]);
        // 直接内嵌空白判断逻辑，避免再调用非静态的isWhitespace
        if (!(c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v')) {
            return false;
        }
    }
    return true;
}

// 2. 判断字符是否为空白（基于lvstring的空白定义）
bool ChapterHtmlSlimParser::isWhitespaceChar(unsigned int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v' ||
           c == 0x00A0 || // 不换行空格
           c == 0x3000;   // 中文全角空格
}

// 3. 判断是否为CJK字符（复刻lvstring.h的lStr_isCJK）
bool ChapterHtmlSlimParser::isCJKChar(unsigned int c) {
    if (c >= 0x2E80) {
        if (c < 0xA000) {
            return true; // 核心中文字符区（2E80-9FFF）
        } else if (c >= 0xAC00 && c < 0xD800) {
            return true; // 韩文
        } else if (c >= 0xF900 && c <= 0xFAFF) {
            return true; // CJK兼容字符
        } else if (c >= 0xFF00 && c <= 0xFFEF) {
            return true; // 全角ASCII（中文排版中视为CJK）
        }
    }
    return false;
}

// 4. 判断是否为英文标点（基于lvstring的标点定义）
bool ChapterHtmlSlimParser::isEnglishPunctChar(unsigned int c) {
    return (c >= 0x21 && c <= 0x2F) || (c >= 0x3A && c <= 0x40) ||
           (c >= 0x5B && c <= 0x60) || (c >= 0x7B && c <= 0x7E) ||
           c == 0x2013 || c == 0x2014; // 英文破折号
}

// 5. 判断是否为中文标点
bool ChapterHtmlSlimParser::isChinesePunctChar(unsigned int c) {
    return (c >= 0x3001 && c <= 0x3003) || // 、。！
           (c >= 0xFF01 && c <= 0xFF0F) || // 全角!@#$%^&*()_+
           (c >= 0xFF1A && c <= 0xFF1F) || // 全角:;"'<>=?
           (c >= 0xFF3B && c <= 0xFF40) || // 全角[]^`
           (c >= 0xFF5B && c <= 0xFF65);   // 全角{}|~、。
}

// 6. 获取字符类型（核心分发函数）
ChapterHtmlSlimParser::CharType ChapterHtmlSlimParser::getCharType(unsigned int c) {
    if (isWhitespaceChar(c)) {
        return CHAR_TYPE_SPACE;
    } else if (isCJKChar(c)) {
        return CHAR_TYPE_CJK;
    } else if (c <= 0x7F) { // ASCII字符
        if (isEnglishPunctChar(c)) {
            return CHAR_TYPE_PUNCT;
        } else {
            return CHAR_TYPE_ASCII;
        }
    } else if (isChinesePunctChar(c)) {
        return CHAR_TYPE_PUNCT;
    } else {
        return CHAR_TYPE_OTHER;
    }
}

// 7. UTF-8转Unicode（处理多字节字符，关键！）
int ChapterHtmlSlimParser::utf8ToUnicode(const char* str, int len, int& pos, unsigned int& out_c) {
    if (pos >= len) return -1;
    
    unsigned char b = static_cast<unsigned char>(str[pos]);
    int bytes = 0;
    
    if ((b & 0x80) == 0) {
        // 单字节（ASCII）
        out_c = b;
        bytes = 1;
    } else if ((b & 0xE0) == 0xC0) {
        // 双字节
        if (pos + 1 >= len) return -1;
        out_c = ((b & 0x1F) << 6) | (static_cast<unsigned char>(str[pos+1]) & 0x3F);
        bytes = 2;
    } else if ((b & 0xF0) == 0xE0) {
        // 三字节（中文核心）
        if (pos + 2 >= len) return -1;
        out_c = ((b & 0x0F) << 12) | ((static_cast<unsigned char>(str[pos+1]) & 0x3F) << 6) | 
                (static_cast<unsigned char>(str[pos+2]) & 0x3F);
        bytes = 3;
    } else if ((b & 0xF8) == 0xF0) {
        // 四字节
        if (pos + 3 >= len) return -1;
        out_c = ((b & 0x07) << 18) | ((static_cast<unsigned char>(str[pos+1]) & 0x3F) << 12) |
                ((static_cast<unsigned char>(str[pos+2]) & 0x3F) << 6) | (static_cast<unsigned char>(str[pos+3]) & 0x3F);
        bytes = 4;
    } else {
        return -1; // 无效UTF-8
    }
    
    pos += bytes;
    return bytes;
}

void XMLCALL ChapterHtmlSlimParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  if (self->skipUntilDepth < self->depth) {
    return;
  }

  // 快速判断整段空白（保留原有逻辑）
  if (self->isOnlyWhitespace(reinterpret_cast<const char*>(s), len)) {
    return;
  }

  int pos = 0;
  unsigned int current_c;
  CharType last_type = CHAR_TYPE_SPACE; // 上一个字符类型
  int word_start = 0; // 当前单词/字符的起始位置

  while (pos < len) {
    // 解析当前UTF-8字符为Unicode
    int bytes = self->utf8ToUnicode(reinterpret_cast<const char*>(s), len, pos, current_c);
    if (bytes < 0) {
      pos++; // 无效字符，跳过
      continue;
    }

    CharType current_type = self->getCharType(current_c);

    // ========== 核心分词逻辑 ==========
    // 1. 空白字符：刷新当前缓冲区，仅保留单个空格
    if (current_type == CHAR_TYPE_SPACE) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      // 仅写入单个空格（避免连续空白）
      if (MAX_WORD_SIZE >= 2) {
        self->partWordBuffer[0] = ' ';
        self->partWordBuffer[1] = '\0';
        self->partWordBufferIndex = 1;
        self->flushPartWordBuffer();
      }
      last_type = CHAR_TYPE_SPACE;
      continue;
    }

    // 2. CJK字符（中文）：单字分词
    if (current_type == CHAR_TYPE_CJK) {
      // 先刷新之前的非CJK内容
      if (self->partWordBufferIndex > 0 && last_type != CHAR_TYPE_CJK) {
        self->flushPartWordBuffer();
      }
      // 写入当前CJK字符（保留完整多字节）
      if (self->partWordBufferIndex + bytes < MAX_WORD_SIZE) {
        for (int i = 0; i < bytes; i++) {
          self->partWordBuffer[self->partWordBufferIndex++] = s[pos - bytes + i];
        }
        // CJK单字直接刷新（按字分词）
        self->flushPartWordBuffer();
      }
      last_type = CHAR_TYPE_CJK;
      continue;
    }

    // 3. 标点符号：独立分词（或跟随前一个单词）
    if (current_type == CHAR_TYPE_PUNCT) {
      // 先刷新之前的内容
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      // 写入标点
      if (self->partWordBufferIndex + bytes < MAX_WORD_SIZE) {
        for (int i = 0; i < bytes; i++) {
          self->partWordBuffer[self->partWordBufferIndex++] = s[pos - bytes + i];
        }
        self->flushPartWordBuffer();
      }
      last_type = CHAR_TYPE_PUNCT;
      continue;
    }

    // 4. ASCII字符（英文/数字）：按单词分词（直到空白/标点/CJK）
    if (current_type == CHAR_TYPE_ASCII) {
      // 如果上一个字符不是ASCII，先刷新缓冲区
      if (self->partWordBufferIndex > 0 && last_type != CHAR_TYPE_ASCII) {
        self->flushPartWordBuffer();
      }
      // 写入当前ASCII字符，避免缓冲区溢出
      if (self->partWordBufferIndex >= MAX_WORD_SIZE - 1) {
        self->flushPartWordBuffer();
      }
      self->partWordBuffer[self->partWordBufferIndex++] = static_cast<char>(current_c);
      last_type = CHAR_TYPE_ASCII;
      continue;
    }

    // 5. 其他字符：默认处理
    if (self->partWordBufferIndex > 0 && last_type != CHAR_TYPE_OTHER) {
      self->flushPartWordBuffer();
    }
    if (self->partWordBufferIndex + bytes < MAX_WORD_SIZE) {
      for (int i = 0; i < bytes; i++) {
        self->partWordBuffer[self->partWordBufferIndex++] = s[pos - bytes + i];
      }
      self->flushPartWordBuffer();
    }
    last_type = CHAR_TYPE_OTHER;
  }

  // 循环结束后，刷新剩余的ASCII单词（避免英文单词截断）
  if (self->partWordBufferIndex > 0) {
    if (!self->isOnlyWhitespace(self->partWordBuffer, self->partWordBufferIndex)) {
      self->flushPartWordBuffer();
    } else {
      self->partWordBufferIndex = 0;
      memset(self->partWordBuffer, 0, MAX_WORD_SIZE);
    }
  }

  // 保留原有长文本拆分逻辑
  if (self->currentTextBlock->size() > 750) {
    Serial.printf("[%lu] [EHP] Text block too long, splitting into multiple pages\n", millis());
    self->currentTextBlock->layoutAndExtractLines(
        self->renderer, self->fontId, self->viewportWidth,
        [self](const std::shared_ptr<TextBlock>& textBlock) { self->addLineToPage(textBlock); }, false);
  }
}


void XMLCALL ChapterHtmlSlimParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Check if any style state will change after we decrement depth
  // If so, we MUST flush the partWordBuffer with the CURRENT style first
  // Note: depth hasn't been decremented yet, so we check against (depth - 1)
  const bool willPopStyleStack =
      !self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth - 1;
  const bool willClearBold = self->boldUntilDepth == self->depth - 1;
  const bool willClearItalic = self->italicUntilDepth == self->depth - 1;
  const bool willClearUnderline = self->underlineUntilDepth == self->depth - 1;

  const bool styleWillChange = willPopStyleStack || willClearBold || willClearItalic || willClearUnderline;
  const bool headerOrBlockTag = isHeaderOrBlock(name);

  // Flush buffer with current style BEFORE any style changes
  if (self->partWordBufferIndex > 0) {
    // Flush if style will change OR if we're closing a block/structural element
    const bool isInlineTag = !headerOrBlockTag && strcmp(name, "table") != 0 &&
                             !matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS) && self->depth != 1;
    const bool shouldFlush = styleWillChange || headerOrBlockTag || matches(name, BOLD_TAGS, NUM_BOLD_TAGS) ||
                             matches(name, ITALIC_TAGS, NUM_ITALIC_TAGS) ||
                             matches(name, UNDERLINE_TAGS, NUM_UNDERLINE_TAGS) || strcmp(name, "table") == 0 ||
                             matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS) || self->depth == 1;

    if (shouldFlush) {
      self->flushPartWordBuffer();
      // If closing an inline element, the next word fragment continues the same visual word
      if (isInlineTag) {
        self->nextWordContinues = true;
      }
    }
  }

  self->depth -= 1;

  // Leaving skip
  if (self->skipUntilDepth == self->depth) {
    self->skipUntilDepth = INT_MAX;
  }

  // Leaving bold tag
  if (self->boldUntilDepth == self->depth) {
    self->boldUntilDepth = INT_MAX;
  }

  // Leaving italic tag
  if (self->italicUntilDepth == self->depth) {
    self->italicUntilDepth = INT_MAX;
  }

  // Leaving underline tag
  if (self->underlineUntilDepth == self->depth) {
    self->underlineUntilDepth = INT_MAX;
  }

  // Pop from inline style stack if we pushed an entry at this depth
  // This handles all inline elements: b, i, u, span, etc.
  if (!self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth) {
    self->inlineStyleStack.pop_back();
    self->updateEffectiveInlineStyle();
  }

  // Clear block style when leaving header or block elements
  if (headerOrBlockTag) {
    self->currentCssStyle.reset();
    self->updateEffectiveInlineStyle();
  }
}

bool ChapterHtmlSlimParser::parseAndBuildPages() {
  auto paragraphAlignmentBlockStyle = BlockStyle();
  paragraphAlignmentBlockStyle.textAlignDefined = true;
  // Resolve None sentinel to Justify for initial block (no CSS context yet)
  const auto align = (this->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                         ? CssTextAlign::Justify
                         : static_cast<CssTextAlign>(this->paragraphAlignment);
  paragraphAlignmentBlockStyle.alignment = align;
  startNewTextBlock(paragraphAlignmentBlockStyle);

  const XML_Parser parser = XML_ParserCreate(nullptr);
  if (parser) Serial.printf("[%lu] [EHP] Parser created\n", millis());
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

  // Get file size to decide whether to show indexing popup.
  if (popupFn && file.size() >= MIN_SIZE_FOR_POPUP) {
    popupFn();
  }

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
    vTaskDelay(1);
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

  // Apply horizontal left inset (margin + padding) as x position offset
  const int16_t xOffset = line->getBlockStyle().leftInset();
  currentPage->elements.push_back(std::make_shared<PageLine>(line, xOffset, currentPageNextY));
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

  // Apply top spacing before the paragraph (stored in pixels)
  const BlockStyle& blockStyle = currentTextBlock->getBlockStyle();
  if (blockStyle.marginTop > 0) {
    currentPageNextY += blockStyle.marginTop;
  }
  if (blockStyle.paddingTop > 0) {
    currentPageNextY += blockStyle.paddingTop;
  }

  // Calculate effective width accounting for horizontal margins/padding
  const int horizontalInset = blockStyle.totalHorizontalInset();
  const uint16_t effectiveWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;

  currentTextBlock->layoutAndExtractLines(
      renderer, fontId, effectiveWidth,
      [this](const std::shared_ptr<TextBlock>& textBlock) { addLineToPage(textBlock); });

  // Apply bottom spacing after the paragraph (stored in pixels)
  if (blockStyle.marginBottom > 0) {
    currentPageNextY += blockStyle.marginBottom;
  }
  if (blockStyle.paddingBottom > 0) {
    currentPageNextY += blockStyle.paddingBottom;
  }

  // Extra paragraph spacing if enabled (default behavior)
  if (extraParagraphSpacing) {
    currentPageNextY += lineHeight / 2;
  }
}
