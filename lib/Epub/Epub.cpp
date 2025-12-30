#include "Epub.h"

#include <HardwareSerial.h>
#include <JpegToBmpConverter.h>
#include <SD.h>
#include <ZipFile.h>

#include <map>

#include "Epub/FsHelpers.h"
#include "Epub/parsers/ContainerParser.h"
#include "Epub/parsers/ContentOpfParser.h"
#include "Epub/parsers/TocNcxParser.h"

bool Epub::findContentOpfFile(std::string* contentOpfFile) const {
  const auto containerPath = "META-INF/container.xml";
  size_t containerSize;

  // Get file size without loading it all into heap
  // 获取文件大小，无需完整加载到堆内存
  if (!getItemSize(containerPath, &containerSize)) {
    Serial.printf("[%lu] [EBP] Could not find or size META-INF/container.xml\n", millis());
    return false;
  }

  ContainerParser containerParser(containerSize);

  if (!containerParser.setup()) {
    return false;
  }

  // Stream read (reusing your existing stream logic)
  // 流式读取（复用你现有的流处理逻辑）
  if (!readItemContentsToStream(containerPath, containerParser, 512)) {
    Serial.printf("[%lu] [EBP] Could not read META-INF/container.xml\n", millis());
    containerParser.teardown();
    return false;
  }

  // Extract the result
  // 提取结果
  if (containerParser.fullPath.empty()) {
    Serial.printf("[%lu] [EBP] Could not find valid rootfile in container.xml\n", millis());
    containerParser.teardown();
    return false;
  }

  *contentOpfFile = std::move(containerParser.fullPath);

  containerParser.teardown();
  return true;
}

bool Epub::parseContentOpf(const std::string& contentOpfFilePath) {
  size_t contentOpfSize;
  if (!getItemSize(contentOpfFilePath, &contentOpfSize)) {
    Serial.printf("[%lu] [EBP] Could not get size of content.opf\n", millis());
    return false;
  }

  ContentOpfParser opfParser(getBasePath(), contentOpfSize);

  if (!opfParser.setup()) {
    Serial.printf("[%lu] [EBP] Could not setup content.opf parser\n", millis());
    return false;
  }

  if (!readItemContentsToStream(contentOpfFilePath, opfParser, 1024)) {
    Serial.printf("[%lu] [EBP] Could not read content.opf\n", millis());
    opfParser.teardown();
    return false;
  }

  // Grab data from opfParser into epub
  // 从 opfParser 中提取数据并写入 epub
  title = opfParser.title;
  if (!opfParser.coverItemId.empty() && opfParser.items.count(opfParser.coverItemId) > 0) {
    coverImageItem = opfParser.items.at(opfParser.coverItemId);
  }

  if (!opfParser.tocNcxPath.empty()) {
    tocNcxItem = opfParser.tocNcxPath;
  }

  for (auto& spineRef : opfParser.spineRefs) {
    if (opfParser.items.count(spineRef)) {
      spine.emplace_back(spineRef, opfParser.items.at(spineRef));
    }
  }

  Serial.printf("[%lu] [EBP] Successfully parsed content.opf\n", millis());

  opfParser.teardown();
  return true;
}

bool Epub::parseTocNcxFile() {
  // the ncx file should have been specified in the content.opf file
  // 该 ncx 文件应已在 content.opf 文件中声明
  if (tocNcxItem.empty()) {
    Serial.printf("[%lu] [EBP] No ncx file specified\n", millis());
    return false;
  }

  size_t tocSize;
  if (!getItemSize(tocNcxItem, &tocSize)) {
    Serial.printf("[%lu] [EBP] Could not get size of toc ncx\n", millis());
    return false;
  }

  TocNcxParser ncxParser(contentBasePath, tocSize);

  if (!ncxParser.setup()) {
    Serial.printf("[%lu] [EBP] Could not setup toc ncx parser\n", millis());
    return false;
  }

  if (!readItemContentsToStream(tocNcxItem, ncxParser, 1024)) {
    Serial.printf("[%lu] [EBP] Could not read toc ncx stream\n", millis());
    ncxParser.teardown();
    return false;
  }

  this->toc = std::move(ncxParser.toc);

  Serial.printf("[%lu] [EBP] Parsed %d TOC items\n", millis(), this->toc.size());

  ncxParser.teardown();
  return true;
}

// load in the meta data for the epub file
// 加载该 EPUB 文件的元数据
bool Epub::load() {
  Serial.printf("[%lu] [EBP] Loading ePub: %s\n", millis(), filepath.c_str());
  ZipFile zip("/sd" + filepath);

  std::string contentOpfFilePath;
  if (!findContentOpfFile(&contentOpfFilePath)) {
    Serial.printf("[%lu] [EBP] Could not find content.opf in zip\n", millis());
    return false;
  }

  Serial.printf("[%lu] [EBP] Found content.opf at: %s\n", millis(), contentOpfFilePath.c_str());

  contentBasePath = contentOpfFilePath.substr(0, contentOpfFilePath.find_last_of('/') + 1);

  if (!parseContentOpf(contentOpfFilePath)) {
    Serial.printf("[%lu] [EBP] Could not parse content.opf\n", millis());
    return false;
  }

  if (!parseTocNcxFile()) {
    Serial.printf("[%lu] [EBP] Could not parse toc\n", millis());
    return false;
  }

  initializeSpineItemSizes();
  Serial.printf("[%lu] [EBP] Loaded ePub: %s\n", millis(), filepath.c_str());

  return true;
}

void Epub::initializeSpineItemSizes() {
  setupCacheDir();

  size_t spineItemsCount = getSpineItemsCount();
  size_t cumSpineItemSize = 0;
  if (SD.exists((getCachePath() + "/spine_size.bin").c_str())) {
    File f = SD.open((getCachePath() + "/spine_size.bin").c_str());
    uint8_t data[4];
    for (size_t i = 0; i < spineItemsCount; i++) {
      f.read(data, 4);
      cumSpineItemSize = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
      cumulativeSpineItemSize.emplace_back(cumSpineItemSize);
      // Serial.printf("[%lu] [EBP] Loading item %d size %u to %u %u\n", millis(),
      //     i, cumSpineItemSize, data[1], data[0]);
    }
    f.close();
  } else {
    File f = SD.open((getCachePath() + "/spine_size.bin").c_str(), FILE_WRITE);
    uint8_t data[4];
    // determine size of spine items
    // 确定分割项的大小
    for (size_t i = 0; i < spineItemsCount; i++) {
      std::string spineItem = getSpineItem(i);
      size_t s = 0;
      getItemSize(spineItem, &s);
      cumSpineItemSize += s;
      cumulativeSpineItemSize.emplace_back(cumSpineItemSize);

      // and persist to cache
      data[0] = cumSpineItemSize & 0xFF;
      data[1] = (cumSpineItemSize >> 8) & 0xFF;
      data[2] = (cumSpineItemSize >> 16) & 0xFF;
      data[3] = (cumSpineItemSize >> 24) & 0xFF;
      // Serial.printf("[%lu] [EBP] Persisting item %d size %u to %u %u\n", millis(),
      //     i, cumSpineItemSize, data[1], data[0]);
      f.write(data, 4);
    }

    f.close();
  }
  Serial.printf("[%lu] [EBP] Book size: %lu\n", millis(), cumSpineItemSize);
}

bool Epub::clearCache() const {
  if (!SD.exists(cachePath.c_str())) {
    Serial.printf("[%lu] [EPB] Cache does not exist, no action needed\n", millis());
    return true;
  }

  if (!FsHelpers::removeDir(cachePath.c_str())) {
    Serial.printf("[%lu] [EPB] Failed to clear cache\n", millis());
    return false;
  }

  Serial.printf("[%lu] [EPB] Cache cleared successfully\n", millis());
  return true;
}

void Epub::setupCacheDir() const {
  if (SD.exists(cachePath.c_str())) {
    return;
  }

  // Loop over each segment of the cache path and create directories as needed
  // 遍历缓存路径的每个分段，并根据需要创建目录
  for (size_t i = 1; i < cachePath.length(); i++) {
    if (cachePath[i] == '/') {
      SD.mkdir(cachePath.substr(0, i).c_str());
    }
  }
  SD.mkdir(cachePath.c_str());
}

const std::string& Epub::getCachePath() const { return cachePath; }

const std::string& Epub::getPath() const { return filepath; }

const std::string& Epub::getTitle() const { return title; }

std::string Epub::getCoverBmpPath() const { return cachePath + "/cover.bmp"; } //新增图片支持

const std::string& Epub::getCoverImageItem() const { return coverImageItem; }

bool Epub::generateCoverBmp() const {
  // Already generated, return true
  if (SD.exists(getCoverBmpPath().c_str())) {
    return true;
  }

  if (coverImageItem.empty()) {
    Serial.printf("[%lu] [EBP] No known cover image\n", millis());
    return false;
  }

  // 1. 校验封面图片后缀是否为.jpg（4字符）或.jpeg（5字符）
if (coverImageItem.substr(coverImageItem.length() - 4) == ".jpg" ||
    coverImageItem.substr(coverImageItem.length() - 5) == ".jpeg") {
  // 2. 打印日志：开始从JPG生成BMP（带毫秒时间戳，嵌入式调试用）
  Serial.printf("[%lu] [EBP] Generating BMP from JPG cover image\n", millis());

  // 3. 打开SD卡中的临时JPG文件（路径：缓存目录/.cover.jpg，写入模式，true=创建（若不存在））
  File coverJpg = SD.open((getCachePath() + "/.cover.jpg").c_str(), FILE_WRITE, true);
  // 4. 将coverImageItem指向的JPG内容读取并写入到临时JPG文件（每次读1024字节，分批写入）
  readItemContentsToStream(coverImageItem, coverJpg, 1024);
  // 5. 关闭临时JPG文件（嵌入式文件操作必须显式关闭，否则数据可能丢失）
  coverJpg.close();

  // 6. 重新以只读模式打开临时JPG文件（准备转换）
  coverJpg = SD.open((getCachePath() + "/.cover.jpg").c_str(), FILE_READ);
  // 7. 打开BMP输出文件（路径由getCoverBmpPath()指定，写入模式，创建（若不存在））
  File coverBmp = SD.open(getCoverBmpPath().c_str(), FILE_WRITE, true);
  // 8. 调用JPG转BMP工具类：将JPG文件流转为BMP文件流，返回转换是否成功
  const bool success = JpegToBmpConverter::jpegFileToBmpStream(coverJpg, coverBmp);
  // 9. 关闭转换涉及的两个文件
  coverJpg.close();
  coverBmp.close();
  // 10. 删除临时JPG文件（清理缓存，释放SD卡空间）
  SD.remove((getCachePath() + "/.cover.jpg").c_str());

  // 11. 若转换失败：打印错误日志 + 删除生成的BMP文件（避免残留无效文件）
  if (!success) {
    Serial.printf("[%lu] [EBP] Failed to generate BMP from JPG cover image\n", millis());
    SD.remove(getCoverBmpPath().c_str());
  }
  // 12. 打印转换结果日志（成功/失败）
  Serial.printf("[%lu] [EBP] Generated BMP from JPG cover image, success: %s\n", millis(), success ? "yes" : "no");
  // 13. 返回转换结果（bool）
  return success;
} else {
  // 14. 非JPG/JPEG格式：打印日志跳过处理
  Serial.printf("[%lu] [EBP] Cover image is not a JPG, skipping\n", millis());
}

  return false;
}


std::string normalisePath(const std::string& path) {
  std::vector<std::string> components;
  std::string component;

  for (const auto c : path) {
    if (c == '/') {
      if (!component.empty()) {
        if (component == "..") {
          if (!components.empty()) {
            components.pop_back();
          }
        } else {
          components.push_back(component);
        }
        component.clear();
      }
    } else {
      component += c;
    }
  }

  if (!component.empty()) {
    components.push_back(component);
  }

  std::string result;
  for (const auto& c : components) {
    if (!result.empty()) {
      result += "/";
    }
    result += c;
  }

  return result;
}

uint8_t* Epub::readItemContentsToBytes(const std::string& itemHref, size_t* size, bool trailingNullByte) const {
  const ZipFile zip("/sd" + filepath);
  const std::string path = normalisePath(itemHref);

  const auto content = zip.readFileToMemory(path.c_str(), size, trailingNullByte);
  if (!content) {
    Serial.printf("[%lu] [EBP] Failed to read item %s\n", millis(), path.c_str());
    return nullptr;
  }

  return content;
}

bool Epub::readItemContentsToStream(const std::string& itemHref, Print& out, const size_t chunkSize) const {
  const ZipFile zip("/sd" + filepath);
  const std::string path = normalisePath(itemHref);

  return zip.readFileToStream(path.c_str(), out, chunkSize);
}

bool Epub::getItemSize(const std::string& itemHref, size_t* size) const {
  const ZipFile zip("/sd" + filepath);
  const std::string path = normalisePath(itemHref);

  return zip.getInflatedFileSize(path.c_str(), size);
}

int Epub::getSpineItemsCount() const { return spine.size(); }

size_t Epub::getCumulativeSpineItemSize(const int spineIndex) const { return cumulativeSpineItemSize.at(spineIndex); }

std::string& Epub::getSpineItem(const int spineIndex) {
  if (spineIndex < 0 || spineIndex >= spine.size()) {
    Serial.printf("[%lu] [EBP] getSpineItem index:%d is out of range\n", millis(), spineIndex);
    return spine.at(0).second;
  }

  return spine.at(spineIndex).second;
}

EpubTocEntry& Epub::getTocItem(const int tocTndex) {
  if (tocTndex < 0 || tocTndex >= toc.size()) {
    Serial.printf("[%lu] [EBP] getTocItem index:%d is out of range\n", millis(), tocTndex);
    return toc.at(0);
  }

  return toc.at(tocTndex);
}

int Epub::getTocItemsCount() const { return toc.size(); }

// work out the section index for a toc index
// 根据目录索引确定章节索引
int Epub::getSpineIndexForTocIndex(const int tocIndex) const {
  if (tocIndex < 0 || tocIndex >= toc.size()) {
    Serial.printf("[%lu] [EBP] getSpineIndexForTocIndex: tocIndex %d out of range\n", millis(), tocIndex);
    return 0;
  }

  // the toc entry should have an href that matches the spine item
  // so we can find the spine index by looking for the href
  // 目录条目应包含与书脊项匹配的链接地址
  // 因此我们可通过查找该链接地址来确定书脊索引
  for (int i = 0; i < spine.size(); i++) {
    if (spine[i].second == toc[tocIndex].href) {
      return i;
    }
  }

  Serial.printf("[%lu] [EBP] Section not found\n", millis());
  // not found - default to the start of the book
  return 0;
}

int Epub::getTocIndexForSpineIndex(const int spineIndex) const {
  if (spineIndex < 0 || spineIndex >= spine.size()) {
    Serial.printf("[%lu] [EBP] getTocIndexForSpineIndex: spineIndex %d out of range\n", millis(), spineIndex);
    return -1;
  }

  // the toc entry should have an href that matches the spine item
  // so we can find the toc index by looking for the href
  // 目录条目应包含与书脊项匹配的链接地址
  // 因此我们可通过查找该链接地址来确定目录索引
  for (int i = 0; i < toc.size(); i++) {
    if (toc[i].href == spine[spineIndex].second) {
      return i;
    }
  }

  Serial.printf("[%lu] [EBP] TOC item not found\n", millis());
  return -1;
}

size_t Epub::getBookSize() const {
  if (spine.empty()) {
    return 0;
  }
  return getCumulativeSpineItemSize(getSpineItemsCount() - 1);
}

// Calculate progress in book
// 计算书籍阅读进度
uint8_t Epub::calculateProgress(const int currentSpineIndex, const float currentSpineRead) {
  size_t bookSize = getBookSize();
  if (bookSize == 0) {
    return 0;
  }
  size_t prevChapterSize = (currentSpineIndex >= 1) ? getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
  size_t curChapterSize = getCumulativeSpineItemSize(currentSpineIndex) - prevChapterSize;
  size_t sectionProgSize = currentSpineRead * curChapterSize;
  return round(static_cast<float>(prevChapterSize + sectionProgSize) / bookSize * 100.0);
}
