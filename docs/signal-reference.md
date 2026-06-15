# swp_stack_trace Demo — 信号详解

> 基于 [crash_demo.cpp](../src/example/crash_demo.cpp) 中覆盖的全部信号，逐一解释每种信号的来源、含义和触发原理。

---

## 什么是信号

信号（signal）是 Unix/Linux 内核向进程发送的一种**软件中断**。当某个事件发生时——比如进程访问了非法内存、执行了非法指令、或者用户按下了 Ctrl+C——内核会向目标进程递送一个信号。进程收到信号后，可以根据信号类型选择三种响应方式之一：

1. **执行默认动作**（终止、忽略、core dump 等）
2. **执行自定义 handler**（进程预先注册的信号处理函数）
3. **忽略信号**（前提是该信号允许被忽略）

`swp_stack_trace` 库的核心设计就是让你在自己的信号 handler 中安全地打印崩溃时的调用栈。

---

## 一、程序错误信号

这类信号是 CPU/MMU（内存管理单元）在执行指令时检测到硬件异常后，由内核转换为信号发给进程的。最常见的线上崩溃都来自这一类。

### SIGSEGV (11) — 段错误，无效内存访问

**触发原理**：CPU 在执行访存指令（load/store）时，MMU 发现目标虚拟地址没有合法的页表映射，或者当前权限不允许该操作（比如试图写只读页）。CPU 产生一个**页错误（page fault）**，内核检查后发现这次访问确实是非法访问而并非缺页中断，于是向进程发送 SIGSEGV。

**常见原因**：
- 解引用空指针（地址 `0x0` 在各平台上永远不映射）
- 访问已释放的内存（悬垂指针指向的页已被 munmap）
- 写入只读内存（字符串字面量在 `.rodata` 段中不可写）
- 栈溢出（递归耗尽栈空间，触碰 guard page）

**demo 中的触发方式**：

| crash 类型 | 如何触发 |
|-----------|---------|
| `sigsegv` | `int* p = nullptr; *p = 42` |
| `wild_ptr_write` | 写 `0xDEADBEEF`，一个确定未映射的地址 |
| `null_virtual` | `nullptr->virtual_method()`，虚函数调用需要读取对象首部的 vtable 指针 |
| `write_rodata` | `const_cast` 后写字符串字面量，所在页为只读 |
| `stack_overflow` | 递归在栈上反复分配 1KB，最终触碰到栈底的 guard page |
| `heap_buf_of` | `malloc(1)` 后向 64KB 外写入，跨越分配的堆页 |
| `exec_nx` | mmap 一页写指令但不设 PROT_EXEC，jmp 过去执行 |

---

### SIGBUS (10) — 总线错误

**触发原理**：SIGBUS 和 SIGSEGV 都和内存访问有关，但原因不同。SIGSEGV 是"这个地址根本没映射"或"权限不对"，而 SIGBUS 通常是"地址本身有效，但这次访问在硬件层面不被支持"。典型场景包括：

- **非对齐访问（unaligned access）**：某些 CPU 架构（ARM、SPARC）要求特定类型的数据必须对齐到特定地址边界。比如 `int` 必须对齐 4 字节，访问地址 `0x1` 会触发 SIGBUS
- **mmap 文件被截断**：用 mmap 映射了一个文件，之后该文件被外部截断，访问截断区域外的地址会触发 SIGBUS
- **硬件故障**：物理内存损坏

**demo 中的触发方式**：

| crash 类型 | 如何触发 |
|-----------|---------|
| `sigbus` | mmap 一页后 `mprotect(PROT_NONE)` 将其彻底锁死，再写页末 |

> **x86_64 和 ARM64 的差异**：x86_64 默认允许非对齐访问（硬件会自动处理，只是稍慢），所以用 `0x1` 写 `int` 在 x86_64 上不会触发 SIGBUS，只会触发 SIGSEGV。ARM64 对非对齐更敏感，用奇数地址大概率收到 SIGBUS。

---

### SIGFPE (8) — 浮点异常 / 算术异常

**触发原理**：虽然名字叫"浮点异常"，实际涵盖所有算术运算错误。CPU 在执行除法指令时，如果除数为零、或商超出目标寄存器的表示范围（如 `INT_MIN / -1`），会产生**除法错误异常（#DE on x86）**，内核将其转换为 SIGFPE 发给进程。

**常见原因**：
- 整数除零
- `INT_MIN / -1`（商溢出）
- 浮点异常使能时的浮点除零 / 溢出 / 下溢 / 无效操作（需要先 `feenableexcept` 开启）

**demo 中的触发方式**：

| crash 类型 | 如何触发 |
|-----------|---------|
| `sigfpe` | x86_64：内联汇编 `div 0`；ARM64：用 `__builtin_trap()` 替代（ARM64 整数除零不产生信号！） |

> **ARM64 的特殊性**：ARM64 架构的硬件除法指令不会对除零产生异常，而是静默返回 0。只有 x86_64 上才触发 SIGFPE。浮点除零需要在程序中手动开启异常才能使能。

---

### SIGILL (4) — 非法指令

**触发原理**：CPU 取指（fetch）阶段，指令译码器（decoder）发现当前字节序列不构成任何合法指令。或指令本身合法但当前特权级别不允许执行。CPU 产生**非法指令异常（#UD on x86）**，内核转为 SIGILL。

**常见原因**：
- 二进制文件损坏或版本不匹配
- 在旧 CPU 上执行新指令集（比如在不支持 AVX512 的 CPU 上执行 AVX512 指令）
- 跳转到数据段（把数据当代码执行）
- 手动插桩：`ud2`（x86_64）或 `.long 0x00000000`（ARM64）指令

**demo 中的触发方式**：

| crash 类型 | 如何触发 |
|-----------|---------|
| `sigill` | x86_64: `ud2` 指令；ARM64: `.long 0x00000000` |

---

### SIGABRT (6) — 进程中止

**触发原理**：SIGABRT 不是由硬件异常产生的，而是**软件主动发送**的。通常由 C 标准库的 `abort()` 函数调用 `raise(SIGABRT)` 触发。进程自身决定"无法继续安全执行"时，主动请求终止。

**常见触发路径**：
| 触发源 | 内部调用链 |
|--------|----------|
| `std::abort()` 或 `::abort()` | `abort()` → `raise(SIGABRT)` |
| `assert(cond)` 失败 | `__assert_fail` → `abort()` |
| glibc malloc 检测到堆损坏 | `malloc_printerr` → `abort()` |
| `__cxa_pure_virtual` 纯虚函数调用 | `__cxa_pure_virtual` → `abort()` |
| `std::terminate()` 未捕获的异常 | `std::terminate` → `abort()` |
| `__stack_chk_fail` 栈 canary 被改写 | `__stack_chk_fail` → `abort()` |

**demo 中的触发方式**：

| crash 类型 | 如何触发 |
|-----------|---------|
| `sigabrt` | 直接调用 `std::abort()` |
| `double_free` | `free(p)` 两次，allocator 检测到 double-free 后 `abort()` |
| `pure_virtual` | 在基类构造函数中调用纯虚函数 `→ __cxa_pure_virtual → abort()` |
| `invalid_delete` | `free(&stack_var)`，传栈地址给 free，malloc 检测到后 `abort()` |
| `terminate` | `noexcept` 函数内 `throw → std::terminate → abort()` |
| `stack_buf_of` | 栈缓冲区溢出改写 canary → `__stack_chk_fail → abort()` |

---

### SIGTRAP (5) — 调试陷阱

**触发原理**：这是专门为调试器设计的信号。CPU 执行 `int3`（x86）或 `brk`（ARM）等断点指令时触发。也用于 `__builtin_trap()` 的 fallback。内核也可以在某些条件下发送 SIGTRAP（如 `PTRACE_EVENT` 事件）。

**demo 中的触发方式**：

| crash 类型 | 如何触发 |
|-----------|---------|
| `sigtrap` | `__builtin_trap()` |

---

### SIGSYS (31) — 非法系统调用

**触发原理**：进程调用 `syscall()` 时传入了一个**内核不认识的系统调用号**。或者 seccomp（安全计算模式）沙箱规则拦截了某个系统调用。内核检测到后发送 SIGSYS。

**demo 中的触发方式**：

| crash 类型 | 如何触发 |
|-----------|---------|
| `sigsys` | `syscall(99999)`，传入不存在的系统调用号 |

> **容器 / 沙箱场景**：Docker 默认 seccomp profile 会拦截部分系统调用。如果你的程序在高版本内核上编译后再在低版本内核上运行，也可能因未知调用号触发 SIGSYS。

---

### SIGEMT (7) — 仿真陷阱（非 POSIX）

**触发原理**：这是 BSD 系统的老遗留信号，最初用于浮点仿真器（模拟没有硬件 FPU 的 CPU）。现代系统基本不再使用。Linux 和 macOS 均**未定义**此信号。demo 中用 `#if defined(SIGEMT)` 条件编译，仅在支持该信号的系统上安装 handler。

**demo 中的触发方式**：

| crash 类型 | 如何触发 |
|-----------|---------|
| `sigemt` | `__builtin_trap()` 近似模拟 |

---

## 二、终端交互信号

这类信号由**终端驱动程序**或 **init/进程管理器**产生，体现的是"人"或"系统"对进程的控制意图，而非程序自身错误。

### SIGINT (2) — 中断

**触发原理**：用户在终端按下 **Ctrl+C** 时，终端驱动程序向**前台进程组的所有进程**发送 SIGINT。信号从终端 → 内核 tty 子系统 → 进程。

**为什么需要知道它**：线上服务可能在前台运行（如开发环境），Ctrl+C 是最常见的非正常终止方式。捕获 SIGINT 能让你看到"程序当时卡在哪"。

**demo 中的触发方式**：

| crash 类型 | 如何触发 |
|-----------|---------|
| `sigint` | `raise(SIGINT)` 模拟 Ctrl+C |

---

### SIGTERM (15) — 终止

**触发原理**：`kill <pid>` 命令的默认信号。也是容器编排系统（Kubernetes、Docker）停止容器时首先发送的信号。由另一个进程（或 init 系统）通过 `kill()` 系统调用发送。

**为什么重要**：K8s 停止 Pod 时发送 SIGTERM → 等待 30 秒 → 如果进程还未退出则 SIGKILL。未捕获 SIGTERM 意味着进程连"打扫战场"的机会都没有。

**demo 中的触发方式**：

| crash 类型 | 如何触发 |
|-----------|---------|
| `sigterm` | `raise(SIGTERM)` 模拟 `kill <pid>` |

---

### SIGQUIT (3) — 退出并 core dump

**触发原理**：用户按下 **Ctrl+\\** 时由终端驱动程序产生。和 SIGINT 的区别是，**SIGQUIT 的默认行为会产生 core dump 文件**。

**为什么有用**：不需要杀死进程就能拿到堆栈。向运行中的进程发送 `kill -QUIT <pid>`：
- 收到 SIGQUIT → handler 打印堆栈 → 恢复 SIG_DFL → raise → core dump
- 进程继续运行还是退出由你的 handler 决定

**demo 中的触发方式**：

| crash 类型 | 如何触发 |
|-----------|---------|
| `sigquit` | `raise(SIGQUIT)` 模拟 Ctrl+\\ |

---

### SIGHUP (1) — 挂断

**触发原理**：最初用于终端挂断（调制解调器断开），现在主要用于**守护进程收到重载配置指令**。当控制终端关闭时，内核向该终端上的会话首进程发送 SIGHUP。守护进程也可以手动 `kill -HUP <pid>` 来通知重载配置。

**为什么不是"崩溃"**：SIGHUP 通常**不是错误**。nginx、PostgreSQL 等守护进程收到 SIGHUP 后会重新读取配置文件并保持运行，而非退出。

**demo 中的触发方式**：

| crash 类型 | 如何触发 |
|-----------|---------|
| `sighup` | `raise(SIGHUP)` |

---

## 三、资源限制信号

内核在进程超出 `setrlimit()` 设定的资源上限时，自动向进程发送这类信号。它们是操作系统的"防御机制"，防止单一进程耗尽系统资源。

### SIGXCPU (24) — CPU 时间超限

**触发原理**：当进程的 CPU 累计使用时间超过 `setrlimit(RLIMIT_CPU, ...)` 设定的软限制时，内核发送 SIGXCPU。如果进程处理完该信号后继续运行，内核每秒再发一次，直到硬限制才强制 SIGKILL。

**demo 中的触发方式**：

| crash 类型 | 如何触发 |
|-----------|---------|
| `sigxcpu` | `setrlimit(RLIMIT_CPU, 1)` 设为 1 秒 → 死循环空转 → 1 秒后收到信号 |

---

### SIGXFSZ (25) — 文件大小超限

**触发原理**：当进程对某个文件执行 `write()` 时，如果当前文件的字节数 + 本次写入量超过了 `setrlimit(RLIMIT_FSIZE, ...)` 设定的软限制，内核发送 SIGXFSZ，`write()` 返回 -1 并设 `errno = EFBIG`。

**陷阱**：handler 内部写日志文件同样受此限制！如果限制设得太小（如 1 字节），handler 内的 `write()` 也会失败。demo 中设定为 8KB，确保 handler 有足够空间记录堆栈。

**demo 中的触发方式**：

| crash 类型 | 如何触发 |
|-----------|---------|
| `sigxfsz` | `setrlimit(RLIMIT_FSIZE, 8192)` → 写入 100KB 数据 |

---

### SIGVTALRM (26) — 虚拟定时器

**触发原理**：`setitimer(ITIMER_VIRTUAL, ...)` 设置一个**用户态 CPU 时间**定时器。内核只统计进程在用户态消耗的 CPU 时间，I/O 等待和内核态时间不计入。到期时发送 SIGVTALRM。

**和 SIGALRM 的区别**：SIGALRM 算的是墙上时钟（真实时间流逝），SIGVTALRM 只算用户态 CPU 时间。

**demo 中的触发方式**：

| crash 类型 | 如何触发 |
|-----------|---------|
| `sigvtalrm` | `setitimer(ITIMER_VIRTUAL, 100ms)` → 死循环消耗用户态 CPU |

---

### SIGPROF (27) — Profiling 定时器

**触发原理**：和 SIGVTALRM 类似，但 `ITIMER_PROF` 同时统计**用户态 + 内核态** CPU 时间（仍然不包括 I/O 等待）。gperftools、Python profiler 等工具用这个信号定期中断进程来采样调用栈。

**demo 中的触发方式**：

| crash 类型 | 如何触发 |
|-----------|---------|
| `sigprof` | `setitimer(ITIMER_PROF, 100ms)` → 死循环消耗 CPU |

---

## 四、管道与定时器信号

### SIGPIPE (13) — 管道破裂

**触发原理**：进程向一个**读端已关闭**的管道或套接字执行 `write()` 时，内核返回 `EPIPE` 错误，并向进程发送 SIGPIPE。

**为什么生产环境要忽略**：假设你写了一个网络服务，客户端突然断开了 TCP 连接。你的服务往已关闭的 socket 里 `send()` → 收到 SIGPIPE → 默认行为是**进程直接被杀死**。一次客户端断连导致整个服务宕掉，这显然不对。

```cpp
// 正确做法：全局忽略 SIGPIPE，让 write/send 返回 EPIPE 由应用层处理
signal(SIGPIPE, SIG_IGN);
```

**demo 中的触发方式**：

| crash 类型 | 如何触发 |
|-----------|---------|
| `sigpipe` | `pipe()` → `close(读端)` → `write(写端)` |

> **唯一建议在生产代码中 `SIG_IGN` 的信号。**

---

### SIGALRM (14) — 实时定时器

**触发原理**：`alarm(n)` 告知内核在 **n 秒后**（墙上时钟）向进程发送 SIGALRM。基于墙上时钟，包含进程被挂起的时间。`setitimer(ITIMER_REAL)` 功能相同但精度更高（微秒级）。

**常见用途**：为可能阻塞的系统调用设置超时。

```cpp
signal(SIGALRM, handler);
alarm(5);
read(fd, buf, size);  // 5 秒后自动被 SIGALRM 中断
alarm(0);             // 取消
```

**demo 中的触发方式**：

| crash 类型 | 如何触发 |
|-----------|---------|
| `sigalrm` | `alarm(1)` + `pause()` 等待信号到达 |

---

## 信号速查表

| 信号 | 编号 | 来源 | 默认行为 | 能否捕获 | 能否忽略 |
|------|------|------|---------|---------|---------|
| SIGHUP | 1 | 终端挂起 | 终止 | 能 | 能 |
| SIGINT | 2 | Ctrl+C | 终止 | 能 | 能 |
| SIGQUIT | 3 | Ctrl+\\ | 终止 + core | 能 | 能 |
| SIGILL | 4 | CPU 非法指令 | 终止 + core | 能 | 能 |
| SIGTRAP | 5 | 断点指令 | 终止 + core | 能 | 能 |
| SIGABRT | 6 | abort() | 终止 + core | 能 | 能 |
| SIGEMT | 7 | 仿真陷阱 | 终止 + core | 能 | 能 |
| SIGFPE | 8 | 算术异常 | 终止 + core | 能 | 能 |
| SIGKILL | 9 | kill -9 | 终止 | **否** | **否** |
| SIGBUS | 10 | 总线错误 | 终止 + core | 能 | 能 |
| SIGSEGV | 11 | 段错误 | 终止 + core | 能 | 能 |
| SIGSYS | 12 | 非法系统调用 | 终止 + core | 能 | 能 |
| SIGPIPE | 13 | 管道破裂 | 终止 | 能 | 能 |
| SIGALRM | 14 | alarm() | 终止 | 能 | 能 |
| SIGTERM | 15 | kill 默认 | 终止 | 能 | 能 |
| SIGSTOP | 17 | Ctrl+Z / kill -stop | 暂停 | **否** | **否** |
| SIGXCPU | 24 | CPU 超限 | 终止 + core | 能 | 能 |
| SIGXFSZ | 25 | 文件大小超限 | 终止 + core | 能 | 能 |
| SIGVTALRM | 26 | 虚拟定时器 | 终止 | 能 | 能 |
| SIGPROF | 27 | Profiling | 终止 | 能 | 能 |

## demo 未覆盖的信号

| 信号 | 原因 |
|------|------|
| SIGKILL (9) | **内核强制终止**，不可被捕获或忽略——这是操作系统最后的杀手锏 |
| SIGSTOP (19) | **内核强制暂停**，不可被捕获——防止恶意进程拒绝被调试 |
| SIGCHLD (17) | 子进程状态变化，通常由 `waitpid()` 在应用层处理，不属于崩溃场景 |
| SIGURG (23) | TCP 带外数据（OOB），epoll 模式下极少使用 |
| SIGWINCH (28) | 终端窗口大小变化，仅交互式程序关注 |
| SIGUSR1/2 (30,10) | 应用自定义——测试已覆盖 SIGUSR1（单元测试中用管道捕获堆栈） |

---

## 参考资料

- [POSIX Signal Concepts](https://pubs.opengroup.org/onlinepubs/9699919799/basedefs/V1_chap03.html#tag_03_03)
- [Linux man 7 signal](https://man7.org/linux/man-pages/man7/signal.7.html)
- [macOS signal(3)](x-man-page://3/signal)
