# 08 · 平台实现要点与已知坑

本文记录后端的实现要点与已知坑。目标：**上层只见字节流、尺寸与能力位集**，不感知平台。

---

## 1. 后端职责边界

后端（`tc_backend`，内部抽象）只做四件事：

1. 打开 / 关闭终端句柄
2. 搬运字节（读 / 写）
3. 切换终端模式（raw / cooked、altscreen、鼠标等序列的**写入**）
4. 报告尺寸与平台事件

后端**不解析**任何转义序列，**不判定**能力。解析在输入层，判定在能力层。

---

## 2. 参考实现：POSIX（Linux / macOS / BSD）

### 2.1 句柄

| 项 | 策略 |
| --- | --- |
| 输入 | 优先 `/dev/tty`（保证即使 stdin 被重定向仍能读键盘）；失败则 stdin |
| 输出 | stdout；若 stdout 被重定向而 `/dev/tty` 可写，仍写 stdout（保持管道语义可预测） |
| 非 TTY | `isatty` 失败 → 非 headless 时返回 `TC_ERR_NOT_A_TTY` |

### 2.2 raw mode

手写 `termios` 标志（不用 `cfmakeraw`，它非标准且各平台略有差异）：

```
关闭：ECHO、ICANON、ISIG、IEXTEN、IXON、ICRNL、OPOST（部分保留）、BRKINT、INPCK、ISTRIP
开启：CS8
控制字符：VMIN = 1，VTIME = 0（或按"带超时读"需要临时调整）
```

- 保存原始 `termios`，`leave` 时 `tcsetattr(TCSANOW)` 恢复
- `TCSAFLUSH` vs `TCSANOW`：进入时用 `TCSAFLUSH` 丢弃已排队输入（避免启动前的输入涌入）
- 退出时恢复后**不清空**输入队列

### 2.3 信号

| 信号 | 处理 |
| --- | --- |
| `SIGWINCH` | 只置标志 + 自管道写 1 字节唤醒 `poll`；不在处理器内调用库 API |
| `SIGINT` | `capture_ctrl_c` 时忽略（或捕获后置标志，产出 `TC_EV_KEY`）；否则保持默认 |
| `SIGTERM` / `SIGHUP` | 默认触发 `leave` 后退出；开启 `signal_events` 时改为投递 `TC_EV_QUIT` |
| `SIGPIPE` | 忽略（输出写到已关闭管道时不终止进程） |

自管道（self-pipe）是唯一可靠的"让 `poll` 被信号唤醒"手段。

### 2.4 读取与等待

- 输入 fd 置 `O_NONBLOCK`（raw 模式设置时一并处理），退出时恢复原标志
- **贪婪读取**：`read` 循环调用至 `EAGAIN`（或达到 `read_batch_max_bytes`），把已到达字节一次取干
- `read` 返回 `-1` 且 `errno == EINTR` → 重试
- `EAGAIN` → 视为"这批没了"，结束本轮读取（**不等待、不 sleep**）
- 可选 `FIONREAD` 预取可读字节数，减少一次多余 `read`
- 读到 `0` 字节：终端关闭（返回 `TC_ERR_IO`）
- `poll` 只在 `tc_wait_event(timeout > 0)` 时使用；`timeout = 0` 时不调用 `poll`，直接非阻塞读

### 2.5 尺寸

- `ioctl(TIOCGWINSZ)`；失败时回退到环境变量 `COLUMNS` / `LINES`，再失败用 80×24
- 像素尺寸：`TIOCGWINSZ` 的 `ws_xpixel` / `ws_ypixel` 常为 0；为 0 时尝试 `ESC [ 14 t`（可选，成本高，默认不做）
- 容器 / CI 中 `SIGWINCH` 可能不触发 → 每次 `present` 前惰性 `ioctl` 兜底

### 2.6 写入

- `write` 可能部分写入 → 循环写直到写完或出错
- `EINTR` 重试；`EAGAIN`（非阻塞）退化为重试或直接失败（默认阻塞写）
- 大帧分块写，但保持单次 `present` 内顺序

### 2.7 编码与终端特性

| 项 | 说明 |
| --- | --- |
| 输入编码 | 直接是 UTF-8 字节流（现代终端），交给文本引擎解码 |
| 传统终端 | 少数环境可能发 Latin-1；库不猜测，按 UTF-8 严格解码，非法即替换 |
| tmux / screen | 内层终端；`TERM` 为 `screen-*` / `tmux-*`；鼠标需外层也支持（通常 OK） |
| macOS Terminal / iTerm2 | iTerm2 支持 Kitty 协议子集与真彩；Terminal.app 能力较弱（无真彩、鼠标有限） |
| Linux console（tty1） | 无真彩、字体有限、不支持部分序列；能力层按内置库降级 |

---

## 3. 已知坑与规避

| 坑 | 影响 | 规避 |
| --- | --- | --- |
| stdout 被重定向到文件/管道 | 写到文件而非终端 | 非 TTY 时按配置报错或走 headless |
| stdin 被重定向 | 读不到键盘 | 优先打开 `/dev/tty` |
| `SIGWINCH` 不触发（容器 / CI / 伪终端转发） | 尺寸不更新 | 每次 present 前惰性查询尺寸 |
| 部分写入 | 撕裂帧 | 循环写；出错返回 `TC_ERR_IO` 并不更新 front |
| `ECHO` 未关（raw 失败） | 按键回显破坏画面 | `enter` 后校验 mode，失败返回错误 |
| 终端不支持 1006 | 鼠标坐标上限 223 | 能力位决定启用哪种模式 |
| tmux 内层鼠标 | 需外层也启用 | 正常写序列即可；不支持时能力降级 |
| 非法 UTF-8（终端或粘贴） | 乱码 | 严格解码 + U+FFFD 替换 + 日志 |
| 粘贴内容含 Esc 序列 | 注入风险 | 括号粘贴模式下不解析内容；非粘贴模式由上层自行校验 |
| 崩溃后终端处于 raw | 用户终端不可用 | `leave` 幂等 + `atexit` + 信号钩子；恢复序列为编译期常量 |
| 高 DPI / 字体变化 | 像素尺寸不准 | 像素尺寸仅作参考，布局一律用行列 |
| Linux console 字体限制 | 无法显示某些字符 | 库不处理字体；由用户选择终端与字体 |

---

## 4. 后端自检清单（实现阶段）

- [ ] 非 TTY 下能安全创建（返回受限能力而非崩溃）
- [ ] raw 进入与恢复可重复 100 次无状态漂移
- [ ] 尺寸在信号缺失时能惰性刷新
- [ ] 输入在"逐字节喂入"与"整块喂入"两种情况下结果一致
- [ ] 大帧写入不会撕裂（循环写 + 错误返回）
- [ ] 信号处理器内不调用库 API
- [ ] 退出与崩溃路径都能恢复终端（含 raw、altscreen、鼠标、光标）
