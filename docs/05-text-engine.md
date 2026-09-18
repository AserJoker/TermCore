# 05 · 文本引擎（`tc_text_*`）

文本引擎是 TermCore 中**独立于渲染层**的模块，负责"文本到底是什么、占多宽、怎么切"。
输入解析层与输出渲染层共用它，保证"按键产生的字符"与"画到屏幕上的字符"解释一致。

---

## 1. 定位与原则

| 原则 | 说明 |
| --- | --- |
| 独立可用 | 只依赖 `<stddef.h>` / `<stdint.h>` / `<stdbool.h>`，不依赖 `tc_term_t` / `tc_surface_t`；上层布局框架可只用 `termcore/text.h` 做测量 |
| 无状态 | 所有函数只依赖入参；跨调用状态（如不完整 UTF-8 序列）由调用方持有，不藏在库里 |
| 无分配 | 全模块不调用 allocator，可在热路径随意调用 |
| 纯函数 | 相同输入必得相同输出；不做 locale 推断、不读环境变量 |
| 双模式 | ASCII 与 Unicode 两套语义，行为差异集中在本文 §3 |
| 数据源 | 完整 UAX#29 由可选 ICU 后端提供；无 ICU 时保守降级（§9） |

---

## 2. 头文件与类型

```c
#include <termcore/text.h>

typedef enum tc_text_mode {
    TC_TEXT_ASCII   = 0,   /* 严格 7bit */
    TC_TEXT_UNICODE = 1    /* 按 UAX#29 字符簇 + EAW 宽度 */
} tc_text_mode;

typedef struct tc_grapheme {
    const char* begin;      /* 簇起始（借用，不拥有） */
    const char* end;        /* 簇结束（不含） */
    uint32_t    first_cp;   /* 簇首码点（便于快速判断） */
    int32_t     width;      /* 该簇的列宽（0 / 1 / 2） */
    uint32_t    reserved[2];
} tc_grapheme;

typedef struct tc_text_metrics {
    int32_t columns;     /* 显示列宽总和 */
    int32_t graphemes;   /* 字符簇数 */
    int32_t codepoints;  /* 码点数 */
    int32_t bytes;       /* 已消耗的字节数 */
} tc_text_metrics;
```

> `tc_grapheme` 中的指针是**借用**，只在输入字符串有效期内有效；库不拷贝、不持有。

---

## 3. 双模式语义

### 3.1 ASCII 模式（`TC_TEXT_ASCII`）

| 规则 | 行为 |
| --- | --- |
| 编码 | 严格 7bit；**不做** UTF-8 多字节解码 |
| 单元 | 一个字节 = 一个字符簇 = 一个 cell |
| 宽度 | 恒为 1（包括控制字符占位场景） |
| 非 ASCII（≥ 0x80） | 输出替换字符；默认 `?`，可由 `tc_text_config` 设为 U+FFFD（`EF BF BD`） |
| C0 控制符（< 0x20、0x7F） | 视为宽度 1 的簇（渲染层按控制符策略处理，通常不绘制） |
| 组合与宽字符 | 不存在此概念，一字节一格 |

用途：极简终端、ASCII-only 应用、以及"终端不支持 Unicode"时的降级（能力层可据此把默认模式切成 ASCII）。

### 3.2 Unicode 模式（`TC_TEXT_UNICODE`）

| 规则 | 行为 |
| --- | --- |
| 编码 | 严格 UTF-8 解码（见 §4） |
| 单元 | UAX#29 字符簇（见 §5） |
| 宽度 | EAW 决定，0 / 1 / 2（见 §6） |
| 组合符 | 宽度为 0，并入前一个簇 |
| emoji ZWJ 序列 | 整个序列算一个簇，宽度 2 |
| 区域指示符（国旗） | 成对算一个簇，宽度 2 |
| 无效序列 | 单个非法字节按 U+FFFD 处理；流末端的不完整序列整个结算为一个 U+FFFD（见 §4.2） |

### 3.3 模式选择

- term 级默认：`tc_term_set_text_mode`
- 绘制调用可逐次覆盖（见 `06-api-render.md`）
- 模式只影响**文本解释**，不影响颜色、样式、鼠标等其它行为

---

## 4. UTF-8 解码

```c
typedef enum tc_utf8_err {
    TC_UTF8_OK         = 0,   /* 成功得到一个码点 */
    TC_UTF8_INCOMPLETE = 1,   /* 字节不足（可能被切分），需要更多字节 */
    TC_UTF8_INVALID    = 2    /* 非法序列，已按替换字符处理 */
} tc_utf8_err;

tc_utf8_err tc_text_utf8_next(const char** s, const char* end, uint32_t* out_cp);
tc_utf8_err tc_text_utf8_validate(const char* s, const char* end, size_t* bad_offset);
size_t      tc_text_utf8_encode(uint32_t cp, char* out4);   /* 返回写入字节数，0 表示不可编码 */
```

### 4.1 严格规则

| 情形 | 判定 |
| --- | --- |
| 1 字节 `00–7F` | 合法 |
| 2 字节 `C2–DF 80–BF` | 合法（**拒绝 overlong** `C0`/`C1`） |
| 3 字节 `E0 A0–BF 80–BF`、`E1–EC 80–BF 80–BF`、`ED 80–9F 80–BF`、`EE–EF …` | 合法（**拒绝 UTF-16 代理区** `ED A0–BF`） |
| 4 字节 `F0 90–BF …`、`F1–F3 80–BF …`、`F4 80–8F …` | 合法（上限 U+10FFFF，拒绝 `F5–FF`） |
| 续字节缺失 | `TC_UTF8_INCOMPLETE`（**不消费**任何字节，留给调用方继续喂） |
| 孤立续字节 / 非法首字节 / overlong / 代理区 / 超范围 | `TC_UTF8_INVALID`，消费**一个**字节并产出 U+FFFD |
| 嵌入的 NUL | 视为 1 字节码点 U+0000（调用方决定如何处理） |

### 4.2 跨块切分

输入解析与注入可能把序列切开（如一次只喂 1 字节）。约定：

- 解码器**不持有**跨调用状态；状态由调用方（解析器/绘制器）用"残留缓冲"保存
- `TC_UTF8_INCOMPLETE` 时不推进指针，调用方保留这些字节，等下一批数据后重试
- 流结束时仍有不完整序列 → 按 `TC_UTF8_INVALID` 结算：**整个剩余序列**作为一个 U+FFFD 消费（不再逐字节拆开）
- 残留缓冲有上限（解析器侧 256 字节），超限丢弃并诊断

---

## 5. 字符簇：完整 UAX#29

```c
bool tc_text_grapheme_next(const char** s, const char* end, tc_grapheme* out);
bool tc_text_grapheme_next_off(const char* s, size_t len, size_t* inout_offset, tc_grapheme* out);
```

实现 UAX#29 的**完整**字素簇边界规则（Grapheme Cluster Boundaries），含 emoji 扩展：

| 规则 | 内容 |
| --- | --- |
| GB1 / GB2 | 字符串起止处断开 |
| GB3 | `CR × LF` 不中断（CRLF 是一个簇） |
| GB4 / GB5 | Control / CR / LF 前后强制断开 |
| GB6 / GB7 / GB8 | Hangul（L × V/LV/LVT、LV/LVT × T）组合不断开 |
| GB9 | `× (Extend | ZWJ)`：组合标记与 ZWJ 并入前簇 |
| GB9a | `× SpacingMark`：间距标记并入前簇 |
| GB9b | `Prepend ×`：前置符与后续字符合为一簇 |
| GB11 | `ExtPict × ZWJ × ExtPict`：emoji ZWJ 序列不断开（👨‍👩‍👧 是一簇） |
| GB12 / GB13 | `Regional_Indicator` 成对：RI × RI 不断开，第三个 RI 处断开（🇨🇳 一簇，🇨🇳🇺🇸 两簇） |
| GB999 | 其它情况断开 |

需要的 Unicode 属性表：

`Grapheme_Cluster_Break` 属性（CR / LF / Control / Extend / ZWJ / Regional_Indicator / Prepend / SpacingMark / L / V / T / LV / LVT / Other）与 `Extended_Pictographic`。

### 5.1 语义要点

- 组合符（如 U+0301）永不独立成簇，宽度 0，渲染时并入前一格
- 若字符串以组合符开头（无前簇），它自己成为一个簇，宽度 0（渲染层按"独占一格显示替换/放置"策略处理，见 `06`）
- ZWJ 序列只在两端都是 `Extended_Pictographic` 时合并；否则 ZWJ 按 GB9 并入前簇
- 变体选择符 VS15（U+FE0E）/ VS16（U+FE0F）属 `Extend`，并入前簇，但**影响宽度**（见 §6）
- 簇内码点数量上限：实现设一个安全上限（如 32），超过强制断开，防止病态输入

### 5.2 迭代示例

```c
const char* s = text; const char* end = text + len;
tc_grapheme g;
while (s < end && tc_text_grapheme_next(&s, end, &g)) {
    /* g.begin..g.end 是一个字符簇；列宽由 tc_text_grapheme_width(&g, …)
     * 计算（next 本身不填 g.width，因其签名无模式参数，见 §6.3） */
}
```

---

## 6. 宽度

### 6.1 判定顺序（Unicode 模式）

```
1. 若簇含 VS16(U+FE0F) 且首码点为 Emoji_Presentation 或 Extended_Pictographic → 2
2. 若簇含 VS15(U+FE0E) → 按"文本呈现"处理，宽度取 EAW（通常 1）
3. 若簇是 emoji ZWJ 序列（GB11）→ 2
4. 若簇是成对的 Regional_Indicator → 2
5. 取簇中**首个非组合**码点的 EAW：
     W / F        → 2
     A (Ambiguous)→ ambiguous_wide ? 2 : 1
     其余（Na/H/N）→ 1
6. 组合符 / 控制符 / 默认忽略 → 0
```

细节：

| 情形 | 宽度 |
| --- | --- |
| 纯组合簇（以组合符开头） | 0 |
| 控制字符（C0/C1） | 0（渲染层不为其分配 cell） |
| TAB | 由渲染层/上层解释（引擎返回 0，调用方自行跳格） |
| 换行符 | 0，且不参与列宽累计 |
| 未分配码点（Cn） | 1（保守） |
| 替换字符 U+FFFD | 1 |
| 单一 RI（未成对） | 1（按 EAW，RI 属 A 类，受 ambiguous 配置影响） |

### 6.2 Ambiguous 宽度

`ambiguous_wide` 是**显式入参**，不由库推断 locale：

- 应用可用自己的方式判断 locale（如 `setlocale` + 语言/地区），再传 `true`
- 东亚环境（ja/zh/ko）建议 `true`；其它建议 `false`
- 库不做隐式 locale 探测，保证纯函数特性与可测试性

### 6.3 API

```c
int32_t tc_text_char_width(uint32_t cp, tc_text_mode mode, bool ambiguous_wide);
int32_t tc_text_grapheme_width(const tc_grapheme* g, tc_text_mode mode, bool ambiguous_wide);
int32_t tc_text_string_width(const char* utf8, size_t len, tc_text_mode mode, bool ambiguous_wide);
```

> ASCII 模式下以上函数恒返回 1（空串返回 0），便于调用方写无条件代码。

---

## 7. 测量与排版辅助

```c
tc_text_metrics tc_text_measure(const char* utf8, size_t len,
                                tc_text_mode mode, bool ambiguous_wide);

bool tc_text_truncate_columns(const char* utf8, size_t len, int32_t max_columns,
                              tc_text_mode mode, bool ambiguous_wide,
                              size_t* out_bytes, int32_t* out_columns, bool* truncated);

bool tc_text_wrap_next(const char* utf8, size_t len, int32_t max_columns,
                       tc_text_mode mode, bool ambiguous_wide,
                       size_t* out_break_bytes);
```

### 7.1 `tc_text_measure`

- 遍历全部簇，累加宽度，返回 `{columns, graphemes, codepoints, bytes}`
- 遇到非法序列按替换字符计入（不提前终止）
- `len == 0` → 全 0 的 metrics

### 7.2 `tc_text_truncate_columns`

规则：

1. 按簇累加宽度，超过 `max_columns` 的**整个簇**不纳入（簇不允许被拆开）
2. 输出：截断处的字节偏移、实际列宽、是否发生截断
3. 不添加省略号（省略号策略属上层：上层可先按 `max-1` 截断再自行追加 `…`）
4. 尾部空格处理留给上层（引擎不做 trim）

### 7.3 `tc_text_wrap_next`（换行点计算）

提供**实用级**断行（不是完整 UAX#14）：

| 规则 | 行为 |
| --- | --- |
| 主规则 | 在不超过 `max_columns` 的最后一个"断行机会"处断开 |
| 断行机会 | 空格 / 制表符之后；CJK 字符之间；连字符 `-` 之后 |
| 禁止 | 不在字符簇中间断开；不在组合符与其基字符之间断开 |
| 禁止（禁则） | 不在行首放置 `)`, `]`, `}`, `、`, `。`, `！` 等（内置小表，可由上层覆盖策略） |
| 超长词 | 无断行机会时强制按列宽硬断（保证进度） |
| 返回 | 下一行起始的字节偏移；返回 `false` 表示文本结束 |

> 这是"实用级"而非标准级：目标是让上层布局库有开箱可用的换行，而不是实现完整 UAX#14（需要更大的表与更高的复杂度）。上层可完全不用它、自己做排版。

---

## 8. 单元格写入规则（与渲染层的接口）

渲染层调用文本引擎把文本写进 cell 网格。规则：

| 情形 | 行为 |
| --- | --- |
| 宽度 1 簇 | 占 1 个 cell，内容为簇的 UTF-8 字节 |
| 宽度 2 簇 | 占 2 个 cell：首格存字节 + `TC_ATTR_WIDE_HEAD`；次格存空 + `TC_ATTR_WIDE_CONT` |
| 宽度 0 簇 | 不占新 cell：合并进前一格的字节序列（受单格最大字节数限制） |
| 覆盖宽字符的一半 | 被覆盖的半格与其配对格一并清除（避免残留半个汉字） |
| 单格字节上限 | 簇字节数超过 cell 容量（默认 15 字节 + 长度）时，按替换处理或截断（可配置） |
| ASCII 模式 | 一字节一格，无占位、无合并 |

宽字符完整性是渲染层的硬约束：**不允许出现只有 WIDE_HEAD 而无 WIDE_CONT 的状态**，
也不允许 WIDE_CONT 出现在行首（行首的孤立 WIDE_CONT 会被清成普通空格）。

---

## 9. Unicode 数据源：仅 ICU 后端（含无 ICU 降级）

**架构决策（2026-09）：采用"仅 ICU 后端"**。grapheme 切分与宽度判定以
ICU 为唯一完整实现；不再维护内置 UCD 生成表工具链（`tools/gen_ucd_tables.*`
计划已废弃）。`TERMCORE_USE_ICU=ON` 时行为与 ICU 版本对齐；
`OFF` 时提供**保守降级**，保证 API 可用但不保证完整 UAX#29 语义。

### 9.1 ICU 后端（`TERMCORE_USE_ICU=ON`）

```cmake
cmake -DTERMCORE_USE_ICU=ON ...
```

| 功能 | ICU 实现 |
| --- | --- |
| 簇切分 | `ubrk_open(UBRK_CHARACTER, "")` + `ubrk_next`（完整 UAX#29，含 GB11 emoji ZWJ、GB12/13 RI 配对、GB3 CRLF、GB6/7/8 Hangul） |
| 宽度 | `u_charType`（组合标记 Mn/Me/Mc → 0）+ `u_getIntPropertyValue(UCHAR_EAST_ASIAN_WIDTH)`（W/F → 2，A → 随 `ambiguous_wide`） |
| emoji 宽度 | `UCHAR_EMOJI_PRESENTATION` / `UCHAR_EXTENDED_PICTOGRAPHIC`（VS16、ZWJ 序列 → 2） |

数据加载：

- ICU 以 **stubdata**（`icudt*.dat` 不嵌入二进制）构建，数据包在运行时外部加载
- `third_party/icu` 构建后把 `icudt74l.dat` 复制到 `${CMAKE_BINARY_DIR}/data/`
- 应用启动时调用 `icu_data_init(data_dir)`（见 `third_party/icu/icu_data.h`）
  加载数据；`gtest_discover_tests` 的每个测试进程需自行初始化
- 数据文件与库版本必须匹配（ICU 74.x）

### 9.2 无 ICU 降级（`TERMCORE_USE_ICU=OFF`，默认）

不做 UAX#29 簇合并，行为退化为：

| 项目 | 降级行为 |
| --- | --- |
| 簇切分 | 按码点切分（一个码点一个簇），CRLF、组合符、ZWJ、RI、Hangul 均不合并 |
| 宽度 | 内置区间表：CJK/全角/emoji 区间 → 2；组合标记区间表（Mn/Me/Mc 常用块 + 变体选择符）→ 0；EAW Ambiguous 采样区间 → 随 `ambiguous_wide`；其余 → 1 |
| 组合符 | 不并入基字符簇；`tc_text_grapheme_width` 对纯组合簇仍返回 0 |

降级路径的区间表是**采样**的（覆盖常用块），与 ICU 的完整 UCD 数据可能有差异；
需要完整语义的构建应启用 ICU。

### 9.3 一致性

- ICU 构建是行为基准；降级构建只保证"合理近似 + 稳定 API"
- 测试在两种构建下都跑：依赖完整簇合并语义的用例在无 ICU 时跳过（`TC_REQUIRE_ICU()`）
- ICU 数据由外部 `icudt*.dat` 提供，跟随仓库内固定的 ICU 74.2 源版本

---

## 10. 测试集

| 类别 | 来源 / 内容 | 断言方式 |
| --- | --- | --- |
| 字素簇 | UAX#29 官方 `GraphemeBreakTest.txt` 全量 | 逐条比对断点是 `÷` 还是 `×` |
| emoji | ZWJ 家族序列、VS16、VS15、RI 国旗（成对与奇数个）、键帽序列 | 簇数 + 宽度 |
| 宽度 | EastAsianWidth 官方数据抽样 + CJK/全角/半角/组合/控制字符 | 与表一致 |
| Ambiguous | 同一串在 `ambiguous_wide` 真假两种取值下的宽度差异 | 差值正确 |
| ASCII 模式 | 非 ASCII 字节替换、宽度恒 1、不解码多字节 | 单元格数 == 字节数 |
| UTF-8 | 合法 / overlong / 代理区 / 截断 / 孤立续字节 / 超范围 | 错误分类与替换行为 |
| 跨块 | 按 1 字节喂入长序列 | 结果与一次性喂入一致 |
| 测量 / 截断 / 换行 | 混合 CJK + emoji + 组合符的字符串 | 列宽、截断偏移、换行点 |
| Fuzz | 随机字节流 | 不崩溃、不越界、宽度与非负 |
| 一致性 | ICU 构建 vs 无 ICU 降级构建 | 降级跳过依赖簇合并的用例；其余一致 |

> 依赖完整 UAX#29 簇合并语义的用例（GB3 CRLF、GB6/7/8 Hangul、GB9 组合符、
> GB11 emoji ZWJ、GB12/13 RI 配对）在无 ICU 构建下跳过（`TC_REQUIRE_ICU()`）。

---

## 11. 性能与复杂度

| 操作 | 复杂度 | 说明 |
| --- | --- | --- |
| 解码 1 码点 | O(1) | 按首字节分支 |
| 宽度判定（ICU） | O(1) | `u_charType` + `u_getIntPropertyValue` 属性查询 |
| 宽度判定（降级） | O(区间表项数) | 内置区间线性扫描（表较小，可接受） |
| 簇切分（ICU） | O(簇内码点数) | ubrk 每次调用解码至多 33 码点 + 一次迭代 |
| 簇切分（降级） | O(1) | 单码点 |
| 测量整串 | O(n) | 顺序扫描，无分配 |

优化约定：

- 渲染层可缓存"上一次的簇结果"（同一行内连续绘制时常见），但引擎本身无状态
- ICU 路径每次 `grapheme_next` 新建迭代器；上层若追求极致性能可在热路径复用 `UBreakIterator`（引擎不持有状态，由上层缓存）
- 热路径无分配、无锁、无 locale 调用

---

## 12. 明确不支持

| 不支持 | 原因 / 建议 |
| --- | --- |
| 双向文本（bidi） | 需要完整排版引擎；上层如需请自行处理 |
| 复杂字形整形（阿拉伯语、梵文） | 同上 |
| 字体 / 字形 / 字号 | 由终端与用户决定，库零参与 |
| 非 UTF-8 输入编码（GBK / Shift-JIS / Latin-1） | 输入一律按 UTF-8 解释；ASCII 模式按字节 |
| 完整 UAX#14 断行 | 提供实用级换行，够用即可 |
| 大小写映射、排序、正则 | 不属本库范围 |

---

## 13. API 汇总

```c
/* 解码 */
tc_utf8_err tc_text_utf8_next(const char** s, const char* end, uint32_t* out_cp);
tc_utf8_err tc_text_utf8_validate(const char* s, const char* end, size_t* bad_offset);
size_t      tc_text_utf8_encode(uint32_t cp, char* out4);

/* 字符簇 */
bool tc_text_grapheme_next(const char** s, const char* end, tc_grapheme* out);
bool tc_text_grapheme_next_off(const char* s, size_t len, size_t* inout_offset, tc_grapheme* out);

/* 宽度 */
int32_t tc_text_char_width(uint32_t cp, tc_text_mode mode, bool ambiguous_wide);
int32_t tc_text_grapheme_width(const tc_grapheme* g, tc_text_mode mode, bool ambiguous_wide);
int32_t tc_text_string_width(const char* utf8, size_t len, tc_text_mode mode, bool ambiguous_wide);

/* 测量与排版 */
tc_text_metrics tc_text_measure(const char* utf8, size_t len, tc_text_mode mode, bool ambiguous_wide);
bool tc_text_truncate_columns(const char* utf8, size_t len, int32_t max_columns, tc_text_mode mode,
                              bool ambiguous_wide, size_t* out_bytes, int32_t* out_columns, bool* truncated);
bool tc_text_wrap_next(const char* utf8, size_t len, int32_t max_columns, tc_text_mode mode,
                       bool ambiguous_wide, size_t* out_break_bytes);

/* 可选配置（仅 ASCII 替换字符等少数项，仍是纯函数式：配置作为入参或显式设置） */
typedef struct tc_text_config {
    uint32_t replacement_cp;   /* 默认 '?'（ASCII）/ U+FFFD（Unicode） */
    int32_t  tab_columns;      /* TAB 的列宽，默认 0（由调用方解释） */
    uint32_t reserved[2];
} tc_text_config;
void tc_text_config_default(tc_text_config* cfg);
```
