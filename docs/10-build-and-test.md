# 10 · 构建、测试与验收

本文定义工程结构、CMake 组织、Unicode 表生成、测试策略、demo 清单与各里程碑的验收标准。

---

## 1. 目录结构

```
TermCore/
├── CMakeLists.txt
├── CMakePresets.json          # windows-clang / linux-clang / windows-msvc
├── cmake/
│   ├── TermCoreWarnings.cmake     # 警告集（clang-cl / MSVC 与 GNU 分流）
│   └── TermCoreDependencies.cmake # GoogleTest 获取策略
├── include/termcore/
│   ├── tc.h              # 总头
│   ├── tc_export.h       # TC_API 符号可见性
│   ├── tc_status.h       # 状态码
│   ├── tc_version.h      # 版本与 ABI
│   ├── tc_memory.h       # allocator
│   ├── tc_platform.h     # 平台探测
│   ├── tc_term.h         # 控制层（docs/02）
│   ├── tc_caps.h         # 能力层（docs/03）
│   ├── tc_input.h        # 输入层（docs/04）
│   ├── tc_text.h         # 文本引擎（docs/05）
│   └── tc_surface.h      # 渲染层（docs/06）
├── src/                       # 按层分目录，GLOB 自动收录新增 *.c
│   ├── common/           # status、version
│   ├── memory/           # allocator（含测试用计数分配器）
│   ├── platform/         # 平台后端（win32 / posix）
│   ├── control/          # 控制层（docs/02）
│   ├── caps/             # 能力层（docs/03）
│   ├── input/            # 输入层（docs/04）
│   ├── text/             # 文本引擎 + 内置 Unicode 表（生成）
│   └── render/           # 渲染层（docs/06）
├── tools/                     # gen_ucd_tables.py（待落地）
├── tests/                     # GoogleTest，*.cpp
├── examples/
└── docs/
```

产物：

| 产物 | 名称 |
| --- | --- |
| 库 | `termcore.lib` / `libtermcore.a`（静态，默认）；`TERMCORE_BUILD_SHARED=ON` 时为动态库 |
| 测试 | `termcore_test`（GoogleTest，`ctest` 驱动） |
| 示例 | `termcore_hello`（随层数增加扩展） |
| ICU 变体 | `libtermcore_icu.a`（可选，后缀区分） |
| 导出包 | `termcore-config.cmake`、`termcore-targets.cmake` |

---

## 2. CMake 组织

```cmake
cmake_minimum_required(VERSION 3.21)
project(termcore VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)         # 库：C11
set(CMAKE_CXX_STANDARD 20)       # 仅测试：C++20（GoogleTest）

option(TERMCORE_BUILD_TESTS    "构建 GoogleTest 测试" ON)
option(TERMCORE_BUILD_EXAMPLES "构建示例"             ON)
option(TERMCORE_BUILD_SHARED   "构建动态库"           OFF)
option(TERMCORE_WERROR         "警告视为错误"         OFF)
```

| Target | 内容 |
| --- | --- |
| `termcore` | 库（默认静态）；`include/` 为 `PUBLIC` 包含目录 |
| `termcore_test` | GoogleTest，`tests/*.cpp` 自动 GLOB，`gtest_discover_tests` 注册到 ctest |
| `termcore_hello` | 最小示例（版本 / 平台 / 分配器） |
| `termcore_icu`（可选） | ICU 后端的库变体（待落地） |

### 2.1 工具链

| 平台 | 工具链 | 状态 |
| --- | --- | --- |
| Windows | `clang` + `clang++`（MSVC target）+ `ninja` + MSVC 的 `link.exe` | 主工具链，已验证 |
| Windows | `cl` + `ninja`（`windows-msvc` 预设） | 备选，实验 |
| Linux / WSL | `clang` + `ninja`（`linux-clang` 预设） | 已验证（WSL2 Ubuntu：CMake 4.2 / clang 21 / ninja 1.13） |
| macOS | — | 无验证环境，暂搁置 |

构建命令：

```bash
# Windows（PowerShell）
cmake --preset windows-clang
cmake --build --preset windows-clang
ctest --preset windows-clang

# Release
cmake --preset windows-clang-release

# WSL / Linux（在 WSL 内执行，源码位于 /mnt/d/projects/TermCore）
cmake --preset linux-clang
cmake --build --preset linux-clang
ctest --preset linux-clang
```

### 2.2 依赖：GoogleTest

`TERMCORE_GTEST_SOURCE` 决定来源（`cmake/TermCoreDependencies.cmake`）：

| 值 | 行为 | 适用 |
| --- | --- | --- |
| `AUTO`（默认） | `third_party/googletest` 存在则用之，否则 `FETCH` | 常规开发 |
| `LOCAL` | `add_subdirectory(third_party/googletest)` | 离线构建（把源码放到该目录，已被 `.gitignore` 忽略） |
| `FETCH` | `FetchContent` 拉 `v1.17.0`（首次配置需网络，之后走 `build/_deps` 缓存） | 默认路径 |
| `SYSTEM` | `find_package(GTest REQUIRED)` | vcpkg / 发行版包 |

离线的两种快捷方式：

```bash
# 1) 复用已有源码，不重新下载
cmake --preset windows-clang -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=<path-to-googletest-src>

# 2) 用 vcpkg 安装的 gtest
cmake --preset windows-clang -DTERMCORE_GTEST_SOURCE=SYSTEM \
      -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake
```

> Windows 上必须让 gtest 与工程使用同一 CRT，依赖模块已强制 `gtest_force_shared_crt=ON`。

编译要求：

- C11（`-std=c11`），**禁用 GNU 扩展**；POSIX 后端需 `_POSIX_C_SOURCE` / `_DEFAULT_SOURCE`
- 测试为 C++20；库与测试用同一套警告：GNU 系 `-Wall -Wextra -Wpedantic`，MSVC 系 `/W4 /utf-8`；`TERMCORE_WERROR=ON` 追加 `-Werror` / `/WX`
- 警告只作用于工程目标，第三方（GoogleTest）保留其自身设置
- 头文件可被 C++ 编译（公开头一律 `extern "C"`；测试用例本身即 C++，天然校验）

---

## 3. Unicode 表生成

```
tools/gen_ucd_tables.py  →  src/text/ucd_tables_generated.c / .h
```

| 表 | UCD 源文件 |
| --- | --- |
| `Grapheme_Cluster_Break` | `GraphemeBreakProperty.txt` |
| `Extended_Pictographic` | `emoji-data.txt` |
| `East_Asian_Width` | `EastAsianWidth.txt` |
| `Emoji_Presentation` | `emoji-data.txt` |
| 组合 / 默认忽略 | `DerivedCoreProperties.txt` + 通用类别推导 |

- 生成结果**随源码提交**（离线可构建，无需网络）
- 编码方式：区间表 + 两级索引（BMP 常用区一级索引，其余二分）
- 体积目标：全部表合计 ≤ 100 KB
- 生成器记录所用 Unicode 版本，写入生成文件头部注释

可选 ICU：

```cmake
cmake -DTERMCORE_USE_ICU=ON ..
```

- 链接系统 ICU（`icu-uc` 等），数据由系统提供，**不打包 `.dat`**
- 同一套 `tc_text_*` API；测试集在两种构建下都要通过
- 默认关闭，保证"零强制依赖"

---

## 4. 测试策略

### 4.1 分层

| 层 | 位置 | 依赖 | 覆盖 |
| --- | --- | --- | --- |
| 纯逻辑单测 | `tests/unit` | 无 I/O | 文本引擎、解码、簇、宽度、测量、颜色降级、序列解析、diff |
| 注入式端到端 | `tests/e2e` | headless + 注入输入 + 捕获 sink | 事件语义、渲染结果、能力降级、变更回调、帧视图 |
| 真机（opt-in） | `tests/manual` | 真实 TTY | 兼容性、鼠标、resize、焦点、真实终端差异 |

### 4.2 断言框架

自带极简框架（无第三方）：

```c
#define TC_ASSERT(cond)            /* 失败即记录并计数 */
#define TC_ASSERT_EQ(a, b)
#define TC_ASSERT_STR_EQ(a, b)
#define TC_ASSERT_MEM_EQ(a, b, n)
```

- 失败不中断（累计计数），结束时汇总并返回非零退出码
- 支持按名称过滤用例（命令行参数）
- 无分配依赖，可在任意环境跑

### 4.3 关键测试清单

**文本引擎（`tests/unit/text_*`）**

- UAX#29 官方 `GraphemeBreakTest.txt` 全量
- emoji：ZWJ 家族、VS16 / VS15、RI 国旗（成对与奇数个）、键帽序列
- 宽度：CJK、全角、半角、组合、控制、Ambiguous 两种配置
- UTF-8：合法、overlong、代理区、截断、孤立续字节、超范围、逐字节喂入
- 测量 / 截断 / 换行点：混合字符串
- ASCII 模式：替换字符、宽度恒 1、不解码多字节
- Fuzz：随机字节不崩溃、不越界

**输入（`tests/unit/input_*` + `tests/e2e`）**

- 键：可打印、控制字符、功能键、方向键、各修饰组合
- 协议：xterm 修饰参数、Kitty 三种事件类型、传统与 Kitty 混合、畸形序列
- 鼠标：SGR 全部按钮与动作、滚轮、修饰、X10 兼容、坐标转换、越界裁剪
- 粘贴：含换行 / Tab / UTF-8 / 超大 / 空
- ESC 结算：孤立 ESC、ESC+字符、双 ESC（用虚拟时钟）；断言拉取零延迟（不随 `esc_timeout_ms` 阻塞）
- 贪婪读取：一次 write 被拆成多次 read、读到空即停、单批上限后剩余下次继续
- resize：合并、像素尺寸
- 注入与接管：优先级、切分、source 错误、恢复

**渲染（`tests/unit/render_*` + `tests/e2e`）**

- diff：无变化不输出、run 合并、SGR 最小化、光标最小化
- 宽字符：占位、覆盖半格、行首孤立续格
- 颜色降级：真彩 → 256 → 16 → 单色
- present：失败不更新 front、sink 失败隔离、frame_id 递增
- 剪枝正确性：行哈希快路径与 `strict_diff` 逐格比对结果一致；"标脏但未变"的行被跳过；有效区间裁剪后输出不变
- 图层：blit 源只读可复用、源矩形越界报错、目标越界裁剪、OVER 跳过透明格、自拷贝报错、宽字符跨界
- 运行时开关：开 / 关幂等（重复设同值不重复写序列）、`CREATED` 态设置延后到 `enter` 生效、能力不支持返回 `TC_ERR_UNSUPPORTED` 且状态不变、渐进特性 `requested ≠ effective` 可查、鼠标四档切换正确、关闭后残留序列被识别但不产出事件、`leave` 只关已生效项
- 帧视图：布局、只读、失效语义

**能力（`tests/unit/caps_*`）**

- 环境变量优先级、`NO_COLOR`、CI 行为
- 覆盖 API 生效
- DA 查询超时与降级（虚拟时钟）
- 特性列表：逐项 `origin` / `user_confirmed` 正确、`set_caps_bits` 增量写入不动其它位、`apply_caps_profile` 整体替换、写入后开关 `effective` 随动
- profile 序列化：二进制与文本**往返一致**、`cap = 0` 求长度、版本不符返回 `TC_ERR_VERSION`、损坏字节返回 `TC_ERR_PARSE`
- 指纹：`TERM` / `TERM_PROGRAM` / DA id 变化即不匹配；`saved_at_ms` 不参与比对；缺项按默认（低估）
- 探测向导：headless + 脚本化 `probe_answer` / `probe_feed` 走完整个流程；AUTO 项自动判定、超时转 CONFIRM、跳过即保持默认、`finish` 后来源为 `TC_CAPS_ORIGIN_PROBE`
- 探测渲染：样例按 `TC_CAPS_SAMPLE_RAW` **绕过降级**画出、只写给定矩形、不产生事件

### 4.4 质量门

| 门 | 要求 |
| --- | --- |
| 单测 | 全绿；关键模块覆盖率 ≥ 80%（行覆盖） |
| Sanitizer | ASan + UBSan 下全绿（GCC / Clang） |
| 警告 | 零警告（`-Werror` / `/WX`） |
| 内存 | 用例结束零泄漏（可用自定义 allocator 统计断言） |
| Fuzz | 解析器与文本引擎随机输入 10^6 次不崩溃 |
| 双构建 | 内置表构建与 ICU 构建都跑关键用例 |
| C++ 头校验 | 公开头可被 C++ 编译 |

---

## 5. demo 清单

| demo | 演示 |
| --- | --- |
| `events_demo` | 打印所有事件的 kind 与字段（键、鼠标、焦点、粘贴、resize）；退出时用 CTRL_C / `q` |
| `mouse_demo` | 鼠标按下 / 拖动 / 滚轮绘制；显示坐标与修饰键 |
| `palette_demo` | 16 / 256 / 真彩色板与渐变；直观验证颜色降级 |
| `scroll_demo` | `scroll_rect` 滚动区域 + 双缓冲局部更新 |
| `unicode_width_demo` | CJK、emoji ZWJ、组合符、RI 国旗的宽度与占位；可切 ASCII / Unicode 模式 |

每个 demo 都应支持 `--headless` 便于 CI 冒烟（跑几帧后退出并自检无错误）。

---

## 6. 里程碑与验收标准

### M1 · 骨架 + 控制层

| 项 | 验收 |
| --- | --- |
| 构建 | CMake 在各目标平台（GCC / Clang）均能构建 |
| 生命周期 | `create / enter / leave / destroy` 可用；`leave` 幂等 |
| raw / altscreen / 光标 / 尺寸 | 真实终端生效 |
| 信号 | SIGWINCH → resize 事件；CTRL_C → 按键事件 |
| 崩溃安全 | 强制 kill 与 `CTRL_C` 默认行为下终端可恢复；`atexit` 生效 |
| headless | 无 TTY 可创建并运行 |

### M2 · 能力检测

| 项 | 验收 |
| --- | --- |
| 检测链 | 环境变量 > DA > terminfo/termcap > 内置库 > 保守默认 |
| 不阻塞 | DA 超时 200ms 内返回；CI / 非 TTY 自动跳过 |
| 降级 | 在 xterm / tmux / `TERM=dumb` / 未知 TERM 下得到合理能力集 |
| 覆盖 API | 可用覆盖构造任意能力组合（测试用） |

### M3 · 文本引擎

| 项 | 验收 |
| --- | --- |
| UAX#29 | 官方 `GraphemeBreakTest.txt` 全绿 |
| 宽度 | EAW 一致；emoji ZWJ / VS16 / RI 正确 |
| 双模式 | ASCII 严格 7bit；Unicode 完整 |
| 排版辅助 | 测量 / 截断 / 换行点用例全绿 |
| 零分配 | 全部纯函数；压力下无分配 |
| 两种后端 | 内置表与可选 ICU 构建都通过关键用例 |

### M4 · 输入解析

| 项 | 验收 |
| --- | --- |
| 解码 | 严格 UTF-8；跨块切分、非法序列处理正确 |
| 键鼠 | 功能键、修饰键、SGR 鼠标全部动作、滚轮、焦点、粘贴 |
| 协议 | Kitty 渐进启用与降级正确；与传统序列混合不误判 |
| ESC 结算 | 虚拟时钟下确定性可测；单次拉取耗时不随 `esc_timeout_ms` 增长（零延迟读取 + 到期结算） |
| 注入 / 接管 | 优先级与接管协议符合契约 |

### M5 · 渲染 + 可测试性

| 项 | 验收 |
| --- | --- |
| 绘制 | 单元格级 + 批量辅助均正确；越界返回错误 |
| diff | 只输出变化；run 合并与 SGR / 光标最小化生效 |
| present | 同步阻塞、单次写、失败不更新 front |
| 多 sink | 同帧广播、失败隔离 |
| 变更回调 | diff 阶段同步、借用语义清晰 |
| 帧视图 | 零拷贝只读、`frame_id` 失效自检 |
| 离线 | CI 中 headless + 注入输入 + 捕获 sink 全绿 |
| 真机 | 5 个 demo 在 Linux / macOS / BSD 的各类终端表现一致 |
| 性能 | 200×50 全脏帧 diff + 输出 < 5ms（参考值，不含终端解析） |

---

## 7. 发布与版本

| 项 | 约定 |
| --- | --- |
| 版本号 | `MAJOR.MINOR.PATCH`，另有 `tc_abi_version()` |
| 兼容规则 | 见 `07-memory-and-errors.md` §3.5 |
| 发布物 | 头文件 + 静态库 + CMake 包配置 + 文档 |
| 变更记录 | 记录 API 新增 / 行为变更 / 已知问题 |
| 支持矩阵 | 明确编译器与平台的最低版本（GCC 8+、Clang 8+；Linux、macOS、BSD 等） |
