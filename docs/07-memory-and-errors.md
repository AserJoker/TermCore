# 07 · 内存、错误码、ABI 与线程模型

本文是所有公开 API 的"契约层"：谁分配、谁释放、错误怎么表达、ABI 怎么保持稳定、线程怎么用。
其它文档中的 API 都默认遵守本文约定，不再重复说明。

---

## 1. allocator

### 1.1 接口

```c
typedef struct tc_allocator {
    void* (*alloc)(void* ctx, size_t size, size_t align);
    void* (*realloc)(void* ctx, void* ptr, size_t new_size, size_t align);
    void  (*free)(void* ctx, void* ptr);
    void* ctx;
} tc_allocator;
```

约定：

| 约定 | 说明 |
| --- | --- |
| 对齐 | `align` 为 2 的幂，至少 `alignof(void*)`；实现必须满足 |
| 零长度 | `size == 0` 的行为由实现决定，库不依赖它返回非 NULL |
| 失败 | 返回 `NULL`，库转换为 `TC_ERR_NOMEM` 并**保证不崩溃**、不留半初始化对象 |
| `free(NULL)` | 必须是安全的空操作 |
| 线程 | 分配器实现需自行保证线程安全；库只在调用方线程调用它 |
| 生命周期 | allocator 必须在它分配的所有对象释放**之后**才失效 |

### 1.2 注入点

```c
tc_allocator* tc_allocator_default(void);   /* 库自带：malloc / free 包装 */

typedef struct tc_term_options {
    /* ... */
    tc_allocator* allocator;                /* NULL 表示用默认分配器 */
} tc_term_options;
```

注入规则：

- 只在 `tc_term_create` 处注入一次；term 持有的 allocator 会传给从它创建的 surface
- surface 也可以单独携带 allocator（当它是独立创建时），默认继承 term 的
- 文本引擎 `tc_text_*` **完全不分配**，因此不需要 allocator
- 库内部绝不直接调用 `malloc` / `free`（构建期可用静态分析或链接期检查验证）

### 1.3 所有权规则

| 对象 | 创建 | 释放 | 备注 |
| --- | --- | --- | --- |
| `tc_term_t` | `tc_term_create` | `tc_term_destroy` | 自动执行 `leave` |
| `tc_surface_t` | `tc_surface_create` | `tc_surface_destroy` | 不因 term 销毁而自动释放 |
| sink 上下文 `ctx` | 调用方 | 调用方（通过 `dispose`） | 库只在 remove / destroy 时调 `dispose` |
| source 上下文 `ctx` | 调用方 | 调用方（通过 `dispose`） | 替换或 destroy 时调 `dispose` |
| 事件内的 `paste.text` | 库 | 库 | **有效期至下一次取事件**，需要留存请自行拷贝 |
| `tc_frame_view` | 库（借用） | 无需释放 | 下一次写 surface 或 present 后失效 |
| `tc_change` 内的数据指针 | 库（借用） | 无需释放 | 仅在回调期间有效 |

### 1.4 分配时机（硬性约束）

| 路径 | 分配 |
| --- | --- |
| `tc_get_event` / `tc_peek_event` / `tc_wait_event` | **禁止** |
| `tc_present` 与所有 `tc_surface_*` 绘制 | **禁止** |
| 所有 `tc_text_*` | **禁止**（纯函数） |
| `tc_term_create` / `tc_term_enter` / `tc_term_leave` / `tc_term_destroy` | 允许 |
| `tc_surface_create` / `tc_surface_resize` / `tc_surface_destroy` | 允许 |
| `tc_term_add_sink` / `tc_term_remove_sink` | 允许 |
| `tc_term_inject_input` / `tc_term_inject_event` | 允许（队列扩容） |
| `tc_term_set_input_source` | 允许 |

实现守则：热路径所需的一切缓冲在 `create` / `resize` 时一次性备好，运行期只复用。

---

## 2. 错误码 `tc_status`

### 2.1 设计原则

- **只返回状态码**，不保留"最后一次错误"式的全局错误状态
- 状态码是**稳定的枚举值**：只追加，不重排、不复用
- 需要细节时用**日志 sink**（`tc_set_log_sink`）输出人类可读诊断，默认静默
- 错误码表达"**发生了什么类别的问题**"，不表达具体位置

### 2.2 枚举

```c
typedef enum tc_status {
    TC_OK = 0,

    /* 通用 */
    TC_ERR_FAIL          = 1,   /* 未分类失败 */
    TC_ERR_INVALID_ARG   = 2,   /* 参数为 NULL / 越界 / 非法组合 */
    TC_ERR_STATE         = 3,   /* 当前状态不允许该操作（如未 enter 就 present） */
    TC_ERR_NOMEM         = 4,   /* 分配失败 */
    TC_ERR_UNSUPPORTED   = 5,   /* 当前平台 / 后端 / 能力不支持 */
    TC_ERR_NOT_FOUND     = 6,   /* 句柄 / id 不存在 */
    TC_ERR_VERSION       = 7,   /* 格式 / ABI / profile 版本不兼容（见 03 §10.3） */

    /* I/O 与后端 */
    TC_ERR_IO            = 10,  /* 读写失败 */
    TC_ERR_NOT_A_TTY     = 11,  /* 不是终端（且未启用 headless） */
    TC_ERR_PERM          = 12,  /* 权限不足 */
    TC_ERR_BACKEND       = 13,  /* 后端初始化或调用失败 */

    /* 时间与容量 */
    TC_ERR_TIMEOUT       = 20,  /* 等待超时（部分 API 表示"无数据"，非致命） */
    TC_ERR_OVERFLOW      = 21,  /* 缓冲容量不足 / 事件队列溢出 */
    TC_ERR_TRUNCATED     = 22,  /* 数据被截断但已部分处理 */

    /* 解析与文本 */
    TC_ERR_PARSE         = 30,  /* 转义序列解析失败（已尽力恢复） */
    TC_ERR_ENCODING      = 31,  /* 非法编码（已按替换字符处理） */

    /* 可插拔组件 */
    TC_ERR_SINK          = 40,  /* 一个或多个 sink 回调失败（其它 sink 已正常投递） */
    TC_ERR_SOURCE        = 41,  /* 外部输入源返回失败 */

    TC_ERR_COUNT
} tc_status;
```

### 2.3 分类说明与使用规范

| 码 | 是否致命 | 典型返回场景 | 调用方应如何处理 |
| --- | --- | --- | --- |
| `TC_OK` | — | 成功 | 继续 |
| `TC_ERR_INVALID_ARG` | 是（调用错误） | NULL 句柄、坐标越界、非法 options 组合 | 修代码 |
| `TC_ERR_STATE` | 是（调用错误） | 未 `enter` 就 `present`；已 `destroy` | 检查生命周期 |
| `TC_ERR_NOMEM` | 是 | 分配失败 | 释放资源或退出 |
| `TC_ERR_UNSUPPORTED` | 否 | 当前能力不支持（如无鼠标能力时启用鼠标） | 降级使用该特性 |
| `TC_ERR_VERSION` | 否 | profile / 序列化格式版本不兼容 | 丢弃缓存并重新探测（见 `03` §10.3） |
| `TC_ERR_IO` | 视情况 | 终端写入失败 | 可重试；库不更新 front 缓冲，下帧自动重试 |
| `TC_ERR_TIMEOUT` | 否 | 能力查询 / 等待超时 | 视为"不可用"，走降级 |
| `TC_ERR_OVERFLOW` | 否 | 事件队列满 | 提高容量或改为丢弃最旧策略 |
| `TC_ERR_TRUNCATED` | 否 | 注入字节过长被截断 | 分块注入 |
| `TC_ERR_PARSE` / `TC_ERR_ENCODING` | 否 | 遇到非法序列 | 库已恢复并继续；可查日志 |
| `TC_ERR_SINK` | 否 | 某 sink 回调失败 | 其它 sink 已收到数据；可移除该 sink |
| `TC_ERR_SOURCE` | 视情况 | 外部 source 报错 | 替换或恢复默认源 |

### 2.4 返回值与输出参数约定

统一形状：

```c
tc_status tc_xxx(tc_term_t* t, <in 参数...>, <out 参数...>);
```

| 约定 | 说明 |
| --- | --- |
| 返回 `TC_OK` 才保证 out 参数被写入 | 失败时 out 参数**保持不变**（不写半值） |
| 布尔型 out 参数用 `bool* has` | 如 `tc_get_event(t, &ev, &has)`：`TC_OK` + `has=false` 表示"暂无事件"，非错误 |
| 数值 out 参数用指针 | 如 `tc_term_get_size(t, &cols, &rows)` |
| 句柄 out 参数用 `T** out` | 失败时置 `*out = NULL` |
| 可选 out 参数可为 NULL | 如 `tc_surface_draw_text(..., int* out_w)` 传 NULL 表示不关心宽度 |
| 超时不是错误 | `tc_wait_event` 超时返回 `TC_OK` + `ready=false` |
| 部分成功要显式说明 | 如 sink 部分失败返回 `TC_ERR_SINK`，但帧已写出 |

### 2.5 没有 last error 的替代方案

```c
typedef void (*tc_log_fn)(tc_log_level level, const char* message, void* ud);
tc_status tc_set_log_sink(tc_log_fn fn, void* ud);
```

- 默认静默（无任何输出，绝不写 stdout）
- 日志级别：`TC_LOG_ERROR` / `TC_LOG_WARN` / `TC_LOG_INFO` / `TC_LOG_TRACE`
- `TC_LOG_TRACE` 可打印协议级字节流，用于调试解析问题
- 日志回调内同样禁止重入调用库 API（除只读查询外）

---

## 3. ABI 稳定性

### 3.1 公开面构成

只允许三类东西出现在公开头文件里：

1. **不透明句柄**：`typedef struct tc_term tc_term_t;`（定义不可见，布局可自由变更）
2. **POD 结构体**：固定布局的纯数据（`tc_cell`、`tc_color`、`tc_rect`、`tc_event`、`tc_frame_view` …）
3. **函数与枚举常量**

不允许出现：内部结构体定义、宏展开的实现细节、平台头（如 `<termios.h>`）、`size_t` 依赖平台差异的类型（改用 `uint32_t` / `uint64_t`）。

### 3.2 POD 规则

| 规则 | 说明 |
| --- | --- |
| 只追加字段 | 新字段只能加在末尾 |
| 不重排、不改类型、不改语义 | 已有字段永久冻结 |
| 显式保留字段 | 预留 `uint32_t reserved[...]` 便于未来扩展而不破坏布局 |
| 不使用位域跨字节语义 | 需要标志位时用显式整数字段 + 掩码常量 |
| 固定宽度类型 | 用 `int32_t` / `uint16_t` 等，不用 `int` / `long` |
| 不含指针成员（对外可见的数据） | 例外：`tc_frame_view.cells`、`tc_event.paste.text` 这类**借用**指针，语义在文档中固定 |

示例：

```c
typedef struct tc_rect {
    int32_t x, y;
    int32_t w, h;
    uint32_t reserved[2];
} tc_rect;
```

### 3.3 枚举规则

- 枚举值只追加，不重排、不删除、不复用数值
- 位标志用 `1u << n` 形式定义，禁止连续赋值后当位集用
- 新增枚举成员追加在末尾（保留 `TC_XXX_COUNT` 哨兵以便运行时探测版本能力）

### 3.4 函数规则

- 函数只增不改：不允许改变已有函数的签名或语义
- 需要演进时新增 `tc_xxx_ex` 或 `tc_xxx2`，旧函数保留（可标注 deprecated）
- 所有导出函数使用统一调用约定与可见性宏：

```c
#define TC_API      /* 导出可见性：如 __attribute__((visibility("default"))) 或平台等价物 */
#define TC_CALL     /* 调用约定：按需定义，默认为空 */
```

### 3.5 版本约定

```c
#define TC_VERSION_MAJOR 0
#define TC_VERSION_MINOR 1
#define TC_VERSION_PATCH 0

uint32_t tc_version(void);              /* (major<<16)|(minor<<8)|patch */
uint32_t tc_abi_version(void);          /* ABI 兼容版本号，单独演进 */
```

| 变更类型 | 版本变化 |
| --- | --- |
| 新增函数、新增 POD 末尾字段、新增枚举值 | `MINOR` +1（`tc_abi_version` 不变） |
| 改变已有语义、改变 POD 布局、删除符号 | `MAJOR` +1，`tc_abi_version` +1 |
| 内部实现、修复行为 | `PATCH` +1 |

### 3.6 编译期兼容

- C11 + `<stdint.h>` / `<stdbool.h>`；**禁用 GNU 扩展**与匿名结构体/union 扩展之外的编译器特性
- 匿名 union 使用 C11 标准写法（各主流编译器均支持）
- 不依赖 `#pragma pack`；结构体自然对齐
- 头文件可被 C++ 包含（`extern "C"` 包裹）
- 静态库产物：`libtermcore.a`（GCC / Clang）

---

## 4. 线程模型（契约声明）

**TermCore 是单线程库。**

| 条款 | 说明 |
| --- | --- |
| T1 | 库内部不创建线程、不使用线程局部存储、不加锁 |
| T2 | 同一个 `tc_term_t` / `tc_surface_t` 句柄不得在多个线程并发使用 |
| T3 | 不同句柄之间也无共享全局可变状态（除日志 sink 与默认分配器，需调用方保证其线程安全） |
| T4 | 所有回调（变更、sink、source、日志）都在**调用方线程同步**执行 |
| T5 | 信号处理函数中不调用库 API；信号只置标志，事件在拉取路径生成 |
| T6 | 若调用方要跨线程驱动（如网络线程推事件），必须自行串行化：把注入动作排队到持有句柄的线程执行 |

> 违反 T2 的后果是未定义行为，不做检测（检测成本与收益不匹配）。

---

## 5. 回调重入规则总表

| 回调 | 触发时机 | 允许 | 禁止 |
| --- | --- | --- | --- |
| `tc_change_fn`（变更） | `tc_present` 内部的 diff 阶段 | 只读查询、拷贝数据、入队到别处 | `tc_present`、写 surface、增删 sink |
| `sink.on_bytes` / `on_change` / `on_frame` | present 的广播阶段 | 拷贝数据、转发 | 同上；且不得修改将要写出的数据 |
| `source.wait_ready` / `read` | 拉取路径 | 返回数据 | 调用任何会触发拉取或 present 的 API |
| `tc_log_fn` | 任意 | 只读格式化输出 | 调用库 API（除 `tc_version` 等纯只读查询） |

违反禁止条款属于调用方错误，库不做检测。

---

## 6. 常量命名与取值约定

| 类别 | 前缀 | 示例 |
| --- | --- | --- |
| 事件类型 | `TC_EV_` | `TC_EV_KEY`、`TC_EV_MOUSE`、`TC_EV_RESIZE`、`TC_EV_FOCUS`、`TC_EV_PASTE` |
| 键码 | `TC_KEY_` | `TC_KEY_F1`、`TC_KEY_UP`、`TC_KEY_ENTER`、`TC_KEY_ESCAPE` |
| 修饰键（位标志） | `TC_MOD_` | `TC_MOD_SHIFT`、`TC_MOD_CTRL`、`TC_MOD_ALT`、`TC_MOD_SUPER` |
| 鼠标 | `TC_MOUSE_` | `TC_MOUSE_LEFT`、`TC_MOUSE_DOWN`、`TC_MOUSE_WHEEL_UP` |
| 能力（位标志） | `TC_CAP_` | `TC_CAP_TRUECOLOR`、`TC_CAP_MOUSE_SGR`、`TC_CAP_FOCUS` |
| 样式（位标志） | `TC_STYLE_` | `TC_STYLE_BOLD`、`TC_STYLE_ITALIC`、`TC_STYLE_UNDERLINE` |
| 颜色类型 | `TC_COLOR_` | `TC_COLOR_DEFAULT`、`TC_COLOR_INDEXED`、`TC_COLOR_RGB` |
| 文本模式 | `TC_TEXT_` | `TC_TEXT_ASCII`、`TC_TEXT_UNICODE` |
| 状态 | `TC_OK` / `TC_ERR_` | 见 §2 |
| sink 级别 | `TC_SINK_` | `TC_SINK_BYTES`、`TC_SINK_CHANGES`、`TC_SINK_FRAME` |
| 日志级别 | `TC_LOG_` | `TC_LOG_ERROR` … `TC_LOG_TRACE` |

所有枚举值一旦发布即冻结。
