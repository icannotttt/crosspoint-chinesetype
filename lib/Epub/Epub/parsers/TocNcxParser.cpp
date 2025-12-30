#include "TocNcxParser.h"

#include <HardwareSerial.h>

bool TocNcxParser::setup() {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    Serial.printf("[%lu] [TOC] Couldn't allocate memory for parser\n", millis());
    return false;
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  return true;
}

bool TocNcxParser::teardown() {
  if (parser) {
    XML_ParserFree(parser);
    parser = nullptr;
  }
  return true;
}

size_t TocNcxParser::write(const uint8_t data) { return write(&data, 1); }

size_t TocNcxParser::write(const uint8_t* buffer, const size_t size) {
  if (!parser) return 0;

  const uint8_t* currentBufferPos = buffer;
  auto remainingInBuffer = size;

  while (remainingInBuffer > 0) {
    void* const buf = XML_GetBuffer(parser, 1024);
    if (!buf) {
      Serial.printf("[%lu] [TOC] Couldn't allocate memory for buffer\n", millis());
      return 0;
    }

    const auto toRead = remainingInBuffer < 1024 ? remainingInBuffer : 1024;
    memcpy(buf, currentBufferPos, toRead);

    if (XML_ParseBuffer(parser, static_cast<int>(toRead), remainingSize == toRead) == XML_STATUS_ERROR) {
      Serial.printf("[%lu] [TOC] Parse error at line %lu: %s\n", millis(), XML_GetCurrentLineNumber(parser),
                    XML_ErrorString(XML_GetErrorCode(parser)));
      return 0;
    }

    currentBufferPos += toRead;
    remainingInBuffer -= toRead;
    remainingSize -= toRead;
  }
  return size;
}

void XMLCALL TocNcxParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
// 注：此逻辑依赖于 navPoint 的 label 和 content 出现在所有嵌套 navPoint 之前，以下写法是合规的：
// <navPoint>
//   <navLabel><text>第1章</text></navLabel>
//   <content src="ch1.html"/>
//   <navPoint> ...嵌套内容... </navPoint>
// </navPoint>
//
// 以下写法**不**合规：
// <navPoint>
//   <navPoint> ...嵌套内容... </navPoint>
//   <navLabel><text>第1章</text></navLabel>
//   <content src="ch1.html"/>
// </navPoint>

  auto* self = static_cast<TocNcxParser*>(userData);

  if (self->state == START && strcmp(name, "ncx") == 0) {
    self->state = IN_NCX;
    return;
  }

  if (self->state == IN_NCX && strcmp(name, "navMap") == 0) {
    self->state = IN_NAV_MAP;
    return;
  }

  // Handles both top-level and nested navPoints
  // 同时处理顶级和嵌套的 navPoint 节点
  if ((self->state == IN_NAV_MAP || self->state == IN_NAV_POINT) && strcmp(name, "navPoint") == 0) {
    self->state = IN_NAV_POINT;
    self->currentDepth++;

    self->currentLabel.clear();
    self->currentSrc.clear();
    return;
  }

  if (self->state == IN_NAV_POINT && strcmp(name, "navLabel") == 0) {
    self->state = IN_NAV_LABEL;
    return;
  }

  if (self->state == IN_NAV_LABEL && strcmp(name, "text") == 0) {
    self->state = IN_NAV_LABEL_TEXT;
    return;
  }

  if (self->state == IN_NAV_POINT && strcmp(name, "content") == 0) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "src") == 0) {
        self->currentSrc = atts[i + 1];
        break;
      }
    }
    return;
  }
}

void XMLCALL TocNcxParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<TocNcxParser*>(userData);
  if (self->state == IN_NAV_LABEL_TEXT) {
    self->currentLabel.append(s, len);
  }
}

void XMLCALL TocNcxParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<TocNcxParser*>(userData);

  if (self->state == IN_NAV_LABEL_TEXT && strcmp(name, "text") == 0) {
    self->state = IN_NAV_LABEL;
    return;
  }

  if (self->state == IN_NAV_LABEL && strcmp(name, "navLabel") == 0) {
    self->state = IN_NAV_POINT;
    return;
  }

  if (self->state == IN_NAV_POINT && strcmp(name, "navPoint") == 0) {
    self->currentDepth--;
    if (self->currentDepth == 0) {
      self->state = IN_NAV_MAP;
    }
    return;
  }

  if (self->state == IN_NAV_POINT && strcmp(name, "content") == 0) {
    // At this point (end of content tag), we likely have both Label (from previous tags) and Src.
    // This is the safest place to push the data, assuming <navLabel> always comes before <content>.
    // NCX spec says navLabel comes before content.
    // 至此（content 标签结束位置），我们应已获取到标签文本（来自之前的标签）和资源路径。
    // 假设 <navLabel> 标签始终位于 <content> 标签之前，此处是存入数据的最安全位置。
    // NCX 规范明确要求 navLabel 需置于 content 之前。
    if (!self->currentLabel.empty() && !self->currentSrc.empty()) {
      std::string href = self->baseContentPath + self->currentSrc;
      std::string anchor;

      const size_t pos = href.find('#');
      if (pos != std::string::npos) {
        anchor = href.substr(pos + 1);
        href = href.substr(0, pos);
      }

      // Push to vector
      self->toc.emplace_back(self->currentLabel, href, anchor, self->currentDepth);

      // Clear them so we don't re-add them if there are weird XML structures
      self->currentLabel.clear();
      self->currentSrc.clear();
    }
  }
}
