# swp_stack_trace Demo — 信号参考手册

> 基于 [crash_demo.cpp](../src/example/crash_demo.cpp) 的完整信号覆盖说明

## 信号分类总览

本 demo 注册了 **19 种信号**，分为四类：**程序错误**、**终端交互**、**资源限制**、**管道与定时器**。每种信号都展示了工业级服务在生产环境中应如何处理。

---

## 一、程序错误信号

这类信号由 CPU / 内核 / 运行时库在检测到程序逻辑错误时自动产生，是线上崩溃的主要来源。

### SIGSEGV — 无效内存访问

| 属性 | 说明 |
|------|------|
| 默认行为 | 终止进程 + core dump |
| 是否可以捕获 | 可以（但 handler 必须快速返回或 exit） |
| 是否可以忽略 | 不可以（UB 后进程状态不可靠） |
| 生产处理建议 | 记录堆栈 → 恢复 SIG_DFL → re-raise → 产出 core dump |

**demo 覆盖的触发场景**：

| crash 类型 | 触发手段 | 生产场景对应 |
|-----------|---------|------------|
| `sigsegv` | 空指针解引用 `*nullptr` | 最经典的崩溃 |
| `stack_overflow` | 无限递归 `1KB × ∞` | 递归边界错误、局部变量过大 |
| `wild_ptr_write` | 写 `0xDEADBEEF` | 野指针、悬垂指针 |
| `null_virtual` | `nullptr->virtual_method()` | 已析构对象调用虚函数 |
| `write_rodata` | 向字符串字面量写入 | 误用 `const_cast` 修改常量 |
| `heap_buf_of` | `malloc(1)` 后写 64KB | 堆缓冲区溢出 |
| `exec_nx` | mmap 无 PROT_EXEC 后 jmp | JIT 沙箱绕过、shellcode 注入 |

> **SIGSEGV 是生产中最常见的崩溃信号。** 必须捕获并记录堆栈。

---

### SIGFPE — 算术异常

| 属性 | 说明 |
|------|------|
| 默认行为 | 终止进程 + core dump |
| 是否可以捕获 | 可以 |
| 是否可以忽略 | 可以（但可能陷入死循环） |
| 生产处理建议 | 记录调用上下文，安全退出 |

**demo 触发方法**：

| crash 类型 | 触发手段 | 生产场景对应 |
|-----------|---------|------------|
| `sigfpe` | x86_64 `div 0` 内联汇编 | 除零错误、浮点异常 |

> **注意**：ARM64 上整数除零**不产生** SIGFPE（硬件静默返回 0），实际生产环境中浮点除零（如 `1.0/0.0` + FE_DIVBYZERO 使能）才会触发。

---

### SIGILL — 非法指令

| 属性 | 说明 |
|------|------|
| 默认行为 | 终止进程 + core dump |
| 是否可以捕获 | 可以 |
| 是否可以忽略 | 不建议 |
| 生产处理建议 | 记录堆栈 + 紧急退出 |

**demo 触发方法**：

| crash 类型 | 触发手段 | 生产场景对应 |
|-----------|---------|------------|
| `sigill` | x86_64 `ud2` / ARM64 `.long 0x00000000` | 二进制损坏、指令集不匹配 |

---

### SIGABRT — 进程异常终止

| 属性 | 说明 |
|------|------|
| 默认行为 | 终止进程 + core dump |
| 是否可以捕获 | 可以 |
| 是否可以忽略 | 可以（但进程即将退出） |
| 生产处理建议 | 记录堆栈 → 清理资源 → 退出 |

**demo 覆盖的触发场景**：

| crash 类型 | 触发手段 | 生产场景对应 |
|-----------|---------|------------|
| `sigabrt` | 显式调用 `abort()` | `assert()` 失败 |
| `double_free` | `free(p); free(p)` | 双重释放，allocator 检测后 abort |
| `pure_virtual` | ctor 中调用纯虚函数 | 类层次结构设计错误 |
| `invalid_delete` | `free(&stack_var)` | 野指针传给 free/delete |
| `terminate` | `noexcept` 函数中 throw | 未捕获的 C++ 异常 |
| `stack_buf_of` | 栈上 `char[16]` 写 4KB → canary | 栈缓冲区溢出，__stack_chk_fail |

> **SIGABRT 是仅次于 SIGSEGV 的第二常见线上崩溃原因。**

---

### SIGBUS — 总线错误

| 属性 | 说明 |
|------|------|
| 默认行为 | 终止进程 + core dump |
| 是否可以捕获 | 可以 |
| 是否可以忽略 | 不建议 |
| 生产处理建议 | 同 SIGSEGV 处理 |

**demo 触发方法**：

| crash 类型 | 触发手段 | 生产场景对应 |
|-----------|---------|------------|
| `sigbus` | mmap + mprotect(PROT_NONE) 后写入 | mmap 文件被截断后访问、硬件故障 |

---

### SIGTRAP — 调试陷阱

| 属性 | 说明 |
|------|------|
| 默认行为 | 终止进程 + core dump |
| 是否可以捕获 | 可以 |
| 是否可以忽略 | 取决于场景 |
| 生产处理建议 | 生产环境中视为崩溃处理；开发环境让调试器接管 |

**demo 触发方法**：

| crash 类型 | 触发手段 | 生产场景对应 |
|-----------|---------|------------|
| `sigtrap` | `__builtin_trap()` | 断点、调试预警 |

---

### SIGSYS — 非法系统调用

| 属性 | 说明 |
|------|------|
| 默认行为 | 终止进程 + core dump |
| 是否可以捕获 | 可以 |
| 是否可以忽略 | 不建议 |
| 生产处理建议 | 记录堆栈 + 退出 |

**demo 触发方法**：

| crash 类型 | 触发手段 | 生产场景对应 |
|-----------|---------|------------|
| `sigsys` | `syscall(99999)` | seccomp 沙箱拦截、内核版本不兼容 |

---

### SIGEMT — 仿真陷阱（非 POSIX，BSD / 老 Unix）

| 属性 | 说明 |
|------|------|
| 默认行为 | 终止进程 + core dump |
| Linux 可用性 | 通常不可用（内核未定义） |
| macOS 可用性 | 不可用（未定义） |
| 生产处理建议 | 若平台支持，同 SIGBUS 处理 |

**demo 触发方法**：

| crash 类型 | 触发手段 | 生产场景对应 |
|-----------|---------|------------|
| `sigemt` | `__builtin_trap()` 近似 | 浮点仿真器错误、硬件故障（极罕见） |

> **注意**：demo 中 SIGEMT 使用 `#if defined(SIGEMT)` 条件编译，在 Linux/macOS 上 handler 实际不会安装。

---

## 二、终端交互信号

这类信号由用户或进程管理器主动发送，生产环境中应正确处理以实现**优雅关闭**。

### SIGINT — 中断信号（Ctrl+C）

| 属性 | 说明 |
|------|------|
| 默认行为 | 终止进程 |
| 是否建议捕获 | **强烈建议** |
| 生产处理建议 | 打印堆栈（用于排查卡死原因）→ 关闭连接 → 释放锁 → 退出 |

**demo 触发方法**：

| crash 类型 | 触发手段 |
|-----------|---------|
| `sigint` | `raise(SIGINT)` 模拟 Ctrl+C |

```cpp
// 生产环境示例：信号 handler 中设置全局标志，主循环检查后优雅退出
volatile sig_atomic_t g_shutdown = 0;
void handle_sigint(int) { g_shutdown = 1; }
// main loop: while (!g_shutdown) { ... }
```

---

### SIGTERM — 终止信号（kill 默认）

| 属性 | 说明 |
|------|------|
| 默认行为 | 终止进程 |
| 是否建议捕获 | **必须**（容器/K8s 通过 SIGTERM 优雅终止） |
| 生产处理建议 | 打印堆栈 → 关闭监听端口 → 等待请求处理完 → 退出；超时则 `_exit` |

**demo 触发方法**：

| crash 类型 | 触发手段 |
|-----------|---------|
| `sigterm` | `raise(SIGTERM)` 模拟 `kill <pid>` |

> **关键**：容器编排系统（K8s、Docker）通过 SIGTERM 先通知进程退出（默认有 30s 超时），超时后发送 SIGKILL。未处理 SIGTERM 会导致请求被强行中断。

---

### SIGQUIT — 退出信号（Ctrl+\）

| 属性 | 说明 |
|------|------|
| 默认行为 | 终止进程 + core dump |
| 是否建议捕获 | **建议捕获** |
| 生产处理建议 | 记录堆栈（核心！）→ 优雅关闭 → 可能产生 core dump |

**demo 触发方法**：

| crash 类型 | 触发手段 |
|-----------|---------|
| `sigquit` | `raise(SIGQUIT)` 模拟 Ctrl+\ |

> **技巧**：SIGQUIT 是唯一一个"默认行为会 core dump 的外部信号"。可以在不杀死进程的情况下向运行中的进程发送 `kill -QUIT <pid>`，获取堆栈又产生 core dump。

---

### SIGHUP — 挂断信号

| 属性 | 说明 |
|------|------|
| 默认行为 | 终止进程 |
| 是否建议捕获 | **强烈建议** |
| 生产处理建议 | **不退出！** 重新加载配置文件、重开日志文件 |

**demo 触发方法**：

| crash 类型 | 触发手段 |
|-----------|---------|
| `sighup` | `raise(SIGHUP)` 模拟终端挂断 |

```cpp
// 生产环境示例：守护进程收到 SIGHUP → 重载配置
void handle_sighup(int) {
    reload_config();    // 重新读取配置文件
    reopen_logs();      // 日志文件轮转后重新打开
}
```

> **最佳实践**：nginx、PostgreSQL 等主流守护进程都使用 SIGHUP 实现配置热加载，而非退出。

---

## 三、资源限制信号

这类信号由内核在进程超出 `setrlimit` 设定限制时产生，用于防止单一进程耗尽系统资源。

### SIGXCPU — CPU 时间超限

| 属性 | 说明 |
|------|------|
| 默认行为 | 终止进程 + core dump |
| 是否建议捕获 | **建议捕获** |
| 生产处理建议 | 记录堆栈 + 保存状态 → 尽快退出 |

**demo 触发方法**：

| crash 类型 | 触发手段 |
|-----------|---------|
| `sigxcpu` | `setrlimit(RLIMIT_CPU, 1)` + 死循环 |

> `setrlimit(RLIMIT_CPU, ...)` 在进程运行超过设定秒数后先发送 SIGXCPU；若进程未退出，每秒再发送一次直到达到 `rlim_max`。

---

### SIGXFSZ — 文件大小超限

| 属性 | 说明 |
|------|------|
| 默认行为 | 终止进程 + core dump |
| 是否建议捕获 | **建议捕获** |
| 生产处理建议 | 清理临时文件 → 通知客户端 |
| **特殊注意** | handler 中写日志也会受 FSIZE 限制！handler 内应避免写文件，或临时提升限制 |

**demo 触发方法**：

| crash 类型 | 触发手段 |
|-----------|---------|
| `sigxfsz` | `setrlimit(RLIMIT_FSIZE, 8192)` + 写 100KB 数据 |

> **陷阱**：handler 中写日志文件同样受 FSIZE 限制。demo 设为 8KB 而非 1 字节，确保 handler 有足够空间记录堆栈。生产环境应在 handler 中 `setrlimit(RLIMIT_FSIZE, {RLIM_INFINITY, RLIM_INFINITY})` 后再写日志。

---

### SIGVTALRM — 虚拟定时器

| 属性 | 说明 |
|------|------|
| 默认行为 | 终止进程 |
| 是否建议捕获 | 视使用场景 |
| 生产处理建议 | 轻量级统计、定期采样堆栈；不应在 handler 中做重操作 |

**demo 触发方法**：

| crash 类型 | 触发手段 |
|-----------|---------|
| `sigvtalrm` | `setitimer(ITIMER_VIRTUAL, 100ms)` + 死循环 |

> `ITIMER_VIRTUAL` 只统计**进程在用户态消耗的 CPU 时间**，I/O 等待不算。`ITIMER_PROF` 也统计内核态时间。`ITIMER_REAL` 统计墙上时钟（等价于 alarm）。

---

### SIGPROF — Profiling 定时器

| 属性 | 说明 |
|------|------|
| 默认行为 | 终止进程 |
| 是否建议捕获 | 视使用场景 |
| 生产处理建议 | 专用于 profiler（如 gperftools）；不应在通用服务中捕获 |

**demo 触发方法**：

| crash 类型 | 触发手段 |
|-----------|---------|
| `sigprof` | `setitimer(ITIMER_PROF, 100ms)` + 死循环 |

---

## 四、管道与定时器信号

### SIGPIPE — 向无读者的管道/套接字写入

| 属性 | 说明 |
|------|------|
| 默认行为 | 终止进程 |
| 是否建议捕获 | **不捕获，直接忽略！** |
| 生产处理建议 | 全局 `signal(SIGPIPE, SIG_IGN)`，让 `write()` 返回 `EPIPE` 由应用层处理 |

**demo 触发方法**：

| crash 类型 | 触发手段 |
|-----------|---------|
| `sigpipe` | `pipe()` + `close(读端)` + `write(写端)` |

```cpp
// 生产环境正确做法：在 main() 开头忽略 SIGPIPE
signal(SIGPIPE, SIG_IGN);
// 然后在 send/write 处检查 EPIPE 错误码来处理断连
```

> **这是唯一建议在生产代码中直接 `SIG_IGN` 的信号。** demo 为了演示完整性仍注册了 handler，但实际集成时应使用 `SIG_IGN`。

---

### SIGALRM — 定时器信号

| 属性 | 说明 |
|------|------|
| 默认行为 | 终止进程 |
| 是否建议捕获 | 视使用场景 |
| 生产处理建议 | 超时检测、心跳；handler 应极轻量（设置标志位） |

**demo 触发方法**：

| crash 类型 | 触发手段 |
|-----------|---------|
| `sigalrm` | `alarm(1)` + `pause()` |

```cpp
// 生产环境示例：为阻塞调用设置超时
void handle_sigalrm(int) { /* 仅设置标志位 */ }
signal(SIGALRM, handle_sigalrm);
alarm(5);
read(fd, buf, size);  // 5 秒后自动中断
alarm(0);             // 取消
```

---

## 信号默认行为速查表

| 信号 | 编号 | 默认行为 | 建议处理 |
|------|------|---------|---------|
| SIGHUP | 1 | 终止 | 重载配置 |
| SIGINT | 2 | 终止 | 优雅退出 |
| SIGQUIT | 3 | 终止+core | 优雅退出+core |
| SIGILL | 4 | 终止+core | 记录+退出 |
| SIGTRAP | 5 | 终止+core | 记录+退出 |
| SIGABRT | 6 | 终止+core | 记录+退出 |
| SIGEMT | 7 | 终止+core | 记录+退出（非 POSIX） |
| SIGFPE | 8 | 终止+core | 记录+退出 |
| SIGBUS | 10 | 终止+core | 记录+退出 |
| SIGSEGV | 11 | 终止+core | 记录+退出 |
| SIGSYS | 12 | 终止+core | 记录+退出 |
| SIGPIPE | 13 | 终止 | **忽略**（用 SIG_IGN） |
| SIGALRM | 14 | 终止 | 轻量标志位 |
| SIGTERM | 15 | 终止 | 优雅退出 |
| SIGXCPU | 24 | 终止+core | 记录+保存状态 |
| SIGXFSZ | 25 | 终止+core | 清理+退出 |
| SIGVTALRM | 26 | 终止 | 轻量统计 |
| SIGPROF | 27 | 终止 | 专用于 profiler |

## demo 中未捕获的信号

以下信号因性质特殊，demo 有意不捕获（遵循生产环境最佳实践）：

| 信号 | 原因 |
|------|------|
| SIGKILL (9) | 不可捕获（内核强制终止） |
| SIGSTOP (19) | 不可捕获（内核强制暂停） |
| SIGCHLD (20) | 子进程状态变化，通常由 `waitpid` 处理 |
| SIGURG (23) | 带外数据到达，TCP 场景专用 |
| SIGIO (29) | 异步 I/O，应用层用 epoll 替代 |
| SIGWINCH (28) | 终端窗口大小变化，不适用 |
| SIGUSR1/2 (30/31) | 应用自定义信号，demo 中 SIGUSR1 用于单元测试 |

---

## 参考资料

- [POSIX Signal Concepts](https://pubs.opengroup.org/onlinepubs/9699919799/basedefs/V1_chap03.html#tag_03_03)
- [POSIX Async-Signal-Safe Functions](https://pubs.opengroup.org/onlinepubs/9699919799/functions/V2_chap02.html#tag_15_04_03)
- [Linux man 7 signal](https://man7.org/linux/man-pages/man7/signal.7.html)
- [macOS signal(3)](x-man-page://3/signal)
