#include "Page.h"

#include <HardwareSerial.h>
#include <Serialization.h>
#include <../GfxRenderer/GfxRenderer.h>
//gd
#include "../../src/CrossPointSettings.h"

// gd:专门绘制水平虚线的函数（仅适配你的场景，参数：渲染器、起始X、Y、结束X、虚线段长/间隔）
void PageLine::drawDashedLine(GfxRenderer& renderer, int x1, int y, int x2, bool isDark) const {
  int startX = std::min(x1, x2);
  int endX = std::max(x1, x2);
  int currentX = startX;

  const int actualDash = 20;  
  const int actualGap = 10;    

  while (currentX < endX) {
    int segmentEndX = std::min(currentX + actualDash, endX);
    // 关键：先把!isDark改成true，强制画黑色实线段（排除颜色问题）
    renderer.drawLine(currentX, y, segmentEndX, y, true);
    currentX = segmentEndX + actualGap;
  }
}
//gd
void PageLine::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  // 1. 先渲染文字
  block->render(renderer, fontId, xPos + xOffset, yPos + yOffset);

  //if (CrossPointSettings::getInstance().extraline){
  //// 用屏幕宽度 + 文字高度计算虚线 ----
  //int screenWidth = renderer.getScreenWidth(); // 获取屏幕总宽度（GfxRenderer一般都有这个接口）
  //int textHeight = renderer.getFontAscenderSize(fontId); // 文字高度（确定虚线Y坐标）

  //// 计算虚线坐标
  //int lineXStart = 0; // 从屏幕最左侧开始
  //int lineXEnd = screenWidth; // 到屏幕最右侧结束
  //int lineY = yPos + yOffset + textHeight + 10; // 虚线垂直位置（仅用到textHeight）

  //// 绘制全屏宽度的水平虚线
  //drawDashedLine(renderer, lineXStart, lineY, lineXEnd, lineY);
  //}
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

void Page::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) const {
  for (auto& element : elements) {
    element->render(renderer, fontId, xOffset, yOffset);
  }
}

bool Page::serialize(FsFile& file) const {
  const uint32_t count = elements.size();
  serialization::writePod(file, count);

  for (const auto& el : elements) {
    // Only PageLine exists currently
    serialization::writePod(file, static_cast<uint8_t>(TAG_PageLine));
    if (!el->serialize(file)) {
      return false;
    }
  }

  return true;
}

std::unique_ptr<Page> Page::deserialize(FsFile& file) {
  auto page = std::unique_ptr<Page>(new Page());

  uint32_t count;
  serialization::readPod(file, count);

  for (uint32_t i = 0; i < count; i++) {
    uint8_t tag;
    serialization::readPod(file, tag);

    if (tag == TAG_PageLine) {
      auto pl = PageLine::deserialize(file);
      page->elements.push_back(std::move(pl));
    } else {
      Serial.printf("[%lu] [PGE] Deserialization failed: Unknown tag %u\n", millis(), tag);
      return nullptr;
    }
  }

  return page;
}
