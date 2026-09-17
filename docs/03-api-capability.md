# 03 · 能力检测层 API

能力检测层的职责是**回答"当前终端支持什么"**，并据此驱动渲染层与输入层的降级。

> 边界：库**只识别能力**，不选择、不启动、不推荐终端模拟器；也不处理字体。
> 终端与字体是用户的选择，库对已连接的终端做"能力体检"。

---

## 1. 能力位集

```c
typedef enum tc_cap {
    /* 颜色 */
    TC_CAP_COLOR_16       = 1u << 0,
    TC_CAP_COLOR_256      = 1u << 1,
    TC_CAP_TRUECOLOR      = 1u << 2,

    /* 屏幕与光标 */
    TC_CAP_ALT_SCREEN     = 1u << 3,
    TC_CAP_CURSOR_SHAPE   = 1u << 4,
    TC_CAP_CURSOR_HIDE    = 1u << 5,
    TC_CAP_TITLE          = 1u << 6,
    TC_CAP_SYNC_UPDATE    = 1u << 7,   /* 模式 2026 */

    /* 样式 */
    TC_CAP_STYLE_BOLD     = 1u << 8,
    TC_CAP_STYLE_DIM      = 1u << 9,
    TC_CAP_STYLE_ITALIC   = 1u << 10,
    TC_CAP_STYLE_UNDERLINE= 1u << 11,
    TC_CAP_STYLE_BLINK    = 1u << 12,
    TC_CAP_STYLE_REVERSE  = 1u << 13,
    TC_CAP_STYLE_STRIKE   = 1u << 14,
    TC_CAP_STYLE_UNDERCURL= 1u << 15,

    /* 输入 */
    TC_CAP_MOUSE_X10      = 1u << 16,  /* 1000 */
    TC_CAP_MOUSE_DRAG     = 1u << 17,  /* 1002 */
    TC_CAP_MOUSE_MOVE     = 1u << 18,  /* 1003 */
    TC_CAP_MOUSE_SGR      = 1u << 19,  /* 1006，v1 主路径 */
    TC_CAP_FOCUS_EVENTS   = 1u << 20,  /* 1004 */
    TC_CAP_BRACKETED_PASTE= 1u << 21,  /* 2004 */
    TC_CAP_KITTY_KEYBOARD = 1u << 22,  /* 渐进式键盘协议 */
    TC_CAP_MODIFY_OTHER   = 1u << 23,  /* modifyOtherKeys（xterm） */

    /* 文本 */
    TC_CAP_UNICODE        = 1u << 24,  /* 终端能正确显示宽字符，决定默认文本模式 */
    TC_CAP_PIXEL_SIZE     = 1u << 25,  /* 可查询像素尺寸 */

    TC_CAP_NONE           = 0
} tc_cap;

typedef struct tc_caps {
    uint64_t flags;             /* tc_cap 位或 */
    tc_color_level color;       /* 见 §3 */
    int32_t  max_colors;        /* 16 / 256 / 0（真彩时为 0，表示不限制） */
    tc_mouse_mode mouse;        /* 可用鼠标模式 */
    uint32_t reserved[2];
} tc_caps;
```

查询 API：

```c
tc_status tc_term_get_caps(const tc_term_t* t, tc_caps* out);
bool      tc_caps_has(const tc_caps* c, tc_cap flag);
tc_status tc_term_get_color_level(const tc_term_t* t, tc_color_level* out);
```

---

## 2. 检测优先级

```
1. 用户确认结论（探测 / 缓存 profile）与显式覆盖（API / 环境变量 TERMCORE_*）—— 最高优先级
        │
2. 环境推断（TERM / COLORTERM / NO_COLOR / CI / TERM_PROGRAM …）
        │
3. DA 查询（若 query_capabilities 且是 TTY 且有超时预算）
        │
4. terminfo / termcap 解析（按 TERM 名称）
        │
5. 内置 TERM 能力库（常见 TERM 的固化结论）
        │
6. 保守默认（最低能力集）
```

原则：

- **越靠前越可信**，一旦得到结论即停止（不做"投票"）
- **任何一步失败都不得阻塞启动**：DA 查询有超时（默认 200ms），超时即跳过
- **宁可低估**：不确定时按不支持处理（低估只影响观感，高估会产生乱码）
- headless / 非 TTY：`query_capabilities` 强制失效，直接走第 6 档

---

## 3. 颜色能力等级

```c
typedef enum tc_color_level {
    TC_COLOR_LEVEL_MONO = 0,   /* 无颜色：忽略所有颜色 */
    TC_COLOR_LEVEL_16,         /* ANSI 16 色（含 bright） */
    TC_COLOR_LEVEL_256,        /* xterm-256color 索引色 */
    TC_COLOR_LEVEL_TRUECOLOR   /* 24 位真彩 */
} tc_color_level;
```

判定顺序：

| 条件 | 结论 |
| --- | --- |
| `NO_COLOR` 非空（且非 "0"） | `MONO` |
| `TERMCORE_COLOR_LEVEL` 显式设置 | 取该值 |
| `COLORTERM` 含 `truecolor` / `24bit` | `TRUECOLOR` |
| `TERM` 含 `-truecolor` / `24bit` / `direct` | `TRUECOLOR` |
| `TERM` 含 `256color` | `256` |
| `TERM` 为 `xterm` / `screen` / `tmux-*` 等且 DA 或 terminfo 表明支持 | `256` 或 `TRUECOLOR`（按 terminfo `RGB` / `setrgbf`） |
| 未知 | `16`（保守默认） |

降级应用（渲染层执行，此处定义规则）：

| 请求 | TRUECOLOR | 256 | 16 | MONO |
| --- | --- | --- | --- | --- |
| RGB 真彩 | `ESC[38;2;r;g;b m` | 映射到最近 256 索引 | 映射到最近 16 色 | 忽略颜色 |
| 256 索引 | 直接输出 | 直接输出 | 映射到最近 16 色 | 忽略 |
| 16 ANSI | 直接输出 | 直接输出 | 直接输出 | 忽略 |
| 默认色 | `ESC[39/49 m` | 同 | 同 | 忽略 |

映射算法：真彩 → 256 用标准 6×6×6 立方 + 24 级灰阶的欧氏距离最近；→ 16 用亮度 + 色相近似。
算法是**纯函数**，便于单测与快照比对。

---

## 4. 环境变量

| 变量 | 作用 | 备注 |
| --- | --- | --- |
| `TERM` | terminfo / termcap 与内置库的主索引 | `TERM=dumb` → 最低能力集 |
| `COLORTERM` | `truecolor` / `24bit` 判定真彩 | 优先于 `TERM` 的位数推断 |
| `NO_COLOR` | 非空即视为无颜色 | 遵循 no-color.org 约定 |
| `TERMCORE_COLOR_LEVEL` | 显式指定 `mono` / `16` / `256` / `truecolor` | 最高优先级，测试与用户强制用 |
| `TERMCORE_CAPS` | 显式能力位集（十六进制） | 测试降级路径用 |
| `TERMCORE_NO_QUERY` | 非空则禁用 DA 查询 | CI 用 |
| `CI` | 非空时默认禁用 DA 查询（除非显式开启） | 避免 CI 中卡住 |
| `TERM_PROGRAM` | `vscode` / `iTerm.app` / `Apple_Terminal` 等 | 纳入内置库判定 |

> 库不读取与终端**选择**相关的配置（如"应该用哪个终端"），只读"当前终端是什么"。

---

## 5. terminfo / termcap 解析

范围限定：只解析库真正用到的少数能力，不做完整 terminfo 实现。

| 需要的 terminfo 能力 | 用途 |
| --- | --- |
| `colors` | 最大颜色数 |
| `RGB` / `setrgbf` / `setrgbb` | 真彩支持 |
| `smcup` / `rmcup` | alternate screen |
| `civis` / `cnorm` | 光标显隐 |
| `Ss` / `Se`（或 `Cs`） | 光标形状 |
| `kmous` | 鼠标序列前缀 |
| `XM` | SGR 鼠标（1006）支持 |

解析要点：

- 支持 `terminfo` 文件格式（magic `0x011A` 的旧格式为主，新格式 `0x021E` 尽力支持）
- 搜索路径：`TERMINFO` → `$HOME/.terminfo` → `TERMINFO_DIRS` → `/usr/share/terminfo` 等（按平台）
- termcap 作为后备（解析 `:co#80:...` 风格）
- 解析失败不报错，只降级到下一档
- 无 terminfo 的环境跳过该步，走内置库 + 环境变量 + DA

### 内置 TERM 库

为常见 TERM 固化结论，避免依赖文件系统：

`xterm-256color`、`xterm`、`screen-256color`、`screen`、`tmux-256color`、`tmux`、`linux`、`vt100`、`dumb`、`ansi`、`cygwin`、`ms-terminal`、`wezterm`、`alacritty`、`kitty`、`foot`、`konsole`、`gnome`、`vscode`。

每条记录给出：颜色等级、鼠标模式、altscreen、样式、Unicode 支持等。

---

## 6. DA 查询

```c
typedef struct tc_da_info {
    bool    got_primary;      /* 收到 ESC [ ? ... c */
    bool    got_secondary;    /* 收到 ESC [ > ... c */
    int32_t primary_id;       /* 如 1 / 6 / 62 / 64 */
    int32_t version;          /* 次 DA 中的终端版本（部分终端提供） */
    uint32_t reserved[2];
} tc_da_info;
```

流程：

1. 仅在 `query_capabilities == true`、是 TTY、非 CI、非 headless 时执行
2. 写 `ESC [ c` 与 `ESC [ > 0 c`，等待响应直到 `capability_timeout_ms`
3. 期间的输入字节**同时**喂给解析器（避免吞掉真实用户按键）
4. 超时未响应 → 标记"查询失败"，降级到下一档，返回 `TC_OK`（不是错误）
5. 解析出已知终端 ID → 更新能力（如 `62` → xterm；`1` → VT100；`77` → kitty 等）
6. 恢复：查询前已开启 raw，查询后会消费掉响应，不留残余字节在事件队列

禁用方式：`opt.query_capabilities = false`、`TERMCORE_NO_QUERY`、CI、headless 任一成立即跳过。

---

## 7. 输入协议启用策略

`tc_term_enter` 按能力决定是否写启用序列（能力不足则**不写**，而不是写了无效序列）：

| 选项 | 启用序列 | 依赖能力 |
| --- | --- | --- |
| `mouse_mode = CLICK` | `ESC[?1000h` + `ESC[?1006h` | `TC_CAP_MOUSE_X10`；有 `TC_CAP_MOUSE_SGR` 才写 1006 |
| `mouse_mode = DRAG` | 上 + `ESC[?1002h` | `TC_CAP_MOUSE_DRAG` |
| `mouse_mode = MOTION` | 上 + `ESC[?1003h` | `TC_CAP_MOUSE_MOVE` |
| `focus_events` | `ESC[?1004h` | `TC_CAP_FOCUS_EVENTS` |
| `bracketed_paste` | `ESC[?2004h` | `TC_CAP_BRACKETED_PASTE` |
| `kitty_keyboard` | `ESC[>1u` 渐进启用 | `TC_CAP_KITTY_KEYBOARD`；失败降级（见 `04`） |

降级链（以鼠标为例）：SGR(1006) → X10(1000) → 不支持（`opt.mouse` 无效但**不报错**，应用需按 `caps` 自行决定交互）。

---

## 8. 降级矩阵（总表）

| 能力缺失 | 渲染 / 输入行为 |
| --- | --- |
| 无真彩 | RGB → 256 索引 |
| 无 256 | 索引 → 16 色 |
| 无颜色（`NO_COLOR`） | 丢弃所有颜色，保留样式 |
| 无 italic | 丢弃该样式位（不产生 `ESC[3m`） |
| 无 undercurl | 退化为 underline |
| 无 altscreen | 不写 smcup/rmcup，正常绘制 |
| 无光标形状 | 只做显隐 |
| 无同步更新 | 不包裹 2026 模式序列 |
| 无 SGR 鼠标 | 退回 X10（坐标上限 223）；再无则不支持鼠标 |
| 无 Kitty 键盘 | 传统序列解析（无法区分部分修饰键组合、无释放事件） |
| 无 Unicode 支持 | 默认文本模式切到 `TC_TEXT_ASCII`（仍可被应用覆盖） |
| 无焦点事件 | 不产生 `TC_EV_FOCUS` |
| 无括号粘贴 | 粘贴表现为一串普通按键事件 |

---

## 9. 可测试性：能力覆盖

为了使降级路径可测，提供显式覆盖接口：

```c
tc_status tc_term_set_caps_override(tc_term_t* t, const tc_caps* caps);  /* NULL 清除覆盖 */
tc_status tc_caps_from_env(tc_caps* out);                                 /* 仅按环境变量推导，供测试 */
```

- 覆盖后所有层（渲染颜色降级、输入协议启用）都按覆盖值工作
- 单元测试可用它构造"只有 16 色""无鼠标""ASCII 模式"等场景
- 覆盖是**显式 API**，不会隐式发生
- 交互式探测（§11）的结论也走同一通道写入，只是来源标记为 `TC_CAPS_ORIGIN_PROBE`

---

## 10. 特性列表（profile）：获取与写入

"能力"不只是一个位集，还要回答**每一项从哪来、是否被确认过、能不能存盘复用**。
能力层因此对外给出一份完整的**特性列表（profile）**：它既能整体读出，也能整体写回。

### 10.1 数据

```c
typedef enum tc_caps_origin {
    TC_CAPS_ORIGIN_DEFAULT = 0,  /* 保守默认 */
    TC_CAPS_ORIGIN_BUILTIN,      /* 内置 TERM 库 */
    TC_CAPS_ORIGIN_TERMINFO,     /* terminfo / termcap */
    TC_CAPS_ORIGIN_INFER,        /* 环境变量推断 */
    TC_CAPS_ORIGIN_QUERY,        /* DA 查询 */
    TC_CAPS_ORIGIN_OVERRIDE,     /* 应用显式覆盖 / 环境变量强制 */
    TC_CAPS_ORIGIN_PROBE         /* 用户确认过的探测结论 */
} tc_caps_origin;

typedef struct tc_caps_entry {
    uint64_t       bit;
    bool           supported;
    tc_caps_origin origin;
    bool           user_confirmed;    /* 是否经过人机交互确认 */
    int64_t        decided_at_ms;
    uint32_t       reserved[2];
} tc_caps_entry;

/* 环境指纹：判断"上次存盘的结论如今还灵不灵" */
typedef struct tc_caps_fingerprint {
    char     term[64];           /* TERM */
    char     term_program[64];   /* TERM_PROGRAM / 终端名 */
    char     colorterm[32];      /* COLORTERM */
    int32_t  da_primary_id;      /* DA 主响应 */
    int32_t  da_version;         /* 次 DA 版本（若有） */
    uint32_t lib_abi_version;
    uint64_t profile_version;    /* profile 格式版本 */
    int64_t  saved_at_ms;
    uint32_t reserved[2];
} tc_caps_fingerprint;

typedef struct tc_caps_profile {
    uint64_t       flags;        /* 结论位集（同 tc_caps.flags） */
    tc_caps        caps;         /* 颜色等级 / 最大色数 / 鼠标模式等派生字段 */
    tc_caps_entry* entries;      /* 逐项明细，库内分配 */
    size_t         entry_count;
    tc_caps_fingerprint fp;
    uint64_t       reserved[2];
} tc_caps_profile;
```

### 10.2 获取与写入

```c
tc_status tc_term_get_caps_profile(const tc_term_t* t, tc_caps_profile* out);
tc_status tc_caps_profile_dispose(tc_caps_profile* p);
tc_status tc_caps_get_entry(const tc_caps_profile* p, uint64_t bit, const tc_caps_entry** e);
tc_status tc_term_get_caps_fingerprint(const tc_term_t* t, tc_caps_fingerprint* out);

/* 写入：增量改位 / 整体写入 / 应用一份 profile */
tc_status tc_term_set_caps_bits(tc_term_t* t, uint64_t clear, uint64_t set, tc_caps_origin origin);
tc_status tc_term_set_caps_override(tc_term_t* t, const tc_caps* caps);      /* 见 §9 */
tc_status tc_term_apply_caps_profile(tc_term_t* t, const tc_caps_profile* p);
```

| 约定 | 说明 |
| --- | --- |
| 快照 | profile 是**某一时刻的快照**；`entries` 由库分配，`tc_caps_profile_dispose` 释放 |
| 写入粒度 | `set_caps_bits` 只动指定位（未提及的位不变）；`apply_caps_profile` 整体替换 |
| 来源 | 写入带 `origin`；`PROBE` 与 `OVERRIDE` 同属第 1 档，压过推断与查询结果（见 §2） |
| 立即生效 | 写入后渲染降级、输入协议启用、以及 `02` §4 里 runtime 开关的 `effective` 立刻按新值工作 |
| 已 enter | 允许写入；受影响的协议位会按**当前开关状态重放**（该开的补写 on，该关的补写 off），与 `02` §4 同源 |
| 宁可低估 | 未写入 / 未确认的位保持默认（不支持），不做乐观猜测 |

### 10.3 序列化与缓存（库不做文件 IO）

库**不读配置、不写缓存目录**：序列化由库提供，存在哪里由应用决定。

```c
tc_status tc_caps_profile_serialize(const tc_caps_profile* p, void* buf, size_t cap, size_t* need);
tc_status tc_caps_profile_deserialize(const void* buf, size_t len, tc_caps_profile* out);
tc_status tc_caps_profile_to_text(const tc_caps_profile* p, char* buf, size_t cap, size_t* need);
tc_status tc_caps_profile_from_text(const char* text, size_t len, tc_caps_profile* out);
tc_status tc_caps_profile_match(const tc_caps_profile* p, const tc_caps_fingerprint* now, bool* ok);
```

| 约定 | 说明 |
| --- | --- |
| 二进制格式 | magic `TCCP` + 格式版本 + 长度前缀字段；`need` 可先传 `cap = 0` 求长度 |
| 文本格式 | 稳定 `key=value` 行（易于 diff、手工修正、随应用配置一起存放） |
| 版本不符 | 返回 `TC_ERR_VERSION`：应用应丢弃缓存并重新探测（不尝试兼容旧格式） |
| 指纹 | `TERM` / `TERM_PROGRAM` / `COLORTERM` / DA id / DA version 参与比对；`saved_at_ms` 不参与 |
| 不匹配不是错误 | 只说明"环境可能变了"；应用自决：全部重探，或只重探 `user_confirmed == false` 的项 |
| 新增能力位 | 旧 profile 缺项 → 缺项按默认（低估），并在日志 sink 记一条 `TC_LOG_INFO` |

---

## 11. 交互式探测（probe）

有些能力**问不回来**：DA 不回答、terminfo 缺失、终端谎报。
真彩、样式、Unicode 宽度、鼠标是否真上报、Kitty 键盘是否真生效——最终只能**让人看一眼**。

职责划分（关键）：

| 谁 | 做什么 |
| --- | --- |
| **库** | 给出**探测项清单**；提供**每项样例的渲染**（在 surface 上画色卡 / 样式行 / 提示框 / 宽字符样本）；能自动判定的项自动判定；汇总结论**写入能力位集**；profile 序列化 |
| **应用** | 渲染自己的向导 UI（文案、导航、进度、本地化）；把库画的样例摆进自己的版面；收集用户回答并回传；负责读盘与存盘 |

> 库**不做**：UI 流程、按键处理、多语言文案、文件读写、网络。
> 库只回答"画什么"与"怎么判定"，其余都是应用的事。

### 11.1 探测项清单（catalog）

```c
typedef enum tc_caps_probe_kind {
    TC_PROBE_AUTO = 0,   /* 库自行判定：查询或喂事件即可得结论 */
    TC_PROBE_VISUAL,     /* 渲染样例，用户观察后多选一 */
    TC_PROBE_CONFIRM     /* 渲染样例，用户回答是 / 否 */
} tc_caps_probe_kind;

typedef struct tc_caps_probe_item {
    uint64_t           bit;           /* 关联的 tc_cap 位（可多位） */
    const char*        id;            /* 稳定 ID："color.level" / "mouse.sgr" … */
    const char*        title;         /* 默认标题（英文；应用应替换为本地化文案） */
    const char*        question;      /* 提问（同上） */
    tc_caps_probe_kind kind;
    int32_t            choice_count;  /* VISUAL 项的可选答案数 */
    const char* const* choices;       /* 答案文案数组 */
    uint32_t           reserved[2];
} tc_caps_probe_item;

const tc_caps_probe_item* tc_caps_probe_catalog(size_t* count);   /* 静态表，无需释放 */
```

清单（节选）：

| id | kind | 问用户什么 | 判定方式 |
| --- | --- | --- | --- |
| `color.level` | VISUAL | 这条渐变带你能看出多少颜色？ | 单色 / 16 / 256 / 平滑渐变（真彩） |
| `style.italic` 等 | CONFIRM | 这一行是斜体 / 下划线 / 删除线吗？ | 是 / 否 |
| `cursor.shape` | CONFIRM | 光标变成竖线了吗？ | 是 / 否 |
| `unicode.width` | VISUAL | 「你好」占两格且对齐吗？ | 正确 / 挤在一格 / 错位 |
| `mouse.*` | AUTO | 请在框内点击并拖动 | 喂鼠标事件，判定 X10 / DRAG / SGR |
| `focus` | AUTO | 请切到别的窗口再切回来 | 收到 1004 序列即支持 |
| `paste` | AUTO | 请粘贴一段文本 | 收到 2004 包裹即支持 |
| `kitty` | AUTO + CONFIRM | 请按 Ctrl+Shift+A 与方向键 | 收到 u 编码即支持；否则追问"是否看到乱码" |
| `sync` | VISUAL | 快速滚动时看到撕裂 / 闪烁吗？ | 是 / 否 |
| `pixel.size` | AUTO | — | 查询像素尺寸，超时即判不支持 |

### 11.2 渲染封装（render kit）

```c
tc_status tc_caps_render_sample(tc_surface_t* s, tc_rect area, uint64_t bit, uint32_t flags);
```

| bit | 画什么 |
| --- | --- |
| `TC_CAP_TRUECOLOR` / `COLOR_256` / `COLOR_16` | 渐变色带 + 网格色块 + 索引标注 |
| `TC_CAP_STYLE_*` | 每种样式一行样例文本 |
| `TC_CAP_CURSOR_SHAPE` / `CURSOR_HIDE` | 光标观察提示（配合控制层开关即时生效便于观察） |
| `TC_CAP_MOUSE_*` | 可点击 / 拖动的方框与提示 |
| `TC_CAP_UNICODE` | CJK、emoji、组合字符的对齐样本 |
| `TC_CAP_SYNC_UPDATE` | 高频闪烁对比块 |
| 其余 | 通用文本样例 |

| 约定 | 说明 |
| --- | --- |
| 纯绘制 | 只写 surface，不改能力、不写终端、不产生事件；应用随后自行 present |
| 绕过降级 | 必须带 `TC_CAPS_SAMPLE_RAW` 画**未降级**的样式，否则"不支持的样式"根本画不出来，探测永远测不出问题 |
| 可快照 | 不依赖真实终端；headless 下同样可画，供快照测试 |
| 版面 | 应用给定 `area`，库只在该矩形内绘制，不覆盖应用的提示与按钮 |

### 11.3 探测会话

```c
tc_status tc_caps_probe_begin(tc_term_t* t, uint64_t bits, uint32_t flags, tc_caps_probe** out);
tc_status tc_caps_probe_current(const tc_caps_probe* p, size_t* index, size_t* total,
                                const tc_caps_probe_item** item);
tc_status tc_caps_probe_render(tc_caps_probe* p, tc_surface_t* s, tc_rect area); /* 画当前项 */
tc_status tc_caps_probe_feed(tc_caps_probe* p, const tc_event* ev);   /* 喂事件：AUTO 项自动推进 */
tc_status tc_caps_probe_answer(tc_caps_probe* p, int32_t choice);     /* 用户回答 → 推进 */
tc_status tc_caps_probe_skip(tc_caps_probe* p);                       /* 跳过：该项保持默认（低估） */
tc_status tc_caps_probe_back(tc_caps_probe* p);                       /* 回到上一项改答案 */
tc_status tc_caps_probe_finish(tc_caps_probe* p, tc_caps* out);       /* 汇总并写入 term（out 可 NULL） */
void      tc_caps_probe_abort(tc_caps_probe* p);
```

| 约定 | 说明 |
| --- | --- |
| 顺序 | 按 catalog；AUTO 项先跑（能自动定的不打扰用户），剩余项才进人机交互 |
| 幂等 / 可回退 | `answer` 重复调用覆盖当前项答案；`back` 回上一项 |
| 跳过 | 视为"未确认"，该项沿用推断值（宁可低估） |
| 不吞事件 | `feed` 只消费与当前项相关的事件，其余事件返回值告知调用方"未消费"，由应用照常处理 |
| 超时 | AUTO 项有等待预算（默认复用 `capability_timeout_ms`）；超时则转 CONFIRM 或直接判不支持 |
| 写入 | `finish` 以 `TC_CAPS_ORIGIN_PROBE` 写入 term；已 enter 时同步重放受影响的协议（见 §10.2） |
| 线程 / 重入 | 单线程；不得在渲染或变更回调内调用 `feed` |
| headless | 完全可用：事件来自注入，结论可脚本化，用于测试向导流程本身 |

### 11.4 首次启动的完整流程

```c
/* 1) 应用自己读盘（库不做文件 IO） */
uint8_t* blob; size_t n;
if (app_read_file(app_caps_path(&blob, &n))) {
    tc_caps_profile saved;
    if (tc_caps_profile_deserialize(blob, n, &saved) == TC_OK) {
        tc_caps_fingerprint now;
        tc_term_get_caps_fingerprint(term, &now);
        bool ok = false;
        tc_caps_profile_match(&saved, &now, &ok);
        if (ok) { tc_term_apply_caps_profile(term, &saved); goto ready; }   /* 命中缓存，零交互 */
    }
}

/* 2) 未命中 → 应用渲染向导；库只负责画样例、判定、写入 */
tc_caps_probe* pr = NULL;
tc_caps_probe_begin(term, 0 /* 全部 */, 0, &pr);
for (;;) {
    const tc_caps_probe_item* it = NULL; size_t i = 0, total = 0;
    tc_caps_probe_current(pr, &i, &total, &it);
    if (!it) break;                                  /* 跑完 */

    app_render_wizard_frame(s, i, total, it);        /* 应用自己的文案 / 进度 / 按钮 */
    tc_caps_probe_render(pr, s, app_sample_area());  /* 库画这一项的样例 */
    tc_present(term, s);

    if (it->kind == TC_PROBE_AUTO) {
        tc_event ev; bool has;
        while (tc_wait_event(term, 100, &ev, &has) == TC_OK && has)
            tc_caps_probe_feed(pr, &ev);             /* 自动推进 */
    } else {
        int32_t choice = app_ask_user(it);           /* 应用自己的 UI */
        if (choice < 0) tc_caps_probe_skip(pr); else tc_caps_probe_answer(pr, choice);
    }
}
tc_caps_probe_finish(pr, NULL);                      /* 结论写入 term */

/* 3) 应用存盘 */
tc_caps_profile prof; tc_term_get_caps_profile(term, &prof);
size_t need = 0; tc_caps_profile_serialize(&prof, NULL, 0, &need);
/* … 分配 need 字节，再 serialize，写文件 … */
```

| 阶段 | 行为 |
| --- | --- |
| 首次 | 无缓存 → 向导 → 结论写入 → 应用存盘（可同时存文本版备查） |
| 后续 | 读盘 → 指纹匹配 → `apply_caps_profile`；**零交互、零查询，不阻塞启动** |
| 环境变化 | 指纹不匹配 → 应用自决"全部重探"或"只重探未确认项" |
| 换终端 | `TERM` / `TERM_PROGRAM` / DA id 变化即视为新环境 |
| 随时重跑 | 设置页提供"重新检测"，复用同一套向导 |
| 不替用户决定 | 库不主动弹 UI、不自动写文件；一切由应用发起 |

### 11.5 与运行时开关的关系

| 概念 | 谁决定 | 例子 |
| --- | --- | --- |
| 能力（**能不能**） | 终端 + 用户确认（§10 / §11） | 终端支持 SGR 鼠标 |
| 开关（**要不要**） | 应用（见 `02` §4） | 编辑模式下把鼠标关掉 |

- 探测只改**能力**，不改应用的开关意图：`effective = requested && 能力允许`。
- 缓存 profile 里存的是能力，不是开关；开关状态由应用自己持久化（若需要）。

---

## 12. API 汇总

```c
/* 查询 */
tc_status tc_term_get_caps(const tc_term_t* t, tc_caps* out);
bool      tc_caps_has(const tc_caps* c, tc_cap flag);
tc_status tc_term_get_color_level(const tc_term_t* t, tc_color_level* out);

/* 覆盖（测试 / 用户强制） */
tc_status tc_term_set_caps_override(tc_term_t* t, const tc_caps* caps);
tc_status tc_caps_from_env(tc_caps* out);

/* 颜色降级（纯函数，也可直接单测） */
tc_status tc_caps_downgrade_rgb(const tc_caps* c, uint8_t r, uint8_t g, uint8_t b,
                                tc_color* out);
tc_status tc_caps_downgrade_indexed(const tc_caps* c, uint8_t idx256, tc_color* out);

/* 特性列表：获取 / 写入（见 §10） */
tc_status tc_term_get_caps_profile(const tc_term_t* t, tc_caps_profile* out);
tc_status tc_caps_profile_dispose(tc_caps_profile* p);
tc_status tc_caps_get_entry(const tc_caps_profile* p, uint64_t bit, const tc_caps_entry** e);
tc_status tc_term_get_caps_fingerprint(const tc_term_t* t, tc_caps_fingerprint* out);
tc_status tc_term_set_caps_bits(tc_term_t* t, uint64_t clear, uint64_t set, tc_caps_origin origin);
tc_status tc_term_apply_caps_profile(tc_term_t* t, const tc_caps_profile* p);

/* 特性列表：序列化（库不做文件 IO，见 §10.3） */
tc_status tc_caps_profile_serialize(const tc_caps_profile* p, void* buf, size_t cap, size_t* need);
tc_status tc_caps_profile_deserialize(const void* buf, size_t len, tc_caps_profile* out);
tc_status tc_caps_profile_to_text(const tc_caps_profile* p, char* buf, size_t cap, size_t* need);
tc_status tc_caps_profile_from_text(const char* text, size_t len, tc_caps_profile* out);
tc_status tc_caps_profile_match(const tc_caps_profile* p, const tc_caps_fingerprint* now, bool* ok);

/* 交互式探测（见 §11） */
const tc_caps_probe_item* tc_caps_probe_catalog(size_t* count);
tc_status tc_caps_render_sample(tc_surface_t* s, tc_rect area, uint64_t bit, uint32_t flags);
tc_status tc_caps_probe_begin(tc_term_t* t, uint64_t bits, uint32_t flags, tc_caps_probe** out);
tc_status tc_caps_probe_current(const tc_caps_probe* p, size_t* index, size_t* total,
                                const tc_caps_probe_item** item);
tc_status tc_caps_probe_render(tc_caps_probe* p, tc_surface_t* s, tc_rect area);
tc_status tc_caps_probe_feed(tc_caps_probe* p, const tc_event* ev);
tc_status tc_caps_probe_answer(tc_caps_probe* p, int32_t choice);
tc_status tc_caps_probe_skip(tc_caps_probe* p);
tc_status tc_caps_probe_back(tc_caps_probe* p);
tc_status tc_caps_probe_finish(tc_caps_probe* p, tc_caps* out);
void      tc_caps_probe_abort(tc_caps_probe* p);
```

---

## 13. 返回码

| 场景 | 返回 |
| --- | --- |
| 查询成功（含"查询超时降级"） | `TC_OK` |
| 参数 NULL | `TC_ERR_INVALID_ARG` |
| DA 查询超时 | `TC_OK` + 能力按下一档（诊断走日志 sink，级别 `TC_LOG_INFO`） |
| 不支持的能力请求（如启用 altscreen 但无该能力） | `TC_ERR_UNSUPPORTED` |
| terminfo 解析失败 | `TC_OK`（降级），日志 `TC_LOG_WARN` |
| profile 格式版本不符 | `TC_ERR_VERSION`（应用应丢弃缓存并重新探测，见 §10.3） |
| profile 字节 / 文本不合法 | `TC_ERR_PARSE` |
| 探测会话已结束后仍 `feed` / `answer` | `TC_ERR_STATE` |
| 探测结论写入成功 | `TC_OK`，来源标记为 `TC_CAPS_ORIGIN_PROBE` |

---

## 14. 平台无关性

判定全部在能力层完成：能力层把平台差异收敛成**能力位集**，
控制层与渲染层只见位集、不感知平台；平台特有的实现要点见 `08-platform-notes.md`。
