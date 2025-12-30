from PIL import Image
import sys

def image_to_c_array(img_path, output_var_name="CrossLarge"):
    """
    将图片转换为墨水屏用的C语言字节数组（uint8_t）
    新增：逆时针旋转90° + 黑白互换（二值反色）
    :param img_path: 你的图片路径（比如 "my_pic.jpg"）
    :param output_var_name: 生成的数组变量名（默认CrossLarge）
    """
    # 1. 打开图片并预处理：缩放128×128 → 逆时针旋转90° → 黑白二值化
    try:
        img = Image.open(img_path)
        # 缩放为128×128（保持比例，不足补白）
        img = img.resize((128, 128), Image.Resampling.LANCZOS)
        # 核心1：逆时针旋转90°（PIL的rotate方法：expand=True避免裁剪）
        img = img.rotate(90, expand=True)
        # 重新缩回到128×128（旋转后尺寸可能偏移，强制校准）
        img = img.resize((128, 128), Image.Resampling.LANCZOS)
        # 转为黑白二值图（0=黑，255=白）
        img = img.convert("1")
    except Exception as e:
        print(f"打开/处理图片失败：{e}")
        return

    # 2. 逐行取模 + 核心2：黑白互换（反色）+ 转十六进制字节数组
    pixel_data = list(img.getdata())
    byte_array = []
    for i in range(0, len(pixel_data), 8):
        byte = 0
        for j in range(8):
            if i + j < len(pixel_data):
                # 黑白互换：原白(255)→黑(1)，原黑(0)→白(0)（适配墨水屏显示）
                pixel = 1 if pixel_data[i+j] == 255 else 0  # 反色核心逻辑
                byte |= (pixel << (7 - j))
        byte_array.append(byte)

    # 3. 生成和原代码格式一致的C数组
    c_code = f"#pragma once\n#include <cstdint>\n\n"
    c_code += f"static const uint8_t {output_var_name}[] = {{\n"
    # 每行16个字节，和原代码排版一致
    for i in range(0, len(byte_array), 16):
        line_bytes = byte_array[i:i+16]
        hex_str = ", ".join([f"0x{byte:02X}" for byte in line_bytes])
        c_code += f"    {hex_str},\n"
    c_code += "};\n"

    # 4. 保存到文件
    with open("image_array.h", "w", encoding="utf-8") as f:
        f.write(c_code)
    print(f"转换完成！已完成：128×128缩放 + 逆时针旋转90° + 黑白互换")
    print("数组文件：image_array.h（直接复制替换原代码的CrossLarge即可）")

# ------------------- 运行脚本 -------------------
if __name__ == "__main__":
    # ！！！替换成你的图片
    YOUR_IMAGE_PATH = "xiaomao.jpg"
    image_to_c_array(YOUR_IMAGE_PATH, "CrossLarge")