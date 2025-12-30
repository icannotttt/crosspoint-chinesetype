#ifndef PNG_TO_BMP_CONVERTER_H
#define PNG_TO_BMP_CONVERTER_H

#include <Arduino.h>
#include <FS.h>

class PngToBmpConverter {
public:
    // 核心转换函数：PNG文件转2-bit BMP流
    bool pngFileToBmpStream(File& pngFile, Print& bmpOut);

private:
    // 2-bit灰度映射（复用原逻辑）
    static uint8_t grayscaleTo2Bit(const uint8_t grayscale);
    
    // BMP头写入（复用原逻辑）
    static void writeBmpHeader(Print& bmpOut, const int width, const int height);
    
    // BMP数据写入工具函数
    static inline void write16(Print& out, const uint16_t value);
    static inline void write32(Print& out, const uint32_t value);
    static inline void write32Signed(Print& out, const int32_t value);

    // 最大分辨率限制（480*800）
    static const int MAX_WIDTH = 480;
    static const int MAX_HEIGHT = 800;
};

#endif // PNG_TO_BMP_CONVERTER_H