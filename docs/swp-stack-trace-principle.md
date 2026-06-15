# swp_stack_trace 原理文档

> **一份「总—分—总」结构的技术文档，由浅入深解释本库的设计与实现。**
>
> 作者：Claude Code / tshua | 日期：2026-06-16 | C++17 / LLVM libunwind / Linux

---

## 一、总览：这是什么，解决什么问题

**`swp_stack_trace` 是一个 C++17 异步信号安全的崩溃堆栈打印库。** 三句话概括：

1. 如果你写的 C++ 服务在线上因为某个信号（如 SIGSEGV）崩溃了，`swp_stack_trace::print_stacktrace()` 可以在信号 handler 里安全地打印出**当前的完整调用链**。
2. 库本身**不安装任何信号 handler**，也**不修改进程的信号处理行为**——集成方在自己的 handler 中调用它，打印完后自行决定是 re-raise、core dump 还是优雅退出。
3. 考虑到信号 handler 的执行环境极其受限，库的实现**只用栈上缓冲区和 `write()` 系统调用**，绝不触碰 `malloc`、`printf`、C++ 异常或任何非异步安全的操作。

```cpp
// 最简单的集成：3 行代码
#include "swp_stack_trace.h"
void my_handler(int sig) {
    swp_stack_trace::print_stacktrace(STDERR_FILENO);  // 打印堆栈
    std::signal(sig, SIG_DFL);                          // 恢复默认
    std::raise(sig);                                     // 重新抛出
}
```

---

## 二、背景：信号与崩溃

### 2.1 信号是什么

信号（signal）是 Unix/Linux 内核向进程发送的一种**软件中断**。当 CPU 检测到非法操作（如除零、访问无效地址），或用户/系统主动发送信号时，内核将对应的信号递送给目标进程。

信号的四种归宿：

```
┌──────────────────────────────────────────────────────────┐
│  信号递送（kernel → target process）                      │
│                                                          │
│  ┌─────────────┐  ┌─────────────┐  ┌──────────────┐     │
│  │ 执行默认动作  │  │ 自定义 handler│  │ 忽略信号     │     │
│  │ (term/core)  │  │ (sigaction)  │  │ (SIG_IGN)    │     │
│  └─────────────┘  └─────────────┘  └──────────────┘     │
│                                                          │
│  不可捕获/忽略：SIGKILL (9)、SIGSTOP (19)                 │
│  不可忽略但可捕获：绝大多数信号                            │
└──────────────────────────────────────────────────────────┘
```

一个信号从诞生到被处理的全过程：

```
  CPU 硬件异常
  (除零 / 缺页 / 非法指令)
       │
       ▼
  内核异常处理程序
  (trap handler)
       │
       ├── 评估异常类型
       │
       ▼
  内核发送信号
  (force_sig / send_signal)
       │
       ▼
  进程控制块
  (task_struct → pending signals)
       │
       ▼
  准备返回用户态时检查 pending
  (do_signal / handle_signal)
       │
       ▼
  调用用户注册的 handler
  (sigaction → sa_handler)
       │
       ▼
  用户的 handler 中调用
  swp_stack_trace::print_stacktrace()   ← 我们的库
       │
       ▼
  handler 返回 / re-raise
```

> 关键约束：信号 handler 在进程的原始上下文中执行，但**被中断的代码可能正持有锁**（如 `malloc` 的内部锁）。因此 handler 中只能使用 POSIX 列出的「异步信号安全」函数。

### 2.2 async-signal-safe 约束

POSIX 标准定义了约 120 个可以在信号 handler 中安全调用的函数。核心原则：

- ✅ `write()`、`read()`、`open()`、`close()` — 直接系统调用
- ✅ `_exit()`、`abort()` — 终止型调用
- ✅ `signal()`、`raise()`、`sigaction()` — 信号管理本身
- ❌ `printf()`、`malloc()`、`fopen()` — 内部有锁/有缓冲区
- ❌ 任何 C++ STL 容器 — 可能在构造/析构时分配内存

**一个经典的死锁场景：**

```
线程 A 中：
  malloc() 获取 arena_lock
    → 正在操作空闲链表
    → 收到 SIGSEGV                     ← 信号中断！
    → 进入 handler
    → handler 中调用 printf()          ← printf 内部也调 malloc！
    → malloc 尝试获取 arena_lock       ← 死锁！
```

本库通过**只用 `write()` + 栈缓冲区**来彻底规避这个问题。

---

## 三、核心原理

### 3.1 栈展开（Stack Unwinding）原理

#### 3.1.1 什么是调用栈

每一次函数调用，CPU 都会在栈上分配一个**栈帧（stack frame）**，包含：

```
  高地址
  ┌──────────────────┐
  │  main 的栈帧      │  ← 局部变量 + 返回地址
  ├──────────────────┤
  │  crash_level_3   │  ← 调用 crash_level_2 前保存的 LR/FP
  ├──────────────────┤
  │  crash_level_2   │  ← 调用 crash_level_1 前保存的 LR/FP
  ├──────────────────┤
  │  crash_level_1   │  ← 调用 g_trigger_fn 前保存的 LR/FP
  ├──────────────────┤
  │  trigger_sigsegv  │  ← 当前执行到 *p = 42
  └──────────────────┘
  低地址  (SP →)
```

每个帧的**返回地址（return address, LR/PC）**记录了「当前函数返回后，应该回到哪里继续执行」。栈展开（unwinding）就是**沿着帧指针链（FP chain）或 DWARF unwind table 逐帧回溯这些返回地址**，从而还原出完整的调用链。

#### 3.1.2 两种展开方式

| | Frame-Pointer 方式 | DWARF Unwind Table 方式 |
|---|---|---|
| 依赖 | 编译器保留 `rbp`/`fp` 寄存器 | ELF 中的 `.eh_frame` / `.debug_frame` 段 |
| 速度 | 快（只读取寄存器） | 慢（需要解析 DWARF 字节码） |
| 可靠性 | 受 `-fomit-frame-pointer` 影响 | 可靠（独立于优化级别） |
| 信号安全 | 完全安全 | libunwind 的 local unwind 是安全的 |

`libunwind` 使用 DWARF 方式，因此不受编译器优化选项影响。

#### 3.1.3 libunwind 工作流程

```
print_stacktrace(fd)
  │
  ├── 1. unw_getcontext(&ctx)
  │       获取当前 CPU 寄存器快照（SP、PC、FP 等）
  │
  ├── 2. unw_init_local(&cursor, &ctx)
  │       用寄存器快照初始化 unwind cursor
  │
  ├── 3. do { unw_step(&cursor) } while (cursor 还有上一帧)
  │       │
  │       ├── unw_get_reg(&cursor, UNW_REG_IP, &pc)
  │       │     从当前帧读取返回地址（PC）
  │       │
  │       ├── unw_get_proc_name(&cursor, buf, sizeof(buf), &offset)
  │       │     从符号表读取函数名（mangled C++ 名）
  │       │
  │       └── write(fd, "#N  0x<pc>  <name>+<offset>\n")
  │             将格式化后的行写入 fd
  │
  └── 4. 返回
```

**为什么 libunwind 是信号安全的？**

- `unw_getcontext()` — 纯寄存器读取（读 `ucontext_t`），无系统调用
- `unw_init_local()` — 纯内存初始化，在栈上分配 cursor
- `unw_step()` — 读取进程地址空间中已映射的 `.eh_frame` 段（程序加载时 kernel 已 mmap）
- `unw_get_reg()` — 从 cursor 结构体中读寄存器值
- `unw_get_proc_name()` — 读 `.symtab` / `.strtab`（同样是早已映射的只读内存）

所有这些操作只读取**进程自己地址空间中已有的数据**，不调用 `open`、`malloc` 或任何可能阻塞的系统调用。

### 3.2 手写格式化：为什么不用 printf/snprintf

POSIX 标准中 `snprintf` 实际上**也是 async-signal-safe**（在较新的版本中），但我们仍选择手写格式化，因为：

1. **零依赖**：不依赖具体 glibc/libc 实现的内部细节
2. **无缓冲**：`printf` 族函数可能有内部缓冲区，在极端崩溃场景下缓冲区状态不可靠
3. **可审计**：每行代码一目了然，没有隐藏的分配或锁

```cpp
// 我们自己的格式器——完全是栈上操作 + write()
void write_hex(int fd, std::uintptr_t value) {
    char buf[64];           // 栈上固定大小
    int i = 0;
    do {
        buf[i++] = "0123456789abcdef"[value & 0xF];
        value >>= 4;
    } while (value != 0);
    write_string(fd, "0x");
    while (i > 0) write_char(fd, buf[--i]);
}
```

### 3.3 完整数据流图

```
  ┌─────────────────────────────────────────────────────────────────────┐
  │                        信号处理完整时序                             │
  │                                                                     │
  │  用户程序运行中                                                     │
  │    │                                                                │
  │    │  segfault / div0 / ...                                         │
  │    ▼                                                                │
  │  ┌─────────────────────┐                                            │
  │  │     内核 CPU 异常     │                                           │
  │  │  发送信号到进程       │                                           │
  │  └─────────┬───────────┘                                            │
  │            │                                                        │
  │            │  do_signal()                                           │
  │            ▼                                                        │
  │  ┌─────────────────────┐                                            │
  │  │   用户 signal handler │  ← 使用方注册的 handler (SA_ONSTACK)       │
  │  │     print_stacktrace │  ← 我们的库，只做 write() + libunwind      │
  │  │     re-raise 或 exit  │  ← 交还给使用方决定                        │
  │  └─────────┬───────────┘                                            │
  │            │                                                        │
  │            ├── 选项 A: SIG_DFL + raise → core dump + 终止           │
  │            ├── 选项 B: _exit(EXIT_FAILURE) → 直接退出，无 core       │
  │            └── 选项 C: 返回 → 继续执行（不推荐，UB 后不安全）          │
  └─────────────────────────────────────────────────────────────────────┘
```

---

## 四、架构设计：零侵入

### 4.1 设计原则

| 原则 | 含义 | 实现方式 |
|------|------|---------|
| **不安装 handler** | 库的代码中没有任何 `sigaction` / `signal` 调用 | 集成方在自己的代码中注册 handler |
| **不吞信号** | handler 执行完后必须让信号继续传播 | 使用方调用 `SIG_DFL + raise` 或直接 `_exit` |
| **不分配内存** | 信号路径上零动态分配 | 所有数据在栈上 (`char buf[N]`) |
| **不抛异常** | `noexcept` 修饰，绝不 unwind 出 handler | 所有 libunwind 失败 `return` / `continue` |
| **不影响 core dump** | 先打印堆栈，再 re-raise | 内核生成的 core dump 内容是原始崩溃上下文 |
| **不拿锁** | 不调用任何带锁的函数 | 只用 `write(fd, ...)` —— POSIX async-signal-safe |

### 4.2 与使用方的关系

```
  ┌──────────────────────────────┐
  │  使用方的应用程序              │
  │                              │
  │  main() {                    │
  │    signal(SIGSEGV, my_hdr);  │  ← 使用方安装 handler
  │    service->run();            │
  │  }                           │
  │                              │
  │  void my_hdr(int sig) {      │
  │    swp_stack_trace            │
  │     ::print_stacktrace(fd);  │  ← 库：只做打印
  │                               │
  │    signal(sig, SIG_DFL);      │  ← 使用方决定后续行为
  │    raise(sig);                │
  │  }                           │
  └──────────────────────────────┘
  
  swp_stack_trace 库
  ┌──────────────────────────────┐
  │  void print_stacktrace(fd)   │
  │    unw_getcontext            │
  │    unw_init_local            │
  │    while (unw_step > 0)      │
  │      unw_get_reg(IP)         │
  │      unw_get_proc_name       │
  │      write(fd, ...)          │  ← 只做 write()+libunwind
  │                              │
  │  不调 sigaction              │
  │  不调 exit / abort            │
  │  不用 malloc / printf         │
  └──────────────────────────────┘
```

---

## 五、接口设计

### 5.1 公共 API

```cpp
// src/include/swp_stack_trace.h
namespace swp_stack_trace {

// 以异步安全方式将当前调用栈写入 fd。
// 默认输出到 STDERR_FILENO (fd=2)。
// 若 fd 无效或写入失败，静默失败（noexcept）。
void print_stacktrace(int fd = STDERR_FILENO) noexcept;

} // namespace swp_stack_trace
```

**只有一个函数，一个默认参数，一个修饰符。**

- 参数 `fd`：目标文件描述符。可以是一个预先 `open()` 好的日志文件、pipe、或 socket
- `noexcept`：承诺不抛异常。在信号 handler 中抛异常是未定义行为
- 静默失败：`fd < 0` 立即返回；`write()` 失败后停止写入；`unw_step()` 失败后结束循环

### 5.2 编译与链接

```bash
g++ your_app.cpp -lswp_stack_trace -lunwind
```

依赖链：`your_app → libswp_stack_trace.a → libunwind.so`

### 5.3 支持的触发方式

```
调用方自行注册 handler (sigaction/signal)
  → handler 中调用 print_stacktrace(fd)
    → 写入到 fd (stderr / 文件 / pipe / socket)
```

使用方也可以不注册任何 handler，直接在异常捕获或调试代码中调用 `print_stacktrace()`（此时不涉及任何信号安全约束）。

---

## 六、输出格式与离线解析

### 6.1 原始输出

```
#0  0x00005555555551a9  _ZN4Test4funcEi+0x25
#1  0x00005555555551d8  main+0x18
```

每行 = 帧序号 + PC 地址 + mangled C++ 函数名 + 函数内偏移。

**为什么是 mangled 名？** `demangle`（`abi::__cxa_demangle`）内部会 `malloc`，不是 async-signal-safe。库只输出 raw 信息，符号美化留给离线工具。

### 6.2 离线解析流程

```
  ┌──────────────────┐
  │ 崩溃时 stderr/日志 │
  │ #0 0x... _ZN...  │
  │ #1 0x... main    │
  └────────┬─────────┘
           │
           ▼
  ┌──────────────────────────────────────┐
  │         symbolize_stack.py            │
  │                                      │
  │  1. 正则提取 PC 地址 (0x...)           │
  │  2. 批量调用 addr2line / atos         │
  │     - Linux: addr2line -e <bin> <pc>  │
  │     - macOS: atos -o <bin> -l <base>  │
  │  3. 追加 file:line 到原始行           │
  └──────────┬───────────────────────────┘
             │
             ▼
  ┌──────────────────────────────────────┐
  │ #0 0x...  _ZN...  at main.cpp:42     │
  │ #1 0x...  main    at main.cpp:17     │
  └──────────────────────────────────────┘
```

**macOS 特例 (ASLR)**：macOS 使用 ASLR（地址随机化），运行时 PC = 文件偏移 + 随机 slide。`symbolize_stack.py` 通过 `nm` 找到符号的文件偏移，反推 slide 值，再传给 `atos`。

### 6.3 工具链

| 脚本 | 用法 | 用途 |
|------|------|------|
| `./run.sh` | 编译 + 运行 demo | 快速验证 |
| `./run_tests.sh` | 编译 + 运行 3 个单元测试 | CI/回归 |
| `./symbolize_stack.py <bin>` | stdin/管道模式 | 实时解析 |
| `./symbolize_stack.py` (无参) | 读取默认 `/tmp/swp_crash.log` | 事后分析 |
| `./test_all_crashes.py` | 清空 → 编译 → 跑 30 种 crash | 全量验证 |

---

## 七、安全性与边界

### 7.1 凭什么说它是安全的

对照 POSIX async-signal-safe 要求逐项验证：

| 检查项 | 实现 | 状态 |
|--------|------|------|
| 所有 IO 用 `write()` | ✅ `::write(fd, buf, len)` | POSIX 明确安全 |
| 不调 `malloc` / `free` | ✅ 全部 `char buf[N]` 栈分配 | |
| 不调 `printf` / `snprintf` | ✅ 手写 `write_hex` / `write_decimal` | |
| 不拿锁 | ✅ 不调 pthread 函数、不碰 C++ STL | |
| 不抛异常 | ✅ `noexcept` | |
| 不修改进程全局状态 | ✅ 无全局变量、不改 `errno` | |
| libunwind 本地路径 | ✅ `unw_init_local` 不分配内存 | LLVM 文档明确说明 |

### 7.2 不能保证什么

| 场景 | 风险 | 缓解 |
|------|------|------|
| 栈已损坏 | libunwind 读到垃圾数据，展开失败/输出乱码 | 输出只做 best-effort，错误静默返回 |
| 堆已损坏 | 库本身不碰堆，不受影响 | — |
| 崩溃发生在 `write()` 内部 | 理论上不可能——`write()` 是系统调用，内核处理 | — |
| 崩溃信号嵌套 | 可能丢失部分输出 | 使用 `SA_NODEFER` 可允许嵌套，但默认屏蔽本信号 |
| 栈溢出 | handler 没有栈空间执行 | 使用方需配置 `sigaltstack` + `SA_ONSTACK` |

### 7.3 会影响 core dump 吗

**不会。** 本库的 handler 调用顺序：

```
1. print_stacktrace()        ← 记录原始崩溃栈（此时上下文未变）
2. signal(sig, SIG_DFL)      ← 恢复信号的默认行为
3. raise(sig)                ← 内核发送信号 → 默认行为 → 终止 + core dump
```

内核生成的 core dump 文件包含的是 `raise(sig)` 时的上下文，而我们的堆栈文本是**第 1 步就打印好了的**。两者互补：文本得到原始现场、core dump 得到完整内存。

### 7.4 与第三方 allocator 的兼容性

本库不调用 `malloc`/`free`/`new`/`delete`，与 tcmalloc、jemalloc、mimalloc 等自定义分配器零交互。即使崩溃发生在分配器内部，本库也不会与之发生递归调用或二次破坏。

---

## 八、关键数据结构

### 8.1 libunwind 核心类型

```cpp
// 上下文：CPU 寄存器快照
unw_context_t context;
unw_getcontext(&context);          // 等同于 getcontext()，读当前寄存器

// Cursor：栈展开的"迭代器"
unw_cursor_t cursor;
unw_init_local(&cursor, &context); // 从 context 初始化 cursor，指向当前帧

// 展开循环
while (unw_step(&cursor) > 0) {   // >0: 成功展开到上一帧; 0: 栈顶，结束
    unw_word_t pc;
    unw_get_reg(&cursor, UNW_REG_IP, &pc);          // 读当前帧的 PC
    unw_get_proc_name(&cursor, buf, len, &offset);  // 读当前帧的函数名
}
```

### 8.2 我们的格式化缓冲区

```cpp
constexpr std::size_t k_name_buffer_size = 256;    // libunwind 函数名缓冲
constexpr std::size_t k_hex_buffer_size = 64;       // 16 进制 PC 格式化缓冲
constexpr int k_decimal_buffer_size = 16;           // 10 进制帧序号格式化缓冲
```

全部栈上分配，在函数入口处创建，退出时自动回收（RAII via stack unwinding）。

---

## 九、运行与验证

### 9.1 单元测试覆盖

```bash
./run_tests.sh
```

3 个测试用例：

| 测试 | 验证点 |
|------|--------|
| `does_not_crash` | 普通调用不崩溃 |
| `output_contains_current_function` | 输出包含当前函数名（用 `dup2` 捕获 stderr） |
| `works_from_signal_handler` | 信号 handler 内正常工作（用 `SIGUSR1` 触发） |

### 9.2 全量崩溃测试

```bash
./test_all_crashes.py
```

覆盖 19 种信号 + 30 种崩溃场景，每种自动验证堆栈解析到正确的 `file:line`。

---

## 十、总结

```
         ┌────────────────────────────────────────────┐
         │           swp_stack_trace 核心配方           │
         │                                            │
         │  输入：信号 handler 中的 file descriptor     │
         │  产出：人类可读的调用栈文本                   │
         │                                            │
         │  配方：                                     │
         │    libunwind     ← 栈展开（DWARF 方式）      │
         │  + write()       ← 输出（async-signal-safe） │
         │  + 栈缓冲区       ← 格式转换（零分配）        │
         │  + noexcept       ← 错误处理（静默失败）      │
         │  + 零信号管理     ← 不安装/修改信号行为       │
         └────────────────────────────────────────────┘
```

**三个设计动词**：

1. **不侵入** — 库不安装 handler、不吞信号、不分配内存、不抛异常
2. **只打印** — 单一职责：读 libunwind → 格式化 → write()
3. **静默失败** — 任何错误都不影响原有程序，最坏情况是一个 no-op

**一个核心 trade-off**：为了 async-signal-safe，牺牲了"直接输出 demangled 函数名 + 源码行号"的便利。这部分通过离线工具 `symbolize_stack.py` 补偿。

---

## 参考资料

- [LLVM libunwind](https://github.com/llvm/llvm-project/tree/main/libunwind)
- [POSIX Async-Signal-Safe Functions](https://pubs.opengroup.org/onlinepubs/9699919799/functions/V2_chap02.html#tag_15_04_03)
- [libunwind API 文档](https://www.nongnu.org/libunwind/man/libunwind(3).html)
- [docs/signal-reference.md](signal-reference.md) — 本项目的信号详解
- [docs/known-issues.md](known-issues.md) — macOS/Linux 平台差异
- [docs/test-report.md](test-report.md) — 30 种 crash 类型测试报告
