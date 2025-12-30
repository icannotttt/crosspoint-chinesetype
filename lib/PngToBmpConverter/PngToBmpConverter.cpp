#include "PngToBmpConverter.h"
#include "lodepng.h"

#include <cstdio>
#include <cstring>

// 2-bit灰度映射（完全复用）
uint8_t PngToBmpConverter::grayscaleTo2Bit(const uint8_t grayscale) {
    // 0-63→0, 64-127→1, 128-191→2, 192-255→3
    return grayscale >> 6;
}

// BMP数据写入工具函数（完全复用）
inline void PngToBmpConverter::write16(Print& out, const uint16_t value) {
    out.write(value & 0xFF);
    out.write((value >> 8) & 0xFF);
}

inline void PngToBmpConverter::write32(Print& out, const uint32_t value) {
    out.write(value & 0xFF);
    out.write((value >> 8) & 0xFF);
    out.write((value >> 16) & 0xFF);
    out.write((value >> 24) & 0xFF);
}

inline void PngToBmpConverter::write32Signed(Print& out, const int32_t value) {
    out.write(value & 0xFF);
    out.write((value >> 8) & 0xFF);
    out.write((value >> 16) & 0xFF);
    out.write((value >> 24) & 0xFF);
}

// BMP头写入（完全复用）
void PngToBmpConverter::writeBmpHeader(Print& bmpOut, const int width, const int height) {
    const int bytesPerRow = (width * 2 + 31) / 32 * 4;  // 2bit/像素，4字节对齐
    const int imageSize = bytesPerRow * height;
    const uint32_t fileSize = 70 + imageSize;  // 14+40+16 + 图像数据

    // BMP文件头（14字节）
    bmpOut.write('B');
    bmpOut.write('M');
    write32(bmpOut, fileSize);    // 文件总大小
    write32(bmpOut, 0);           // 保留字段
    write32(bmpOut, 70);          // 像素数据偏移（14+40+16）

    // DIB头（BITMAPINFOHEADER，40字节）
    write32(bmpOut, 40);          // 头大小
    write32Signed(bmpOut, width); // 宽度
    write32Signed(bmpOut, -height); // 高度（负数=从上到下）
    write16(bmpOut, 1);           // 色彩平面数
    write16(bmpOut, 2);           // 2bit/像素
    write32(bmpOut, 0);           // 无压缩
    write32(bmpOut, imageSize);   // 图像数据大小
    write32(bmpOut, 2835);        // 水平分辨率（72DPI）
    write32(bmpOut, 2835);        // 垂直分辨率
    write32(bmpOut, 4);           // 使用的颜色数
    write32(bmpOut, 4);           // 重要颜色数

    // 2bit调色板（4色×4字节=16字节）
    uint8_t palette[16] = {
        0x00,0x00,0x00,0x00, // 0:黑
        0x55,0x55,0x55,0x00, // 1:深灰
        0xAA,0xAA,0xAA,0x00, // 2:浅灰
        0xFF,0xFF,0xFF,0x00  // 3:白
    };
    for (uint8_t c : palette) bmpOut.write(c);
}

// 核心：PNG逐行解码转BMP（低内存版）
bool PngToBmpConverter::pngFileToBmpStream(File& pngFile, Print& bmpOut) {
    Serial.printf("[%lu] [PNG] Start converting (low mem mode)\n", millis());

    // 步骤1：读取PNG文件到内存缓冲区（300KB限制）
    size_t pngFileSize = pngFile.size();
    if (pngFileSize > 300 * 1024) { // 超过300KB限制
        Serial.printf("[%lu] [PNG] File too big: %zu > 307200 bytes\n", millis(), pngFileSize);
        return false;
    }

    uint8_t* pngBuffer = (uint8_t*)malloc(pngFileSize);
    if (!pngBuffer) {
        Serial.printf("[%lu] [PNG] Malloc PNG buffer failed\n", millis());
        return false;
    }

    size_t readBytes = pngFile.read(pngBuffer, pngFileSize);
    if (readBytes != pngFileSize) {
        Serial.printf("[%lu] [PNG] Read failed: %zu/%zu\n", millis(), readBytes, pngFileSize);
        free(pngBuffer);
        return false;
    }

    // 步骤2：初始化lodepng逐行解码器
    lodepng::Decoder decoder;
    decoder.info_raw().color = LCT_RGBA;  // 强制解码为RGBA（兼容所有PNG格式）
    decoder.info_raw().bitdepth = 8;      // 8bit/通道
    decoder.set_convert(true);            // 自动转换色彩格式

    // 步骤3：解码PNG头部（获取分辨率）
    lodepng::State state;
    unsigned char* headerBuf = nullptr;
    size_t headerSize = 0;
    LodePNGError decodeErr = lodepng_decode_header(&headerBuf, &headerSize, &state, pngBuffer, pngFileSize);
    if (decodeErr != 0) {
        Serial.printf("[%lu] [PNG] Decode header failed: %s\n", millis(), lodepng_error_text(decodeErr));
        free(pngBuffer);
        return false;
    }

    // 校验分辨率（≤480×800）
    unsigned int width = state.info_png.width;
    unsigned int height = state.info_png.height;
    if (width > MAX_WIDTH || height > MAX_HEIGHT) {
        Serial.printf("[%lu] [PNG] Res too big: %ux%u > %ux%u\n", millis(), width, height, MAX_WIDTH, MAX_HEIGHT);
        free(pngBuffer);
        lodepng_free(headerBuf);
        return false;
    }
    Serial.printf("[%lu] [PNG] Res: %ux%u (mem used: ~2KB)\n", millis(), width, height);

    // 步骤4：写入BMP头
    writeBmpHeader(bmpOut, width, height);

    // 步骤5：初始化逐行解码
    decoder.start(pngBuffer, pngFileSize);
    free(pngBuffer); // 释放PNG文件缓冲区（核心优化：仅保留解码状态）

    // 内存分配：仅缓存单行PNG像素 + 单行BMP数据
    size_t rowPngSize = width * 4;  // RGBA：4字节/像素
    uint8_t* rowPngBuf = (uint8_t*)malloc(rowPngSize);  // 单行PNG缓存（480×4=1920字节）
    int bytesPerRowBmp = (width * 2 + 31) / 32 * 4;     // 单行BMP字节数（含填充）
    uint8_t* rowBmpBuf = (uint8_t*)malloc(bytesPerRowBmp); // 单行BMP缓存（≤240字节）

    if (!rowPngBuf || !rowBmpBuf) {
        Serial.printf("[%lu] [PNG] Malloc row buffer failed\n", millis());
        free(rowPngBuf);
        free(rowBmpBuf);
        return false;
    }

    // 步骤6：逐行解码PNG并转换为BMP
    unsigned int row = 0;
    while (true) {
        // 解码一行PNG像素
        decodeErr = decoder.decode(rowPngBuf, rowPngSize);
        if (decodeErr == LODEPNG_ERROR_NO_ERROR) {
            // 成功解码一行，处理为BMP格式
            memset(rowBmpBuf, 0, bytesPerRowBmp); // 清空BMP行缓冲区

            for (unsigned int x = 0; x < width; x++) {
                // 提取RGBA像素（忽略Alpha）
                uint8_t r = rowPngBuf[x*4];
                uint8_t g = rowPngBuf[x*4+1];
                uint8_t b = rowPngBuf[x*4+2];

                // RGB转灰度（整数近似公式）
                uint8_t gray = (r * 30 + g * 59 + b * 11) / 100;
                uint8_t twoBit = grayscaleTo2Bit(gray);

                // 2bit打包（4像素/字节，高位优先）
                int byteIdx = (x * 2) / 8;
                int bitOffset = 6 - ((x * 2) % 8); // 6→4→2→0
                rowBmpBuf[byteIdx] |= (twoBit << bitOffset);
            }

            // 写入BMP行数据（含4字节对齐填充）
            bmpOut.write(rowBmpBuf, bytesPerRowBmp);
            row++;
            continue;
        }

        // 解码结束/错误处理
        if (decodeErr == LODEPNG_ERROR_DONE) {
            // 所有行解码完成
            if (row != height) {
                Serial.printf("[%lu] [PNG] Incomplete decode: %u/%u rows\n", millis(), row, height);
                free(rowPngBuf);
                free(rowBmpBuf);
                return false;
            }
            break;
        }

        // 其他错误
        Serial.printf("[%lu] [PNG] Decode row %u failed: %s\n", millis(), row, lodepng_error_text(decodeErr));
        free(rowPngBuf);
        free(rowBmpBuf);
        return false;
    }

    // 步骤7：清理资源
    free(rowPngBuf);
    free(rowBmpBuf);
    Serial.printf("[%lu] [PNG] Convert success (total rows: %u)\n", millis(), row);
    return true;
}