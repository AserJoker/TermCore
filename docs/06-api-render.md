# 06 · 输出渲染层 API（`tc_surface_t` + `tc_present`）

渲染层负责"把单元格画出来，并高效地同步到终端"。
模型是**离屏单元格网格 + 行级 diff**：

- **surface 是离屏单元格网格**：可创建任意数量、任意尺寸，**不绑定屏幕**，可反复复用
- 调用方在 surface 上直接绘制 cell；同一 surface 同时是绘制目标与提交的最终帧
- 最后由 `tc_present` 把 surface 提交到屏幕——这是唯一真正写终端的动作

---

## 1. 职责

- 离屏渲染目标（surface）与批量绘制辅助
- Unicode 簇 → cell 的占位与宽字符规则（调用文本引擎）
- 屏幕镜像（term 持有）与行级 diff
- 转义序列生成与单次写出
- 颜色与样式降级（按能力位集）
- 增量变更回调、多 sink 广播、只读帧视图

不做：布局、组件、图层合成、换行排版策略（上层可用文本引擎自己做）。

---

## 2. 数据模型（POD）

```c
typedef enum tc_color_kind {
    TC_COLOR_DEFAULT = 0,   /* 终端默认色 */
    TC_COLOR_INDEXED = 1,   /* 0–255 索引色 */
    TC_COLOR_RGB     = 2    /* 24 位真彩 */
} tc_color_kind;

typedef struct tc_color {
    uint8_t kind;           /* tc_color_kind */
    uint8_t r, g, b;        /* RGB 模式时有效 */
    uint8_t idx;            /* INDEXED 模式时有效 */
    uint8_t reserved[3];
} tc_color;

typedef enum tc_style {
    TC_STYLE_NONE       = 0,
    TC_STYLE_BOLD       = 1u << 0,
    TC_STYLE_DIM        = 1u << 1,
    TC_STYLE_ITALIC     = 1u << 2,
    TC_STYLE_UNDERLINE  = 1u << 3,
    TC_STYLE_BLINK      = 1u << 4,
    TC_STYLE_REVERSE    = 1u << 5,
    TC_STYLE_HIDDEN     = 1u << 6,
    TC_STYLE_STRIKE     = 1u << 7,
    TC_STYLE_UNDERCURL  = 1u << 8,
    TC_STYLE_DOUBLE_UNDERLINE = 1u << 9
} tc_style;

typedef enum tc_cell_attr {
    TC_ATTR_NONE        = 0,
    TC_ATTR_WIDE_HEAD   = 1u << 0,   /* 宽字符首格 */
    TC_ATTR_WIDE_CONT   = 1u << 1    /* 宽字符续格（无内容） */
} tc_cell_attr;

#define TC_CELL_TEXT_CAP 15

typedef struct tc_cell {
    tc_color fg;
    tc_color bg;
    uint16_t style;      /* tc_style 位或 */
    uint16_t attr;       /* tc_cell_attr 位或 */
    uint8_t  len;        /* text 有效字节数 */
    char     text[TC_CELL_TEXT_CAP + 1];  /* UTF-8，含结尾 NUL */
    uint8_t  reserved[2];
} tc_cell;

typedef struct tc_rect { int32_t x, y, w, h; uint32_t reserved[2]; } tc_rect;

/* 绘制文本时使用的属性组（不含字符内容） */
typedef struct tc_style_attr {
    tc_color fg;
    tc_color bg;
    uint16_t style;      /* tc_style 位或 */
    uint16_t reserved[3];
} tc_style_attr;
```

设计取舍：

- `tc_cell` 内联文本（16 字节）而非指针：零分配、缓存友好、diff 时可逐字节比较
- 一个 cell 容纳一个字符簇；超过容量的簇按配置处理（截断或替换为 `…`/`?`）
- `text` 存 UTF-8 原字节，输出时直接写入（无需再编码）

---

## 3. surface 生命周期

```c
tc_status tc_surface_create(tc_term_t* t, int32_t cols, int32_t rows, tc_surface_t** out);
tc_status tc_surface_resize(tc_surface_t* s, int32_t cols, int32_t rows);
void      tc_surface_destroy(tc_surface_t* s);

tc_status tc_surface_get_size(const tc_surface_t* s, int32_t* cols, int32_t* rows);
tc_status tc_surface_set_text_mode(tc_surface_t* s, tc_text_mode mode);
tc_status tc_surface_set_ambiguous_wide(tc_surface_t* s, bool wide);
```

| 函数 | 说明 |
| --- | --- |
| `create` | 分配**一份**单元格缓冲；初始为"默认空格 + 默认属性"；整屏标记脏 |
| `resize` | 重分配并复用；内容按策略保留左上角（可配置为清空）；**整屏标记脏** |
| `destroy` | 释放；不自动从 term 注销（若有帧视图在外，调用方需自行保证不再使用） |

- **surface 不绑定屏幕**：`t` 仅提供 allocator、默认文本模式与能力上下文；可同时存在任意多个 surface，尺寸互不相干
- **surface 是纯离屏网格**，只持有一份单元格缓冲；"屏幕镜像（front）"与 `outbuf` 由 **term** 持有（见 §5）
- surface 可**反复复用**：每帧清空后重绘，或局部更新后再次 present
- 尺寸可与终端不同：present 时按"取较小区域"处理（可配置为返回错误）

---

## 4. 绘制 API

```c
tc_status tc_surface_put_cell(tc_surface_t* s, int32_t x, int32_t y, const tc_cell* c);
tc_status tc_surface_fill_rect(tc_surface_t* s, const tc_rect* r, const tc_cell* c);
tc_status tc_surface_clear(tc_surface_t* s);
tc_status tc_surface_draw_text(tc_surface_t* s, int32_t x, int32_t y,
                               const char* utf8, const tc_style_attr* attr,
                               tc_text_mode mode, int32_t* out_consumed_columns);
```

| API | 语义 |
| --- | --- |
| `put_cell` | 写单个 cell；坐标越界返回 `TC_ERR_INVALID_ARG` |
| `fill_rect` | 矩形内填同一 cell（宽字符占位会被正确处理） |
| `clear` | 全屏填默认空格（保留默认属性） |
| `draw_text` | 从 (x,y) 起写入字符串，自动按簇占位；**不换行**、不裁剪策略由调用方决定 |

约定：

- 坐标 0 基；越界一律返回 `TC_ERR_INVALID_ARG`（不静默忽略，便于发现布局 bug）
- 每次写入标记对应行为脏
- 绘制路径**零分配**
- `draw_text` 的 `attr` 为 NULL 时用 surface 默认属性；`mode` 用 `TC_TEXT_UNICODE` 或显式指定
- `out_consumed_columns` 可为 NULL

### 4.1 文本写入规则

| 情形 | 行为 |
| --- | --- |
| 宽度 1 簇 | 写 1 格 |
| 宽度 2 簇 | 写 2 格（首格 `TC_ATTR_WIDE_HEAD` + 内容，次格 `TC_ATTR_WIDE_CONT` + 空内容） |
| 宽度 0 簇（组合符） | 追加到前一格 `text`（受容量限制；超限则忽略该簇） |
| 覆盖宽字符半格 | 同时清除被覆盖格与其配对格 |
| 孤立 `WIDE_CONT` 在行首 | 清除为普通空格 |
| 超出右边界 | 默认停止写入（不回绕）；可选策略为返回 `TC_ERR_INVALID_ARG` |
| TAB | 按调用方策略：引擎返回 0 宽，`draw_text` 视作 1 个空格（可配置跳到下一个 tab 位） |
| 控制字符 | 不写入（跳过），避免把控制序列画进缓冲 |
| ASCII 模式 | 一字节一格，无簇合并、无宽字符 |

---

## 5. surface 缓冲、屏幕镜像与脏标记

| 缓冲 | 归属 | 用途 |
| --- | --- | --- |
| **cells（back）** | surface | 调用方绘制的目标（离屏网格，单份） |
| **front（屏幕镜像）** | **term** | 上次 present 成功后终端的已知状态 |
| **脏行位图** | surface | 每行脏标记；`fill_rect` / `draw_text` 等标记受影响行 |
| **行哈希数组** | surface + term | 每行一个 `uint64`，用于整行跳过（§5.1） |
| **行区间 span** | surface | 每行 `[x0, x1)` 有效列区间，用于横向裁剪（§5.2） |
| **outbuf** | term | 生成的转义序列；复用，按 2 倍增长 |

- surface 只持有自己的单元格缓冲；**front 与 outbuf 属于 term**，因此：
  - 多个 surface 共存不会重复占用屏幕镜像内存
  - 切换 present 的 surface 不需要"整屏重绘"（front 始终是屏幕的真实状态）
- `clear` 标记全屏脏
- front 为"未知"时（首帧、term 尺寸变化、resume）整屏脏

### 5.1 行哈希（row hash）：一次比较跳过整行

surface 的每一行、以及 term 的 front 每一行，各维护一个 `uint64_t` 行哈希：

```c
/* 对一行 cell 的全部有效字节做 FNV-1a 64；写入 cell 前必须规范化（未使用字节清零） */
uint64_t h = FNV1A64(cells + y * stride, (size_t)cols * sizeof(tc_cell));
```

| 项 | 约定 |
| --- | --- |
| 何时算 | **惰性**：写操作只置"该行 hash 失效"；present 的 diff 阶段对脏行各重算一次 |
| 怎么比 | 脏行先比 `row_hash(back,y) == row_hash(front,y)`：相等 → 清脏位、**跳过整行**（不进入逐格扫描） |
| 覆盖内容 | 参与计算的必须是 cell 全部有效字节（fg/bg/style/attr/len/text）；**padding 与 reserved 必须清零**，否则相同内容算出不同 hash |
| 收益 | "标脏但其实没变"的行从 O(行宽) 降到 O(1)：光标移动、悬停、重绘同内容等场景几乎零扫描 |
| 碰撞 | 64 位，碰撞概率可忽略；提供 `strict_diff`（见 `02-api-control.md`）关闭快路径，用于测试与排错 |
| 分配 | 行哈希数组随缓冲在 `create` / `resize` 时一次分配；运行期零分配 |

### 5.2 有效区间与脏矩形：只扫"有内容的列"

```
每行维护 span[x0, x1)：本行"被触及过"的列区间
  clear → 区间清空（整行可跳过）
  put_cell / fill_rect / draw_text → 取并集扩展区间
  diff 只遍历 [x0, x1)
```

| 项 | 约定 |
| --- | --- |
| 行区间 | 每行 2 个 `int32`；`x0 >= x1` 表示空行 → 直接跳过 |
| surface 脏矩形 | 所有脏行的包围盒；`tc_surface_dirty_rect()` 可查（自上次 present 起累积） |
| present 之后 | 脏位与区间清空（新的一帧重新累积）；行哈希保留 |
| 收益 | 稀疏画面（大量空白）扫描量接近 0 |
| 与行哈希的关系 | 互补：行哈希做"纵向剪枝"，有效区间做"横向剪枝" |

---

## 6. diff 算法

### 6.1 主循环

```
for y in 0..rows-1:
    if 行 y 未脏: continue                                      # ① 脏行剪枝
    if row_hash(back,y) == row_hash(front,y): 清脏位; continue   # ② 行哈希剪枝
    for x in span_x0(y) .. span_x1(y) - 1:                       # ③ 有效区间剪枝
        # 到这里才做逐格比较
        if back[y][x] == front[y][x]: continue
        # 找到变化起点 → 向后扩展出"同属性最长 run"
        run_end = 扩展：连续变化的 cell，且 fg/bg/style 相同
        定位光标（若不在 (x,y)）
        设置 SGR（若与当前不同）
        输出 run 的文本（跳过 WIDE_CONT）
        x = run_end
```

要点：

- **三级剪枝**：脏行 → 行哈希 → 有效区间，最后才做逐格比较；只有"内容真的可能变"的区间会被扫到
- 复杂度：O(脏行数 + Σ 有效区间宽度)，而不是 O(行数 × 行宽)
- **run 合并**：连续且属性相同的 cell 一次性输出，避免逐格设置 SGR
- **跳过宽字符续格**：`WIDE_CONT` 不产生输出
- **光标最小化**：维护虚拟光标位置；若下一 run 紧接上一 run 末尾，省略光标定位
- **SGR 最小化**：维护"当前 SGR 状态"，相同则不重复输出；变化时只输出差异部分

### 6.2 光标定位序列

- 需要跳转时输出 `ESC [ y ; x H`（1 基）
- 优化：若目标行相同列更小 → 用 `ESC [ n C`（右移）；行不同但列相同 → 用 `ESC [ n d`（行绝对）等
- 结束帧时把光标放到"调用方指定位置"（默认放在最后变化处或由 API 指定）

### 6.3 清除优化

若某行整行变为空白且长度超过阈值，直接输出 `ESC [ 2 K`（清行）比逐格写空格更短。
阈值与策略为内部实现细节（可配置）。

### 6.4 取舍：稠密网格 vs 稀疏存储 / 矢量指令

"只存有效单元格（稀疏）"或"记录矢量指令（display list，重绘时重放）"都能减少遍历。
本设计仍以**稠密 POD 网格**作为唯一权威数据，用 §5.1 / §5.2 的元数据做剪枝：

| 方案 | 评价 |
| --- | --- |
| 稀疏存储（只存非空格 + 索引） | 省内存；但破坏零拷贝帧视图的**连续行布局**（`stride` 索引、memcmp、外部工具直接读），收益主要集中在大面积空白场景 |
| 矢量指令 / display list（记录 `fill_rect` / `draw_text`，重绘时重放） | 引入指令缓冲（分配或固定容量上限）、重放开销，且"指令语义 ≠ 最终像素"会让调试与快照测试变复杂；图层复用是上层（widget 层）的事，本库不做布局 |
| **采用：稠密网格 + 行哈希 + 有效区间 + 脏矩形** | 遍历量已接近稀疏方案（三层剪枝后只剩真正变化的格子），同时保留 POD 布局、零拷贝视图、逐格 memcmp 与快照可测性，**运行期零分配** |

结论：遍历不是"无脑逐格"——脏行 → 行哈希 → 有效区间三层剪枝之后，
才对可能变化的格子做 32 字节 memcmp 与 run 合并扫描。

---

## 7. 输出生成与 outbuf

| 项 | 策略 |
| --- | --- |
| 缓冲 | outbuf 复用，按 2 倍增长，峰值不缩容 |
| 写入 | 帧末**单次 write**（避免撕裂）；大帧必要时分块但保持顺序 |
| 同步更新 | 能力支持时包裹 `ESC [ ? 2026 h` … `ESC [ ? 2026 l` |
| 光标隐藏 | 已在 `enter` 时隐藏；帧内不重复输出 |
| 颜色降级 | 生成前按能力位集降级（见 §8） |
| 失败处理 | write 失败 → 返回 `TC_ERR_IO`，**不更新 front**，下一帧自动重试 |

字节量估算：最坏情况每行一次光标定位（≈10B）+ 每 run 一次 SGR（≈20B）+ 文本（UTF-8 原字节）。
80×24 全屏重绘约 3–6 KB，典型局部更新远小于此。

---

## 8. 颜色与样式降级

降级发生在**生成序列之前**，由能力位集驱动（规则见 `03-api-capability.md` §3）：

- `TC_COLOR_RGB` → 真彩可用时 `ESC[38;2;r;g;b m`；否则降到 256 / 16 / 忽略
- 索引色 → 256 可用时 `ESC[38;5;idx m`；否则映射到 16
- 样式位：能力缺失则丢弃该位；`UNDERCURL` → `UNDERLINE`
- 不支持颜色（`NO_COLOR` / `MONO`）：只输出样式与文本

降级是**纯函数**（`tc_caps_downgrade_rgb` / `tc_caps_downgrade_indexed`），可单测与快照比对。

---

## 9. `tc_present`

```c
tc_status tc_present(tc_term_t* t, tc_surface_t* s);
```

流程：

1. 状态检查（term 必须 ACTIVE；surface 必须有效）
2. 尺寸校正（按策略取较小区域；不一致时按配置返回或裁剪）
3. diff → 生成序列到 outbuf（期间触发**变更回调**）
4. 广播 sink：`CHANGES`（已在 diff 阶段逐条回调）→ `BYTES`（本次要写出的字节）→ `FRAME`（帧视图）
5. 写真实终端（单次 write；同步更新模式包裹）
6. 成功则把本帧内容同步到 term 的 front（只同步脏行，不整屏复制），`frame_id++`；失败保留 front 不更新
7. 返回聚合状态

| 场景 | 返回 |
| --- | --- |
| 成功 | `TC_OK` |
| 部分 sink 失败 | `TC_ERR_SINK`（帧已写出，其它 sink 已收到） |
| 终端写入失败 | `TC_ERR_IO`（front 未更新，下帧重试） |
| 未 enter | `TC_ERR_STATE` |
| 尺寸不匹配且配置为严格 | `TC_ERR_INVALID_ARG` |
| 分配失败（控制面） | `TC_ERR_NOMEM` |

约束：

- `tc_present` 是**同步阻塞**的：返回时一帧已提交完毕
- 一次 present 只提交**一张最终 surface**；调用方应先在 surface 上完成本帧全部绘制
- 回调内**禁止**调用 `tc_present` 或写 surface
- 每帧零分配（outbuf 已在 create 时备好；增长只在极少数首帧发生，之后复用）

---

## 10. 增量变更回调

> **调试基础设施（§10–§12），先于渲染核心实现。** 多 sink 广播、变更回调与只读帧视图是
> 离线测试、快照比对、录制回放与外部工具集成的**基础**：没有它们，渲染正确性无法在
> CI / headless 下验证，真实终端的差异也无从诊断。因此实现顺序上，§10–§12 的观测通道
> 与 surface 绘制、diff 一起落地，而不是最后补。

```c
typedef enum tc_change_kind {
    TC_CHANGE_CELL   = 1,   /* 单个单元格变化 */
    TC_CHANGE_RECT   = 2,   /* 矩形区域变化（含新内容快照） */
    TC_CHANGE_SCROLL = 3,   /* 区域滚动（diff 识别出的整体位移；与绘制 API 无关） */
    TC_CHANGE_CLEAR  = 4,   /* 清屏 / 整行清空 */
    TC_CHANGE_CURSOR = 5    /* 光标位置变化 */
} tc_change_kind;

typedef struct tc_change {
    tc_change_kind kind;
    tc_rect  rect;          /* 受影响区域 */
    int32_t  dx, dy;        /* SCROLL 时的偏移 */
    const tc_cell* cells;   /* RECT 时的只读内容（行优先，stride 为 rect.w）；其余为 NULL */
    int32_t  stride;        /* RECT 时有效 */
    int32_t  cursor_x, cursor_y;  /* CURSOR 时有效 */
    uint64_t frame_id;
    uint32_t reserved[2];
} tc_change;

typedef void (*tc_change_fn)(const tc_change* ch, void* ud);
tc_status tc_set_change_sink(tc_term_t* t, tc_change_fn cb, void* ud);   /* ud 为 NULL / cb 为 NULL 表示注销 */
```

| 约定 | 说明 |
| --- | --- |
| 时机 | diff 阶段**同步**逐条回调（在写出字节之前） |
| 粒度 | 默认按 run 合并为 `TC_CHANGE_RECT`；单个 cell 变化用 `TC_CHANGE_CELL` |
| 数据所有权 | `cells` 为借用，仅回调期间有效；需要留存请拷贝 |
| 重入 | 回调内禁止 `tc_present`、禁止写 surface、禁止增删 sink |
| 失败 | 回调无返回值；异常由调用方自行处理（库不做保护） |
| 与输入事件回调的区别 | 输入事件**只拉取不回调**；此处是渲染变更，属用户明确要求的增量通知能力 |

---

## 11. 多 sink 广播

```c
typedef enum tc_sink_level {
    TC_SINK_BYTES   = 1,
    TC_SINK_CHANGES = 2,
    TC_SINK_FRAME   = 4
} tc_sink_level;

typedef struct tc_sink_vtable {
    void (*on_bytes)(void* ctx, const void* bytes, size_t len);
    void (*on_change)(void* ctx, const tc_change* ch);
    void (*on_frame)(void* ctx, const tc_frame_view* view);
    void (*dispose)(void* ctx);
} tc_sink_vtable;

tc_status tc_term_add_sink(tc_term_t* t, const tc_sink_vtable* vt, void* ctx,
                           unsigned levels, uint32_t* out_id);
tc_status tc_term_remove_sink(tc_term_t* t, uint32_t id);
```

| 规则 | 说明 |
| --- | --- |
| 顺序 | 按挂载顺序；同一帧所有 sink 看到同一份数据 |
| levels | 只投递声明的级别；可位或 |
| 内置终端 sink | 默认存在（`TC_SINK_BYTES`），headless 下自动不挂载，可显式禁用 |
| 失败隔离 | 单 sink 失败不影响其它；聚合为 `TC_ERR_SINK` |
| 未实现的回调 | 可为 NULL（跳过） |

典型：终端 sink + 捕获 sink（BYTES，用于快照测试）+ 帧 sink（FRAME，供外部工具拉全量）。

---

## 12. 只读帧视图

```c
typedef struct tc_frame_view {
    const tc_cell* cells;   /* 行优先，行距为 stride */
    int32_t cols, rows;
    int32_t stride;         /* 每行 cell 数（>= cols） */
    uint64_t frame_id;      /* 帧序号，用于自检是否失效 */
    uint32_t reserved[2];
} tc_frame_view;

tc_status tc_surface_view(const tc_surface_t* s, tc_frame_view* out);
```

| 约定 | 说明 |
| --- | --- |
| 零拷贝 | 直接指向 surface 的单元格缓冲，不复制 |
| 只读 | 调用方**不得**写入；写入属未定义行为 |
| 失效 | 下一次写 surface 或 `present` 后失效；用 `frame_id` 比对自检 |
| 生命周期 | 不需要释放；surface 销毁后无效 |
| 用途 | 全量帧快照、截图比对、外部工具转发（网络由外部实现） |

> 库只提供不可变视图，不做序列化、压缩与网络传输：外部工具（如 WebSocket 服务）自行取走数据。

---

## 13. 性能估算

| 场景 | 估算 |
| --- | --- |
| 单元格内存 | `tc_cell` ≈ 32 字节；200×50 的 surface ≈ 320 KB（单缓冲）；term 的屏幕镜像另占约 320 KB（全局仅一份） |
| 行元数据 | 200×50：行哈希 50×8 B + 行区间 50×8 B ≈ 0.8 KB（相对 cells 可忽略） |
| diff | 三层剪枝后 O(脏行数 + Σ 有效区间宽度)；最坏（全脏）约 10K 次 32 字节 memcmp ≈ 亚毫秒级，典型帧远小于此 |
| 输出 | 全屏重绘 80×24 ≈ 3–6 KB；200×50 ≈ 20–40 KB（罕见） |
| 写入 | 单次 write；终端侧解析为主要成本 |
| 分配 | 首帧可能有 outbuf 增长；之后每帧 0 次分配 |

优化清单（实现阶段）：

- **行哈希：整行一次比较即跳过**（标脏但未变的行）
- **有效区间 / 脏矩形：横向只扫触及过的列**
- 行级脏标记 + run 合并（主要手段）
- SGR 与光标状态记忆，避免重复输出
- 宽字符续格跳过
- 整行清空用 `ESC [ 2 K`
- outbuf 复用与批量写

---

## 14. 典型用法

```c
/* 1) 单个 surface：直接绘制，作为提交的最终帧 */
tc_surface_t* s = NULL;
tc_surface_create(term, cols, rows, &s);

tc_style_attr attr;
attr.fg = tc_color_rgb(0xE0, 0xE0, 0xE0);
attr.bg = tc_color_indexed(24);
attr.style = TC_STYLE_BOLD;

/* 2) 逐帧绘制（局部更新：只重画变化区域即可） */
tc_surface_draw_text(s, 0, 0, "终端 TUI", &attr, TC_TEXT_UNICODE, NULL);
tc_surface_put_cell(s, 5, 3, &(tc_cell){ .fg = tc_color_rgb(0xFF,0,0), .text = "X", .len = 1 });

/* 3) 提交：diff 只输出变化，单次 write */
tc_present(term, s);
```

复杂界面（状态栏、弹窗等）由调用方自行组织：可以在一个 surface 上直接拼装，
也可以维护多个 surface、把各自内容写入同一个最终 surface 后 present（本库不提供图层合成，
如何拼装是上层布局策略）。

---

## 15. API 汇总

```c
/* 生命周期 */
tc_status tc_surface_create(tc_term_t* t, int32_t cols, int32_t rows, tc_surface_t** out);
tc_status tc_surface_resize(tc_surface_t* s, int32_t cols, int32_t rows);
void      tc_surface_destroy(tc_surface_t* s);
tc_status tc_surface_get_size(const tc_surface_t* s, int32_t* cols, int32_t* rows);
tc_status tc_surface_set_text_mode(tc_surface_t* s, tc_text_mode mode);
tc_status tc_surface_set_ambiguous_wide(tc_surface_t* s, bool wide);

/* 绘制 */
tc_status tc_surface_put_cell(tc_surface_t* s, int32_t x, int32_t y, const tc_cell* c);
tc_status tc_surface_fill_rect(tc_surface_t* s, const tc_rect* r, const tc_cell* c);
tc_status tc_surface_clear(tc_surface_t* s);
tc_status tc_surface_draw_text(tc_surface_t* s, int32_t x, int32_t y, const char* utf8,
                               const tc_style_attr* attr, tc_text_mode mode,
                               int32_t* out_consumed_columns);

/* 提交与观测 */
tc_status tc_present(tc_term_t* t, tc_surface_t* s);
tc_status tc_surface_dirty_rect(const tc_surface_t* s, tc_rect* out);
tc_status tc_set_change_sink(tc_term_t* t, tc_change_fn cb, void* ud);
tc_status tc_term_add_sink(tc_term_t* t, const tc_sink_vtable* vt, void* ctx,
                           unsigned levels, uint32_t* out_id);
tc_status tc_term_remove_sink(tc_term_t* t, uint32_t id);
tc_status tc_surface_view(const tc_surface_t* s, tc_frame_view* out);

/* 辅助构造 */
tc_cell  tc_cell_blank(void);
tc_color tc_color_rgb(uint8_t r, uint8_t g, uint8_t b);
tc_color tc_color_indexed(uint8_t idx);
tc_color tc_color_default(void);
```
