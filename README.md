# swp_stack_trace

一个 C++17 异步信号安全的崩溃堆栈打印库，基于 LLVM libunwind。库本身不安装信号处理函数、不吞信号、不分配内存。

## 依赖

- CMake ≥ 3.20
- C++17 编译器（GCC ≥ 7, Clang ≥ 5）
- LLVM libunwind

在 Debian/Ubuntu 上安装 libunwind：

```bash
sudo apt-get install libunwind-dev
```

## 构建

```bash
cmake -S . -B build
cmake --build build -j
```

## 运行示例

```bash
./run.sh
# 或
./build/src/example/crash_demo
```

示例程序会触发崩溃信号，在崩溃前打印调用栈到 stderr。

## 使用方式

```cpp
#include "swp_stack_trace.h"
#include <csignal>
#include <cstdlib>

void my_crash_handler(int sig) {
    swp_stack_trace::print_stacktrace(STDERR_FILENO);

    // 恢复默认 handler 并重新抛出信号，保留 core dump 行为
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

int main() {
    std::signal(SIGSEGV, my_crash_handler);
    std::signal(SIGFPE, my_crash_handler);
    // ...
}
```

编译链接：

```bash
g++ your_app.cpp -lswp_stack_trace -lunwind
```

## 输出格式

```
#0  0x00005555555551a9  _ZN4Test4funcEi+0x25
#1  0x00005555555551d8  main+0x18
```

函数名为 mangled 形式。事后可用 `addr2line` 离线翻译：

```bash
addr2line -e ./your_binary 0x00005555555551a9
```

也可以直接通过 `symbolize_stack.py` 自动解析整段堆栈输出：

```bash
./run.sh 2>&1 | ./symbolize_stack.py ./build/src/example/crash_demo
```

> 注意：现代 Linux 默认开启 PIE，PC 为虚拟地址。对于 PIE 可执行文件或共享库，离线使用 `addr2line` 时需要结合 `/proc/self/maps` 计算模块内偏移。

## 测试

```bash
./run_tests.sh
```

或直接运行：

```bash
ctest --test-dir build --verbose
```

## 注意事项

- 本库不在信号路径中分配内存，但无法保证栈/堆已彻底损坏时 100% 成功。
- 若需处理栈溢出（stack overflow）导致的 `SIGSEGV`，请提前使用 `sigaltstack` 配置备用信号栈。
- macOS 上整数除零不会触发 `SIGFPE`，示例程序在 macOS ARM64 上会自动使用 `SIGILL` 演示。
