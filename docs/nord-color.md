# Nord 调色板完整文档

> 北极风格的蓝灰色调色板，专为代码编辑、终端界面和 UI 设计而打造。  
> 由 [Arctic Ice Studio](https://www.arcticicestudio.com) 设计，强调低对比度、护眼与和谐的极光氛围。

Nord 调色板包含 **16 种颜色**，分为四个语义组别（Polar Night、Snow Storm、Frost、Aurora），并编号为 `nord0` 至 `nord15`。  
该配色方案广泛应用于代码编辑器（VS Code、Neovim、Sublime Text）、终端模拟器（Alacritty、iTerm2）、桌面环境及设计工具中。

---

## 官方完整色表

### Polar Night（极夜）—— 深色背景与基础暗色调
用于编辑器背景、边框、深层容器等。

| 编号  | Hex       | 描述              |
| ----- | --------- | ----------------- |
| nord0 | `#2e3440` | 最暗背景          |
| nord1 | `#3b4252` | 次背景 / 行高亮   |
| nord2 | `#434c5e` | 分割线 / 注释     |
| nord3 | `#4c566a` | 注释文字 / 非活动 |

### Snow Storm（雪暴）—— 亮色文本与浅色 UI 基础
用于前景文本、卡片背景、高亮区域。

| 编号  | Hex       | 描述            |
| ----- | --------- | --------------- |
| nord4 | `#d8dee9` | 主要文本 / 浅色 |
| nord5 | `#e5e9f0` | 较亮背景 / 选中 |
| nord6 | `#eceff4` | 最亮背景 / 代码 |

### Frost（霜）—— 核心强调色，蓝灰调
用于函数名、关键字、链接、UI 高亮。

| 编号   | Hex       | 描述         |
| ------ | --------- | ------------ |
| nord7  | `#8fbcbb` | 柔和水蓝     |
| nord8  | `#88c0d0` | 经典 Nord 蓝 |
| nord9  | `#81a1c1` | 深蓝灰       |
| nord10 | `#5e81ac` | 最深蓝       |

### Aurora（极光）—— 彩色强调，用于语法高亮
用于字符串、数字、错误、警告等。

| 编号   | Hex       | 描述         |
| ------ | --------- | ------------ |
| nord11 | `#bf616a` | 红（错误）   |
| nord12 | `#d08770` | 橙（警告）   |
| nord13 | `#ebcb8b` | 黄（关键字） |
| nord14 | `#a3be8c` | 绿（字符串） |
| nord15 | `#b48ead` | 紫（特殊）   |

---

## 常用快捷色板（10 色简化版）

日常使用中最常被引用的 10 种颜色，覆盖背景、文本、强调色及极光色。

| 用途示例          | 编号   | Hex       | 预览描述     |
| ----------------- | ------ | --------- | ------------ |
| 背景（暗）        | nord0  | `#2e3440` | 深灰蓝       |
| 次背景/注释       | nord1  | `#3b4252` | 稍亮灰蓝     |
| 文本（亮）        | nord4  | `#d8dee9` | 浅灰白       |
| 霜蓝1（主要高亮） | nord8  | `#88c0d0` | 经典 Nord 蓝 |
| 霜蓝2             | nord9  | `#81a1c1` | 稍深蓝       |
| 红（字符串/错误） | nord11 | `#bf616a` | 柔和砖红     |
| 橙                | nord12 | `#d08770` | 暖橙         |
| 黄/金             | nord13 | `#ebcb8b` | 柔和金黄     |
| 绿（成功/变量）   | nord14 | `#a3be8c` | 清新绿       |
| 紫（特殊/链接）   | nord15 | `#b48ead` | 淡紫         |

---

## CSS 变量定义（可直接复制）

```css
:root {
  --nord0:  #2e3440;
  --nord1:  #3b4252;
  --nord2:  #434c5e;
  --nord3:  #4c566a;
  --nord4:  #d8dee9;
  --nord5:  #e5e9f0;
  --nord6:  #eceff4;
  --nord7:  #8fbcbb;
  --nord8:  #88c0d0;
  --nord9:  #81a1c1;
  --nord10: #5e81ac;
  --nord11: #bf616a;
  --nord12: #d08770;
  --nord13: #ebcb8b;
  --nord14: #a3be8c;
  --nord15: #b48ead;
}
```

---

## 其他常用格式

### Tailwind CSS 配置
```js
// tailwind.config.js
module.exports = {
  theme: {
    extend: {
      colors: {
        nord0: '#2e3440',
        nord1: '#3b4252',
        nord2: '#434c5e',
        nord3: '#4c566a',
        nord4: '#d8dee9',
        nord5: '#e5e9f0',
        nord6: '#eceff4',
        nord7: '#8fbcbb',
        nord8: '#88c0d0',
        nord9: '#81a1c1',
        nord10: '#5e81ac',
        nord11: '#bf616a',
        nord12: '#d08770',
        nord13: '#ebcb8b',
        nord14: '#a3be8c',
        nord15: '#b48ead',
      },
    },
  },
};
```

### Alacritty (YAML) 格式示例
```yaml
colors:
  primary:
    background: '#2e3440'
    foreground: '#d8dee9'
  normal:
    black:   '#3b4252'
    red:     '#bf616a'
    green:   '#a3be8c'
    yellow:  '#ebcb8b'
    blue:    '#81a1c1'
    magenta: '#b48ead'
    cyan:    '#88c0d0'
    white:   '#e5e9f0'
  bright:
    black:   '#4c566a'
    red:     '#bf616a'
    green:   '#a3be8c'
    yellow:  '#ebcb8b'
    blue:    '#81a1c1'
    magenta: '#b48ead'
    cyan:    '#8fbcbb'
    white:   '#eceff4'
```

---

## 官方资源

- 官方网站：[https://www.nordtheme.com](https://www.nordtheme.com)  
- 官方配色样本下载（支持 Figma、Adobe、Sketch 等）  
- 各平台端口列表（VS Code、Vim、终端、Slack 等）

---

## 使用建议

- **深色主题**：使用 nord0 作为主背景，nord1 作为行高亮或侧边栏，nord4 作为主要文本。  
- **强调**：采用 nord8 或 nord9 作为链接、函数名，保持视觉连贯。  
- **警示**：nord11 用于错误信息，nord13 用于警告，nord14 用于成功状态。  
- **代码高亮**：可搭配 Aurora 组颜色赋予不同语法元素区分度，同时保持整体柔和。

Nord 致力于提供低眼部疲劳的舒适体验，是长时间编码和界面设计的理想选择。