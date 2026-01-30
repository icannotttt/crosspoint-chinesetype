# 版本信息

基于 **crosspoint 0.14.0** 版本修改而来，感谢以下开源项目及其贡献者：

- 主项目：[crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader)
- 自选字体功能参考：[ruby-builds/crosspoint-reader (custom-fonts分支)](https://github.com/ruby-builds/crosspoint-reader/tree/feature/custom-fonts)

---

# 当前进度

- **EPUB**：基本完成中文化适配
- **XTC**：实现动态管理功能
- **TXT**：目录解析逻辑如下：
  - 优先按“第n章”格式提取目录
  - 若无匹配目录或提取失败，则自动启用按字节分卷的兜底方案
  - *注：这部分是在原项目更新前独立编写的*

---

# 字体说明

## 内置字体
项目最初为满足个人阅读需求，内置字体选用 **汉仪空山楷**。  
因存储空间有限，当前字符覆盖范围如下（欢迎建议扩展）：

```python
intervals = [
    # 基础字符
    (0x0000, 0x007F),  # ASCII（英文、数字、基础标点）
    (0x0080, 0x00FF),  # 拉丁扩展（含 0x00B7「·」）
    # (0x0100, 0x017F),  # 拉丁扩展-A（可选）
    # (0x0180, 0x024F),  # 拉丁扩展-B（可选）
    
    # 东亚文字
    (0x3000, 0x303F),  # 中文标点（全角符号、括号等）
    ## (0x3040, 0x309F),  # 日语平假名（已注释）
    ## (0x30A0, 0x30FF),  # 日语片假名（已注释）
    (0x4E00, 0x9FFF),  # 统一汉字
    (0xFF00, 0xFFEF),  # 全角ASCII及标点
    
    # 扩展符号
    (0x2000, 0x206F),  # 通用标点与排版符号
]
```

## 自选字体

用户可以通过lib\EpdFont\scripts下的fontconvert.py进行字体转换，转换完后放入fonts/文件夹中，具体可见：[ruby-builds/crosspoint-reader (custom-fonts分支)](https://github.com/ruby-builds/crosspoint-reader/tree/feature/custom-fonts)

但是自选字体会耗费一定的内存，导致页面存储不了，所以只能半刷--参考官方2bit-xtc效果

## 刷机指导


1. 需要一根typec线连接你的电脑和x4
2. 下载release页面下的bin文件
3. 打开 https://xteink.dve.al/ 页面，在OTA fast flash controls部分选择下载好的bin文件，点击flash firmware from file

首次刷机建议做好保存，在full flash controls界面下，选择save full flash，备份一下你的官方固件




# 主页
主页自动生成封面，所以返回主页的时候时间会比较长，属于正常现象

# bugs
其他：退出阅读界面的时候会有死机现象，长按电源键重启即可

其他：目录最好不要太长，现在识别目录字符串有限，建议绝对路径不要太长

其他：书名最好不要包含空格及特殊符号，会有识别不到的风险

Epub: epub阅读现仅支持打开前200章节，这部分预计半月内补全。我现在有一个想法，但是这个想法目前死机频率有点高，所以先回退200章版本

Epub: epub暂不支持长段落，会导致机器卡死

Epub:大文件打开有问题，解压可能需要很久，暂不建议使用

Epub: 打开epub最好等一会儿，这部分写的不如官方固件完整，耗时较长，而且第一次打不开不要紧，尝试3~5次，写入缓存成功后可以打开hhh

XTC:xtc仅支持2bit版本，请注意，1bit版本现在测试有点问题，

TXT：txt先仅支持阅读一段时间后向前翻页，点击打开不支持立即翻页，可以按确认键选择章节

# 问题解决
万事先重启

重启解决不了的，拔出sd卡，删除根目录下的.crosspoint文件夹

