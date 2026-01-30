#include "ChapterHtmlSlimParser.h"

#include <GfxRenderer.h>
#include <HardwareSerial.h>
#include <SDCardManager.h>
#include <expat.h>

#include "../Page.h"
#include "../htmlEntities.h"

// 定义最大单词长度（根据你的项目需求调整，这里给一个默认值）
#ifndef MAX_WORD_SIZE
#define MAX_WORD_SIZE 256
#endif

const char* HEADER_TAGS[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
constexpr int NUM_HEADER_TAGS = sizeof(HEADER_TAGS) / sizeof(HEADER_TAGS[0]);

// Minimum file size (in bytes) to show progress bar - smaller chapters don't benefit from it
constexpr size_t MIN_SIZE_FOR_PROGRESS = 50 * 1024;  // 50KB

// 【块级标签】保留兼容预留，仅处理br/p，其余不操作
const char* BLOCK_TAGS[] = {"p", "br", "div", "li", "blockquote", "block", "left", "note",};
constexpr int NUM_BLOCK_TAGS = sizeof(BLOCK_TAGS) / sizeof(BLOCK_TAGS[0]);

// 【粗体/斜体】按需添加，无则留空，不影响解析
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
bool ChapterHtmlSlimParser::matches(const char* tag_name, const char* possible_tags[], const int possible_tag_count) {
  // 函数逻辑不变，仅添加类作用域限定
  for (int i = 0; i < possible_tag_count; i++) {
    if (strcmp(tag_name, possible_tags[i]) == 0) {
      return true;
    }
  }
  return false;
}

// 统一资源释放函数，避免内存泄漏
void ChapterHtmlSlimParser::cleanupResources(XML_Parser parser, FsFile& file) {
  // 新增日志：追踪资源释放
  //Serial.printf("[%lu] [EHP] 进入cleanupResources，开始释放资源\n", millis());
  if (parser) {
    XML_StopParser(parser, XML_FALSE);
    XML_SetElementHandler(parser, nullptr, nullptr);
    XML_SetCharacterDataHandler(parser, nullptr);
    XML_ParserFree(parser);
    //Serial.printf("[%lu] [EHP] 解析器资源释放完成\n", millis());
  }
  if (file) {
    file.close();
    //Serial.printf("[%lu] [EHP] 文件资源关闭完成\n", millis());
  }
}

// start a new text block if needed
void ChapterHtmlSlimParser::startNewTextBlock(const TextBlock::BLOCK_STYLE style) {
  // 新增日志：追踪文本块创建（关键：定位哪个块创建时出问题）
  //Serial.printf("[%lu] [EHP] 进入startNewTextBlock，样式：%d，当前文本块是否存在：%s\n", 
   //             millis(), 
   //             style, 
   //             (currentTextBlock ? "是" : "否"));
  // 1. 若当前有文本块且非空，先分页，避免内容叠加
  if (currentTextBlock && !currentTextBlock->isEmpty()) {
    //Serial.printf("[%lu] [EHP] 现有文本块非空，先执行分页操作\n", millis());
    makePages();
  }
  // 2. 直接重置/创建新块，C++11兼容，无任何空指针访问
  currentTextBlock.reset(new ParsedText(style, extraParagraphSpacing));
  // 3. 同步更新样式，仅为兼容少量嵌套场景，无依赖不崩溃
  currentBlockStyle = style;
  // 新增日志：追踪文本块创建完成
  //Serial.printf("[%lu] [EHP] startNewTextBlock执行完成，新样式：%d\n", millis(), style);
}

void XMLCALL ChapterHtmlSlimParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
    auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
    
    // 新增日志：追踪标签解析入口（关键：定位哪个标签开始解析时崩溃）
    //Serial.printf("[%lu] [EHP] 进入startElement，当前深度：%d\n", millis(), self->depth);
    
    // 新增：防护空指针（name为空直接返回，避免崩溃）
    if (!name || *name == '\0') {
        //Serial.printf("[%lu] [EHP] startElement警告：标签名称为空，深度+1后返回\n", millis());
        self->depth += 1;
        return;
    }
    
    // 新增日志：追踪当前解析的标签名称
    //Serial.printf("[%lu] [EHP] 开始解析标签：<%s>，当前深度：%d\n", millis(), name, self->depth);

    // 跳过指定无文本标签，直接深度+1，不处理内部内容
    if (matches(name, SKIP_TAGS, NUM_SKIP_TAGS)) {
        //Serial.printf("[%lu] [EHP] 标签<%s>属于跳过标签，设置跳过深度为：%d\n", 
        //              millis(), 
        //              name, 
        //              std::min(self->skipUntilDepth, self->depth));
        self->skipUntilDepth = std::min(self->skipUntilDepth, self->depth);
        self->depth += 1;
        //Serial.printf("[%lu] [EHP] 跳过标签<%s>处理完成，新深度：%d\n", millis(), name, self->depth);
        return;
    }

    // 跳过状态中，仅更新深度，不处理
    if (self->depth >= self->skipUntilDepth) {
        //Serial.printf("[%lu] [EHP] 处于跳过状态，标签<%s>不处理，深度+1\n", millis(), name, self->depth);
        self->depth += 1;
        return;
    }

    // 标题标签：居中对齐+粗体，兼容h1-h6所有层级
    if (matches(name, HEADER_TAGS, NUM_HEADER_TAGS)) {
        //Serial.printf("[%lu] [EHP] 标签<%s>是标题标签，创建居中对齐文本块\n", millis(), name);
        self->startNewTextBlock(TextBlock::CENTER_ALIGN);
        self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
        //Serial.printf("[%lu] [EHP] 标题标签<%s>粗体深度设置为：%d\n", millis(), name, self->boldUntilDepth);
    }
    // 块级标签：处理所有承载文本的标签，避免文本丢失，无空指针
    else if (matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS)) {
        //Serial.printf("[%lu] [EHP] 标签<%s>是块级标签，创建两端对齐文本块\n", millis(), name);
        // 所有文本块标签统一用JUSTIFIED样式，避免格式错乱
        // 无论是p/block/left/note，都正常创建文本块，提取内容
        self->startNewTextBlock(TextBlock::JUSTIFIED);
        
        // ===== 新增：判断是否是<p>标签，设置isInPTag为true =====
        if (strcmp(name, "p") == 0) {
            //Serial.printf("[%lu] [EHP] 进入<p>标签，标记isInPTag为true\n", millis());
            self->isInPTag = true;
        }
        // =======================================================
        
        // 仅br标签做特殊处理（换行），其他文本块标签正常提取即可
        if (strcmp(name, "br") == 0) {
            //Serial.printf("[%lu] [EHP] 标签<br>，准备添加换行符\n", millis());
            // 追加换行符，保证换行效果，同时避免空文本
            if (self->currentTextBlock) {
                self->currentTextBlock->addWord("\n", EpdFontFamily::REGULAR);
                //Serial.printf("[%lu] [EHP] <br>标签换行符添加完成\n", millis());
            } else {
                //Serial.printf("[%lu] [EHP] <br>标签警告：currentTextBlock为空，无法添加换行符\n", millis());
            }
        }
    }
    // 粗体/斜体标签：按深度生效，兼容嵌套
    else if (matches(name, BOLD_TAGS, NUM_BOLD_TAGS)) {
        self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
        //Serial.printf("[%lu] [EHP] 标签<%s>是粗体标签，粗体深度设置为：%d\n", millis(), name, self->boldUntilDepth);
    } else if (matches(name, ITALIC_TAGS, NUM_ITALIC_TAGS)) {
        self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
        //Serial.printf("[%lu] [EHP] 标签<%s>是斜体标签，斜体深度设置为：%d\n", millis(), name, self->italicUntilDepth);
    } else {
        //Serial.printf("[%lu] [EHP] 标签<%s>是未知标签，不做额外处理\n", millis(), name);
    }

    self->depth += 1;
    //Serial.printf("[%lu] [EHP] 标签<%s>解析完成，新深度：%d\n", millis(), name, self->depth);

}

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

// 修正后的 characterData 函数：保留UTF-8解析/中英文拆分，增加无空指针防护，修正跳过逻辑
void XMLCALL ChapterHtmlSlimParser::characterData(void* userData, const XML_Char* s, int len) {
    // 新增日志：追踪文本数据解析入口
    //Serial.printf("[%lu] [EHP] 进入characterData，文本长度：%d\n", millis(), len);
    
    // 【最外层非法参数防护】避免空指针和无效长度导致崩溃
    if (!userData || !s || len <= 0) {
        //Serial.printf("[%lu] [EHP] characterData警告：参数非法（userData：%s，s：%s，len：%d）\n", 
        //              millis(), 
         //             (userData ? "有效" : "无效"), 
        //              (s ? "有效" : "无效"), 
         //             len);
        return;
    }

    auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

    // 【修正跳过逻辑bug】正确判断是否处于跳过状态
    if (self->depth >= self->skipUntilDepth) {
        //Serial.printf("[%lu] [EHP] characterData：处于跳过状态，文本不处理（当前深度：%d，跳过深度：%d）\n", 
        //              millis(), 
        //              self->depth, 
        //              self->skipUntilDepth);
        return;
    }

    // 【修改点：移除新建TextBlock的代码，替换为直接防护】
    // 不强行新建TextBlock（构造参数不匹配），为空则直接跳过后续操作，避免空指针崩溃
    if (!self->currentTextBlock) {
        //Serial.printf("[%lu] [EHP] characterData警告：currentTextBlock为空，无法处理文本\n", millis());
        return;
    }

    // ===== 替换：删除原有流式暂存，新增「超阈值自动拆分虚拟<p>」核心逻辑 =====
    std::string current_text(s, len);
    std::string text_to_parse = current_text; // 默认要解析的文本就是当前文本

    // 1. 确定当前字体样式（提前提取，方便后续拆分时复用）
    EpdFontFamily::Style fontStyle = EpdFontFamily::REGULAR;
    if (self->boldUntilDepth < self->depth && self->italicUntilDepth < self->depth) {
        fontStyle = EpdFontFamily::BOLD_ITALIC;
    } else if (self->boldUntilDepth < self->depth) {
        fontStyle = EpdFontFamily::BOLD;
    } else if (self->italicUntilDepth < self->depth) {
        fontStyle = EpdFontFamily::ITALIC;
    }

    if (self->isInPTag) {
        // 2. 遍历当前文本，累计有效字符数，达到阈值自动拆分
        for (size_t idx = 0; idx < current_text.length(); ) {
            const unsigned char* u8 = (const unsigned char*)&current_text[idx];
            int charLen = ChapterHtmlSlimParser::getUtf8CharLength(u8);
            
            // 跳过无效UTF-8字符（和后续解析逻辑一致）
            if (charLen == 0 || charLen > 4 || idx + charLen > current_text.length()) {
                idx++;
                continue;
            }

            std::string currentChar(reinterpret_cast<const char*>(u8), charLen);
            idx += charLen;

            // 3. 只累计非空白符（避免空行触发不必要拆分）
            if (!currentChar.empty() && !ChapterHtmlSlimParser::isWhitespace(currentChar[0])) {
                self->currentPCharCount++;

                // 4. 核心：达到安全阈值，且不是虚拟<p>，自动拆分
                if (self->currentPCharCount >= P_SAFE_THRESHOLD && !self->isVirtualP) {
                    //Serial.printf("[%lu] [EHP] <p>拆分：当前段已达%d字符，自动新建虚拟<p>段\n", 
                    //              millis(), P_SAFE_THRESHOLD);

                    // ===== 第一步：手动关闭当前<p>（复用你的收尾逻辑）=====
                    // 清空当前英文缓存
                    if (self->partWordBufferIndex > 0) {
                        if (self->partWordBufferIndex < MAX_WORD_SIZE) {
                            self->partWordBuffer[self->partWordBufferIndex] = '\0';
                        } else {
                            self->partWordBuffer[MAX_WORD_SIZE - 1] = '\0';
                        }
                        self->currentTextBlock->addWord(std::string(self->partWordBuffer), fontStyle);
                        self->partWordBufferIndex = 0;
                    }
                    // 重置当前<p>标记和计数（模拟</p>）
                    self->currentPCharCount = 0;
                    // ======================================================

                    // ===== 第二步：手动新建虚拟<p>（复用你的创建逻辑）=====
                    self->isVirtualP = true; // 标记为虚拟<p>，避免无限递归
                    // 新建空文本块（和原有<p>标签的文本块类型一致）
                    self->startNewTextBlock(TextBlock::JUSTIFIED);
                    // 重置标记，恢复正常解析
                    self->isVirtualP = false;
                    //Serial.printf("[%lu] [EHP] <p>拆分：虚拟<p>创建完成，继续解析剩余内容\n", millis());
                    // ======================================================
                }
            }
        }
    }
    // =======================================================

    // 1. 确定当前字体样式（原有逻辑保留，和上面提前提取的一致，避免冲突）
    //Serial.printf("[%lu] [EHP] characterData：当前字体样式：%d，开始解析UTF-8文本\n", millis(), fontStyle);
    
    // ===== 保留：替换解析数据源为处理后的text_to_parse（关键，不修改）=====
    const char* parse_s = text_to_parse.c_str();
    int parse_len = text_to_parse.length();
    if (parse_len <= 0) {
        //Serial.printf("[%lu] [EHP] characterData：无有效文本需要解析，直接返回\n", millis());
        return;
    }
    // =======================================================

    // 2. 逐字节解析UTF-8字符（原有核心逻辑保留，仅修改循环的len→parse_len，s→parse_s）
    int i = 0;
    while (i < parse_len) { // 改动1：len → parse_len（适配新的解析数据源）
        const unsigned char* u8 = (const unsigned char*)&parse_s[i]; // 改动2：s → parse_s（适配新的解析数据源）
        // 调用静态函数获取UTF-8字符长度
        int charLen = ChapterHtmlSlimParser::getUtf8CharLength(u8);
        
        // 【强化越界/无效编码防护】避免循环越界
        if (charLen == 0 || charLen > 4) { // UTF-8最大字符长度为4，直接过滤无效长度
            i++;
            continue;
        }
        if (i + charLen > parse_len) { // 改动3：len → parse_len（适配新的解析数据源）
            i++;
            continue;
        }

        // 3. 提取单个UTF-8字符（原有逻辑保留）
        std::string currentChar(reinterpret_cast<const char*>(u8), charLen);
        i += charLen;

        // 4. 跳过空白符（调用静态函数，原有逻辑保留，增加防护）
        if (currentChar.empty() || ChapterHtmlSlimParser::isWhitespace(currentChar[0])) {
            // 空白符触发：清空英文缓存
            if (self->partWordBufferIndex > 0) {
                // 边界防护，避免缓存越界（原有逻辑保留）
                if (self->partWordBufferIndex < MAX_WORD_SIZE) {
                    self->partWordBuffer[self->partWordBufferIndex] = '\0';
                } else {
                    self->partWordBuffer[MAX_WORD_SIZE - 1] = '\0';
                }
                // 【空指针防护】currentTextBlock已提前判断，此处可安全调用
                self->currentTextBlock->addWord(std::string(self->partWordBuffer), fontStyle);
                self->partWordBufferIndex = 0;
            }
            continue;
        }

        // 5. 判断字符类型：中文/英文（调用静态函数，原有逻辑保留，增加防护）
        if (ChapterHtmlSlimParser::isChineseChar(u8, charLen)) {
            // 中文：先清空英文缓存，再单个字符添加
            if (self->partWordBufferIndex > 0) {
                // 边界防护，避免缓存越界（原有逻辑保留）
                if (self->partWordBufferIndex < MAX_WORD_SIZE) {
                    self->partWordBuffer[self->partWordBufferIndex] = '\0';
                } else {
                    self->partWordBuffer[MAX_WORD_SIZE - 1] = '\0';
                }
                self->currentTextBlock->addWord(std::string(self->partWordBuffer), fontStyle);
                self->partWordBufferIndex = 0;
            }
            self->currentTextBlock->addWord(currentChar, fontStyle);
        } else {
            // 英文：拼接到字符数组缓存（原有逻辑保留，增加防护）
            if (self->partWordBufferIndex + charLen >= MAX_WORD_SIZE) {
                // 缓存溢出：先添加现有内容，再重置
                if (self->partWordBufferIndex < MAX_WORD_SIZE) {
                    self->partWordBuffer[self->partWordBufferIndex] = '\0';
                } else {
                    self->partWordBuffer[MAX_WORD_SIZE - 1] = '\0';
                }
                self->currentTextBlock->addWord(std::string(self->partWordBuffer), fontStyle);
                self->partWordBufferIndex = 0;
            }
            // 逐字节拷贝到字符数组（原有逻辑保留，确保缓存不越界）
            if (self->partWordBufferIndex + charLen < MAX_WORD_SIZE) { // 额外防护，避免memcpy越界
                memcpy(&self->partWordBuffer[self->partWordBufferIndex], u8, charLen);
                self->partWordBufferIndex += charLen;
            }
        }
    }

    // 6. 处理剩余的英文单词（增加边界防护，原有逻辑保留）
    if (self->partWordBufferIndex > 0) {
        if (self->partWordBufferIndex < MAX_WORD_SIZE) {
            self->partWordBuffer[self->partWordBufferIndex] = '\0';
        } else {
            self->partWordBuffer[MAX_WORD_SIZE - 1] = '\0';
        }
        self->currentTextBlock->addWord(std::string(self->partWordBuffer), fontStyle);
        self->partWordBufferIndex = 0;
    }
    //Serial.printf("[%lu] [EHP] characterData执行完成，文本处理完毕，当前<p>累计%d字符\n", millis(), self->currentPCharCount);
}



void XMLCALL ChapterHtmlSlimParser::endElement(void* userData, const XML_Char* name) {
  // 新增日志：追踪标签结束解析
  //Serial.printf("[%lu] [EHP] 进入endElement，准备关闭标签：<%s>，当前深度：%d\n", 
   //             millis(), 
   //             (name ? name : "未知"), 
   //             static_cast<ChapterHtmlSlimParser*>(userData)->depth);
  
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  if (self->partWordBufferIndex > 0) {
    // Only flush out part word buffer if we're closing a block tag or are at the top of the HTML file.
    // We don't want to flush out content when closing inline tags like <span>.
    const bool shouldBreakText =
        matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS) || matches(name, HEADER_TAGS, NUM_HEADER_TAGS) ||
        matches(name, BOLD_TAGS, NUM_BOLD_TAGS) || matches(name, ITALIC_TAGS, NUM_ITALIC_TAGS) || self->depth == 1;

    if (shouldBreakText) {
      EpdFontFamily::Style fontStyle = EpdFontFamily::REGULAR;
      if (self->boldUntilDepth < self->depth && self->italicUntilDepth < self->depth) {
        fontStyle = EpdFontFamily::BOLD_ITALIC;
      } else if (self->boldUntilDepth < self->depth) {
        fontStyle = EpdFontFamily::BOLD;
      } else if (self->italicUntilDepth < self->depth) {
        fontStyle = EpdFontFamily::ITALIC;
      }

      // 边界防护，避免缓存越界
      if (self->partWordBufferIndex < MAX_WORD_SIZE) {
        self->partWordBuffer[self->partWordBufferIndex] = '\0';
      } else {
        self->partWordBuffer[MAX_WORD_SIZE - 1] = '\0';
      }
      self->currentTextBlock->addWord(std::move(replaceHtmlEntities(self->partWordBuffer)), fontStyle);
      self->partWordBufferIndex = 0;
      //Serial.printf("[%lu] [EHP] endElement：清空英文缓存完成，字体样式：%d\n", millis(), fontStyle);
    }
  }

  self->depth -= 1;
  //Serial.printf("[%lu] [EHP] endElement：标签<%s>深度-1，新深度：%d\n", millis(), (name ? name : "未知"), self->depth);

 if (name && strcmp(name, "p") == 0) {
    //Serial.printf("[%lu] [EHP] endElement：退出<p>标签，重置isInPTag为false\n", millis());
    
    // ===== 新增：<p>关闭时，解析最后暂存的剩余文本（关键收尾）=====
      if (name && strcmp(name, "p") == 0) {
        //Serial.printf("[%lu] [EHP] endElement：退出<p>标签，重置拆分标记\n", millis());
        auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
        self->isInPTag = false;
        self->currentPCharCount = 0; // 重置累计字符数
        self->isVirtualP = false;    // 重置虚拟<p>标记
        //Serial.printf("[%lu] [EHP] endElement：<p>标签收尾完成\n", millis());
    }
    // =======================================================
    
    self->isInPTag = false;
}

  // Leaving skip
  if (self->skipUntilDepth == self->depth) {
    self->skipUntilDepth = INT_MAX;
    //Serial.printf("[%lu] [EHP] endElement：退出跳过状态，重置skipUntilDepth为INT_MAX\n", millis());
  }

  // Leaving bold
  if (self->boldUntilDepth == self->depth) {
    self->boldUntilDepth = INT_MAX;
    //Serial.printf("[%lu] [EHP] endElement：退出粗体状态，重置boldUntilDepth为INT_MAX\n", millis());
  }

  // Leaving italic
  if (self->italicUntilDepth == self->depth) {
    self->italicUntilDepth = INT_MAX;
    //Serial.printf("[%lu] [EHP] endElement：退出斜体状态，重置italicUntilDepth为INT_MAX\n", millis());
  }
  
  //Serial.printf("[%lu] [EHP] endElement：标签<%s>处理完成\n", millis(), (name ? name : "未知"));
}

bool ChapterHtmlSlimParser::parseAndBuildPages() {
  // 新增日志：追踪解析入口
  //Serial.printf("[%lu] [EHP] 进入parseAndBuildPages，开始解析文件：%s\n", millis(), filepath);
  
  // 初始化正文块，设置默认样式
  //Serial.printf("[%lu] [EHP] parseAndBuildPages：初始化默认文本块（两端对齐）\n", millis());
  startNewTextBlock(TextBlock::JUSTIFIED);

  const XML_Parser parser = XML_ParserCreate(nullptr);
  int done;

  if (!parser) {
    //Serial.printf("[%lu] [EHP] Couldn't allocate memory for parser\n", millis());
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
  
  //Serial.printf("[%lu] [EHP] parseAndBuildPages：文件打开成功，文件大小：%lu 字节\n", millis(), totalSize);

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  //Serial.printf("[%lu] [EHP] parseAndBuildPages：Expat解析器配置完成，开始循环读取文件\n", millis());

  do {
    void* const buf = XML_GetBuffer(parser, 1024);
    if (!buf) {
      //Serial.printf("[%lu] [EHP] Couldn't allocate memory for buffer\n", millis());
      cleanupResources(parser, file); // 统一释放资源
      return false;
    }

    const size_t len = file.read(buf, 1024);

    if (len == 0 && file.available() > 0) {
      //Serial.printf("[%lu] [EHP] File read error\n", millis());
      cleanupResources(parser, file); // 统一释放资源
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
      //Serial.printf("[%lu] [EHP] Parse error at line %lu:\n%s\n", millis(), XML_GetCurrentLineNumber(parser),
      //              XML_ErrorString(XML_GetErrorCode(parser)));
      cleanupResources(parser, file); // 统一释放资源
      return false;
    }
  } while (!done);

  // 解析完成后，强制刷新残留的缓存（核心修复：避免章节结尾缓存残留）
  //Serial.printf("[%lu] [EHP] parseAndBuildPages：文件读取完成，开始刷新残留缓存\n", millis());
  if (currentTextBlock && partWordBufferIndex > 0) {
    EpdFontFamily::Style fontStyle = EpdFontFamily::REGULAR;
    if (boldUntilDepth < depth && italicUntilDepth < depth) {
      fontStyle = EpdFontFamily::BOLD_ITALIC;
    } else if (boldUntilDepth < depth) {
      fontStyle = EpdFontFamily::BOLD;
    } else if (italicUntilDepth < depth) {
      fontStyle = EpdFontFamily::ITALIC;
    }
    // 边界防护，避免缓存越界
    if (partWordBufferIndex < MAX_WORD_SIZE) {
      partWordBuffer[partWordBufferIndex] = '\0';
    } else {
      partWordBuffer[MAX_WORD_SIZE - 1] = '\0';
    }
    currentTextBlock->addWord(std::move(replaceHtmlEntities(partWordBuffer)), fontStyle);
    partWordBufferIndex = 0;
    //Serial.printf("[%lu] [EHP] parseAndBuildPages：残留缓存刷新完成\n", millis());
  }

  // 释放解析器和文件资源
  //Serial.printf("[%lu] [EHP] parseAndBuildPages：开始释放解析器和文件资源\n", millis());
  cleanupResources(parser, file);

  // Process last page if there is still text
  //Serial.printf("[%lu] [EHP] parseAndBuildPages：开始处理最后一页文本\n", millis());
  if (currentTextBlock) {
    makePages();
    completePageFn(std::move(currentPage));
    currentPage.reset();
    currentTextBlock.reset();
    //Serial.printf("[%lu] [EHP] parseAndBuildPages：最后一页文本处理完成\n", millis());
  }

  // 解析完成后，强制重置所有状态变量（避免下一章携带脏数据）
  depth = 0;
  skipUntilDepth = INT_MAX;
  boldUntilDepth = INT_MAX;
  italicUntilDepth = INT_MAX;
  currentPageNextY = 0;
  partWordBufferIndex = 0;
  memset(partWordBuffer, 0, sizeof(partWordBuffer));
  currentBlockStyle = TextBlock::JUSTIFIED;
  //Serial.printf("[%lu] [EHP] parseAndBuildPages：所有状态变量重置完成，解析流程结束\n", millis());

  return true;
}

void ChapterHtmlSlimParser::addLineToPage(std::shared_ptr<TextBlock> line) {
  // 新增日志：追踪行添加操作
  //Serial.printf("[%lu] [EHP] 进入addLineToPage，当前页面下一个Y坐标：%d\n", millis(), currentPageNextY);
  
  const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;

  if (currentPageNextY + lineHeight > viewportHeight) {
    //Serial.printf("[%lu] [EHP] addLineToPage：页面空间不足，创建新页面\n", millis());
    completePageFn(std::move(currentPage));
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  currentPage->elements.push_back(std::make_shared<PageLine>(line, 0, currentPageNextY));
  currentPageNextY += lineHeight;
  
  //Serial.printf("[%lu] [EHP] addLineToPage：行添加完成，新的下一个Y坐标：%d\n", millis(), currentPageNextY);
}

void ChapterHtmlSlimParser::makePages() {
  // 新增日志：追踪分页操作
  //Serial.printf("[%lu] [EHP] 进入makePages，开始分页处理\n", millis());
  
  if (!currentTextBlock) {
    //Serial.printf("[%lu] [EHP] !! No text block to make pages for !!\n", millis());
    return;
  }

  if (!currentPage) {
    //Serial.printf("[%lu] [EHP] makePages：当前页面为空，创建新页面\n", millis());
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
    //Serial.printf("[%lu] [EHP] makePages：启用额外段落间距，当前Y坐标：%d\n", millis(), currentPageNextY);
  }
  
  //Serial.printf("[%lu] [EHP] makePages：分页处理完成\n", millis());
}