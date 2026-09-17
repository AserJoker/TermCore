# 09 · 离线测试与外部工具集成

本文定义 TermCore 的"可测试性契约"：**无 TTY 也能完整运行与断言**，并且外部工具
（测试框架、录制器、WebSocket 转发服务等）能用稳定方式拿到输入控制权与输出数据。

---

## 1. 场景

| 场景 | 需求 |
| --- | --- |
| 单元测试 / CI | 无终端、无 TTY；确定性地喂输入、断言输出字节与帧 |
| 集成测试 | 模拟真实交互序列（键盘、鼠标、粘贴、resize）并断言渲染结果 |
| 录制回放 | 录下真实会话的输入与输出，之后离线重放复现问题 |
| 外部工具 | 另一个进程/服务（如 WebSocket 服务）取全量帧或增量变更并转发 |
| 快照比对 | 把帧渲染成文本或图像，与基线比对 |

共同要求：**输入可被外部控制或完全接管，输出可被完整捕获，帧可被全量拉取**。

---

## 2. headless 运行模式

```c
tc_term_options opt;
tc_init_term_options(&opt);
opt.headless = true;          /* 使用 null 后端 */
opt.query_capabilities = false;
tc_term_create(&opt, &term);
tc_term_enter(term);          /* 纯状态切换，不产生字节 */
tc_term_set_size(term, 120, 40);
```

| 行为 | headless 下的表现 |
| --- | --- |
| 后端 | `backend_null`：不打开任何终端句柄、不改 termios / Console mode |
| 输入 | 只来自注入队列或外部 source；真实终端永不读取 |
| 输出 | 只投递给挂载的 sink；无内置终端 sink（除非显式挂载） |
| 能力 | 保守默认能力集，DA 查询强制跳过 |
| 尺寸 | 默认 80×24，`tc_term_set_size` 可随时改变（会触发 `TC_EV_RESIZE`） |
| `enter` / `leave` | 只切换状态，不写任何字节 |
| 信号钩子 | 不注册（无终端可恢复） |

> headless 不是"降级兜底"，而是**一等运行模式**：CI、单元测试与离线工具都跑在这条路径上。

非 headless 但检测不到 TTY 时：

- 默认返回 `TC_ERR_NOT_A_TTY`（显式失败，避免静默跑飞）
- 应用可据此改用 `headless = true` 重建，或提示用户

---

## 3. 输入注入协议

```c
tc_status tc_term_inject_input(tc_term_t* t, const void* bytes, size_t len);  /* 走解析 */
tc_status tc_term_inject_event(tc_term_t* t, const tc_event* ev);             /* 直投事件 */
```

契约：

| 约定 | 说明 |
| --- | --- |
| 任意切分 | 注入字节可逐字节喂，解析器必须与一次性喂入等价 |
| 顺序 | 严格按注入顺序消费；注入队列优先于 source 与真实终端 |
| 走真实解析 | `inject_input` 的字节经过与真实终端**完全相同**的解码与状态机 |
| 直投事件 | `inject_event` 绕过解析，追加到事件队列**队尾**（与解析产出的事件严格 FIFO），用于无法用字节表达的场景（如 `TC_EV_QUIT`、超大粘贴） |
| 校验 | `kind` 非法 / 字段越界 → `TC_ERR_INVALID_ARG`，不入队 |
| 容量 | 队列有上限，超过返回 `TC_ERR_OVERFLOW`（调用方应放慢或分块） |
| 分配 | 允许分配（控制面），但拉取路径仍零分配 |
| 时序 | 注入本身不推进时间；ESC 残留的结算判定发生在拉取路径（见 §8） |
| 贪婪读取 | 注入源一次 `read` 会把注入缓冲取干；`inject_input("\x1b[A")` 与逐字节分批注入解析结果一致 |

示例（确定性测试）：

```c
/* 模拟：按下 'a'，再按 F1，再点击 (3,4)，最后粘贴 "hi" */
tc_term_inject_input(t, "a", 1);
tc_term_inject_input(t, "\x1bOP", 3);
tc_term_inject_input(t, "\x1b[<0;4;5M", 8);    /* SGR 按下，1 基 → 0 基 (3,4) */
tc_term_inject_event(t, &(tc_event){ .kind = TC_EV_PASTE, .u.paste = { .text = "hi", .len = 2 } });

tc_event ev; bool has;
while (tc_get_event(t, &ev, &has) == TC_OK && has) { assert_next(&ev); }
```

---

## 4. 外部输入接管

```c
typedef struct tc_input_source_vtable {
    tc_status (*wait_ready)(void* ctx, int32_t timeout_ms, bool* ready);
    tc_status (*read)(void* ctx, void* buf, size_t cap, size_t* nread);
    void      (*dispose)(void* ctx);
} tc_input_source_vtable;

tc_status tc_term_set_input_source(tc_term_t* t, const tc_input_source_vtable* vt, void* ctx);
```

契约：

| 约定 | 说明 |
| --- | --- |
| 接管范围 | 设置后库**不再读真实终端**；注入队列仍然优先 |
| 恢复 | `tc_term_set_input_source(t, NULL, NULL)` 恢复默认源 |
| `read` | 返回 0 表示"这批没了"（不是 EOF）；返回 `n > 0` 时库会**再次调用**（贪婪读取） |
| `wait_ready` | 超时返回 `TC_OK` + `ready=false`；`<0` 表示无限等待；**不得忙等**（用事件 / 条件变量 / OS 等待） |
| `dispose` | 在替换 source 或 `tc_term_destroy` 时调用**一次** |
| 回调禁止 | 不得在 `read` / `wait_ready` 内调用会触发拉取或 present 的 API |
| 错误 | 返回非 OK → 库返回 `TC_ERR_SOURCE`，不重试（由调用方决定） |

典型实现：

- **回放源**：从录制文件按时间轴吐字节
- **脚本源**：从内存脚本逐条吐
- **网络源**：从 socket 读（需注意：库是单线程，网络读应在调用方线程内完成）

---

## 5. 输出捕获契约

```c
/* 捕获 sink 示例（调用方实现） */
typedef struct capture { uint8_t* buf; size_t len, cap; } capture;

static void on_bytes(void* ctx, const void* bytes, size_t len) {
    capture* c = ctx;
    /* 追加到 c->buf（必要时 realloc） */
}

uint32_t sink_id = 0;
tc_term_add_sink(term, &(tc_sink_vtable){ .on_bytes = on_bytes }, &cap_ctx,
                 TC_SINK_BYTES, &sink_id);
```

| 约定 | 说明 |
| --- | --- |
| levels | `TC_SINK_BYTES`（原始转义序列）/ `TC_SINK_CHANGES`（结构化变更）/ `TC_SINK_FRAME`（帧视图） |
| 同帧一致 | 所有 sink 在同一帧看到同一份数据；顺序按挂载顺序 |
| 失败隔离 | 单 sink 失败不影响其它；聚合为 `TC_ERR_SINK` |
| 数据借用 | `bytes` / `tc_change.cells` / `tc_frame_view.cells` 仅回调期间有效；留存请拷贝 |
| headless | 默认无终端 sink，捕获 sink 就是唯一输出目标；也可在非 headless 下并存 |
| 顺序 | `CHANGES` 在 diff 阶段逐条；`BYTES` 在写出前一次；`FRAME` 在帧末一次 |

断言方式建议：

- **字节级**：比对 outbuf 与基线（适合回归）
- **帧级**：比对 `tc_frame_view` 的 cell 网格（适合语义断言，不受序列细节影响）
- 两者结合：帧级为主，字节级用于协议细节

---

## 6. 增量变更回调契约

```c
tc_status tc_set_change_sink(tc_term_t* t, tc_change_fn cb, void* ud);
```

| 约定 | 说明 |
| --- | --- |
| 时机 | `tc_present` 的 diff 阶段**同步**逐条回调（写字节之前） |
| 粒度 | 默认按 run 合并为 `TC_CHANGE_RECT`；单格变化为 `TC_CHANGE_CELL` |
| 数据借用 | `cells` 仅回调期间有效 |
| 重入禁止 | 回调内禁止 `tc_present`、写 surface、增删 sink |
| 转发模式 | 需要跨线程/异步转发时：回调内**只做拷贝与入队**，由别的线程消费 |
| 注销 | `cb = NULL` 注销 |

变更回调是"渲染增量"，与"输入事件只拉取"不冲突：输入事件永远不回调。

---

## 7. 全量帧视图契约

```c
tc_status tc_surface_view(const tc_surface_t* s, tc_frame_view* out);
/* { const tc_cell* cells; int32_t cols, rows, stride; uint64_t frame_id; } */
```

| 约定 | 说明 |
| --- | --- |
| 零拷贝 | 指向 surface 的单元格缓冲（离屏纹理），不复制 |
| 只读 | 调用方不得写入（未定义行为） |
| 失效规则 | 下一次写 surface 或 `present` 之后失效；用 `frame_id` 自检 |
| 无需释放 | 视图是借用；surface 销毁后无效 |
| 布局 | 行优先，`stride >= cols`；索引 `cells[y * stride + x]` |
| 库不做的事 | 不序列化、不压缩、不传输；这些由外部工具负责 |

**跨线程取帧的正确姿势**（库是单线程）：

1. 取帧与"把帧拷走"必须在持有 surface 的线程内完成
2. 外部线程只消费已拷贝的数据（如拷贝出的 cell 数组或已序列化的文本）
3. 禁止外部线程直接持有 `cells` 指针跨帧使用

---

## 8. 时间控制（确定性测试）

超时相关行为（ESC 歧义、能力查询）默认使用单调时钟。为了测试可确定，提供**时钟注入**：

```c
typedef struct tc_clock_vtable {
    int64_t (*now_ms)(void* ctx);      /* 单调毫秒 */
} tc_clock_vtable;

tc_status tc_term_set_clock(tc_term_t* t, const tc_clock_vtable* vt, void* ctx); /* NULL 恢复真实时钟 */
tc_status tc_term_tick(tc_term_t* t, int64_t delta_ms);   /* 仅在虚拟时钟下有效：推进虚拟时间 */
```

用途：

- 测试"孤立 ESC 结算为 ESC 事件"：注入 `ESC`，`tc_term_tick(150)`，再拉取即可断言（**无需真实等待 150ms**，因为库从不阻塞等待）
- 测试能力查询超时：注入 DA 响应或不注入，`tick` 后断言降级结果
- 回放录制时可用虚拟时钟消除真实等待

约定：虚拟时钟只在显式设置后生效；`tc_term_tick` 在真实时钟下返回 `TC_ERR_STATE`。

---

## 9. 外部工具集成范式（WebSocket 示例）

库**不提供网络能力**。典型集成：宿主程序挂一个 `TC_SINK_FRAME` sink 与变更回调，
把数据交给外部传输层（如 WebSocket 服务）。

```c
/* C 侧：只负责取数据并交给传输层，库不参与网络 */
static void on_frame(void* ctx, const tc_frame_view* v) {
    ws_channel* ch = ctx;
    /* 方式 A：全量 —— 把 v 序列化成紧凑格式后发送（由调用方实现 codec） */
    ws_send_begin(ch, v->frame_id, v->cols, v->rows);
    for (int y = 0; y < v->rows; ++y)
        for (int x = 0; x < v->cols; ++x)
            ws_send_cell(ch, x, y, &v->cells[y * v->stride + x]);
    ws_send_end(ch);
}

static void on_change(void* ctx, const tc_change* c) {
    /* 方式 B：增量 —— 只发变化矩形 */
    if (c->kind == TC_CHANGE_RECT) ws_send_rect(ch, c->frame_id, &c->rect, c->cells, c->stride);
}

/* 挂载：全量 + 增量同时启用，由服务端按需选择 */
tc_term_add_sink(term, &(tc_sink_vtable){ .on_frame = on_frame, .on_change = on_change },
                 ch, TC_SINK_FRAME | TC_SINK_CHANGES, &id);
```

集成建议：

| 方面 | 建议 |
| --- | --- |
| 全量 vs 增量 | 首帧用全量，之后用增量；帧序号用于校验连续性 |
| 频率 | 由调用方控制 present 频率；不要在每帧都做重序列化 |
| 编码 | 由外部工具定义（JSON / 二进制 / 自定义）；库不规定格式 |
| 背压 | 传输慢时在回调里只入队，不阻塞 present |
| 线程 | 序列化与发送放在外部线程；回调内只拷贝 |

---

## 10. 录制与回放

### 10.1 录制内容

| 通道 | 记录 |
| --- | --- |
| 输入 | 原始字节 + 时间戳（虚拟或真实）+ 事件序号；或直接记录结构化事件 |
| 输出 | 每次写出的字节序列 + 帧序号 |
| 环境 | 能力位集与特性列表 profile（逐项来源 + 指纹）、初始尺寸、文本模式、Ambiguous 设置 |
| 版本 | 库版本与 ABI 版本（用于兼容性判断） |

### 10.2 回放一致性要求

- 相同输入 + 相同能力 + 相同初始尺寸 + 相同文本模式 ⇒ 相同事件序列与输出字节
- 时间相关的分支（ESC、能力查询超时）必须能由虚拟时钟复现
- 回放时建议使用 `headless` + 虚拟时钟 + 捕获 sink

### 10.3 实现建议

- 录制用 sink（`TC_SINK_BYTES`）写输出；用 source 包装器记录输入（库不内置录制器）
- 回放用 source 提供输入，或直接 `tc_term_inject_input` 批量注入
- 录制格式不属于库 API（由工具自行定义）

---

## 11. 测试分层

| 层 | 方式 | 覆盖 |
| --- | --- | --- |
| 纯逻辑单测 | 直接调用 `tc_text_*`、颜色降级函数、解析器喂字节 | 文本引擎、解码、簇、宽度、测量、颜色映射、序列解析 |
| 注入式端到端 | headless + 注入输入 + 捕获 sink + 帧断言 | 完整交互链路、diff 正确性、事件语义 |
| 能力矩阵 | `tc_term_set_caps_override` 构造能力组合 | 各降级路径 |
| 探测向导 | headless + 注入事件 + 脚本化 `tc_caps_probe_answer` / `_feed` | 交互式能力探测、profile 序列化往返、指纹匹配 |
| 真机（opt-in） | 真实 TTY 手动或半自动 | 真实终端兼容性、鼠标、resize、焦点 |

断言示例（帧级）：

```c
tc_surface_draw_text(s, 0, 0, "你好", &attr, TC_TEXT_UNICODE, NULL);
tc_present(term, s);

tc_frame_view v; tc_surface_view(s, &v);
assert(v.cells[0].attr & TC_ATTR_WIDE_HEAD);
assert(v.cells[1].attr & TC_ATTR_WIDE_CONT);
```

---

## 12. 常见陷阱

| 陷阱 | 说明 / 规避 |
| --- | --- |
| 跨帧持有 `cells` | 视图借用，下一帧失效；需要留存就拷贝 |
| 跨线程直接调用库 | 单线程库；跨线程必须串行化（见 `07` T2/T6） |
| 在回调里 present | 明确禁止；回调只做拷贝与入队 |
| 注入后立即断言事件 | 注入字节可能需要"解析 + 拉取"两步；先 `tc_get_event` 再断言 |
| 忘记推进时间 | ESC 结算 / 能力超时需要真实等待或 `tc_term_tick` |
| 期望拉取会阻塞 | 库零延迟读取；需要等待就自己 sleep 或用 `tc_wait_event(timeout > 0)` |
| 依赖真实终端字节 | 字节级断言在 headless 下同样成立（序列由库生成），不依赖终端 |
| 录制回放忽略能力差异 | 能力不同会导致输出不同；录制时一并保存能力位集 |

---

## 13. API 汇总（本文涉及）

```c
/* headless 与尺寸 */
bool headless;                    /* tc_term_options 字段 */
tc_status tc_term_set_size(tc_term_t* t, int32_t cols, int32_t rows);

/* 输入控制 */
tc_status tc_term_inject_input(tc_term_t* t, const void* bytes, size_t len);
tc_status tc_term_inject_event(tc_term_t* t, const tc_event* ev);
tc_status tc_term_set_input_source(tc_term_t* t, const tc_input_source_vtable* vt, void* ctx);

/* 输出捕获与广播 */
tc_status tc_term_add_sink(tc_term_t* t, const tc_sink_vtable* vt, void* ctx,
                           unsigned levels, uint32_t* out_id);
tc_status tc_term_remove_sink(tc_term_t* t, uint32_t id);

/* 变更与帧 */
tc_status tc_set_change_sink(tc_term_t* t, tc_change_fn cb, void* ud);
tc_status tc_surface_view(const tc_surface_t* s, tc_frame_view* out);

/* 确定性时间 */
tc_status tc_term_set_clock(tc_term_t* t, const tc_clock_vtable* vt, void* ctx);
tc_status tc_term_tick(tc_term_t* t, int64_t delta_ms);

/* 能力覆盖（降级路径测试） */
tc_status tc_term_set_caps_override(tc_term_t* t, const tc_caps* caps);

/* 特性列表与交互式探测（见 03 §10 / §11）；headless 下可脚本化，结论可作测试 fixture */
tc_status tc_term_get_caps_profile(const tc_term_t* t, tc_caps_profile* out);
tc_status tc_term_apply_caps_profile(tc_term_t* t, const tc_caps_profile* p);
tc_status tc_caps_profile_serialize(const tc_caps_profile* p, void* buf, size_t cap, size_t* need);
tc_status tc_caps_profile_deserialize(const void* buf, size_t len, tc_caps_profile* out);
tc_status tc_caps_probe_begin(tc_term_t* t, uint64_t bits, uint32_t flags, tc_caps_probe** out);
tc_status tc_caps_probe_feed(tc_caps_probe* p, const tc_event* ev);
tc_status tc_caps_probe_answer(tc_caps_probe* p, int32_t choice);
tc_status tc_caps_probe_finish(tc_caps_probe* p, tc_caps* out);
```
