#include "Page.h"

#include <HardwareSerial.h>
#include <Serialization.h>
#include <GfxRenderer.h>
//gd
#include "../../src/CrossPointSettings.h"

// gd:专门绘制水平虚线的函数（仅适配你的场景，参数：渲染器、起始X、Y、结束X、虚线段长/间隔）
void PageLine::drawDashedLine(GfxRenderer& renderer, int x1, int y, int x2, bool isDark) const {
  int startX = std::min(x1, x2);
  int endX = std::max(x1, x2);
  int currentX = startX;

  // 放大参数：段长=12（间隔×4），间隔=3（480px屏幕肉眼清晰）
  const int actualDash = 20;  
  const int actualGap = 10;    

  while (currentX < endX) {
    int segmentEndX = std::min(currentX + actualDash, endX);
    // 关键：先把!isDark改成true，强制画黑色实线段（排除颜色问题）
    renderer.drawLine(currentX, y, segmentEndX, y, true);
    currentX = segmentEndX + actualGap;
  }
}

void PageLine::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  block->render(renderer, fontId, xPos + xOffset, yPos + yOffset);
  //加线
    if (CrossPointSettings::getInstance().extraline){
  // 用屏幕宽度 + 文字高度计算虚线 ----
  int screenWidth = renderer.getScreenWidth(); // 获取屏幕总宽度
  int textHeight = renderer.getLineHeight(fontId); // 文字高度
  //Serial.printf("[%lu] [ERS] 测试能否读取文字高度: %d", millis(),textHeight);

  // 计算虚线坐标
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                  &orientedMarginLeft);
  orientedMarginLeft += SETTINGS.screenMargin_Left;
  orientedMarginRight += SETTINGS.screenMargin_Right;
  int viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  int lineXStart = orientedMarginLeft; // 从屏幕最左侧开始
  int lineXEnd = screenWidth-orientedMarginRight; // 到屏幕最右侧结束
  int lineY = (yPos + yOffset) + textHeight+2; // 在文字下方绘制，+2像素间距

  // 绘制全屏宽度的水平虚线
  drawDashedLine(renderer, lineXStart, lineY, lineXEnd, lineY);
  }
}

bool PageLine::serialize(FsFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);

  // serialize TextBlock pointed to by PageLine
  return block->serialize(file);
}

std::unique_ptr<PageLine> PageLine::deserialize(FsFile& file) {
  int16_t xPos;
  int16_t yPos;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);

  auto tb = TextBlock::deserialize(file);
  return std::unique_ptr<PageLine>(new PageLine(std::move(tb), xPos, yPos));
}

void PageImage::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  // Images don't use fontId or text rendering
  imageBlock->render(renderer, xPos + xOffset, yPos + yOffset);
}

bool PageImage::serialize(FsFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);

  // serialize ImageBlock
  return imageBlock->serialize(file);
}

std::unique_ptr<PageImage> PageImage::deserialize(FsFile& file) {
  int16_t xPos;
  int16_t yPos;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);

  auto ib = ImageBlock::deserialize(file);
  return std::unique_ptr<PageImage>(new PageImage(std::move(ib), xPos, yPos));
}

// void Page::renderPngSleepScreen(GfxRenderer& renderer) const {

//   auto dir = SdMan.open("/bizhi");
//   if (dir && dir.isDirectory()) {
//     std::vector<std::string> files;
//     char name[500];
//     // collect all valid PNG files
//     for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
//       if (file.isDirectory()) {
//         file.close();
//         continue;
//       }
//       file.getName(name, sizeof(name));
//       auto filename = std::string(name);
//       if (filename[0] == '.') {
//         file.close();
//         continue;
//       }

//       // 判断png后缀（对齐txtpng的文件格式判断）
//       std::string ext = filename.substr(filename.length() - 4);
//       for (auto& c : ext) c = tolower(c);
//       if (ext != ".png") {
//         Serial.printf("[%lu] [SLP] Skipping non-.png file name: %s\n", millis(), name);
//         file.close();
//         continue;
//       }
      
//       // 验证PNG文件是否有效（对齐txtpng的文件打开校验）
//       ImageDimensions pngDim;
//       if (!PngToFramebufferConverter::getDimensionsStatic("/bizhi/" + filename, pngDim)) {
//         Serial.printf("[%lu] [SLP] Skipping invalid PNG file: %s\n", millis(), name);
//         file.close();
//         continue;
//       }
//       files.emplace_back(filename);
//       file.close();
//     }
//     const auto numFiles = files.size();
//     if (numFiles > 0) {
//       // 随机选文件（保留原有逻辑）
//       auto randomFileIndex = random(numFiles);
//       while (numFiles > 1 ) {
//         randomFileIndex = random(numFiles);
//       }
      
//       const auto filename = "/bizhi/" + files[randomFileIndex];
//       Serial.printf("[%lu] [SLP] Randomly loading: %s\n", millis(), filename.c_str());
//       delay(100);
      
//       // 配置PNG渲染参数
//       RenderConfig renderConfig;
//       renderConfig.x = 0;                
//       renderConfig.y = 0;                
//       renderConfig.maxWidth = 480;       
//       renderConfig.maxHeight = 800;      
//       renderConfig.useDithering = true;
//       renderConfig.cachePath = "";
      
//       // 解码并渲染PNG
//       PngToFramebufferConverter pngConverter;
//       if (pngConverter.decodeToFramebuffer(filename, renderer, renderConfig)) {
//         // ========== 对齐txtpng的绘制完成后无额外操作，仅刷新 ==========
//         //renderer.displayBuffer(HalDisplay::HALF_REFRESH);
//         //delay(200); // 给屏幕刷新时间
//         dir.close();
//         Serial.printf("[%lu] [SLP] Png draw completed (mode: %d)\n", millis(), renderer.getRenderMode());
//         return;
//       } else {
//         Serial.printf("[%lu] [SLP] Failed to render PNG: %s\n", millis(), filename.c_str());
//       }
//     }
//   }
//   if (dir) dir.close();


//   // 无有效PNG文件，保持底层显示（对齐txtpng的失败处理）
//   Serial.printf("[%lu] [SLP] No valid PNG file, keep default screen\n", millis());
// }



void Page::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) const {
  // =============这里可加阅读背景================
  //renderPngSleepScreen(renderer);
// ======================================
  for (auto& element : elements) {
    element->render(renderer, fontId, xOffset, yOffset);
  }
}

bool Page::serialize(FsFile& file) const {
  const uint16_t count = elements.size();
  serialization::writePod(file, count);

  for (const auto& el : elements) {
    // Use getTag() method to determine type
    serialization::writePod(file, static_cast<uint8_t>(el->getTag()));
    if (!el->serialize(file)) {
      return false;
    }
  }

  return true;
}

std::unique_ptr<Page> Page::deserialize(FsFile& file) {
  auto page = std::unique_ptr<Page>(new Page());

  uint16_t count;
  serialization::readPod(file, count);
 if (count > 1000) {
    Serial.printf("[%lu] [PGE] WARNING: Suspicious element count %d\n", millis(), count);
    return nullptr;
  }
  for (uint16_t i = 0; i < count; i++) {
    uint8_t tag;
    serialization::readPod(file, tag);

    if (tag == TAG_PageLine) {
      auto pl = PageLine::deserialize(file);
      page->elements.push_back(std::move(pl));
    } else if (tag == TAG_PageImage) {
      auto pi = PageImage::deserialize(file);
      page->elements.push_back(std::move(pi));
    } else {
      Serial.printf("[%lu] [PGE] Deserialization failed: Unknown tag %u\n", millis(), tag);
      return nullptr;
    }
  }

  return page;
}
