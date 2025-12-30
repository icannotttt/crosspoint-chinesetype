#pragma once
#include <utility>
#include <vector>
#include <memory>  // 新增：用于std::unique_ptr/std::shared_ptr
#include <SD.h>    // 新增：SD卡操作头文件

// 需确保以下头文件路径与项目实际匹配
#include "blocks/TextBlock.h"
#include "Bitmap.h"           // 新增：BMP解析类头文件
#include "GfxRenderer.h"      // 新增：渲染器头文件
#include "EInkDisplay.h"      // 新增：显示屏相关枚举（如刷新模式）

enum PageElementTag : uint8_t {
  TAG_PageLine = 1,
};

// represents something that has been added to a page
class PageElement {
 public:
  int16_t xPos;
  int16_t yPos;
  explicit PageElement(const int16_t xPos, const int16_t yPos) : xPos(xPos), yPos(yPos) {}
  virtual ~PageElement() = default;
  virtual void render(GfxRenderer& renderer, int fontId) = 0;
  virtual void serialize(std::ostream& os) = 0;
};

// a line from a block element
class PageLine final : public PageElement {
  std::shared_ptr<TextBlock> block;

 public:
  PageLine(std::shared_ptr<TextBlock> block, const int16_t xPos, const int16_t yPos)
      : PageElement(xPos, yPos), block(std::move(block)) {}
  void render(GfxRenderer& renderer, int fontId) override;
  void serialize(std::ostream& os) override;
  static std::unique_ptr<PageLine> deserialize(std::istream& is);
};

class Page {
 private:
  // 新增：BMP背景相关成员（mutable允许const方法修改）
  mutable std::unique_ptr<Bitmap> bgBitmap = nullptr;  // 背景BMP对象（智能指针自动释放）
  mutable bool bgBitmapLoaded = false;                 // 是否已尝试加载BMP
  mutable bool sdChecked = false;                      // 是否已检查SD卡挂载状态

  // 新增：私有方法 - 加载beijing文件夹下的BMP文件
  void loadBeijingBgBitmap() const; 

  // 新增：私有方法 - 渲染BMP背景（无BMP时回退原有小图标）
  void renderBeijingBg(GfxRenderer& renderer) const;

 public:
  // 原有成员保持不变
  std::vector<std::shared_ptr<PageElement>> elements;
  
  // 新增：析构函数（可选，智能指针可自动释放，此处显式声明增强可读性）
  ~Page() = default;

  // 原有方法（render方法逻辑修改在cpp中实现）
  void render(GfxRenderer& renderer, int fontId) const;
  void serialize(std::ostream& os) const;
  static std::unique_ptr<Page> deserialize(std::istream& is);
};