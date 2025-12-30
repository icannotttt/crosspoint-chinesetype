# extract_epub_chars.py
import zipfile
import re
import os

def extract_epub_text(epub_path):
    """提取EPUB中的所有文本内容"""
    text = ""
    # 解压EPUB（本质是ZIP包）
    with zipfile.ZipFile(epub_path, 'r') as zf:
        # 遍历所有文件，提取XHTML/HTML内容
        for file_info in zf.infolist():
            if file_info.filename.endswith(('.xhtml', '.html', '.htm')):
                with zf.open(file_info) as f:
                    # 读取并解码
                    try:
                        content = f.read().decode('utf-8')
                        # 移除HTML标签，只保留文本
                        content = re.sub(r'<[^>]+>', '', content)
                        # 移除多余空白符
                        content = re.sub(r'\s+', '', content)
                        text += content
                    except:
                        continue
    return text

def get_unique_chars(text):
    """提取文本中的所有唯一字符"""
    # 去重并排序
    unique_chars = sorted(list(set(text)))
    # 过滤不可见字符（保留文字+标点）
    valid_chars = []
    for c in unique_chars:
        # 保留：中文、ASCII、中文标点、常用符号
        if (0x4E00 <= ord(c) <= 0x9FFF) or \
           (0x0020 <= ord(c) <= 0x007F) or \
           (0x3000 <= ord(c) <= 0x303F) or \
           (0xFF00 <= ord(c) <= 0xFFEF):
            valid_chars.append(c)
    return valid_chars

if __name__ == "__main__":
    # 替换为你的EPUB文件路径
    EPUB_PATH = "test.epub"
    # 提取文本
    print("正在提取EPUB文本...")
    full_text = extract_epub_text(EPUB_PATH)
    # 提取唯一字符
    print("正在提取唯一字符...")
    unique_chars = get_unique_chars(full_text)
    # 保存到文件
    with open("book_chars.txt", 'w', encoding='utf-8') as f:
        f.write(''.join(unique_chars))
    # 输出统计
    print(f"✅ 提取完成！")
    print(f"📊 统计：")
    print(f"   - 总字符数（去重）：{len(unique_chars)} 个")
    print(f"   - 字符列表已保存到：book_chars.txt")
    # 预览前50个字符
    print(f"🔍 预览字符：{''.join(unique_chars[:50])}...")