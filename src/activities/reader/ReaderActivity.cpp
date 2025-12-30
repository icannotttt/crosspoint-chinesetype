#include "ReaderActivity.h"

#include <SD.h>

#include "Epub.h"
#include "EpubReaderActivity.h"
#include "FileSelectionActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
//新增读取bmp图需要
#include "InputManager.h"
#include <GfxRenderer.h>

std::unique_ptr<Epub> ReaderActivity::loadEpub(const std::string& path) {
  if (!SD.exists(path.c_str())) {
    Serial.printf("[%lu] [   ] File does not exist: %s\n", millis(), path.c_str());
    return nullptr;
  }

  auto epub = std::unique_ptr<Epub>(new Epub(path, "/.crosspoint"));
  if (epub->load()) {
    return epub;
  }

  Serial.printf("[%lu] [   ] Failed to load epub\n", millis());
  return nullptr;
}
//加bmp
// 对齐EPUB的写法，保留std::string和.c_str()，补充核心加载逻辑
// 改返回值为 bool：true=加载成功并输出bitmap，false=失败
// 新增引用参数 bitmap：用于输出加载好的Bitmap对象
// 修复点1：解决变量名遮蔽 + Bitmap无法赋值问题
bool ReaderActivity::loadBmp(const std::string& path, Bitmap& bitmap) {
  // 1. 检查文件是否存在（原有逻辑不变）
  if (!SD.exists(path.c_str())) {
    Serial.printf("[%lu] [BMP] File does not exist: %s\n", millis(), path.c_str());
    return false;
  }

  // 2. 打开文件（原有逻辑不变）
  auto file = SD.open(path.c_str());
  if (!file) {
    Serial.printf("[%lu] [BMP] Failed to open file: %s\n", millis(), path.c_str());
    file.close(); // 提前关闭文件句柄
    return false;
  }

  // 3. 核心修改：删除局部Bitmap创建（避免遮蔽），直接复用传入的bitmap对象
  // （因Bitmap无赋值运算符，需调用方先创建绑定File的bitmap）
  file.close(); // 示例中隐式关闭，这里显式关闭更安全

  // 4. 解析BMP头（原有逻辑不变，直接解析传入的bitmap）
  if (bitmap.parseHeaders() == BmpReaderError::Ok) {
    Serial.printf("[%lu] [BMP] Load success: %s\n", millis(), path.c_str());
    return true; // 加载成功，bitmap参数已输出有效对象
  }

  // 5. 解析失败（原有逻辑不变）
  Serial.printf("[%lu] [BMP] Failed to parse BMP: %s\n", millis(), path.c_str());
  return false;
}

void ReaderActivity::onSelectEpubFile(const std::string& path) {
  // 步骤1：提取文件后缀（原有逻辑不变）
  std::string ext = "";
  size_t dotPos = path.find_last_of('.');
  if (dotPos != std::string::npos) {
    ext = path.substr(dotPos);
    for (char& c : ext) c = tolower(c);
  }

  // 步骤2：按后缀分支加载文件
  if (ext == ".epub") {
    // 加载EPUB（原有逻辑完全不变）
    auto epub = loadEpub(path);
    if (epub) {
      onGoToEpubReader(std::move(epub));
    } else {
      exitActivity();
      enterNewActivity(new FullScreenMessageActivity(renderer, inputManager, "Failed to load epub", REGULAR,
                                                     EInkDisplay::HALF_REFRESH));
      delay(2000);
      onGoToFileSelection();
    }
  } 
  // 修复点2：改为else if，解决else语法匹配错误
  else if (ext == ".bmp") {
    // 步骤1：核心修改 - 先打开File，用File创建Bitmap（解决无默认构造+语法歧义）
    File bmpFile = SD.open(path.c_str());
    if (!bmpFile) { // 极简错误处理，复用原有逻辑
      exitActivity();
      enterNewActivity(new FullScreenMessageActivity(renderer, inputManager, "Failed to open bmp", REGULAR,
                                                     EInkDisplay::HALF_REFRESH));
      delay(2000);
      onGoToFileSelection();
      return;
    }
    // 直接绑定File创建Bitmap，避免空构造+语法歧义
    Bitmap bitmap(bmpFile);

    // 步骤2：调用loadBmp（传参改为已创建的bitmap，逻辑不变）
    if (loadBmp(path, bitmap)) {
      // 步骤3：使用bitmap（原有逻辑不变）
      renderBitmap(bitmap);
      
      // 等待返回键（原有逻辑不变）
      while (!inputManager.wasPressed(InputManager::BTN_BACK)) {
        inputManager.update();
        delay(10);
      }
      renderer.clearScreen();          // 清空帧缓冲区
      renderer.displayBuffer(EInkDisplay::HALF_REFRESH); // 刷新到物理屏幕（和你之前的刷新方式一致）
      delay(10); // 可选：给屏幕刷新留100ms时间（嵌入式屏刷新慢）
      onGoToFileSelection();
    } else {
      // BMP加载失败（原有逻辑不变）
      exitActivity();
      enterNewActivity(new FullScreenMessageActivity(renderer, inputManager, "Failed to load bmp", REGULAR,
                                                     EInkDisplay::HALF_REFRESH));
      delay(2000);
      onGoToFileSelection();
    }
    bmpFile.close(); // 补充关闭文件句柄（最小改动）
  }
  else {
    // 未知文件类型（原有逻辑不变）
    exitActivity();
    enterNewActivity(new FullScreenMessageActivity(renderer, inputManager, "Unsupported file", REGULAR,
                                                   EInkDisplay::HALF_REFRESH));
    delay(2000);
    onGoToFileSelection();
  }
}

void ReaderActivity::renderBitmap(const Bitmap& bitmap) const {
  int x, y;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // ========== 第一步：计算位图的居中/缩放位置 ==========
  // 若位图尺寸超过屏幕，按比例缩放并居中；否则直接居中
  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    const float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    if (ratio > screenRatio) {
      // 图片宽高比大于屏幕 → 垂直居中
      x = 0;
      y = (pageHeight - pageWidth / ratio) / 2;
    } else {
      // 图片宽高比小于屏幕 → 水平居中
      x = (pageWidth - pageHeight * ratio) / 2;
      y = 0;
    }
  } else {
    // 图片尺寸小于屏幕 → 整体居中
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  // ========== 第二步：绘制黑白（BW）基础层 ==========
  renderer.clearScreen(); // 清空屏幕缓冲区（默认BW模式）
  // 绘制位图的黑白层到屏幕缓冲区
  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight);
  // 刷新黑白缓冲区到显示屏（快速半刷新）
  renderer.displayBuffer(EInkDisplay::HALF_REFRESH);

  // ========== 第三步：灰度显示核心逻辑（关键） ==========
  if (bitmap.hasGreyscale()) { // 判断位图是否包含灰度数据
    // 3.1 绘制灰度LSB层（最低有效位，对应低阶灰度）
    bitmap.rewindToData(); // 重置位图数据读取指针，从头读取灰度数据
    renderer.clearScreen(0x00); // 清空灰度缓冲区
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB); // 切换到灰度LSB渲染模式
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight); // 绘制LSB灰度层
    renderer.copyGrayscaleLsbBuffers(); // 将LSB灰度数据拷贝到显存

    // 3.2 绘制灰度MSB层（最高有效位，对应高阶灰度）
    bitmap.rewindToData(); // 再次重置指针，重新读取灰度数据
    renderer.clearScreen(0x00); // 清空缓冲区
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB); // 切换到灰度MSB渲染模式
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight); // 绘制MSB灰度层
    renderer.copyGrayscaleMsbBuffers(); // 将MSB灰度数据拷贝到显存

    // 3.3 刷新灰度缓冲区到显示屏（完成灰度显示）
    renderer.displayGrayBuffer();
    // 3.4 恢复为黑白渲染模式（避免影响后续绘制）
    renderer.setRenderMode(GfxRenderer::BW);
  }
}

void ReaderActivity::onGoToFileSelection() {
  exitActivity();
  enterNewActivity(new FileSelectionActivity(
      renderer, inputManager, [this](const std::string& path) { onSelectEpubFile(path); }, onGoBack));
}

void ReaderActivity::onGoToEpubReader(std::unique_ptr<Epub> epub) {
  exitActivity();
  enterNewActivity(new EpubReaderActivity(renderer, inputManager, std::move(epub), [this] { onGoToFileSelection(); }));
}

void ReaderActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  if (initialEpubPath.empty()) {
    onGoToFileSelection();
    return;
  }

  auto epub = loadEpub(initialEpubPath);
  if (!epub) {
    onGoBack();
    return;
  }

  onGoToEpubReader(std::move(epub));
}
