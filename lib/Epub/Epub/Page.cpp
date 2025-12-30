#include "Page.h"

#include <HardwareSerial.h>
#include <Serialization.h>
#include <GfxRenderer.h> //加阅读背景
#include <SD.h> //加阅读背景
#include <../../src/images/beijing.h> //加阅读背景

namespace {
constexpr uint8_t PAGE_FILE_VERSION = 3;
}

void PageLine::render(GfxRenderer& renderer, const int fontId) { block->render(renderer, fontId, xPos, yPos); }

void PageLine::serialize(std::ostream& os) {
  serialization::writePod(os, xPos);
  serialization::writePod(os, yPos);

  // serialize TextBlock pointed to by PageLine
  // serialize TextBlock pointed to by PageLine
  block->serialize(os);
}

std::unique_ptr<PageLine> PageLine::deserialize(std::istream& is) {
  int16_t xPos;
  int16_t yPos;
  serialization::readPod(is, xPos);
  serialization::readPod(is, yPos);

  auto tb = TextBlock::deserialize(is);
  return std::unique_ptr<PageLine>(new PageLine(std::move(tb), xPos, yPos));
}

 //加阅读背景
// 先确保Page.h中方法声明：void loadBeijingBgBitmap() const;
void Page::loadBeijingBgBitmap() const {
    // 提前释放旧的Bitmap（关键：释放内存）
    if (bgBitmap) {
        bgBitmap.reset();
    }

    // 1. 打开文件夹（仅用基础类型，无容器）
    File dir = SD.open("/beijing");
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return;
    }

    // 2. 第一步遍历：仅统计有效BMP数量（不存储文件名）
    int validFileCount = 0;
    while (true) {
        File file = dir.openNextFile();
        if (!file) break;

        // 跳过目录/隐藏文件
        if (file.isDirectory() || file.name()[0] == '.') {
            file.close();
            continue;
        }

        // 极简后缀判断（仅检查最后4个字符，无字符串操作）
        const char* name = file.name();
        int len = strlen(name);
        if (len < 4) {
            file.close();
            continue;
        }
        // 直接比较最后4个字符（小写，兼容.BMP/.bmp）
        char ext[5] = {0};
        strncpy(ext, name + len - 4, 4);
        for (int i=0; i<4; i++) ext[i] = tolower(ext[i]);
        if (strcmp(ext, ".bmp") != 0) {
            file.close();
            continue;
        }

        validFileCount++; // 仅统计数量，不存储任何数据
        file.close();
    }
    dir.close(); // 先关闭目录，释放资源

    // 无有效文件，直接返回
    if (validFileCount == 0) {
        return;
    }

    // 3. 生成随机索引（仅用int，无内存占用）
    randomSeed(millis());
    int targetIndex = random(validFileCount);
    int currentIndex = 0;

    // 4. 第二步遍历：找到目标索引的文件并加载
    dir = SD.open("/beijing");
    while (true) {
        File file = dir.openNextFile();
        if (!file) break;

        // 重复第一步的过滤逻辑（极简版）
        if (file.isDirectory() || file.name()[0] == '.') {
            file.close();
            continue;
        }
        const char* name = file.name();
        int len = strlen(name);
        if (len < 4) {
            file.close();
            continue;
        }
        char ext[5] = {0};
        strncpy(ext, name + len - 4, 4);
        for (int i=0; i<4; i++) ext[i] = tolower(ext[i]);
        if (strcmp(ext, ".bmp") != 0) {
            file.close();
            continue;
        }

        // 找到目标文件
        if (currentIndex == targetIndex) {
            // 直接加载，仅创建一个Bitmap对象（核心：内存最小化）
            bgBitmap.reset(new Bitmap(file));
            // 解析失败则释放
            if (bgBitmap->parseHeaders() != BmpReaderError::Ok) {
                bgBitmap.reset();
            }
            file.close();
            dir.close();
            return;
        }

        currentIndex++;
        file.close();
    }

    dir.close(); // 兜底关闭
}
// 2. 渲染BMP背景图（适配Page的绘制区域）
void Page::renderBeijingBg(GfxRenderer& renderer) const {
    // 加载图片（极致内存版）
    loadBeijingBgBitmap();

    // 无图片则绘制小图标（原有逻辑）
    if (!bgBitmap) {
        //renderer.drawImage(beijing, 0 ,740, 40, 40);
        //renderer.drawImage(beijing, 440 ,740, 40, 40);
        return;
    }

    // 极简版绘制逻辑（去掉冗余变量，直接计算）
    int x, y;
    int w = renderer.getScreenWidth();
    int h = renderer.getScreenHeight();
    int bmpW = bgBitmap->getWidth();
    int bmpH = bgBitmap->getHeight();

    // 仅保留核心缩放逻辑，去掉浮点数临时变量（可选，进一步减内存）
    if (bmpW > w || bmpH > h) {
        float ratio = (float)bmpW / bmpH;
        float sRatio = (float)w / h;
        x = (ratio > sRatio) ? 0 : (w - h * ratio) / 2;
        y = (ratio > sRatio) ? (h - w / ratio) / 2 : 0;
    } else {
        x = (w - bmpW) / 2;
        y = (h - bmpH) / 2;
    }

    // 仅绘制黑白层（灰度层占用双倍内存，若不需要可去掉）
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.drawBitmap(*bgBitmap, x, y, w, h);
    renderer.displayBuffer(EInkDisplay::HALF_REFRESH);

    // 2. 绘制灰度层（如果有灰度）
    //if (bgBitmap->hasGreyscale()) {
        // 灰度LSB层
       // bgBitmap->rewindToData();
       // renderer.clearScreen(0x00);
       // renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
        //renderer.drawBitmap(*bgBitmap, x, y, pageWidth, pageHeight);
        //renderer.copyGrayscaleLsbBuffers();

        // 灰度MSB层
       // bgBitmap->rewindToData();
       // renderer.clearScreen(0x00);
       // renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
       // renderer.drawBitmap(*bgBitmap, x, y, pageWidth, pageHeight);
       // renderer.copyGrayscaleMsbBuffers();

        // 刷新灰度缓冲区
        //renderer.displayGrayBuffer();
        //renderer.setRenderMode(GfxRenderer::BW); // 恢复黑白模式
    //}
}
void Page::render(GfxRenderer& renderer, const int fontId) const {
    // ========== 第一步：绘制背景图（BMP或原有小图标） ==========
     renderer.drawImage(beijing, 0 ,740, 40, 40);
     renderer.drawImage(beijing, 440 ,740, 40, 40);

    // ========== 第二步：绘制页面元素（保证元素在背景上方） ==========
    for (auto& element : elements) {
        element->render(renderer, fontId);
    }
}

//把page内容输入到ostream里
void Page::serialize(std::ostream& os) const {
  // 1. 写入Page文件的版本号
  serialization::writePod(os, PAGE_FILE_VERSION);

  // 2. 写入Page中元素的数量（uint32_t类型，固定4字节）
  const uint32_t count = elements.size();
  serialization::writePod(os, count);

  // 3. 遍历所有元素，逐个序列化
  for (const auto& el : elements) {
    // 3.1 写入元素类型标记（当前只有PageLine类型）
    serialization::writePod(os, static_cast<uint8_t>(TAG_PageLine));
    // 3.2 调用元素自身的序列化方法，写入元素具体数据
    el->serialize(os);
  }
}

// 静态方法：从输入流反序列化，返回智能指针管理的Page对象（失败返回nullptr）
std::unique_ptr<Page> Page::deserialize(std::istream& is) {
  // 1. 定义版本号变量，存储从流中读取的版本
  uint8_t version;
  // 2. 从输入流中读取1字节的版本号（二进制方式，对应序列化时的writePod）
  serialization::readPod(is, version);
  // 3. 版本号校验：如果和当前支持的版本不一致，反序列化失败
  if (version != PAGE_FILE_VERSION) {
    // 3.1 打印错误日志：millis()是嵌入式系统的毫秒时间戳，输出当前时间、错误原因、未知版本号
    Serial.printf("[%lu] [PGE] Deserialization failed: Unknown version %u\n", millis(), version);
    // 3.2 返回空指针，表示反序列化失败
    return nullptr;
  }

  // 4. 创建一个空的Page对象，用unique_ptr管理（避免内存泄漏，符合RAII）
  auto page = std::unique_ptr<Page>(new Page());

  // 5. 定义元素数量变量，存储从流中读取的元素个数
  uint32_t count;
  // 6. 从输入流读取4字节的元素数量（对应序列化时写入的count）
  serialization::readPod(is, count);

  // 7. 循环读取count个元素（和序列化时的元素数量对应）
  for (uint32_t i = 0; i < count; i++) {
    // 7.1 定义标签变量，存储元素类型标记（1字节）
    uint8_t tag;
    // 7.2 从输入流读取元素类型标记（对应序列化时写入的TAG_PageLine）
    serialization::readPod(is, tag);

    // 7.3 校验元素类型标记：仅支持TAG_PageLine（当前版本）
    if (tag == TAG_PageLine) {
      // 7.3.1 调用PageLine的反序列化方法，读取一行数据并返回PageLine对象（智能指针）
      auto pl = PageLine::deserialize(is);
      // 7.3.2 将PageLine对象移动到Page的elements容器中（避免拷贝，高效）
      page->elements.push_back(std::move(pl));
    } else {
      // 7.4 未知标签：打印错误日志，返回空指针（反序列化失败）
      Serial.printf("[%lu] [PGE] Deserialization failed: Unknown tag %u\n", millis(), tag);
      return nullptr;
    }
  }

  // 8. 所有数据读取完成，返回构建好的Page对象（unique_ptr自动管理内存）
  return page;
}