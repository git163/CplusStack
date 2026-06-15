# 已知问题

## 1. macOS ARM64 上 sigabrt 帧名被误报为 trigger_sigbus

- **现象**：macOS ARM64 上触发 SIGABRT（`std::abort()`）时，堆栈中 `trigger_sigabrt` 所在帧的符号名被 `atos` 解析为 `trigger_sigbus`。
- **原因**：`std::abort()` 是 noreturn 函数，编译器将其调用指令放置在 `trigger_sigabrt` 函数体的最末尾。`abort()` 内部调用 `pthread_kill(SIGABRT)` 时将返回地址（下一条指令的 PC）保存在栈上，该地址恰好落在相邻的 `trigger_sigbus` 函数起始地址上。libunwind / atos 按 PC 查找符号时匹配到了相邻函数。
- **影响**：仅影响符号名称显示，堆栈层级和调用关系仍然正确。实际崩溃原因是 `std::abort()`，handler 中也正确捕获了 `SIGABRT` 信号。
- **平台**：仅 macOS ARM64。Linux ELF + addr2line 不受影响（GNU addr2line 处理函数边界的方式不同）。
- **缓解**：这是 demo 程序中的编译器布局问题，不是 swp_stack_trace 库的缺陷。生产环境中 `abort()` 的调用方通常有足够大的函数体，不会触发此边界情况。

## 2. macOS ARM64 上整数除零不触发 SIGFPE

- **现象**：macOS ARM64 上，C 语言整数除零表达式（`int c = a / b`，其中 `b = 0`）不触发 `SIGFPE` 信号。
- **原因**：ARM64 架构的整数除法指令不产生硬件异常，而是静默返回 0。
- **影响**：crash_demo 中 `sigfpe` 类型在 macOS ARM64 上无法用纯 C 表达式触发，改用 `__builtin_trap()` 充当 fallback。
- **平台**：仅 macOS ARM64。Linux x86_64 上用内联汇编 `div` 可以正常触发。
- **缓解**：生产环境中若需触发 SIGFPE，应在 x86_64 上运行，或使用浮点除零（`1.0 / 0.0`，需开启浮点异常）。

## 3. macOS 上 fork() 后子进程 libunwind 无法展开父进程栈帧

- **现象**：`fork()` 创建的子进程中调用 `print_stacktrace()`，只能展开到 `main`，无法看到 `fork()` 调用方及更上层的函数帧。
- **原因**：macOS 上 libunwind 在 `fork()` 子进程中无法正确读取父进程的 unwind 表。
- **影响**：单元测试中不使用 `fork()` 方案，改用当前进程内的 `dup2` 重定向 + 同步信号触发。
- **平台**：仅 macOS。Linux 上 `fork()` 后 libunwind 可正常展开。

## 4. macOS 上 addr2line 不可用，需用 atos

- **现象**：macOS 默认不含 `addr2line`，即使通过 Homebrew 安装 GNU binutils，其 `addr2line` 也不支持 Mach-O 格式。
- **影响**：`symbolize_stack.py` 在 macOS 上自动选用 `atos` 并自动计算 ASLR slide。
- **平台**：仅 macOS。Linux 上直接使用 GNU addr2line。

## 5. PIE / ASLR 下 addr2line 需要额外处理

- **现象**：现代 Linux 默认开启 PIE（位置无关可执行文件），运行时 PC 地址是虚拟地址。直接用 `addr2line -e binary <virtual_pc>` 可能无法解析。
- **原因**：`addr2line` 期望的是**模块内偏移**（基于 0 的偏移），而非虚拟地址。
- **影响**：对于共享库（`.so`），需要结合 `/proc/self/maps` 计算模块基址偏移。对于可执行文件，GNU addr2line 通常能自动处理 PIE。
- **缓解**：使用 `symbolize_stack.py` 可自动处理；手动使用时请参考 crash_demo 打印的加载基址或 `/proc/<pid>/maps`。

## 6. 栈/堆已严重损坏时 print_stacktrace 可能失败

- **现象**：如果崩溃的根因已导致栈内存损坏或 unwind 元数据不可读，`print_stacktrace` 可能输出乱码、部分帧或完全失败。
- **原因**：libunwind 依赖栈上的 unwind 表（`.eh_frame`）和寄存器上下文，两者在栈损坏时会失效。
- **影响**：这是所有堆栈回溯工具的共性限制，不是 swp_stack_trace 的特有缺陷。
- **缓解**：此时应依赖 core dump 文件进行事后分析。`print_stacktrace` 在 handler 中尽早调用，可最大程度保留原始崩溃现场。

## 7. macOS 上 `__stack_chk_fail` 不触发信号 handler

- **现象**：macOS 上触发栈缓冲区溢出（`stack_buf_of`）时，`__stack_chk_fail` 被调用，但 SIGABRT handler 未被执行。进程直接以 exit code 134（SIGABRT）终止。
- **原因**：macOS 的 `__stack_chk_fail` 实现直接调用 `__abort()` 或 `_exit()`，绕过了正常的 `abort()` → `raise(SIGABRT)` 路径，因此已安装的 SIGABRT handler 不会触发。
- **影响**：`stack_buf_of` 类型在 macOS 上无法通过 handler 捕获堆栈；Linux 上 `__stack_chk_fail` → `abort()` → handler 正常工作。
- **平台**：仅 macOS。
- **缓解**：在 Linux 上验证栈缓冲区溢出场景。macOS 上可通过 crash_demo 正常堆栈打印（deep_call）间接验证该场景的调用链。

## 8. 栈溢出需要 sigaltstack 且回调函数要尽量轻量

- **现象**：栈溢出时，普通信号 handler 因栈空间耗尽无法执行。需要 `sigaltstack` 分配备用信号栈 + `SA_ONSTACK` 标志。
- **影响**：crash_demo 的 `stack_overflow` 类型已内置 sigaltstack 设置，handler 在备用栈上执行。实际集成时使用方需自行配置。
- **注意**：备用栈大小需权衡（太大会浪费内存，太小 handler 无法执行）。推荐使用 `SIGSTKSZ`（POSIX 定义的最小值），本例中为安全起见使用 `SIGSTKSZ`（通常 ≥ 8KB）。
