/**
 * XtcParser.cpp
 *
 * XTC file parsing implementation
 * XTC ebook support for CrossPoint Reader
 */

#include "XtcParser.h"

#include <FsHelpers.h>
#include <HardwareSerial.h>
#include <SDCardManager.h>

#include <cstring>

namespace xtc {

XtcParser::XtcParser()
    : m_isOpen(false),
      m_defaultWidth(DISPLAY_WIDTH),
      m_defaultHeight(DISPLAY_HEIGHT),
      m_bitDepth(1),
      m_hasChapters(false),
      m_lastError(XtcError::OK),
      m_loadBatchSize(200),  // 减小
      m_loadedMaxPage(0),
      m_loadedStartPage(0) {  // 记录当前页表的起始页 
  memset(&m_header, 0, sizeof(m_header));
}

XtcParser::~XtcParser() { close(); }

XtcError XtcParser::open(const char* filepath) {
  // Close if already open
  if (m_isOpen) {
    close();
  }

  // Open file
  if (!SdMan.openFileForRead("XTC", filepath, m_file)) {
    m_lastError = XtcError::FILE_NOT_FOUND;
    return m_lastError;
  }

  // Read header
  m_lastError = readHeader();
  if (m_lastError != XtcError::OK) {
    Serial.printf("[%lu] [XTC] Failed to read header: %s\n", millis(), errorToString(m_lastError));
    m_file.close();
    return m_lastError;
  }

  // Read title & author if available
  if (m_header.hasMetadata) {
    m_lastError = readTitle();
    if (m_lastError != XtcError::OK) {
      Serial.printf("[%lu] [XTC] Failed to read title: %s\n", millis(), errorToString(m_lastError));
      m_file.close();
      return m_lastError;
    }
    m_lastError = readAuthor();
    if (m_lastError != XtcError::OK) {
      Serial.printf("[%lu] [XTC] Failed to read author: %s\n", millis(), errorToString(m_lastError));
      m_file.close();
      return m_lastError;
    }
  }

  // Read page table
  m_lastError = readPageTable();
  if (m_lastError != XtcError::OK) {
    Serial.printf("[%lu] [XTC] Failed to read page table: %s\n", millis(), errorToString(m_lastError));
    m_file.close();
    return m_lastError;
  }

  // ✅ 恢复原有章节读取逻辑（不调用readChapters_gd）
  m_lastError = readChapters();
  if (m_lastError != XtcError::OK) {
    Serial.printf("[%lu] [XTC] Failed to read internal chapters: %s\n", millis(), errorToString(m_lastError));
    // 读取失败时创建默认章节，不退出
    m_chapters.clear();
    std::string chapterName = m_title.empty() ? "全书" : m_title;
    ChapterInfo singleChapter{std::move(chapterName), 0, m_header.pageCount - 1};
    m_chapters.push_back(std::move(singleChapter));
    m_hasChapters = true;
  }

  m_isOpen = true;
  Serial.printf("[%lu] [XTC] Opened file: %s (%u pages, %dx%d, %u internal chapters)\n", millis(), filepath, 
                m_header.pageCount, m_defaultWidth, m_defaultHeight, (uint16_t)m_chapters.size());
  return XtcError::OK;
}

void XtcParser::close() {
  if (m_isOpen) {
    m_file.close();
    m_isOpen = false;
  }
  m_pageTable.clear();
  m_chapters.clear();
  m_title.clear();
  m_hasChapters = false;
  m_loadedMaxPage = 0;
  m_loadedStartPage = 0;
  memset(&m_header, 0, sizeof(m_header));
  
  // ✅ 仅清空独立的GD章节数据，不影响内部状态
  chapterActualCount = 0;
  memset(ChapterList, 0, sizeof(ChapterList));
}

XtcError XtcParser::readHeader() {
  // Read first 56 bytes of header
  size_t bytesRead = m_file.read(reinterpret_cast<uint8_t*>(&m_header), sizeof(XtcHeader));
  if (bytesRead != sizeof(XtcHeader)) {
    return XtcError::READ_ERROR;
  }

  // Verify magic number (accept both XTC and XTCH)
  if (m_header.magic != XTC_MAGIC && m_header.magic != XTCH_MAGIC) {
    Serial.printf("[%lu] [XTC] Invalid magic: 0x%08X (expected 0x%08X or 0x%08X)\n", millis(), m_header.magic,
                  XTC_MAGIC, XTCH_MAGIC);
    return XtcError::INVALID_MAGIC;
  }

  // Determine bit depth from file magic
  m_bitDepth = (m_header.magic == XTCH_MAGIC) ? 2 : 1;

  // Check version
  const bool validVersion = m_header.versionMajor == 1 && m_header.versionMinor == 0 ||
                            m_header.versionMajor == 0 && m_header.versionMinor == 1;
  if (!validVersion) {
    Serial.printf("[%lu] [XTC] Unsupported version: %u.%u\n", millis(), m_header.versionMajor, m_header.versionMinor);
    return XtcError::INVALID_VERSION;
  }

  // Basic validation
  if (m_header.pageCount == 0) {
    return XtcError::CORRUPTED_HEADER;
  }

  Serial.printf("[%lu] [XTC] Header: magic=0x%08X (%s), ver=%u.%u, pages=%u, bitDepth=%u\n", millis(), m_header.magic,
                (m_header.magic == XTCH_MAGIC) ? "XTCH" : "XTC", m_header.versionMajor, m_header.versionMinor,
                m_header.pageCount, m_bitDepth);

  return XtcError::OK;
}

XtcError XtcParser::readTitle() {
  constexpr auto titleOffset = 0x38;
  if (!m_file.seek(titleOffset)) {
    return XtcError::READ_ERROR;
  }

  char titleBuf[128] = {0};
  m_file.read(titleBuf, sizeof(titleBuf) - 1);
  m_title = titleBuf;

  Serial.printf("[%lu] [XTC] Title: %s\n", millis(), m_title.c_str());
  return XtcError::OK;
}

XtcError XtcParser::readAuthor() {
  constexpr auto authorOffset = 0xB8;
  if (!m_file.seek(authorOffset)) {
    return XtcError::READ_ERROR;
  }

  char authorBuf[64] = {0};
  m_file.read(authorBuf, sizeof(authorBuf) - 1);
  m_author = authorBuf;

  Serial.printf("[%lu] [XTC] Author: %s\n", millis(), m_author.c_str());
  return XtcError::OK;
}

XtcError XtcParser::readPageTable() {
  m_pageTable.clear();          
  m_pageTable.shrink_to_fit();  
  if (m_header.pageTableOffset == 0) {
    Serial.printf("[%lu] [XTC] Page table offset is 0, cannot read\n", millis());
    return XtcError::CORRUPTED_HEADER;
  }

  // Seek to page table
  if (!m_file.seek(m_header.pageTableOffset)) {
    Serial.printf("[%lu] [XTC] Failed to seek to page table at %llu\n", millis(), m_header.pageTableOffset);
    return XtcError::READ_ERROR;
  }
  
  // 初始加载：从第0页开始，加载第一批2000页
  uint16_t startPage = 0;
  uint16_t endPage = startPage + m_loadBatchSize - 1;
  if(endPage >= m_header.pageCount) endPage = m_header.pageCount - 1;
  uint16_t loadCount = endPage - startPage + 1;

  m_pageTable.resize(endPage + 1);

  // Read page table entries
  for (uint16_t i = startPage; i <= endPage; i++) {
    PageTableEntry entry;
    size_t bytesRead = m_file.read(reinterpret_cast<uint8_t*>(&entry), sizeof(PageTableEntry));
    if (bytesRead != sizeof(PageTableEntry)) {
      Serial.printf("[%lu] [XTC] Failed to read page table entry %u\n", millis(), i);
      return XtcError::READ_ERROR;
    }

    m_pageTable[i].offset = static_cast<uint32_t>(entry.dataOffset);
    m_pageTable[i].size = entry.dataSize;
    m_pageTable[i].width = entry.width;
    m_pageTable[i].height = entry.height;
    m_pageTable[i].bitDepth = m_bitDepth;

    // Update default dimensions from first page
    if (i == 0) {
      m_defaultWidth = entry.width;
      m_defaultHeight = entry.height;
    }
  }
  m_loadedMaxPage = endPage;
  Serial.printf("[%lu] [XTC] Read %u page table entries (batch 0~%u)\n", millis(), loadCount, endPage);
  return XtcError::OK;
}

// ✅ 原有章节读取函数（内部使用，不影响GD接口）
XtcError XtcParser::readChapters() {
  m_hasChapters = false;
  m_chapters.clear();

  uint8_t hasChaptersFlag = 0;
  if (!m_file.seek(0x0B)) {
    return XtcError::READ_ERROR;
  }
  if (m_file.read(&hasChaptersFlag, sizeof(hasChaptersFlag)) != sizeof(hasChaptersFlag)) {
    return XtcError::READ_ERROR;
  }

  uint64_t chapterOffset = 0;
  if (!m_file.seek(0x30)) {
    return XtcError::READ_ERROR;
  }
  if (m_file.read(reinterpret_cast<uint8_t*>(&chapterOffset), sizeof(chapterOffset)) != sizeof(chapterOffset)) {
    return XtcError::READ_ERROR;
  }

  const uint64_t fileSize = m_file.size();
  if (chapterOffset < sizeof(XtcHeader) || chapterOffset >= fileSize || chapterOffset + 96 > fileSize) {
    // 创建默认章节
    std::string chapterName = m_title.empty() ? "全书" : m_title;
    ChapterInfo singleChapter{std::move(chapterName), 0, m_header.pageCount - 1};
    m_chapters.push_back(std::move(singleChapter));
    m_hasChapters = true;
    return XtcError::OK;
  }

  uint64_t maxOffset = 0;
  if (m_header.pageTableOffset > chapterOffset) {
    maxOffset = m_header.pageTableOffset;
  } else if (m_header.dataOffset > chapterOffset) {
    maxOffset = m_header.dataOffset;
  } else {
    maxOffset = fileSize;
  }

  if (maxOffset <= chapterOffset) {
    std::string chapterName = m_title.empty() ? "全书" : m_title;
    ChapterInfo singleChapter{std::move(chapterName), 0, m_header.pageCount - 1};
    m_chapters.push_back(std::move(singleChapter));
    m_hasChapters = true;
    return XtcError::OK;
  }

  constexpr size_t chapterSize = 96;
  const uint64_t available = maxOffset - chapterOffset;
  const size_t chapterCount = static_cast<size_t>(available / chapterSize);

  if (!m_file.seek(chapterOffset)) {
    return XtcError::READ_ERROR;
  }

  std::vector<uint8_t> chapterBuf(chapterSize);
  for (size_t i = 0; i < chapterCount; i++) {
    if (m_file.read(chapterBuf.data(), chapterSize) != chapterSize) {
      break;
    }
  }

  // 内部默认章节
  std::string chapterName = m_title.empty() ? "全书" : m_title;
  ChapterInfo singleChapter{std::move(chapterName), 0, m_header.pageCount - 1};
  m_chapters.push_back(std::move(singleChapter));
  m_hasChapters = !m_chapters.empty();

  Serial.printf("[%lu] [XTC] 内部章节: [%s] | 共%u页\n", millis(), singleChapter.name.c_str(), m_header.pageCount);
  return XtcError::OK;
}

// ========== ✨ 核心修复：独立的readChapters_gd（完全不修改内部状态） ==========
XtcError XtcParser::readChapters_gd(uint16_t chapterStart) {
    Serial.printf("[%lu] [XTC-GD] === 独立章节读取开始，起始索引=%u ===\n", millis(), chapterStart);
    
    // ✅ 1. 前置检查：文件必须打开
    if (!m_isOpen) {
        Serial.printf("[%lu] [XTC-GD] ❌ 文件未打开\n", millis());
        chapterActualCount = 0;
        memset(ChapterList, 0, sizeof(ChapterList));
        return XtcError::FILE_NOT_FOUND;
    }

    // ✅ 2. 保存当前文件指针（关键：避免影响其他操作）
    uint64_t originalPos = m_file.position();
    Serial.printf("[%lu] [XTC-GD] 保存原指针位置: %llu\n", millis(), originalPos);

    // ✅ 3. 重置GD章节数据（独立清空，不影响m_chapters）
    chapterActualCount = 0;
    memset(ChapterList, 0, sizeof(ChapterList));
    
    // ===== 读取章节开关标记 =====
    uint8_t hasChaptersFlag = 0;
    if (!m_file.seek(0x0B)) {
        Serial.printf("[%lu] [XTC-GD] ❌ seek(0x0B) 失败\n", millis());
        m_file.seek(originalPos); // 恢复指针
        return XtcError::READ_ERROR;
    }
    
    size_t readSize = m_file.read(&hasChaptersFlag, sizeof(hasChaptersFlag));
    Serial.printf("[%lu] [XTC-GD] 章节标记: 读取大小=%u, 值=0x%02X\n", millis(), readSize, hasChaptersFlag);
    
    // ===== 读取章节区起始偏移 =====
    uint64_t chapterOffset = 0;
    if (!m_file.seek(0x30)) {
        Serial.printf("[%lu] [XTC-GD] ❌ seek(0x30) 失败\n", millis());
        m_file.seek(originalPos); // 恢复指针
        return XtcError::READ_ERROR;
    }
    
    readSize = m_file.read(reinterpret_cast<uint8_t*>(&chapterOffset), sizeof(chapterOffset));
    Serial.printf("[%lu] [XTC-GD] 章节偏移: 读取大小=%u, 值=%llu\n", millis(), readSize, chapterOffset);
    
    // ===== 边界检查（宽松版，避免提前退出） =====
    const uint64_t fileSize = m_file.size();
    Serial.printf("[%lu] [XTC-GD] 文件大小: %llu, 页表偏移: %llu\n", millis(), fileSize, m_header.pageTableOffset);

    // 仅做基础检查：偏移不能超过文件大小
    if (chapterOffset >= fileSize || chapterOffset + 96 > fileSize) {
        Serial.printf("[%lu] [XTC-GD] 章节偏移无效，返回空章节\n", millis());
        m_file.seek(originalPos); // 恢复指针
        return XtcError::OK;
    }
    
    // 计算最大可读取的章节数
    constexpr size_t chapterSize = 96;
    uint64_t maxOffset = (m_header.pageTableOffset > 0 && m_header.pageTableOffset < fileSize) 
                        ? m_header.pageTableOffset : fileSize;
    uint64_t available = (maxOffset > chapterOffset) ? (maxOffset - chapterOffset) : 0;
    maxChapterCount = static_cast<size_t>(available / chapterSize);
    
    Serial.printf("[%lu] [XTC-GD] 可读取章节数: %u (最大偏移: %llu, 可用空间: %llu)\n", 
                 millis(), (unsigned int)maxChapterCount, maxOffset, available);

    // 起始索引越界检查
    if (chapterStart >= maxChapterCount) {
        Serial.printf("[%lu] [XTC-GD] 起始索引%u超过最大章节数%u\n", millis(), chapterStart, (unsigned int)maxChapterCount);
        m_file.seek(originalPos); // 恢复指针
        return XtcError::OK;
    }
    
    // ===== 定位到起始章节 =====
    uint64_t startReadOffset = chapterOffset + (chapterStart * chapterSize);
    Serial.printf("[%lu] [XTC-GD] 定位到章节起始位置: %llu\n", millis(), startReadOffset);
    
    if (!m_file.seek(startReadOffset)) {
        Serial.printf("[%lu] [XTC-GD] ❌ 无法定位到章节偏移\n", millis());
        m_file.seek(originalPos); // 恢复指针
        return XtcError::READ_ERROR;
    }
    
    // ===== 读取章节数据（核心修复） =====
    std::vector<uint8_t> chapterBuf(chapterSize);
    int validCount = 0; // 有效章节计数
    
    // 最多读取25章，或到最大可读取数
    size_t readLimit = std::min((size_t)25, maxChapterCount - chapterStart);
    for (size_t i = 0; i < readLimit; i++) {
        // 读取单章数据
        if (m_file.read(chapterBuf.data(), chapterSize) != chapterSize) {
            Serial.printf("[%lu] [XTC-GD] 读取第%lu章失败（已读%u章）\n", millis(), chapterStart + i, validCount);
            break;
        }
        
        // ✅ 修复1：正确解析章节名（跳过空章节）
        char nameBuf[81] = {0};
        memcpy(nameBuf, chapterBuf.data(), 80); // 章节名占前80字节
        
        // 判断是否为空章节（全0）
        bool isEmpty = true;
        for (int j = 0; j < 80; j++) {
            if (nameBuf[j] != 0 && nameBuf[j] != ' ' && nameBuf[j] != '\r' && nameBuf[j] != '\n') {
                isEmpty = false;
                break;
            }
        }
        
        if (isEmpty) {
            Serial.printf("[%lu] [XTC-GD] 第%lu章为空，跳过\n", millis(), chapterStart + i);
            continue;
        }
        
        // ✅ 修复2：正确解析起始页码（偏移0x50，2字节）
        uint16_t startPage = 0;
        memcpy(&startPage, chapterBuf.data() + 0x50, sizeof(uint16_t));
        
        // 页码校正（避免0页）
        startPage = (startPage == 0) ? 0 : (startPage - 1);
        
        // ✅ 修复3：填充独立的ChapterList（不碰m_chapters）
        strncpy(ChapterList[validCount].shortTitle, nameBuf, 63);
        ChapterList[validCount].shortTitle[63] = '\0'; // 确保字符串结束
        ChapterList[validCount].startPage = startPage;
        ChapterList[validCount].chapterIndex = (uint16_t)(chapterStart + i);
        
        Serial.printf("[%lu] [XTC-GD] 有效章节%u: 索引=%u, 名称=[%s], 起始页=%u\n", 
                     millis(), validCount, ChapterList[validCount].chapterIndex,
                     ChapterList[validCount].shortTitle, ChapterList[validCount].startPage);
        
        validCount++;
        
        // 达到25章上限则停止
        if (validCount >= 25) {
            break;
        }
    }
    
    // ✅ 4. 更新独立的章节计数（仅修改chapterActualCount）
    chapterActualCount = (uint8_t)validCount;
    
    // ✅ 5. 恢复文件指针（关键：不影响其他操作）
    m_file.seek(originalPos);
    Serial.printf("[%lu] [XTC-GD] === 独立章节读取完成：恢复指针到%llu，有效章节数=%u ===\n", 
                 millis(), originalPos, chapterActualCount);
    
    return XtcError::OK;
}

// ========== 动态加载相关函数 ==========
XtcError XtcParser::loadNextPageBatch() {
  if(!m_isOpen) return XtcError::FILE_NOT_FOUND;
  if(m_loadedMaxPage >= m_header.pageCount - 1) {
    Serial.printf("[XTC] 已加载全部%u页\n", m_header.pageCount);
    return XtcError::PAGE_OUT_OF_RANGE;
  }
  return loadPageBatchByStart(m_loadedMaxPage + 1);
}

uint16_t XtcParser::getLoadedMaxPage() const {
  return m_loadedMaxPage;
}

uint16_t XtcParser::getPageBatchSize() const {
  return m_loadBatchSize;
}

// ✅ 修复getPageInfo中的索引计算错误
bool XtcParser::getPageInfo(uint32_t pageIndex, PageInfo& info) const {
  if (pageIndex >= m_header.pageCount) return false;
  
  // 检查是否需要加载新批次
  if (pageIndex < m_loadedStartPage || pageIndex > m_loadedMaxPage) {
    auto* self = const_cast<XtcParser*>(this);
    // 修复：直接加载目标页所在的批次，而非按batchSize取整
    self->loadPageBatchByStart((uint16_t)pageIndex); 
  }
  
  // 计算当前页在页表中的相对索引
  uint16_t idx = (uint16_t)(pageIndex - m_loadedStartPage);
  if(idx >= m_pageTable.size()) return false;
  
  info = m_pageTable[idx];
  return true;
}

size_t XtcParser::loadPage(uint32_t pageIndex, uint8_t* buffer, size_t bufferSize) {
  if (!m_isOpen || pageIndex >= m_header.pageCount) {
    m_lastError = (pageIndex >= m_header.pageCount) ? XtcError::PAGE_OUT_OF_RANGE : XtcError::FILE_NOT_FOUND;
    return 0;
  }
  
  // 自动判断是否需要加载新批次
  if (pageIndex < m_loadedStartPage || pageIndex > m_loadedMaxPage) {
    loadPageBatchByStart((uint16_t)pageIndex); // 加载目标页批次
  }
  
  // 计算相对索引
  uint16_t idx = (uint16_t)(pageIndex - m_loadedStartPage);
  if (idx >= m_pageTable.size()) {
    m_lastError = XtcError::PAGE_OUT_OF_RANGE;
    return 0;
  }
  
  const PageInfo& page = m_pageTable[idx];
  if (!m_file.seek(page.offset)) {
    Serial.printf("[%lu] [XTC] Failed to seek to page %u at offset %lu\n", millis(), pageIndex, page.offset);
    m_lastError = XtcError::READ_ERROR;
    return 0;
  }

  XtgPageHeader pageHeader;
  size_t headerRead = m_file.read(reinterpret_cast<uint8_t*>(&pageHeader), sizeof(XtgPageHeader));
  if (headerRead != sizeof(XtgPageHeader)) {
    Serial.printf("[%lu] [XTC] Failed to read page header for page %u\n", millis(), pageIndex);
    m_lastError = XtcError::READ_ERROR;
    return 0;
  }

  const uint32_t expectedMagic = (m_bitDepth == 2) ? XTH_MAGIC : XTG_MAGIC;
  if (pageHeader.magic != expectedMagic) {
    Serial.printf("[%lu] [XTC] Invalid page magic for page %u: 0x%08X (expected 0x%08X)\n", millis(), pageIndex,
                  pageHeader.magic, expectedMagic);
    m_lastError = XtcError::INVALID_MAGIC;
    return 0;
  }

  size_t bitmapSize;
  if (m_bitDepth == 2) {
    bitmapSize = ((static_cast<size_t>(pageHeader.width) * pageHeader.height + 7) / 8) * 2;

  } else {
    bitmapSize = ((pageHeader.width + 7) / 8) * pageHeader.height;
  }

  if (bufferSize < bitmapSize) {
    Serial.printf("[%lu] [XTC] Buffer too small: need %u, have %u\n", millis(), bitmapSize, bufferSize);
    m_lastError = XtcError::MEMORY_ERROR;
    return 0;
  }

  size_t bytesRead = m_file.read(buffer, bitmapSize);
  if (bytesRead != bitmapSize) {
    Serial.printf("[%lu] [XTC] Page read error: expected %u, got %u\n", millis(), bitmapSize, bytesRead);
    m_lastError = XtcError::READ_ERROR;
    return 0;
  }

  m_lastError = XtcError::OK;
  return bytesRead;
}

XtcError XtcParser::loadPageStreaming(uint32_t pageIndex,
                                      std::function<void(const uint8_t* data, size_t size, size_t offset)> callback,
                                      size_t chunkSize) {
  if (!m_isOpen) {
    return XtcError::FILE_NOT_FOUND;
  }

  if (pageIndex >= m_header.pageCount) {
    return XtcError::PAGE_OUT_OF_RANGE;
  }

  // 确保页表已加载
  if (pageIndex < m_loadedStartPage || pageIndex > m_loadedMaxPage) {
    loadPageBatchByStart((uint16_t)pageIndex);
  }
  uint16_t idx = (uint16_t)(pageIndex - m_loadedStartPage);
  if (idx >= m_pageTable.size()) {
    return XtcError::PAGE_OUT_OF_RANGE;
  }
  
  const PageInfo& page = m_pageTable[idx];

  if (!m_file.seek(page.offset)) {
    return XtcError::READ_ERROR;
  }

  XtgPageHeader pageHeader;
  size_t headerRead = m_file.read(reinterpret_cast<uint8_t*>(&pageHeader), sizeof(XtgPageHeader));
  const uint32_t expectedMagic = (m_bitDepth == 2) ? XTH_MAGIC : XTG_MAGIC;
  if (headerRead != sizeof(XtgPageHeader) || pageHeader.magic != expectedMagic) {
    return XtcError::READ_ERROR;
  }

  size_t bitmapSize;
  if (m_bitDepth == 2) {
    bitmapSize = ((static_cast<size_t>(pageHeader.width) * pageHeader.height + 7) / 8) * 2;
  } else {
    bitmapSize = ((pageHeader.width + 7) / 8) * pageHeader.height;
  }

  std::vector<uint8_t> chunk(chunkSize);
  size_t totalRead = 0;

  while (totalRead < bitmapSize) {
    size_t toRead = std::min(chunkSize, bitmapSize - totalRead);
    size_t bytesRead = m_file.read(chunk.data(), toRead);

    if (bytesRead == 0) {
      return XtcError::READ_ERROR;
    }

    callback(chunk.data(), bytesRead, totalRead);
    totalRead += bytesRead;
  }

  return XtcError::OK;
}

bool XtcParser::isValidXtcFile(const char* filepath) {
  FsFile file;
  if (!SdMan.openFileForRead("XTC", filepath, file)) {
    return false;
  }

  uint32_t magic = 0;
  size_t bytesRead = file.read(reinterpret_cast<uint8_t*>(&magic), sizeof(magic));
  file.close();

  if (bytesRead != sizeof(magic)) {
    return false;
  }

  return (magic == XTC_MAGIC || magic == XTCH_MAGIC);
}

// XtcParser.cpp
XtcError XtcParser::loadPageBatchByStart(uint16_t startPage) {
    if(!m_isOpen) {
        Serial.printf("[%lu] [XTC] 加载批次失败：文件未打开\n", millis());
        return XtcError::FILE_NOT_FOUND;
    }
    if(startPage >= m_header.pageCount) {
        Serial.printf("[%lu] [XTC] 加载批次失败：起始页%u超出总页数%u\n", millis(), startPage, m_header.pageCount);
        return XtcError::PAGE_OUT_OF_RANGE;
    }

    // 保存当前状态（加载失败时回滚）
    uint16_t oldStart = m_loadedStartPage;
    uint16_t oldMax = m_loadedMaxPage;
    std::vector<PageInfo> oldPageTable = m_pageTable;

    // 计算批次范围（严格边界校验）
    m_loadedStartPage = startPage;
    uint16_t endPage = startPage + m_loadBatchSize - 1;
    endPage = (endPage >= m_header.pageCount) ? (m_header.pageCount - 1) : endPage;
    uint16_t loadCount = endPage - startPage + 1;

    // 定位页表位置（添加64位偏移校验）
    uint64_t seekOffset = m_header.pageTableOffset + (static_cast<uint64_t>(startPage) * sizeof(PageTableEntry));
    if (seekOffset >= m_file.size()) {
        Serial.printf("[%lu] [XTC] 加载批次失败：偏移%llu超出文件大小%llu\n", millis(), seekOffset, m_file.size());
        // 回滚状态
        m_loadedStartPage = oldStart;
        m_loadedMaxPage = oldMax;
        m_pageTable = oldPageTable;
        return XtcError::READ_ERROR;
    }

    if(!m_file.seek(seekOffset)) {
        Serial.printf("[%lu] [XTC] 加载批次失败：无法定位到偏移%llu\n", millis(), seekOffset);
        // 回滚状态
        m_loadedStartPage = oldStart;
        m_loadedMaxPage = oldMax;
        m_pageTable = oldPageTable;
        return XtcError::READ_ERROR;
    }

    // 加载页表（逐行校验，失败则回滚）
    m_pageTable.resize(loadCount);
    bool loadSuccess = true;
    for(uint16_t i = startPage; i <= endPage; i++) {
        PageTableEntry entry;
        if(m_file.read(reinterpret_cast<uint8_t*>(&entry), sizeof(PageTableEntry)) != sizeof(PageTableEntry)) {
            Serial.printf("[%lu] [XTC] 加载批次失败：读取第%u页表项失败\n", millis(), i);
            loadSuccess = false;
            break;
        }
        uint16_t relIdx = i - startPage;
        m_pageTable[relIdx].offset = static_cast<uint32_t>(entry.dataOffset);
        m_pageTable[relIdx].size = entry.dataSize;
        m_pageTable[relIdx].width = entry.width;
        m_pageTable[relIdx].height = entry.height;
        m_pageTable[relIdx].bitDepth = m_bitDepth;

        // 校验页表项有效性（避免无效偏移）
        if (m_pageTable[relIdx].offset >= m_file.size()) {
            Serial.printf("[%lu] [XTC] 加载批次失败：第%u页偏移%lu超出文件大小\n", millis(), i, m_pageTable[relIdx].offset);
            loadSuccess = false;
            break;
        }
    }

    if (!loadSuccess) {
        // 回滚到旧状态
        m_loadedStartPage = oldStart;
        m_loadedMaxPage = oldMax;
        m_pageTable = oldPageTable;
        return XtcError::READ_ERROR;
    }

    m_loadedMaxPage = endPage;
    Serial.printf("[%lu] [XTC] 加载批次 ✔️ : [%u~%u] (共%u页) | 页表项数=%u\n", 
                 millis(), startPage, endPage, loadCount, (uint16_t)m_pageTable.size());
    return XtcError::OK;
}

// ========== 新增：释放批次内存（解决内存泄漏核心） ==========
// XtcParser.cpp
void XtcParser::releasePageBatchByStart(uint16_t startPage) {
    if (!m_isOpen) {
        Serial.printf("[%lu] [XTC] 释放批次失败：文件未打开\n", millis());
        return;
    }

    // 仅当起始页匹配当前加载批次时才释放（避免误释放）
    if (startPage == m_loadedStartPage && m_loadedMaxPage >= m_loadedStartPage) {
        // 清空页表但保留vector容器（避免后续resize时重新分配内存导致碎片）
        m_pageTable.clear(); 
        // 重置状态为"无有效批次"（关键：后续加载会检测此状态）
        m_loadedStartPage = 0;
        m_loadedMaxPage = 0;
        
        Serial.printf("[%lu] [XTC] 释放批次 ✔️ : [%u~%u] | 页表已清空\n", 
                     millis(), startPage, m_loadedMaxPage);
    } else {
        Serial.printf("[%lu] [XTC] 跳过释放：批次[%u]非当前加载批次[%u~%u]\n", 
                     millis(), startPage, m_loadedStartPage, m_loadedMaxPage);
    }
}

// 之前写的时候没想那么多，没给自己留空，算了，用二分法查章节吧
uint16_t XtcParser::getChapterIndexByPage(uint16_t pageNum) {
    if (!m_isOpen || maxChapterCount == 0) {
        Serial.printf("[%lu] [XTC] 无法找章节：文件未打开或无章节数据\n", millis());
        return 0;
    }

    // 二分法初始化：左边界=0，右边界=最大章节数-1
    uint16_t left = 0;
    uint16_t right = static_cast<uint16_t>(maxChapterCount - 1);
    uint16_t targetChapter = 0; // 最终找到的章节索引

    while (left <= right) {
        // 1. 计算中间位置（避免溢出）
        uint16_t mid = left + ((right - left) / 2);
        
        // 2. 分段读取中间位置所在的章节块（每次读25章，和readChapters_gd逻辑一致）
        uint16_t batchStart = (mid / 25) * 25; // 计算mid所在的25章块起始索引
        readChapters_gd(batchStart); // 读取该块的章节数据到ChapterList
        
        // 3. 找到mid在当前ChapterList中的相对索引
        uint8_t relMid = mid - batchStart;
        if (relMid >= chapterActualCount) {
            // mid超出当前读取的章节块 → 说明右边界过大，缩小右边界
            right = mid - 1;
            continue;
        }

        // 4. 二分核心判断
        uint16_t midChapterStartPage = ChapterList[relMid].startPage;
        if (midChapterStartPage <= pageNum) {
            // 中间章节起始页 ≤ 目标页码 → 记录为候选，继续找更大的索引
            targetChapter = mid;
            left = mid + 1;
        } else {
            // 中间章节起始页 > 目标页码 → 缩小右边界
            right = mid - 1;
        }

        Serial.printf("[%lu] [XTC] 二分查找：mid=%u, 起始页=%u, 目标页=%u → 左=%u, 右=%u\n", 
                     millis(), mid, midChapterStartPage, pageNum, left, right);
    }

    // 5. 验证最终结果（读取目标章节所在块，确认起始页）
    uint16_t finalBatchStart = (targetChapter / 25) * 25;
    readChapters_gd(finalBatchStart);
    uint8_t finalRelIdx = targetChapter - finalBatchStart;
    uint16_t finalStartPage = ChapterList[finalRelIdx].startPage;
    
    Serial.printf("[%lu] [XTC] 最终结果：页码%u对应章节索引%u，章节起始页%u\n", 
                 millis(), pageNum, targetChapter, finalStartPage);
    return targetChapter;
}

}  // namespace xtc