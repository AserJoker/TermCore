# TermCore

终端 TUI 的**底层 API 库**：C11 编写、导出稳定 C ABI、跨平台。

TermCore 只提供四类原语：**控制终端、拉取输入事件、绘制单元格、处理 Unicode 文本**。
它不做 widget / 布局 / 组件系统，也**不负责字体选择与终端选择**——这些完全由使用者决定。

编码范式：句柄式对象、options 由库函数默认填充、调用方自写主循环、surface 句柄 + 显式提交一帧。
库提供机制，不提供策略——widget / 布局 / 组件留给上层。

> **当前阶段：骨架。** 仓库内是设计文档集 + 可构建的工程骨架（`termcore` 库 / `termcore_test` / `termcore_demo`）。
> 实现阶段按 `docs/10-build-and-test.md` 的里程碑推进：先 `common` / `memory` / `platform`，再逐层填充 `control` → `caps` → `input` → `text` → `render`。

---

## 构建

| 平台 | 工具链 | 预设 |
| --- | --- | --- |
| Windows | clang + ninja（MSVC target） | `windows-clang` / `windows-clang-release` |
| Windows（备选） | cl + ninja | `windows-msvc` |
| Linux / WSL | clang + ninja | `linux-clang` / `linux-clang-release` |
| macOS | 无验证环境，暂搁置 | — |

```bash
# Windows（PowerShell）
cmake --preset windows-clang
cmake --build --preset windows-clang
ctest  --preset windows-clang

# WSL / Linux（在 WSL 内执行）
cmake --preset linux-clang
cmake --build --preset linux-clang
ctest  --preset linux-clang
```

测试用 GoogleTest，默认经 `FetchContent` 拉取（首次配置需网络，之后缓存在 `build/_deps`）。
离线可用 `TERMCORE_GTEST_SOURCE=LOCAL`（把源码放到 `third_party/googletest`）或 `SYSTEM`（vcpkg / 系统包），
也可直接复用已有源码：`-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=<path>`。

---

## 特性总览

| 层次 | 能力 |
| --- | --- |
| 控制层 | raw mode 开关、alternate screen 进出、光标显隐与形状、窗口尺寸与 resize 事件、SIGWINCH / CTRL_C 信号事件化、退出与崩溃时的终端恢复、headless 离线模式；**运行时特性开关**（鼠标 / 焦点 / 括号粘贴 / Kitty 键盘 / 同步更新可运行期随时开合，与初始化属性分离） |
| 能力检测层 | TERM / terminfo / termcap 解析、设备属性查询（DA）、环境变量覆盖；产出能力位集驱动降级。**只识别能力，不选择终端**。特性列表（profile）可整体读写并序列化缓存；**交互式探测**：库提供每项样例的渲染与判定，应用渲染向导 UI 让用户确认，结论写回能力位集并可存盘复用（含环境指纹） |
| 输入解析层 | 严格 UTF-8 解码、修饰键与功能键、Kitty 键盘协议及降级、**完整鼠标**（按下 / 释放 / 拖动 / 移动 / 滚轮、SGR 1006、坐标裁剪）、焦点、括号粘贴、**ESC 零延迟结算**（贪婪读取 + 到期判定，库不阻塞 / 不忙等）、resize 事件 |
| 文本引擎 | ASCII / Unicode 双模式；Unicode 模式按完整 **UAX#29 字符簇**计算渲染宽度（emoji ZWJ、变体选择符、区域指示符、复杂组合）；解码 / 簇切分 / 宽度 / 测量 / 截断 / 换行点；无状态无分配纯函数，可独立使用 |
| 输出渲染层 | surface 即**离屏单元格网格**：直接绘制 cell（put_cell / fill_rect / draw_text），最后 `tc_present()` 提交最终帧（同步阻塞）；行级 diff 经**行哈希 + 有效区间**三层剪枝、批量写出；真彩 / 256 / 16 色与样式、宽字符占位；多输出 sink 广播、增量变更回调、只读零拷贝帧视图 |

## 双模式文本引擎

`tc_text_*` 是**独立公开**的模块，不依赖 `tc_term_t` / `tc_surface_t`，上层布局框架可直接调用。

- **ASCII 模式**：严格 7bit。非 ASCII 走替换字符（默认 `?`，可配置 U+FFFD），宽度恒为 1，不走 UTF-8 多字节解码
- **Unicode 模式**：按 UAX#29 字符簇切分，簇宽度由 East Asian Width 决定（W / F = 2，组合符 = 0），Ambiguous 宽度可配置
- 全部为**无状态、无分配纯函数**，`tc_present` / `tc_get_event` 热路径上零分配
- Unicode 数据表默认内置生成（编译进二进制，无运行时数据文件）；可选 ICU 后端（`TERMCORE_USE_ICU=ON`，默认 ON：由 FetchContent 构建 ICU 静态库，`icudt*.dat` 作为外部文件在运行时从 `build/<preset>/data` 加载）

## 可测试性与外部工具

| 能力 | API |
| --- | --- |
| 注入输入 | `tc_term_inject_input`（喂字节，走真实解析路径）、`tc_term_inject_event`（直投事件） |
| 外部接管输入 | `tc_term_set_input_source`（vtable 完全替换，传 NULL 恢复默认） |
| 多输出 | `tc_term_add_sink`（真实终端 + 捕获器 + 自定义 sink，同一帧广播） |
| 增量变更通知 | `tc_set_change_sink`（diff 时同步回调） |
| 全量帧拉取 | `tc_surface_view`（只读零拷贝视图：cell 指针 + 宽高 + 行距 + frame_id） |

库**只提供不可变视图**，不含任何网络能力：外部工具（如 WebSocket 转发）自行取走数据。

## 最小示例

```c
#include <termcore/tc.h>

int main(void) {
    tc_term_options opt;
    tc_init_term_options(&opt);          /* 默认填充，无 cb_size 字段 */
    opt.mouse        = true;
    opt.focus_events = true;

    tc_term_t* term = NULL;
    if (tc_term_create(&opt, &term) != TC_OK) return 1;
    if (tc_term_enter(term) != TC_OK) { tc_term_destroy(term); return 1; }

    int cols = 0, rows = 0;
    tc_term_get_size(term, &cols, &rows);

    tc_surface_t* s = NULL;
    tc_surface_create(term, cols, rows, &s);

    for (;;) {
        /* 1. 非阻塞抽干已就绪事件（贪婪读取，零延迟；库不 sleep、不忙等） */
        tc_event ev; bool has = false;
        while (tc_get_event(term, &ev, &has) == TC_OK && has) {
            switch (ev.kind) {
            case TC_EV_KEY:
                if (ev.u.key.mods & TC_MOD_CTRL && ev.u.key.codepoint == 'q') goto done;
                break;
            case TC_EV_RESIZE:
                tc_surface_resize(s, ev.u.resize.cols, ev.u.resize.rows);
                break;
            case TC_EV_MOUSE: /* ... */ break;
            default: break;
            }
        }

        /* 2. 绘制到 surface */
        tc_surface_clear(s);
        tc_surface_draw_text(s, 0, 0, "Hello, TermCore!", NULL, NULL);

        /* 3. 同步提交一帧 */
        tc_present(term, s);

        /* 4. 帧间隔由调用方自己控制（示例 sleep 省略；库内不 sleep） */
    }

done:
    tc_surface_destroy(s);
    tc_term_leave(term);      /* 幂等，同时挂在 atexit 与信号钩子 */
    tc_term_destroy(term);
    return 0;
}
```

## 设计文档

| 文档 | 内容 |
| --- | --- |
| `docs/00-overview.md` | 目标与非目标、术语表、编程模型、v1 范围、设计原则、里程碑 |
| `docs/01-architecture.md` | 五层职责边界、平台后端抽象、可插拔 I/O、数据流与生命周期状态机 |
| `docs/02-api-control.md` | 控制层 API |
| `docs/03-api-capability.md` | 能力检测层 API 与降级矩阵 |
| `docs/04-api-input.md` | 输入解析层 API 与解析器状态机 |
| `docs/05-text-engine.md` | 双模式文本引擎（UAX#29、宽度、测量排版） |
| `docs/06-api-render.md` | 输出渲染层 API、diff 算法、present 流程 |
| `docs/07-memory-and-errors.md` | allocator、错误码、ABI 与线程模型 |
| `docs/08-platform-notes.md` | 平台实现要点与已知坑 |
| `docs/09-headless-and-tooling.md` | 离线测试与外部工具集成契约 |
| `docs/10-build-and-test.md` | CMake 结构、测试策略、demo 清单、验收标准 |

## 构建开关

```
TERMCORE_BUILD_TESTS     构建 GoogleTest 测试（默认 ON）
TERMCORE_BUILD_EXAMPLES  构建示例（默认 ON）
TERMCORE_BUILD_SHARED    构建动态库（默认 OFF，静态库）
TERMCORE_WERROR          警告视为错误（默认 OFF）
TERMCORE_GTEST_SOURCE    GoogleTest 来源：AUTO | LOCAL | FETCH | SYSTEM（默认 AUTO）
TERMCORE_USE_ICU         ICU 作为 Unicode 后端（默认 ON，FetchContent 构建；置 OFF 用内置生成表）
```

当前骨架已实现：`tc_status_*`、`tc_version_*`、`tc_allocator_*`（含测试用计数分配器）、`tc_platform_*`。

## 非目标

- 不选择、不启动终端模拟器；不处理字体选择与字形渲染
- 不做 widget / 布局 / 组件系统（留给上层库）
- 不提供网络传输、序列化与压缩
- 不做双向文本（bidi）与复杂字形整形（shaping）
- 不内置事件循环、不为输入事件提供回调注册
