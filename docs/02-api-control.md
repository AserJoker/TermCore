# 02 · 控制层 API（`tc_term_t`）

控制层负责"终端状态的进出与安全恢复"：会话的创建与销毁、进入与离开、尺寸与光标、异常路径下的恢复。

---

## 1. 职责

- 创建与销毁终端会话句柄
- 进入 / 离开 TUI 状态（raw mode、alternate screen、鼠标、焦点、光标）
- 光标显隐与形状、光标位置
- 尺寸查询（行列与像素）与 resize 事件
- 信号 / 控制台事件转成事件（SIGWINCH、CTRL_C、退出请求）
- 窗口标题
- 进程退出与崩溃时的终端恢复
- headless（无 TTY）运行模式
- 文本模式（ASCII / Unicode）设置

明确不做：绘制（属渲染层）、事件分发（调用方）、字体与终端选择（用户）。

---

## 2. 句柄与 options

```c
typedef struct tc_term tc_term_t;

typedef struct tc_term_options {
    /* —— 运行时开关的初始值（运行期可用 tc_term_set_feature 改，见 §4） —— */
    bool raw_mode;              /* 默认 true  */
    bool alternate_screen;      /* 默认 true  */
    tc_mouse_mode mouse_mode;   /* 默认 TC_MOUSE_OFF：见 §4.1 */
    bool focus_events;          /* 默认 false */
    bool bracketed_paste;       /* 默认 true  */
    bool kitty_keyboard;        /* 默认 true（探测失败自动降级） */
    bool hide_cursor;           /* 默认 true  */
    bool capture_ctrl_c;        /* 默认 true：CTRL_C 作为事件而非终止进程 */
    bool sync_update;           /* 默认 true：能力支持时启用同步更新模式 */

    /* —— 初始化属性：create 时读取并冻结，运行期不可改 —— */
    bool headless;              /* 默认 false：无 TTY 也能创建与运行 */
    bool query_capabilities;    /* 默认 true：DA 查询；可关（CI / 非 TTY / 测试） */
    const tc_caps_profile* caps_profile;  /* 默认 NULL：应用传入已存盘的特性列表，直接采用，跳过推断与查询（见 03 §10） */
    bool strict_diff;           /* 默认 false：关闭行哈希快路径，逐格比对（排错 / 测试用，见 06 §5.1） */
    tc_text_mode text_mode;     /* 默认 TC_TEXT_UNICODE */
    bool ambiguous_wide;        /* 默认 false：东亚环境下可设 true */

    /* —— 时间与容量（同为初始化属性） —— */
    int32_t capability_timeout_ms;  /* 默认 200 */
    int32_t esc_timeout_ms;         /* 默认 100：孤立 ESC 的结算最后期限（不阻塞，见 04 §11） */
    int32_t read_batch_max_bytes;   /* 默认 4096：单次拉取的贪婪读取上限；0 表示用默认值 */
    int32_t event_queue_capacity;   /* 默认 256；0 表示用默认值 */
    tc_event_overflow overflow;     /* 默认 TC_OVERFLOW_DROP_OLDEST */

    /* —— 可插拔 —— */
    tc_allocator* allocator;        /* NULL 表示用库默认分配器 */

    uint32_t reserved[4];
} tc_term_options;

void tc_init_term_options(tc_term_options* opt);   /* 默认填充，无 cb_size */
```

`tc_init_term_options` 语义：

- 必须先用它填充，再改个别字段（**不使用 cb_size 惯例**）
- 未初始化的 options 行为未定义（库只在 debug 构建下做最低限度的哨兵检查）

---

## 3. 生命周期

```c
tc_status tc_term_create(const tc_term_options* opt, tc_term_t** out);
tc_status tc_term_enter(tc_term_t* t);
tc_status tc_term_leave(tc_term_t* t);      /* 幂等 */
void      tc_term_destroy(tc_term_t* t);    /* 自动 leave */
tc_status tc_term_is_active(const tc_term_t* t, bool* active);
```

| 函数 | 说明 |
| --- | --- |
| `tc_term_create` | 创建句柄、选择后端、做能力探测（受 `query_capabilities` 控制）。**此刻尚未改动终端** |
| `tc_term_enter` | 保存原始终端状态 → 按 §4 的**运行时开关当前值**统一应用（raw / altscreen / 光标 / 鼠标 / 焦点 / 粘贴 / Kitty / 同步更新）→ 注册退出与信号钩子 |
| `tc_term_leave` | 逆序关闭所有已生效开关并恢复（见 §11）；幂等，重复调用返回 `TC_OK` |
| `tc_term_destroy` | 释放句柄与所有内部缓冲；会调用 `tc_term_leave`；会 `dispose` 外部 source 与所有 sink |

状态约束：

| 操作 | `CREATED` | `ACTIVE` | `DESTROYED` |
| --- | --- | --- | --- |
| `enter` | ✅ | `TC_ERR_STATE` | `TC_ERR_STATE` |
| 拉取事件 / `present` | `TC_ERR_STATE` | ✅ | `TC_ERR_STATE` |
| `leave` | ✅（无操作） | ✅ | — |
| 挂载 sink / 注入事件 | ✅ | ✅ | `TC_ERR_STATE` |

### headless 模式

`opt.headless = true` 或运行时检测不到 TTY 时：

- 使用 `backend_null`：不打开任何终端句柄、不改 termios
- `enter` / `leave` 成为纯粹的状态切换（不产生任何字节）
- 尺寸默认 80×24，可由 `tc_term_set_size` 覆盖（也用于测试大屏 / 小屏）
- 输入只来自注入或外部 source；输出只到 sink
- 能力位集为"保守默认"，DA 查询自动跳过，`query_capabilities` 被强制视为 false

> headless 不是"失败降级"，而是一等运行模式：CI、单元测试、离线工具都跑在这条路径上。

```c
tc_status tc_term_set_size(tc_term_t* t, int32_t cols, int32_t rows);  /* 仅 headless / null 后端生效 */
```

---

## 4. 运行时特性开关

终端功能分两类，**区别对待**：

| 类别 | 何时生效 | 运行期能否改变 | 例子 |
| --- | --- | --- | --- |
| **初始化属性** | `create` 时读取并冻结 | 不可改（要改须销毁重建） | `headless`、`query_capabilities`、`strict_diff`、`text_mode`、`ambiguous_wide`、各类超时与容量、`allocator` |
| **运行时开关（feature）** | `enter` 时按初始值应用一次 | 随时开 / 关 | raw mode、altscreen、鼠标、焦点事件、括号粘贴、Kitty 键盘、同步更新、CTRL_C 捕获、光标显隐与形状（见 §7） |

options 中与开关同名的字段只是**初始值**，不是终身设定。

### 4.1 API

```c
typedef enum tc_feature {
    TC_FEATURE_RAW_MODE = 0,
    TC_FEATURE_ALT_SCREEN,
    TC_FEATURE_MOUSE,            /* 开 = 按能力启用鼠标上报；细分模式见 tc_mouse_mode */
    TC_FEATURE_FOCUS_EVENTS,     /* 1004 */
    TC_FEATURE_BRACKETED_PASTE,  /* 2004 */
    TC_FEATURE_KITTY_KEYBOARD,   /* 渐进启用；探测失败自动降级 */
    TC_FEATURE_SYNC_UPDATE,      /* 2026 */
    TC_FEATURE_CAPTURE_CTRL_C,   /* CTRL_C 作为事件而非终止进程 */
    TC_FEATURE_COUNT
} tc_feature;

typedef enum tc_mouse_mode {
    TC_MOUSE_OFF = 0,   /* 不上报 */
    TC_MOUSE_CLICK,     /* 1000：按下 / 释放 / 滚轮 */
    TC_MOUSE_DRAG,      /* 1000 + 1002：加拖动 */
    TC_MOUSE_MOTION     /* 1000 + 1002 + 1003：任意移动也上报 */
} tc_mouse_mode;

tc_status tc_term_set_feature(tc_term_t* t, tc_feature f, bool on);
tc_status tc_term_get_feature(const tc_term_t* t, tc_feature f,
                              bool* requested, bool* effective);
tc_status tc_term_set_mouse_mode(tc_term_t* t, tc_mouse_mode mode);
tc_status tc_term_get_mouse_mode(const tc_term_t* t, tc_mouse_mode* mode);
```

鼠标不是"开 / 关"两态，而是**四档**（关 / 点击 / 拖动 / 移动），因此单列 `tc_mouse_mode`：
`TC_FEATURE_MOUSE = true` 等价于"模式非 OFF"，`= false` 等价于 `TC_MOUSE_OFF`。

### 4.2 语义

| 项 | 约定 |
| --- | --- |
| 幂等 | 设为当前值 → `TC_OK` 且**不重复写序列**（库持有目标状态，不靠"再写一次"保证） |
| `CREATED` 态调用 | 只记录**期望状态**；`enter` 时统一应用（因此可以先配好再进） |
| `ACTIVE` 态调用 | 立即写对应序列并生效 |
| 能力不支持 | 返回 `TC_ERR_UNSUPPORTED`，**状态不变**（可先用能力位集预查，见 `03`） |
| 渐进特性 | Kitty / 同步更新：请求被接受（`TC_OK`），实际可能未生效 → 以 `get_feature` 的 `effective` 为准 |
| 能力变化 | 能力被写入（含用户探测结论，见 `03` §10 / §11）后 `effective` 立即随动：`effective = requested && 能力允许` |
| 关闭后的残留字节 | 序列仍被识别（不误解析为按键），但**不产出事件**（见 `04` §9 / §10） |
| `leave` | 已生效的开关逆序关闭（见 §11）；**期望状态保留**，再次 `enter` 时重新应用 |
| 与专有 API 的关系 | `tc_term_set_raw` / `enter_alt_screen` / `set_cursor_visible` 是同一状态的便捷入口，二者共享同一状态；带额外参数的（光标形状、鼠标模式）仍用专有 API |
| headless | 开关照常记录，不产生任何字节；`effective` 反映"是否被能力允许" |

### 4.3 为什么这样区分

- **初始化属性**影响资源与后端选择（是否开终端句柄、是否做能力探测、文本模式、队列容量），
  运行期改变等于重建内部状态 → 禁止。
- **运行时开关**只影响当前终端状态（写几条序列、改几个标志），改变廉价且可逆 → 必须支持：
  鼠标 / 焦点 / 粘贴这类功能常按**模式**切换（选择文本时关鼠标、进入画布时开拖动、
  全屏预览时关粘贴、运行外部程序前切回 cooked）。

### 4.4 示例

```c
/* 默认关鼠标；进入"画布模式"才开 */
tc_term_set_mouse_mode(term, TC_MOUSE_DRAG);          /* 等价 set_feature(MOUSE, true) + 档位 */

/* 切到"文本编辑模式"：关鼠标与括号粘贴，避免吞掉选择操作 */
tc_term_set_mouse_mode(term, TC_MOUSE_OFF);
tc_term_set_feature(term, TC_FEATURE_BRACKETED_PASTE, false);

/* 查询真实生效情况（Kitty 等渐进特性可能 requested ≠ effective） */
bool req = false, eff = false;
tc_term_get_feature(term, TC_FEATURE_KITTY_KEYBOARD, &req, &eff);
```

---

## 5. Raw mode

```c
tc_status tc_term_set_raw(tc_term_t* t, bool on);
tc_status tc_term_is_raw(const tc_term_t* t, bool* on);
```

- 开启后：关闭回显、关闭行缓冲、关闭 `ISIG`（因此 Ctrl-C / Ctrl-Z 不再产生信号，由库按事件处理）、关闭流控字符处理
- `enter` 时按 `opt.raw_mode` 自动开启；`leave` 时恢复原 `termios` / Console mode 原值
- 运行期可临时切回 cooked（例如调用外部交互式程序前），用完再开
- 统一开关入口：`tc_term_set_feature(t, TC_FEATURE_RAW_MODE, on)`（见 §4），与 `tc_term_set_raw` 共享同一状态

---

## 6. Alternate screen

```c
tc_status tc_term_enter_alt_screen(tc_term_t* t);
tc_status tc_term_leave_alt_screen(tc_term_t* t);
```

- 进入：写 `ESC [ ? 1049 h`（优先，会同时保存光标）；能力不支持时退化为 `ESC [ ? 47 h`，再不支持则跳过
- 离开：写 `ESC [ ? 1049 l`；`leave` 中自动执行
- 不支持 altscreen 时返回 `TC_ERR_UNSUPPORTED`，应用仍需正常工作（只是会污染回滚历史）
- 统一开关入口：`TC_FEATURE_ALT_SCREEN`（见 §4），运行期可进出多次（如临时退回主屏看输出）

---

## 7. 光标

```c
typedef enum tc_cursor_shape {
    TC_CURSOR_DEFAULT = 0,   /* 终端默认 */
    TC_CURSOR_BLOCK,
    TC_CURSOR_UNDERLINE,
    TC_CURSOR_BAR,
    TC_CURSOR_BLINK_BLOCK,
    TC_CURSOR_BLINK_UNDERLINE,
    TC_CURSOR_BLINK_BAR
} tc_cursor_shape;

tc_status tc_term_set_cursor_visible(tc_term_t* t, bool visible);
tc_status tc_term_set_cursor_shape(tc_term_t* t, tc_cursor_shape shape);
tc_status tc_term_set_cursor_pos(tc_term_t* t, int32_t x, int32_t y);  /* 0 基，单元格坐标 */
```

- 形状用 `ESC [ Ps SP q`（DECSCUSR）；不支持时降级为"仅显隐"
- 位置通常由渲染层在 present 时管理；此 API 供显式控制（如让终端光标跟随焦点控件）
- 显隐与形状都是**运行时开关**：随时可改（如编辑态用 BAR、浏览态隐藏）
- `leave` 时强制显示光标并恢复默认形状

---

## 8. 尺寸与 resize

```c
tc_status tc_term_get_size(const tc_term_t* t, int32_t* cols, int32_t* rows);
tc_status tc_term_get_size_px(const tc_term_t* t, int32_t* w, int32_t* h);
```

- 每次 `present` 与 `tc_wait_event` 前会惰性刷新尺寸（如 POSIX：`ioctl(TIOCGWINSZ)`）
- 尺寸变化 → 生成 `TC_EV_RESIZE`（含新行列，像素尺寸可用时为非零，否则为 0）
- 信号（SIGWINCH）只置"需要刷新"标志，**不在信号处理函数内调用 API**
- 应用收到 `TC_EV_RESIZE` 后应 `tc_surface_resize` 并重绘

---

## 9. 信号与控制台事件

| 平台信号 / 事件 | 库的处理 |
| --- | --- |
| 尺寸变化通知（如 POSIX `SIGWINCH`） | 置刷新标志 → 下一次拉取时生成 `TC_EV_RESIZE` |
| 中断信号（如 POSIX `SIGINT`） | `capture_ctrl_c = true` 时转为 `TC_EV_KEY`（`codepoint = 0x03`，`TC_MOD_CTRL`）；否则按系统默认终止进程 |
| `SIGTERM` / `SIGHUP` / `CTRL_BREAK_EVENT` | 默认触发恢复流程后退出；`opt.signal_events`（可选开关）开启时改为投递 `TC_EV_QUIT` 由应用决定 |
| `SIGSEGV` 等致命信号 | 尽力执行恢复（best effort）；不保证 |

约定：

- 信号处理器只写一个 `volatile sig_atomic_t` 标志（POSIX 用自管道唤醒 `poll`）
- 所有库 API 的调用都发生在正常控制流中，不在信号处理器内
- `TC_EV_QUIT` 是可选事件（`TC_EV_KEY` / `TC_EV_MOUSE` / `TC_EV_RESIZE` / `TC_EV_FOCUS` / `TC_EV_PASTE` / `TC_EV_QUIT`）

---

## 10. 标题

```c
tc_status tc_term_set_title(tc_term_t* t, const char* utf8);
```

- 写 `ESC ] 0 ; <title> BEL`（或 ST 终止）
- 标题含非 ASCII 时按当前文本模式处理：ASCII 模式下的非 ASCII 字符按替换字符处理
- 不支持时返回 `TC_ERR_UNSUPPORTED`（不视为致命）

---

## 11. 恢复与崩溃安全

`tc_term_leave` 的固定顺序（逆序原则）。只对**确实生效过的开关**写关闭序列
（状态见 §4；未生效的不写，避免噪声字节）：

1. 结束同步更新模式（若启用）
2. 关闭 Kitty 键盘协议（若启用）
3. 关闭鼠标模式（1000/1002/1003 + 1006 + 1015）
4. 关闭焦点事件（1004）
5. 关闭括号粘贴（2004）
6. 退出 alternate screen
7. 显示光标、恢复默认光标形状、光标移到首行首列（可选）
8. 恢复 raw/cooked 到原状态（恢复保存的 `termios` / Console mode）
9. 冲刷 outbuf（尽力）

多重保障：

| 保障 | 机制 |
| --- | --- |
| 正常退出 | 调用方显式调用 `tc_term_leave` |
| 忘记调用 | `atexit` 钩子（进程内第一个 term 创建时注册一次） |
| 信号终止 | 平台信号钩子（如 POSIX） |
| 崩溃 | best effort：钩子内只做最小序列写出，不做分配 |
| 幂等 | `leave` 可重复调用；`destroy` 内部也调用 |

恢复钩子内**禁止分配**（崩溃路径下堆可能已损坏），因此"恢复序列"是编译期常量字节串。

---

## 12. 文本模式

```c
tc_status tc_term_set_text_mode(tc_term_t* t, tc_text_mode mode);
tc_status tc_term_get_text_mode(const tc_term_t* t, tc_text_mode* mode);
tc_status tc_term_set_ambiguous_wide(tc_term_t* t, bool wide);
```

- term 级默认模式；surface 与具体绘制调用可覆盖（见 `05` / `06`）
- 模式影响：文本宽度计算、非 ASCII 处理、绘制时的簇切分
- 语义细节完全由文本引擎定义（见 `docs/05-text-engine.md`）

---

## 13. 可插拔挂载点（声明，细节见 `09`）

```c
tc_status tc_term_add_sink(tc_term_t* t, const tc_sink_vtable* vt, void* ctx,
                           unsigned levels, uint32_t* out_id);
tc_status tc_term_remove_sink(tc_term_t* t, uint32_t id);
tc_status tc_term_set_input_source(tc_term_t* t, const tc_input_source_vtable* vt, void* ctx);
tc_status tc_term_inject_input(tc_term_t* t, const void* bytes, size_t len);
tc_status tc_term_inject_event(tc_term_t* t, const tc_event* ev);
tc_status tc_set_change_sink(tc_term_t* t, tc_change_fn cb, void* ud);
tc_status tc_set_log_sink(tc_log_fn fn, void* ud);
```

---

## 14. 错误信息与返回码

| 场景 | 返回 |
| --- | --- |
| `opt` 为 NULL | `TC_ERR_INVALID_ARG`（`NULL` opt 表示"全默认"也是允许的，见下） |
| 不是 TTY 且未开 headless | `TC_ERR_NOT_A_TTY` |
| 后端初始化失败 | `TC_ERR_BACKEND` |
| 分配失败 | `TC_ERR_NOMEM` |
| 状态不符（未 enter 就 present） | `TC_ERR_STATE` |
| 能力不支持（如 altscreen） | `TC_ERR_UNSUPPORTED` |
| 写入失败 | `TC_ERR_IO` |

> 约定：`tc_term_create(NULL, &t)` 等价于"全部默认 options"（内部用默认填充），与显式传入默认 options 完全一致。

---

## 15. 典型用法

```c
tc_term_options opt;
tc_init_term_options(&opt);
opt.mouse_mode     = TC_MOUSE_DRAG;       /* 初始值；运行期可改（见 §4） */
opt.focus_events   = true;
opt.ambiguous_wide = (locale_is_cjk());   /* 由应用判断，库不做 locale 策略 */

tc_term_t* term = NULL;
if (tc_term_create(&opt, &term) != TC_OK) return 1;
if (tc_term_enter(term) != TC_OK) { tc_term_destroy(term); return 1; }

/* 运行期按模式切换开关：进编辑区时关鼠标，进画布时开拖动 */
if (mode == MODE_EDIT)  tc_term_set_mouse_mode(term, TC_MOUSE_OFF);
if (mode == MODE_CANVAS) tc_term_set_mouse_mode(term, TC_MOUSE_DRAG);

/* ... 主循环 ... */

tc_term_leave(term);
tc_term_destroy(term);
```

---

## 16. 控制层小结

| 需求 | TermCore |
| --- | --- |
| 创建会话 | `tc_term_create`（初始化属性在此冻结） |
| 进入 / 离开 | `tc_term_enter` / `tc_term_leave`（幂等） |
| 运行时开关 | `tc_term_set_feature` / `tc_term_set_mouse_mode`（见 §4） |
| 尺寸 | `tc_term_get_size` |
| 尺寸变化 | `TC_EV_RESIZE`（拉取） |
| 光标 | `tc_term_set_cursor_*` |
| 标题 | `tc_term_set_title` |
| 销毁 | `tc_term_destroy` |
| 异常处理 | `leave` 幂等 + `atexit` + 信号钩子 |
