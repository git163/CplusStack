# swp_stack_trace 崩溃堆栈打印库设计

- 日期: 2026-06-15
- 作者: Claude Code / tshua
- 状态: 已批准
- 关联: [CLAUDE.md](../../../../CLAUDE.md), [README.md](../../../../README.md)

## 背景

项目 `CplusStack` 当前是一个仅包含占位 `main.cpp` 和烟雾测试的 C++17 模板工程。需要将其改造为一个可复用的崩溃堆栈打印库，供其它 Linux C++ 程序在崩溃时使用。

核心诉求：

- 崩溃发生时能打印调用栈（如除零、越界等同步信号触发时）。
- 库本身不做任何信号处理，不破坏原有程序的 signal handler、core dump 等行为。
- 目标运行环境以 Linux 为主，macOS 能编译即可。

## 目标

- 提供一个 C++17 静态库 `libswp_stack_trace.a`。
- 提供异步信号安全的栈展开与输出接口 `swp_stack_trace::print_stacktrace(int fd)`。
- 不在信号路径中分配内存，不调用 `malloc` / `printf` / `__cxa_demangle` 等非 async-signal-safe 函数。
- 不安装 signal handler、不修改信号默认行为、不吞信号。
- 使用 LLVM libunwind（Apache 2.0 with LLVM Exceptions），闭源商业可用。
- 提供单元测试和可运行的示例程序。

## 非目标

- 不替使用方捕获或处理信号。
- 不在信号 handler 内做 C++ 符号 demangle 或文件行号解析。
- 不处理 C++ 异常堆栈（`try/catch/throw`），只面向 POSIX 信号场景。
- 不保证 macOS 上的完整运行时可移植性，仅要求能编译通过。
- MVP 不提供 CMake `FindSwpStackTrace`、pkg-config 或安装规则。

## 方案

### 架构 / 流程

```
用户程序
    │
    │ 安装自定义 signal handler
    ▼
信号触发（SIGSEGV / SIGFPE / ...）
    │
    ▼
用户 handler
    │
    ├── 调用 swp_stack_trace::print_stacktrace(fd)
    │       │
    │       ▼
    │   libswp_stack_trace.a
    │       │
    │       ├── unw_getcontext()
    │       ├── unw_init_local()
    │       ├── 循环 unw_step()
    │       │       ├── unw_get_reg(UNW_REG_IP)   → PC
    │       │       ├── unw_get_proc_name()        → mangled 函数名
    │       │       └── 手写格式化 + write(fd)
    │       └── 返回
    │
    └── （可选）恢复默认 handler 并重新 raise(sig)，保留 core dump 行为
```

### 关键设计点

1. **零侵入**：库只暴露一个打印函数，不注册 handler、不改 sigaction、不改 terminate handler。
2. **异步信号安全**：实现仅使用 LLVM libunwind 的 async-safe 本地展开 API 和 `write()` 系统调用。
3. **无动态内存**：所有格式化在栈上固定大小缓冲区完成，不调用 `malloc` / `new`。
4. **原始输出**：输出 PC + mangled 函数名 + offset，事后用 `addr2line` 离线翻译。
5. **静默失败**：任何错误（unwind 失败、fd 无效、写入失败）都不抛异常、不 `abort`、不修改 `errno`，直接返回。

### 接口 / 数据结构

```cpp
// src/include/swp_stack_trace.h
#ifndef SWP_STACK_TRACE_H_
#define SWP_STACK_TRACE_H_

#include <unistd.h>

namespace swp_stack_trace {

// 以异步安全方式将当前调用栈写入 fd。
// 默认输出到 STDERR_FILENO。
// 若 fd 无效或写入失败，静默失败（noexcept）。
void print_stacktrace(int fd = STDERR_FILENO) noexcept;

} // namespace swp_stack_trace

#endif // SWP_STACK_TRACE_H_
```

### 输出格式

```
#0  0x00005555555551a9  _ZN4Test4funcEi+0x25
#1  0x00005555555551d8  main+0x18
#2  0x00007ffff7a29d90  __libc_start_call_main+0x80
```

每帧一行，`#N` 为帧序号，`PC` 为程序计数器，函数名为 mangled 形式，offset 为函数内偏移。

事后翻译示例：

```bash
addr2line -e ./your_binary 0x00005555555551a9
```

> 注意：现代 Linux 默认开启 PIE，PC 为虚拟地址。对于 PIE 可执行文件或共享库，离线使用 `addr2line` 时需要结合 `/proc/self/maps` 计算模块内偏移，本库不在 handler 内处理该逻辑。

## 影响范围

- 新增 `src/include/swp_stack_trace.h` 公共头文件。
- 新增 `src/swp_stack_trace.cpp` 实现文件。
- 原 `src/main.cpp` 改为 `src/example/crash_demo.cpp`，作为示例程序。
- `CMakeLists.txt` 改为构建静态库 `libswp_stack_trace.a`、示例程序 `crash_demo` 和单元测试。
- 新增 `tests/swp_stack_trace/TestSwpStackTrace.cpp` 单元测试。
- `README.md` 更新为库的使用说明。

## 风险与对策

| 风险 | 对策 |
|------|------|
| 栈/堆已严重损坏，unwind 二次崩溃 | 库本身不分配内存、不调用复杂函数；所有错误静默处理，降低二次崩溃概率。 |
| `libunwind` 目标环境未安装 | 文档说明安装方式；构建时检测并给出清晰错误。 |
| PIE/ASLR 导致 `addr2line` 偏移不准 | 文档说明离线翻译方法，MVP 不在 handler 内处理。 |
| 重新 `raise(sig)` 后 core dump 记录的是 handler 上下文 | 这是使用方的信号处理策略问题；本库在 `raise` 前打印原始栈，文本与 core 互补。 |
| 栈溢出场景 handler 自身崩溃 | 文档说明需使用 `sigaltstack` 配置备用信号栈。 |

## 实施步骤

- [ ] 创建 `src/include/swp_stack_trace.h` 公共头文件。
- [ ] 创建 `src/swp_stack_trace.cpp`，基于 LLVM libunwind 实现 `print_stacktrace`。
- [ ] 修改 `CMakeLists.txt`：构建静态库 `libswp_stack_trace.a`、链接 libunwind、构建示例程序 `crash_demo`。
- [ ] 将 `src/main.cpp` 迁移为 `src/example/crash_demo.cpp`，演示 signal handler + 重新 raise + 打印堆栈。
- [ ] 创建 `tests/swp_stack_trace/TestSwpStackTrace.cpp`，用子进程触发信号并验证输出。
- [ ] 更新 `README.md` 为库的使用说明、构建方式和注意事项。
- [ ] 将设计文档状态更新为“已批准”。

## 测试计划

### 单元测试

- 使用 `dup2` 将 `STDERR_FILENO` 重定向到 pipe，捕获 `print_stacktrace` 的输出，验证包含当前测试函数的 mangled 名子串。
- 在当前进程中安装 `SIGUSR1` handler，handler 内部调用 `print_stacktrace` 写到 pipe，然后返回；验证输出包含当前测试函数名。
- 验证库不抛异常、不导致测试进程异常退出。

> 注：原计划使用 `fork()` 在子进程中测试，但在 macOS 上 `libunwind` 在 `fork()` 子进程中无法回溯到父函数帧，因此改为在当前进程中使用 `dup2` 和同步信号触发。

### 集成 / 端到端

- 运行 `crash_demo` 示例程序，触发信号后观察 stderr 输出。
- 用 `addr2line` 离线验证 PC 能解析到正确源码位置。

### 平台检查

- Linux：完整运行单元测试和示例。
- macOS：至少保证编译通过（运行时行为不保证一致）。

## 引用

- [LLVM libunwind](https://github.com/llvm/llvm-project/tree/main/libunwind)
- [libunwind API 文档](https://www.nongnu.org/libunwind/man/libunwind(3).html)
- [POSIX Async-Signal-Safe functions](https://pubs.opengroup.org/onlinepubs/9699919799/functions/V2_chap02.html#tag_15_04_03)
- [`addr2line` 文档](https://sourceware.org/binutils/docs/binutils/addr2line.html)
