#ifndef LANGUAGE_MAPPER_H
#define LANGUAGE_MAPPER_H

// 引入必要的头文件（strcmp依赖）
#include <cstring>

/**
 * @brief 英文设置项/选项 转 中文显示文本（底层逻辑仍用英文，仅显示层转换）
 * @param englishName 待转换的英文字符串（设置项名称/选项值）
 * @return const char* 对应的中文字符串，无匹配则返回原英文
 */
static const char* getChineseName(const char* englishName) {
    // 空指针兜底
    if (englishName == nullptr) {
        return "";
    }

    // --------------- 1. 主分类标题映射 ---------------
    if (strcmp(englishName, "Display") == 0) return "显示设置";
    if (strcmp(englishName, "Reader") == 0) return "阅读设置";
    if (strcmp(englishName, "Controls") == 0) return "按钮设置";
    if (strcmp(englishName, "System") == 0) return "系统设置";

    // --------------- 2. 显示设置项名称映射 ---------------
    if (strcmp(englishName, "Sleep Screen") == 0) return "休眠屏样式";
    if (strcmp(englishName, "Sleep Screen Cover Mode") == 0) return "休眠屏封面模式";
    if (strcmp(englishName, "Sleep Screen Cover Filter") == 0) return "休眠屏封面滤镜";
    if (strcmp(englishName, "Status Bar") == 0) return "状态栏";
    if (strcmp(englishName, "Hide Battery %") == 0) return "隐藏电量百分比";
    if (strcmp(englishName, "Refresh Frequency") == 0) return "刷新频率";
    if (strcmp(englishName, "UI Theme") == 0) return "UI主题";
    if (strcmp(englishName, "Sunlight Fading Fix") == 0) return "阳光褪色修复";

    // --------------- 3. 阅读设置项名称映射 ---------------
    if (strcmp(englishName, "Font Family") == 0) return "字体";
    if (strcmp(englishName, "Set Custom Font Family") == 0) return "设置自定义字体";
    if (strcmp(englishName, "Font Size") == 0) return "字号";
    if (strcmp(englishName, "Line Spacing") == 0) return "行间距";
    if (strcmp(englishName, "firstlineintent") == 0) return "首行缩进"; // 兼容原拼写
    if (strcmp(englishName, "word Spacing") == 0) return "字间距";
    if (strcmp(englishName, "Screen Margin") == 0) return "屏幕边距";
    if (strcmp(englishName, "Paragraph Alignment") == 0) return "段落对齐";
    if (strcmp(englishName, "Book's Embedded Style") == 0) return "书籍嵌入样式";
    if (strcmp(englishName, "Hyphenation") == 0) return "自动连字符";
    if (strcmp(englishName, "Reading Orientation") == 0) return "阅读方向";
    if (strcmp(englishName, "Extra Paragraph Spacing") == 0) return "段落额外间距";
    if (strcmp(englishName, "Text Anti-Aliasing") == 0) return "文字抗锯齿";

    // --------------- 4. 按钮设置项名称映射 ---------------
    if (strcmp(englishName, "Remap Front Buttons") == 0) return "重新映射前置按键";
    if (strcmp(englishName, "Side Button Layout (reader)") == 0) return "侧边按键布局（阅读时）";
    if (strcmp(englishName, "Long-press Chapter Skip") == 0) return "长按跳过章节";
    if (strcmp(englishName, "Short Power Button Click") == 0) return "电源键短按";

    // --------------- 5. 系统设置项名称映射 ---------------
    if (strcmp(englishName, "Time to Sleep") == 0) return "自动休眠时间";
    if (strcmp(englishName, "KOReader Sync") == 0) return "KOReader 同步";
    if (strcmp(englishName, "OPDS Browser") == 0) return "OPDS 浏览器";
    if (strcmp(englishName, "Clear Cache") == 0) return "清理缓存";
    if (strcmp(englishName, "Check for updates") == 0) return "检查更新";

    // --------------- 6. 设置选项值通用映射 ---------------
    // 紧凑/正常/宽松
    if (strcmp(englishName, "Tight") == 0) return "紧凑";
    if (strcmp(englishName, "Normal") == 0) return "正常";
    if (strcmp(englishName, "Wide") == 0) return "宽松";
    // 休眠屏样式
    if (strcmp(englishName, "Dark") == 0) return "深色";
    if (strcmp(englishName, "Light") == 0) return "浅色";
    if (strcmp(englishName, "Custom") == 0) return "自定义";
    if (strcmp(englishName, "Cover") == 0) return "封面";
    if (strcmp(englishName, "None") == 0) return "无";
    if (strcmp(englishName, "Cover + Custom") == 0) return "封面+自定义";
    // 封面模式
    if (strcmp(englishName, "Fit") == 0) return "适配";
    if (strcmp(englishName, "Crop") == 0) return "裁剪";
    // 滤镜
    if (strcmp(englishName, "Contrast") == 0) return "增强对比";
    if (strcmp(englishName, "Inverted") == 0) return "反色";
    // 状态栏选项
    if (strcmp(englishName, "No Progress") == 0) return "无进度";
    if (strcmp(englishName, "Full w/ Percentage") == 0) return "完整（带百分比）";
    if (strcmp(englishName, "Full w/ Book Bar") == 0) return "完整（带书籍进度条）";
    if (strcmp(englishName, "Book Bar Only") == 0) return "仅书籍进度条";
    if (strcmp(englishName, "Full w/ Chapter Bar") == 0) return "完整（带章节进度条）";
    // 电量显示
    if (strcmp(englishName, "Never") == 0) return "从不";
    if (strcmp(englishName, "In Reader") == 0) return "阅读时";
    if (strcmp(englishName, "Always") == 0) return "始终";
    // 刷新频率
    if (strcmp(englishName, "1 page") == 0) return "1页";
    if (strcmp(englishName, "5 pages") == 0) return "5页";
    if (strcmp(englishName, "10 pages") == 0) return "10页";
    if (strcmp(englishName, "15 pages") == 0) return "15页";
    if (strcmp(englishName, "30 pages") == 0) return "30页";
    // 主题
    if (strcmp(englishName, "Classic") == 0) return "经典";
    // 字号
    if (strcmp(englishName, "Small") == 0) return "小";
    if (strcmp(englishName, "Medium") == 0) return "中";
    if (strcmp(englishName, "Large") == 0) return "大";
    if (strcmp(englishName, "X Large") == 0) return "特大";
    // 对齐方式
    if (strcmp(englishName, "Justify") == 0) return "两端对齐";
    if (strcmp(englishName, "Left") == 0) return "左对齐";
    if (strcmp(englishName, "Center") == 0) return "居中";
    if (strcmp(englishName, "Right") == 0) return "右对齐";
    if (strcmp(englishName, "Book's Style") == 0) return "书籍原有样式";
    // 阅读方向
    if (strcmp(englishName, "Portrait") == 0) return "默认方向";
    if (strcmp(englishName, "Landscape CW") == 0) return "按钮在左边";
    if (strcmp(englishName, "Inverted") == 0) return "按钮在上边";
    if (strcmp(englishName, "Landscape CCW") == 0) return "按钮在右边";
    // 按键布局
    if (strcmp(englishName, "Prev, Next") == 0) return "上一页, 下一页";
    if (strcmp(englishName, "Next, Prev") == 0) return "下一页, 上一页";
    // 电源键
    if (strcmp(englishName, "Ignore") == 0) return "忽略";
    if (strcmp(englishName, "Sleep") == 0) return "休眠";
    if (strcmp(englishName, "Page Turn") == 0) return "翻页";
    // 休眠时间
    if (strcmp(englishName, "1 min") == 0) return "1分钟";
    if (strcmp(englishName, "5 min") == 0) return "5分钟";
    if (strcmp(englishName, "10 min") == 0) return "10分钟";
    if (strcmp(englishName, "15 min") == 0) return "15分钟";
    if (strcmp(englishName, "30 min") == 0) return "30分钟";


        // 选项
    if (strcmp(englishName, "« Back") == 0) return "<<返回";
    if (strcmp(englishName, "Toggle") == 0) return "选择";
    if (strcmp(englishName, "Up") == 0) return "向上";
    if (strcmp(englishName, "Down") == 0) return "向下";

    // 无匹配项，返回原英文
    return englishName;
}

#endif // LANGUAGE_MAPPER_H