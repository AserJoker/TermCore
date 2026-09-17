# 04 · 输入解析层 API

输入层把"字节流"变成"结构化事件"，并交给调用方主动拉取。
它不含事件循环、不做回调分发（回调只用于渲染变更，见 `09`）。

---

## 1. 职责

- 严格 UTF-8 解码（与文本引擎共用解码器）
- 转义序列状态机：CSI / SS3 / OSC / DCS、功能键、修饰键
- **完整鼠标支持**：按下 / 释放 / 拖动 / 移动 / 滚轮，SGR 1006 主路径，X10 兼容
- 焦点事件、括号粘贴、resize 事件
- 孤立 ESC 与转义序列的区分：**贪婪读取 + 零延迟结算**，库不阻塞、不忙等
- 注入队列与外部输入源接管
- 事件队列与溢出策略

不做：解析终端对查询的响应（那是能力层）、解释应用层语义（那是上层）。

---

## 2. 事件模型

```c
typedef enum tc_event_kind {
    TC_EV_KEY    = 1,
    TC_EV_MOUSE  = 2,
    TC_EV_RESIZE = 3,
    TC_EV_FOCUS  = 4,
    TC_EV_PASTE  = 5,
    TC_EV_QUIT   = 6     /* 可选：SIGTERM / CTRL_BREAK 等退出请求（需开启） */
} tc_event_kind;

typedef struct tc_event {
    tc_event_kind kind;
    uint32_t      seq;          /* 单调递增序号，便于测试与去重 */
    union {
        struct {
            uint32_t codepoint;     /* 可打印字符的 Unicode 码点；无则为 0 */
            tc_key   key;           /* 功能键枚举；无则为 TC_KEY_NONE */
            uint16_t mods;          /* TC_MOD_* 位或 */
            bool     is_release;    /* 按键释放（仅 Kitty 协议可区分） */
        } key;

        struct {
            int32_t          x, y;        /* 0 基单元格坐标 */
            tc_mouse_button  button;
            tc_mouse_action  action;
            uint16_t         mods;
        } mouse;

        struct {
            int32_t cols, rows;
            int32_t pixel_w, pixel_h;     /* 不可用时为 0 */
        } resize;

        struct {
            bool focused;
        } focus;

        struct {
            const char* text;             /* 借用，有效期至下一次取事件 */
            size_t      len;
        } paste;

        struct {
            int32_t reason;               /* TC_QUIT_SIGTERM / TC_QUIT_SIGHUP / TC_QUIT_CTRL_BREAK */
        } quit;
    } u;
    uint32_t reserved[2];
} tc_event;
```

设计说明：

- **结构化 + 联合**：不用 `wParam / lParam`；每个事件只有自己需要的字段
- `paste.text` 是**借用指针**，下一次取事件（或 `present`）后失效，需要留存请拷贝
- `seq` 便于测试断言与"是否处理过"的判断
- POD 布局固定，字段只追加

---

## 3. 键码与修饰键

```c
typedef enum tc_key {
    TC_KEY_NONE = 0,
    /* 编辑键 */
    TC_KEY_ENTER, TC_KEY_TAB, TC_KEY_BACKSPACE, TC_KEY_ESCAPE, TC_KEY_DELETE, TC_KEY_INSERT,
    TC_KEY_HOME, TC_KEY_END, TC_KEY_PAGE_UP, TC_KEY_PAGE_DOWN,
    /* 方向键 */
    TC_KEY_UP, TC_KEY_DOWN, TC_KEY_LEFT, TC_KEY_RIGHT,
    /* 功能键 */
    TC_KEY_F1, TC_KEY_F2, /* … */ TC_KEY_F12,
    /* 小键盘（可区分时） */
    TC_KEY_KP_0, /* … */ TC_KEY_KP_ENTER,
    /* 修饰键自身 */
    TC_KEY_SHIFT, TC_KEY_CTRL, TC_KEY_ALT, TC_KEY_SUPER,
    TC_KEY_COUNT
} tc_key;

typedef enum tc_mod {
    TC_MOD_NONE  = 0,
    TC_MOD_SHIFT = 1u << 0,
    TC_MOD_ALT   = 1u << 1,   /* Meta / Option */
    TC_MOD_CTRL  = 1u << 2,
    TC_MOD_SUPER = 1u << 3,   /* Win / Cmd */
    TC_MOD_HYPER = 1u << 4,
    TC_MOD_META  = 1u << 5,
    TC_MOD_CAPS  = 1u << 6,
    TC_MOD_NUM   = 1u << 7
} tc_mod;
```

编码约定：

| 输入 | `codepoint` | `key` | 示例 |
| --- | --- | --- | --- |
| 可打印字符 | 字符码点 | `TC_KEY_NONE` | `a` → `codepoint='a'`；`中` → `U+4E2D` |
| 控制字符 | 控制码 | `TC_KEY_NONE` | Ctrl-C → `0x03` + `TC_MOD_CTRL` |
| Enter / Tab / Backspace | 0 | 对应枚举 | |
| 功能键 / 方向键 | 0 | 对应枚举 | `↑` → `TC_KEY_UP` |
| 修饰键自身 | 0 | `TC_KEY_SHIFT` 等 | 仅 Kitty 协议下可产生 |

- Shift + 字母：由终端给出的结果码点决定（`A`），同时置 `TC_MOD_SHIFT`
- Ctrl + 字母：按 ASCII 控制码（如 Ctrl-A = 0x01），并置 `TC_MOD_CTRL`
- Alt（Meta）+ 字符：多为 `ESC` + 字符，解析器在同一批次（或结算窗口）内合并为 `TC_MOD_ALT`
- 无法区分的组合（传统协议盲区）：按最接近的可用结果给出，`mods` 不臆造

---

## 4. 拉取三原语

```c
tc_status tc_wait_event(tc_term_t* t, int32_t timeout_ms, bool* ready);
tc_status tc_peek_event(tc_term_t* t, tc_event* out, bool* has);
tc_status tc_get_event (tc_term_t* t, tc_event* out, bool* has);
```

### 4.1 核心约定：库不睡眠、不忙等

| 约定 | 说明 |
| --- | --- |
| **不 sleep** | 库内部永不主动睡眠或自旋等待"可能的后续字节"；`timeout_ms = 0` 时立即返回 |
| **不忙等** | 需要等待时只用 OS 级等待原语（由内核挂起），不消耗 CPU |
| **节奏归调用方** | 主循环的帧间隔（sleep 16ms 之类）由**调用方自己**决定；库只负责"有就给，没有就立刻说没有" |
| **无后台线程** | 没有读取线程、没有定时器线程；所有解析与结算发生在拉取路径 |

理由：终端输入是突发、稀少的。若库为了消解 ESC 歧义而阻塞 100ms，会让渲染停摆 100ms、
动画与 resize 响应变卡。正确做法是"零延迟读 + 残留缓冲 + 调用方 sleep"，
既没有忙等（调用方 sleep 由 OS 挂起），又能把 ESC 的实际延迟压到一帧以内。

### 4.2 语义

| 函数 | 语义 |
| --- | --- |
| `tc_wait_event` | `timeout_ms = 0`：**非阻塞**，尝试读一批字节并解析，有事件则 `ready = true`，否则立刻返回 `false`。`> 0`：OS 级等待字节到达（不忙等）或残留序列到期，取更早者。`< 0`：无限等待（仅适合纯等待型应用，不要在需要渲染的循环里用）。超时返回 `TC_OK` + `ready = false` |
| `tc_peek_event` | 不取走；有事件则填充 `out` 并 `has = true` |
| `tc_get_event` | 取走一条；无事件返回 `TC_OK` + `has = false` |

行为细节：

- 三者在内部会**先尝试读取并解析**：若队列为空且输入源有数据，会贪婪读到空（见 §4.4）喂给解析器，能产出事件就返回
- 一次 `tc_get_event` 最多产出一条事件；一批字节可能产出多条，留待下次拉取
- `tc_wait_event` 内部会惰性刷新尺寸 → 可能产出 `TC_EV_RESIZE`
- 存在"待结算残留"（如孤立 ESC）时，`tc_wait_event` 的**有效超时取 `min(timeout_ms, 剩余到期时间)`**，
  避免睡过头后才结算
- 拉取路径**零分配**（解析器缓冲固定）

### 4.3 推荐循环（调用方自己 sleep）

```c
for (;;) {
    /* 1. 贪婪抽干：一帧内把已到达事件全部处理完（库只管入队尾，取多少由调用方决定） */
    tc_event ev; bool has = false;
    while (tc_get_event(term, &ev, &has) == TC_OK && has) {
        switch (ev.kind) { /* TC_EV_KEY / TC_EV_MOUSE / ... */ }
    }

    /* 2. 渲染并提交 */
    render(app, surface);
    tc_present(term, surface);

    /* 3. 帧间隔由调用方控制：库不 sleep，这里自己睡 */
    sleep_ms(16);          /* 约 60fps；也可按"无事件则更长"动态调整 */
}
```

> `tc_wait_event` 在这种循环里是**可选**的：只有希望在空闲时省电（用 OS 等待代替固定 sleep）
> 才用它，且通常传 `0` 或较小的超时。典型交互应用直接用 `tc_get_event` + 自己 sleep 即可。

### 4.4 贪婪读取（greedy read）

一次拉取不是"读一次"，而是**读到空为止**：

```c
/* 概念代码：库内部 */
for (;;) {
    size_t n = 0;
    tc_status st = src->read(ctx, buf, sizeof buf, &n);   /* 非阻塞 */
    if (st != TC_OK || n == 0) break;                     /* 无数据 / EAGAIN → 停止 */
    feed(parser, buf, n);                                 /* 立即喂给状态机 */
}
```

| 项 | 约定 |
| --- | --- |
| 目的 | 把"本次已到达内核缓冲的所有字节"一次取干，使终端单次 `write` 提交的完整序列（如 `ESC [ A`）落在同一批次 |
| 退出条件 | `read` 返回 `n = 0`（无数据 / `EAGAIN`）、出错、或达到单批上限 |
| 绝不阻塞 | 每一次 `read` 都是非阻塞的；读到空立刻返回，**不为"还没到达的字节"等待** |
| 单批上限 | 默认 4096 字节（可配）；超出部分留在内核缓冲，下次拉取继续，避免超长粘贴独占一帧 |
| 自定义 source | `read` 返回 `n > 0` 时库会**再次调用**；实现方可任意切分，返回 `0` 表示"这批没了" |

配合"库不 sleep"，贪婪读取把 ESC 误判压到极低：终端一次按键通常以单次 `write` 提交，
贪婪读取会把它整体拿到，无需任何等待即可完整解析。只有当字节确实分成两次到达
（高延迟 ssh、慢速终端、极端调度）时才可能残留，此时才轮到 §11 的最后期限机制。

### 4.5 队列消费模型：拉取 = 填充 + 出队

库自持一个**事件队列**（见 §13），它是所有事件的唯一出口。一次拉取分两步：

```
1) 填充（drain-in）                    2) 出队（drain-out）
   按优先级取字节 → 贪婪读到空              队头事件出队
   → 解码 → 状态机 → 产出事件               get：取走
   → 追加到队尾                             peek：只看不取
   （注入字节 / 注入事件 / 内部生成的 resize 同样追加到队尾）
```

```c
/* 概念代码：库内部 */
static tc_status poll_one(tc_term_t* t, tc_event* out, bool take, bool* has) {
    fill(t);                       /* 贪婪读取 + 解析 + 入队尾；也处理注入与 resize */
    if (queue_empty(&t->q)) { *has = false; return TC_OK; }
    if (take) queue_pop(&t->q, out); else queue_front(&t->q, out);
    *has = true; return TC_OK;
}
```

| 要点 | 说明 |
| --- | --- |
| 同批多事件按序入队 | 系统缓冲里同时到达 `ESC` 与 `ESC [ A`（Esc 键 + 上方向键）→ 状态机按字节顺序产出两个事件 → 依次追加到队尾；出队顺序与按键顺序**严格一致** |
| 一批字节可产出多条 | 这是常态（如一次粘贴 + 后续按键）；多余事件留在队里等下次取 |
| 调用方要贪婪抽干 | 一帧内 `while (tc_get_event(term, &ev, &has) == TC_OK && has)` 直到 `has = false`；库不回调、不主动丢弃 |
| 不抽干的代价 | 队尾堆积；只有超过 `event_queue_capacity` 时才触发溢出策略 |
| 事件频率有限 | 事件只来自用户交互（键鼠 / 粘贴）与少量内部事件（resize / focus），**不是持续高吞吐外部 IO**；因此"每帧抽干"开销恒定且极小，默认 256 容量对正常交互远远够用 |
| 零分配 | 填充与出队全程零分配（解析器缓冲与队列在 `create` 时已备好） |

示例：系统缓冲中同时有孤立 `ESC` 与上方向键序列 `ESC [ A`

```text
[1B] [1B 5B 41]                 ← 贪婪读取一次取干
  ├─ 孤立 ESC（其后紧跟 1B，不构成已知引导）→ TC_KEY_ESCAPE   入队尾 #1
  └─ ESC [ A                                → TC_KEY_UP      入队尾 #2
调用方连取两次：先 ESC，再 UP —— 无等待、无歧义、顺序正确
```

---

## 5. 输入源与注入

### 5.1 优先级

```
事件队列（已解析）
   ↑
注入队列：先字节（走解析）后事件（直接入队）
   ↑
外部 source（tc_term_set_input_source 设置后生效）
   ↑
默认终端源（后端句柄）
```

- 注入的**字节**走完整解析路径（覆盖真实解析器，测试价值高）
- 注入的**事件**绕过解析，直接入事件队列（用于无法用字节表达的场景或加速用例）
- 外部 source 一旦设置，库**不再读真实终端**；传 `NULL` 恢复
- 切换 source 不丢弃已入队事件
- 拉取时先按此优先级**填充队尾**，再从队头取（两阶段见 §4.5）

### 5.2 API

```c
tc_status tc_term_inject_input(tc_term_t* t, const void* bytes, size_t len);
tc_status tc_term_inject_event(tc_term_t* t, const tc_event* ev);
tc_status tc_term_set_input_source(tc_term_t* t, const tc_input_source_vtable* vt, void* ctx);

typedef struct tc_input_source_vtable {
    tc_status (*wait_ready)(void* ctx, int32_t timeout_ms, bool* ready);
    tc_status (*read)(void* ctx, void* buf, size_t cap, size_t* nread);
    void      (*dispose)(void* ctx);
} tc_input_source_vtable;
```

契约：

| 项 | 约定 |
| --- | --- |
| 切分自由 | 注入字节可任意切分（逐字节也行），解析器必须正确处理 |
| `read` 返回 0 | 表示"这批没了"（不是 EOF）；库随即结束本轮贪婪读取 |
| `read` 返回 n > 0 | 库会**再次调用** `read`（贪婪读取，见 §4.4）；实现方可任意切分 |
| `wait_ready` 语义 | 同 `tc_wait_event`；超时返回 `TC_OK` + `ready=false`；**不得忙等** |
| `dispose` | 在替换 source、`tc_term_destroy` 时调用一次 |
| 回调禁止 | `read` / `wait_ready` 内不得调用其它会触发拉取或 present 的 API |
| 注入事件校验 | `kind` 非法或字段越界 → `TC_ERR_INVALID_ARG`，事件不入队 |
| 容量 | 注入队列有上限，超出返回 `TC_ERR_OVERFLOW`（调用方应放慢注入） |

---

## 6. 解析器状态机

```
            ┌─────────┐  ESC(0x1B)   ┌─────────┐
   ────────►│ GROUND  ├─────────────►│  ESC    │
            └────┬────┘              └────┬────┘
                 │                        │ '[' → CSI；'O' → SS3；']' → OSC；'P' → DCS
                 │ UTF-8 序列             ▼
            ┌────▼─────┐            ┌──────────┐
            │ UTF8_SEQ │            │ CSI/SS3/ │
            └──────────┘            │ OSC/DCS  │
                                    └────┬─────┘
                                         │ 终结字节 → 产出事件 → 回到 GROUND
```

| 状态 | 内容 |
| --- | --- |
| `GROUND` | 普通字节。可打印 → 走 UTF-8 解码；C0 控制符 → 按键事件 |
| `UTF8_SEQ` | 收集多字节序列（不完整时保留残留，等下一批数据） |
| `ESC` | 判别：同批次内跟 `[` `O` `]` `P` 等 → 进序列；批次结尾的孤立 ESC → 残留待结算（**不等待**） |
| `CSI` / `SS3` | 收集参数字节（`0–9 ; : < = > ?`）直到终结语（`A–Z a–z ~ @` 等） |
| `OSC` / `DCS` | 收集字符串直到 `BEL` 或 `ESC \`；只识别需要的（标题回显等），其余丢弃 |

鲁棒性规则：

| 规则 | 行为 |
| --- | --- |
| 参数个数上限 | 超过（如 16 个）则放弃该序列，回到 GROUND |
| 数值上限 | 单个参数值超过上限则钳制，不溢出 |
| 缓冲上限 | 序列字节超过上限（默认 256）→ 丢弃并回到 GROUND，日志诊断 |
| 非法终结语 | 丢弃序列，回到 GROUND（不把中间字节当文本吐出） |
| 残留到期 | 批次结尾的孤立 ESC 超过 `esc_timeout_ms`（默认 100ms）仍无后续 → 结算为 ESC 按键事件；等待期间**不阻塞** |
| 结束冲刷 | `leave` / `destroy` 时不冲刷（避免误产生事件） |

---

## 7. UTF-8 解码集成

- 解析器调用文本引擎的 `tc_text_utf8_next`（同一份实现，行为与渲染侧完全一致）
- `TC_UTF8_INCOMPLETE` → 保留残留在解析缓冲，等待更多字节
- `TC_UTF8_INVALID` → 产出 codepoint `U+FFFD` 的按键事件（不静默丢弃），并把状态置为 `TC_ERR_ENCODING` 供日志
- ASCII 模式下不做多字节解码：每个字节直接作为一个 7bit 字符事件（非 ASCII 字节按替换字符）
- 组合字符与 emoji：单个按键事件携带的是**一个码点**；字素簇的合并是渲染层的事（输入层不合并簇）

---

## 8. 键序列映射

### 8.1 传统 CSI / SS3

| 序列 | 事件 |
| --- | --- |
| `ESC [ A` / `ESC O A` | `TC_KEY_UP` |
| `ESC [ 1 ; Pm A` | 方向键 + 修饰（Pm 见下） |
| `ESC [ 1 ~` / `ESC [ 7 ~` / `ESC [ 2 ~` | `HOME` / `HOME` / `INSERT` |
| `ESC [ 3 ~` / `ESC [ 5 ~` / `ESC [ 6 ~` | `DELETE` / `PAGE_UP` / `PAGE_DOWN` |
| `ESC [ 11 ~` … `ESC [ 15 ~` | `F1` … `F5` |
| `ESC [ 17 ~` … `ESC [ 21 ~` | `F6` … `F10` |
| `ESC [ 23 ~` / `ESC [ 24 ~` | `F11` / `F12` |
| `ESC [ Z` | `TC_KEY_TAB` + `TC_MOD_SHIFT` |
| `ESC` + 字符（同批次 / 结算窗口内） | 该字符 + `TC_MOD_ALT` |

**xterm 修饰参数**：`Pm = 1 + (Shift?1:0) + (Alt?2:0) + (Ctrl?4:0) + (Meta?8:0)`；
`CSI 27 ; Pm ~` 形式同理。解析器按此拆出 `TC_MOD_*` 位。

### 8.2 Kitty 键盘协议（渐进）

启用：`ESC [ > 1 u`（渐进式）。解析：`CSI [ <unicode-key-code> [ : <shifted> [ : <base> ] ] [ ; <mods> ] [ <event-type> ] u`

| 字段 | 用法 |
| --- | --- |
| unicode-key-code | 直接给出码点 → 可区分 Ctrl+I 与 Tab、Ctrl+M 与 Enter |
| shifted / base | 区分 Shift 产生的不同字符 |
| mods | 位集比传统更丰富（含 Super / Hyper / Meta / CapsLock / NumLock） |
| event-type | `1` 按下（默认）、`2` 重复、`3` 释放 → 置 `is_release` |

降级：

| 情形 | 行为 |
| --- | --- |
| 终端不支持（能力位缺失或探测失败） | 不写启用序列，走传统解析 |
| 写了启用序列但收到的是传统序列 | 自动按传统规则解析（两种解析并存） |
| 只支持部分级别 | 按实际收到的字段解析，缺失字段按传统语义补全 |
| 释放事件缺失 | `is_release` 恒为 `false` |

> 上层不应假设"一定能收到释放事件"：应用需按能力位集自行处理。

### 8.3 modifyOtherKeys（xterm）

`CSI > 4 ; Pm m`：可让普通字符带修饰上报。能力位 `TC_CAP_MODIFY_OTHER` 存在时启用，解析规则与 Kitty 分开实现，产出同样的 `tc_event`。

---

## 9. 鼠标

> 鼠标**是否上报**由控制层的运行时开关决定：`tc_term_set_mouse_mode` /
> `tc_term_set_feature(TC_FEATURE_MOUSE)`（见 `02` §4）。
> 开关关闭期间到达的鼠标序列**仍被识别**（不误解析为按键），但**不产出事件**；
> 档位（CLICK / DRAG / MOTION）决定 `TC_MOUSE_DRAG` / `TC_MOUSE_MOVE` 是否出现。

### 9.1 协议

| 模式 | 序列 | 说明 |
| --- | --- | --- |
| X10（1000） | `ESC [ M Cb Cx Cy` | 坐标上限 223，`Cb` 低 2 位表按钮 |
| SGR（1006） | `ESC [ < Cb ; Cx ; Cy M/m` | **v1 主路径**；`M` 按下 / `m` 释放；坐标无 223 限制 |
| 拖动（1002） | 按下后移动上报 | `action = TC_MOUSE_DRAG` |
| 任意移动（1003） | 无按键也上报 | `action = TC_MOUSE_MOVE` |
| 像素坐标（1016） | 可选 | v1 不启用，保留解析能力 |

### 9.2 事件映射

```c
typedef enum tc_mouse_action {
    TC_MOUSE_DOWN = 1,
    TC_MOUSE_UP,
    TC_MOUSE_DRAG,
    TC_MOUSE_MOVE,
    TC_MOUSE_WHEEL_UP,
    TC_MOUSE_WHEEL_DOWN
} tc_mouse_action;

typedef enum tc_mouse_button {
    TC_MOUSE_NONE   = 0,
    TC_MOUSE_LEFT   = 1,
    TC_MOUSE_MIDDLE = 2,
    TC_MOUSE_RIGHT  = 3,
    TC_MOUSE_WHEEL  = 4,
    TC_MOUSE_X1     = 5,
    TC_MOUSE_X2     = 6
} tc_mouse_button;
```

| 输入 | 映射 |
| --- | --- |
| SGR `0 M` | LEFT + DOWN |
| SGR `0 m` | LEFT + UP |
| SGR `32 M/m` | LEFT + DRAG / MOVE（按是否处于按下态区分：按下态为 DRAG） |
| SGR `64 M` | WHEEL_UP（无释放事件） |
| SGR `65 M` | WHEEL_DOWN |
| SGR 按钮号 + 32 | 移动（无按键时为 MOVE） |
| 修饰位（`4` Shift / `8` Alt / `16` Ctrl） | 拆出到 `mods` |

坐标处理：

- 终端坐标为 **1 基**，库统一转换为 **0 基**
- 超出当前尺寸的坐标：裁剪到 `[0, cols-1]` / `[0, rows-1]`，并在日志中记录（不丢弃事件）
- 负坐标或解析异常 → 丢弃该事件（返回 `TC_ERR_PARSE`，不中断）

拖动状态：库维护"当前按下的按钮"，用于把 MOVE 归类为 DRAG；`enter` 重置该状态。

---

## 10. 焦点与粘贴

> 两者同样是运行时开关（`TC_FEATURE_FOCUS_EVENTS` / `TC_FEATURE_BRACKETED_PASTE`，见 `02` §4），
> 运行期可开可关；关闭期间对应序列被识别但**不产出事件**（未启用括号粘贴时，
> 粘贴表现为一串普通按键事件，应用无法区分，属预期）。

| 特性 | 序列 | 事件 |
| --- | --- | --- |
| 焦点进入 / 离开 | `ESC [ I` / `ESC [ O` | `TC_EV_FOCUS { focused = true/false }` |
| 括号粘贴开始 | `ESC [ 200 ~` | 进入粘贴态 |
| 括号粘贴结束 | `ESC [ 201 ~` | `TC_EV_PASTE { text, len }` |

粘贴处理规则：

- 粘贴态内的字节**不解析为按键**，原样累积（保证内容不被误解释）
- 允许包含换行、制表符等控制字符（原样保留）
- 超大粘贴（超过内部上限）：截断并在日志告警，`len` 反映实际长度
- 未启用括号粘贴时：粘贴表现为一串普通按键事件（应用无法区分，属预期）
- 粘贴内容不以 `\0` 结尾；`text` 借用，下一次取事件后失效
- 粘贴结束但内容为空 → 仍产生 `TC_EV_PASTE`（`len = 0`）

---

## 11. ESC 歧义与结算

孤立 `ESC`（用户按 Esc 键）与 `ESC` 引导的序列天然有歧义。处理原则：
**先靠贪婪读取消歧（零延迟），再靠最后期限兜底（不阻塞）**。

| 场景 | 处理 |
| --- | --- |
| `ESC` 后紧跟 `[` / `O` 等（同一批次内） | 直接进入序列解析，**无任何等待** |
| 批次结尾是孤立 `ESC` | 保留在残留缓冲，本次拉取立即返回（不 sleep、不等待） |
| 下次拉取有新字节 | 与残留拼接解析（`ESC a` → `Alt+A`；`ESC [ A` → `Up`） |
| `ESC` 后紧跟 `ESC` / C0 控制符 | 立即结算前一个 ESC（这类字节不可能构成序列，**无需等待**，例如 `[ESC][ESC[A]` → 先 ESC 再 Up） |
| 下次拉取仍无新字节，且已过最后期限 | 产出 `TC_KEY_ESCAPE` |
| 调用方主动结算 | `tc_term_flush_pending(t)` 立刻结算残留（退出前、或应用已自己等够） |
| `esc_timeout_ms = 0` | 立即结算：孤立 ESC 当次即产出（延迟最低，慢速终端可能误判） |
| 用户在窗口内按两次 ESC | 产出两个 `TC_KEY_ESCAPE` |

结算时机：

- 最后期限（`esc_timeout_ms`，默认 100ms）只在**拉取路径**判定：`now >= 残留到达时间 + esc_timeout_ms`
- 每次 `tc_get_event` / `tc_peek_event` / `tc_wait_event` 都顺带检查 → 实际延迟 ≈ 调用方帧间隔
- `tc_wait_event` 在有残留时，有效超时取 `min(timeout_ms, 剩余到期时间)`，不会睡过头
- 不做后台定时器（保持单线程、无线程）

体验对比：按 Esc 后最多一个帧间隔（如 16ms）就拿到 `TC_KEY_ESCAPE`，期间渲染照常推进；
传统的"阻塞 100ms 等后续字节"会让界面停顿 100ms。
又因为有贪婪读取，绝大多数序列在同一批次内完整到达，最后期限机制
只在字节被真正分片（高延迟 ssh / 慢速终端）时才会用到。

```c
/* 立即结算残留（不完整序列 / 孤立 ESC），返回是否产出事件 */
tc_status tc_term_flush_pending(tc_term_t* t, bool* has);
```

---

## 12. resize 事件

- 触发：平台 resize 通知（如 `SIGWINCH`） / headless 下 `tc_term_set_size`
- 信号只置标志；事件在拉取路径生成
- 事件带 `cols` / `rows`，像素尺寸可用时带 `pixel_w` / `pixel_h`，否则为 0
- 同一尺寸的重复变化会被**合并**（避免刷屏式 resize 产生大量事件）
- 应用应据此 `tc_surface_resize` 并重绘

---

## 13. 事件队列与溢出

事件队列是**所有事件的唯一出口**（消费模型见 §4.5）。

| 项 | 策略 |
| --- | --- |
| 定位 | 解析产出、`tc_term_inject_event` 直投、内部生成（resize 等）**一律按到达顺序 FIFO 入队尾**；调用方从队头取 |
| 入队来源 | ① 解析输入字节（一批可产出多条）② 注入事件 ③ 内部生成（resize 在拉取时惰性检测） |
| 出队义务 | 调用方应在一帧内贪婪抽干；不抽干只会堆积，不会阻塞也不会回调 |
| 结构 | 固定容量环形缓冲（`event_queue_capacity`，默认 256） |
| 默认溢出策略 | `TC_OVERFLOW_DROP_OLDEST`：丢弃最旧，保证新事件 |
| 可选策略 | `TC_OVERFLOW_DROP_NEWEST`：丢弃最新，返回 `TC_ERR_OVERFLOW` |
| 分配 | 队列在 `create` 时分配，运行期零分配 |
| 生命周期 | `paste.text` 指向库内缓冲，下一次取事件失效 |

---

## 14. 返回码与诊断

| 场景 | 返回 |
| --- | --- |
| 正常取到 / 无事件 | `TC_OK`（`has` 区分） |
| 等待超时 | `TC_OK` + `ready=false` |
| 队列溢出（DROP_NEWEST 策略） | `TC_ERR_OVERFLOW` |
| 注入参数非法 | `TC_ERR_INVALID_ARG` |
| 注入队列满 | `TC_ERR_OVERFLOW` |
| 外部 source 报错 | `TC_ERR_SOURCE` |
| 遇到非法序列 / 非法 UTF-8 | `TC_OK`（已恢复并产出事件），诊断写日志 `TC_LOG_WARN` |
| 未 `enter` 就拉取 | `TC_ERR_STATE` |

诊断建议：开启 `TC_LOG_TRACE` 可打印每个字节与状态机转移，用于定位"某终端不识别某键"的问题。

---

## 15. 测试用例清单

| 类别 | 用例 |
| --- | --- |
| 基本按键 | 可打印字符、Enter / Tab / Backspace / ESC、各功能键、方向键 |
| 修饰键 | Shift / Alt / Ctrl / Super 组合；xterm 修饰参数；Kitty 协议三种事件类型 |
| 协议降级 | 同时喂传统序列与 Kitty 序列；只喂传统序列；畸形 Kitty 序列 |
| 鼠标 | SGR 各按钮、释放、拖动、移动、滚轮、修饰键、X10 兼容、坐标 1 基转换、越界裁剪、223 以上坐标 |
| 粘贴 | 含换行 / 制表符 / UTF-8 / 超大内容 / 空粘贴 / 未启用括号粘贴 |
| ESC 结算 | 孤立 ESC、ESC+字符、双 ESC、`esc_timeout_ms` 为 0 / 100ms；断言"单次拉取耗时不随超时增长"（不阻塞） |
| 队列顺序 | 同批 `[ESC][ESC[A]` → 依次产出 ESC、Up；一批字节产出多事件时按序出队；长时间不抽干才触发溢出策略 |
| 贪婪读取 | 一次 write 的序列被拆成多次 read 仍解析正确；读到空即停；超过单批上限的剩余字节下次继续 |
| UTF-8 | 合法多字节、跨块切分（逐字节喂）、非法序列、overlong、代理区、孤立续字节 |
| resize | 尺寸变化合并、像素尺寸有无 |
| 注入与接管 | 注入字节 / 事件、source 接管与恢复、source 报错、优先级 |
| 鲁棒性 | 超长序列、非法终结语、随机 fuzz（不崩溃、不越界、不泄漏） |

---

## 16. API 汇总

```c
/* 拉取（零延迟：读到空即返，不 sleep、不忙等） */
tc_status tc_wait_event(tc_term_t* t, int32_t timeout_ms, bool* ready);
tc_status tc_peek_event(tc_term_t* t, tc_event* out, bool* has);
tc_status tc_get_event (tc_term_t* t, tc_event* out, bool* has);
tc_status tc_term_flush_pending(tc_term_t* t, bool* has);   /* 立即结算残留（孤立 ESC / 不完整序列） */

/* 注入与接管 */
tc_status tc_term_inject_input(tc_term_t* t, const void* bytes, size_t len);
tc_status tc_term_inject_event(tc_term_t* t, const tc_event* ev);
tc_status tc_term_set_input_source(tc_term_t* t, const tc_input_source_vtable* vt, void* ctx);

/* 辅助（可直接单测） */
tc_status tc_key_name(tc_key k, const char** out);       /* 返回静态字符串，便于打印与测试 */
tc_status tc_mods_name(uint16_t mods, char* buf, size_t cap);
```
