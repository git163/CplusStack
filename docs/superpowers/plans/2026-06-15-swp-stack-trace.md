# swp_stack_trace Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build an async-signal-safe crash stack trace static library for Linux C++17 programs using LLVM libunwind, without installing signal handlers.

**Architecture:** A single static library `libswp_stack_trace.a` exposes `print_stacktrace(int fd)`. Internally it uses LLVM libunwind to walk the stack and writes raw PC + mangled function names to the file descriptor using only async-signal-safe operations. A demo program and GTest unit tests verify behavior.

**Tech Stack:** C++17, CMake 3.20+, LLVM libunwind, GoogleTest

---

## File Structure

| File | Responsibility |
|------|----------------|
| `src/include/swp_stack_trace.h` | Public API header declaring `print_stacktrace(int fd)`. |
| `src/swp_stack_trace.cpp` | Library implementation using LLVM libunwind. |
| `src/example/crash_demo.cpp` | Example program showing signal handler + re-raise usage. |
| `src/example/CMakeLists.txt` | Build rules for the example program. |
| `tests/swp_stack_trace/TestSwpStackTrace.cpp` | Unit tests for the library. |
| `CMakeLists.txt` | Top-level build rules: static library, example, tests. |
| `tests/CMakeLists.txt` | Test target linking gtest and the library. |
| `README.md` | Usage documentation. |

Files to delete:
- `src/main.cpp` (replaced by example).
- `tests/TestSmoke.cpp` (replaced by real tests).

---

### Task 1: Create Public Header

**Files:**
- Create: `src/include/swp_stack_trace.h`

- [ ] **Step 1: Write the public header**

```cpp
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

- [ ] **Step 2: Commit**

```bash
git add src/include/swp_stack_trace.h
git commit -m "feat: add swp_stack_trace public header"
```

---

### Task 2: Add Failing Unit Test Skeleton

**Files:**
- Create: `tests/swp_stack_trace/TestSwpStackTrace.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Create the test file**

```cpp
#include <gtest/gtest.h>
#include <unistd.h>

#include "swp_stack_trace.h"

namespace {

void function_for_test() {
    swp_stack_trace::print_stacktrace(STDERR_FILENO);
}

} // namespace

TEST(SwpStackTrace, DoesNotCrash) {
    function_for_test();
    SUCCEED();
}
```

- [ ] **Step 2: Update tests/CMakeLists.txt to link the library**

Replace:
```cmake
target_link_libraries(unit_tests PRIVATE GTest::gtest GTest::gtest_main)
```

With:
```cmake
target_link_libraries(unit_tests PRIVATE GTest::gtest GTest::gtest_main swp_stack_trace unwind)
```

- [ ] **Step 3: Run the test to verify it fails**

Run:
```bash
cmake -S . -B build
cmake --build build --target unit_tests -j
```

Expected: build fails because `swp_stack_trace` target and `swp_stack_trace.cpp` do not exist yet.

- [ ] **Step 4: Commit**

```bash
git add tests/swp_stack_trace/TestSwpStackTrace.cpp tests/CMakeLists.txt
git commit -m "test: add failing skeleton test for swp_stack_trace"
```

---

### Task 3: Create Static Library Target with Stub Implementation

**Files:**
- Create: `src/swp_stack_trace.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write a stub implementation**

```cpp
#include "swp_stack_trace.h"

namespace swp_stack_trace {

void print_stacktrace(int fd) noexcept {
    (void)fd;
    // Intentionally empty stub for TDD; replaced in Task 4.
}

} // namespace swp_stack_trace
```

- [ ] **Step 2: Replace top-level CMakeLists.txt**

New content:

```cmake
cmake_minimum_required(VERSION 3.20)
project(swp_stack_trace LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 默认使用 RelWithDebInfo：优化 + 保留调试符号
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# 调试信息：所有 build type 都保留 -g
add_compile_options(-g)

# 公共头文件
set(PUBLIC_HEADERS
    src/include/swp_stack_trace.h
)

# 静态库
add_library(${PROJECT_NAME} STATIC src/swp_stack_trace.cpp ${PUBLIC_HEADERS})
target_include_directories(${PROJECT_NAME} PUBLIC src/include)
target_link_libraries(${PROJECT_NAME} PRIVATE unwind)

# 示例程序
add_subdirectory(src/example)

# 启用测试
option(BUILD_TESTING "Build unit tests" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
```

- [ ] **Step 3: Create src/example/CMakeLists.txt**

```cmake
add_executable(crash_demo crash_demo.cpp)
target_link_libraries(crash_demo PRIVATE swp_stack_trace)
```

- [ ] **Step 4: Create src/example/crash_demo.cpp placeholder**

```cpp
#include "swp_stack_trace.h"
#include <iostream>

int main() {
    std::cout << "placeholder demo" << std::endl;
    return 0;
}
```

- [ ] **Step 5: Run the test to verify it compiles and passes**

```bash
cmake -S . -B build
cmake --build build --target unit_tests -j
ctest --test-dir build --output-on-failure
```

Expected: `SwpStackTrace.DoesNotCrash` passes.

- [ ] **Step 6: Commit**

```bash
git add src/swp_stack_trace.cpp CMakeLists.txt src/example/CMakeLists.txt src/example/crash_demo.cpp
git commit -m "build: add swp_stack_trace static library target"
```

---

### Task 4: Implement Real Stack Trace Printing

**Files:**
- Modify: `src/swp_stack_trace.cpp`

- [ ] **Step 1: Replace stub with full implementation**

```cpp
#include "swp_stack_trace.h"

#include <cstddef>
#include <cstdint>
#include <libunwind.h>
#include <unistd.h>

namespace swp_stack_trace {
namespace {

constexpr std::size_t k_name_buffer_size = 256;
constexpr std::size_t k_write_buffer_size = 64;

// 异步安全地写入 C 字符串
void write_string(int fd, const char* str) {
    if (fd < 0 || str == nullptr) {
        return;
    }
    std::size_t len = 0;
    while (str[len] != '\0') {
        ++len;
    }
    while (len > 0) {
        const ssize_t written = ::write(fd, str, len);
        if (written < 0) {
            return;
        }
        str += static_cast<std::size_t>(written);
        len -= static_cast<std::size_t>(written);
    }
}

// 异步安全地写入单个字符
void write_char(int fd, char c) {
    if (fd < 0) {
        return;
    }
    ::write(fd, &c, 1);
}

// 异步安全地写入 uintptr_t 十六进制
void write_hex(int fd, std::uintptr_t value) {
    if (fd < 0) {
        return;
    }
    char buf[k_write_buffer_size];
    int i = 0;
    do {
        const int digit = static_cast<int>(value & 0xF);
        buf[i++] = static_cast<char>((digit < 10) ? ('0' + digit) : ('a' + digit - 10));
        value >>= 4;
    } while (value != 0);
    write_string(fd, "0x");
    while (i > 0) {
        write_char(fd, buf[--i]);
    }
}

// 异步安全地写入 unw_word_t 十六进制
void write_unw_word_hex(int fd, unw_word_t value) {
    if (fd < 0) {
        return;
    }
    char buf[k_write_buffer_size];
    int i = 0;
    do {
        const int digit = static_cast<int>(value & 0xF);
        buf[i++] = static_cast<char>((digit < 10) ? ('0' + digit) : ('a' + digit - 10));
        value >>= 4;
    } while (value != 0);
    write_string(fd, "0x");
    while (i > 0) {
        write_char(fd, buf[--i]);
    }
}

// 异步安全地写入非负十进制整数
void write_decimal(int fd, int value) {
    if (fd < 0) {
        return;
    }
    char buf[16];
    int i = 0;
    do {
        buf[i++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0);
    while (i > 0) {
        write_char(fd, buf[--i]);
    }
}

} // namespace

void print_stacktrace(int fd) noexcept {
    if (fd < 0) {
        return;
    }

    unw_cursor_t cursor;
    unw_context_t context;

    if (unw_getcontext(&context) != 0) {
        return;
    }
    if (unw_init_local(&cursor, &context) != 0) {
        return;
    }

    int frame = 0;
    char name_buffer[k_name_buffer_size];

    while (unw_step(&cursor) > 0) {
        unw_word_t pc = 0;
        if (unw_get_reg(&cursor, UNW_REG_IP, &pc) != 0) {
            continue;
        }

        unw_word_t offset = 0;
        const int name_result = unw_get_proc_name(&cursor, name_buffer, sizeof(name_buffer), &offset);

        write_char(fd, '#');
        write_decimal(fd, frame++);
        write_string(fd, "  ");
        write_hex(fd, static_cast<std::uintptr_t>(pc));
        write_string(fd, "  ");

        if (name_result == 0 && name_buffer[0] != '\0') {
            write_string(fd, name_buffer);
            write_string(fd, "+0x");
            write_unw_word_hex(fd, offset);
        } else {
            write_string(fd, "???");
        }
        write_char(fd, '\n');
    }
}

} // namespace swp_stack_trace
```

- [ ] **Step 2: Run tests**

```bash
cmake --build build --target unit_tests -j
ctest --test-dir build --output-on-failure
```

Expected: `SwpStackTrace.DoesNotCrash` passes.

- [ ] **Step 3: Commit**

```bash
git add src/swp_stack_trace.cpp
git commit -m "feat: implement async-signal-safe stack trace printer with libunwind"
```

---

### Task 5: Add Output Verification Test

**Files:**
- Modify: `tests/swp_stack_trace/TestSwpStackTrace.cpp`

- [ ] **Step 1: Add output-capturing test**

Append to the test file:

```cpp
#include <sys/types.h>
#include <sys/wait.h>
#include <cstdlib>
#include <string>

TEST(SwpStackTrace, OutputContainsCurrentFunction) {
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    const pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        // 子进程：关闭读端，打印栈，关闭写端，退出
        close(pipefd[0]);
        swp_stack_trace::print_stacktrace(pipefd[1]);
        close(pipefd[1]);
        _exit(EXIT_SUCCESS);
    }

    close(pipefd[1]);

    char buffer[4096] = {};
    const ssize_t n = read(pipefd[0], buffer, sizeof(buffer) - 1);
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);

    ASSERT_GT(n, 0);
    const std::string output(buffer, static_cast<std::size_t>(n));

    // mangled 名中必然包含当前测试函数名的子串
    EXPECT_NE(output.find("OutputContainsCurrentFunction"), std::string::npos)
        << "Stack trace output was:\n" << output;
}
```

- [ ] **Step 2: Run tests**

```bash
cmake --build build --target unit_tests -j
ctest --test-dir build --output-on-failure
```

Expected: `SwpStackTrace.OutputContainsCurrentFunction` passes.

- [ ] **Step 3: Commit**

```bash
git add tests/swp_stack_trace/TestSwpStackTrace.cpp
git commit -m "test: verify stack trace output contains current function name"
```

---

### Task 6: Add Signal Context Test

**Files:**
- Modify: `tests/swp_stack_trace/TestSwpStackTrace.cpp`

- [ ] **Step 1: Add signal-handler test**

Append to the test file:

```cpp
#include <csignal>

namespace {
int g_signal_test_pipe_fd = -1;

void signal_test_handler(int sig) {
    swp_stack_trace::print_stacktrace(g_signal_test_pipe_fd);

    struct sigaction sa;
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(sig, &sa, nullptr);

    raise(sig);
}

void trigger_signal_in_child(int pipe_write_fd) {
    g_signal_test_pipe_fd = pipe_write_fd;

    struct sigaction sa;
    sa.sa_handler = signal_test_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, nullptr);

    raise(SIGUSR1);
    _exit(EXIT_FAILURE); // 不应该执行到这里
}

} // namespace

TEST(SwpStackTrace, WorksFromSignalHandler) {
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    const pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        close(pipefd[0]);
        trigger_signal_in_child(pipefd[1]);
    }

    close(pipefd[1]);

    char buffer[4096] = {};
    const ssize_t n = read(pipefd[0], buffer, sizeof(buffer) - 1);
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ(WTERMSIG(status), SIGUSR1);

    ASSERT_GT(n, 0);
    const std::string output(buffer, static_cast<std::size_t>(n));

    EXPECT_NE(output.find("trigger_signal_in_child"), std::string::npos)
        << "Stack trace from signal handler was:\n" << output;
}
```

- [ ] **Step 2: Run tests**

```bash
cmake --build build --target unit_tests -j
ctest --test-dir build --output-on-failure
```

Expected: `SwpStackTrace.WorksFromSignalHandler` passes.

- [ ] **Step 3: Commit**

```bash
git add tests/swp_stack_trace/TestSwpStackTrace.cpp
git commit -m "test: verify stack trace printing works inside signal handler"
```

---

### Task 7: Implement Crash Demo Example

**Files:**
- Modify: `src/example/crash_demo.cpp`

- [ ] **Step 1: Replace placeholder with real demo**

```cpp
#include "swp_stack_trace.h"

#include <csignal>
#include <cstdlib>
#include <iostream>

namespace {

void crash_signal_handler(int sig) {
    swp_stack_trace::print_stacktrace(STDERR_FILENO);

    struct sigaction sa;
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(sig, &sa, nullptr);

    raise(sig);
}

void install_handler(int sig) {
    struct sigaction sa;
    sa.sa_handler = crash_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(sig, &sa, nullptr) != 0) {
        std::perror("sigaction");
        std::exit(EXIT_FAILURE);
    }
}

void trigger_division_by_zero() {
    volatile int a = 1;
    volatile int b = 0;
    volatile int c = a / b;  // NOLINT
    (void)c;
}

} // namespace

int main() {
    install_handler(SIGFPE);
    install_handler(SIGSEGV);

    std::cerr << "About to crash..." << std::endl;
    trigger_division_by_zero();

    return 0;
}
```

- [ ] **Step 2: Build and run the demo**

```bash
cmake --build build --target crash_demo -j
./build/src/example/crash_demo
```

Expected: stderr shows stack trace lines like:
```
#0  0x...  _ZN...trigger_division_by_zero... +0x...
#1  0x...  main+0x...
```
Then process terminates due to SIGFPE.

- [ ] **Step 3: Commit**

```bash
git add src/example/crash_demo.cpp
git commit -m "feat: add crash_demo example showing signal handler + re-raise"
```

---

### Task 8: Update README

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Replace README.md with library documentation**

```markdown
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
./build/src/example/crash_demo
```

示例程序会触发除零错误，在崩溃前打印调用栈到 stderr。

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

> 注意：现代 Linux 默认开启 PIE，PC 为虚拟地址。对于 PIE 可执行文件或共享库，离线使用 `addr2line` 时需要结合 `/proc/self/maps` 计算模块内偏移。

## 测试

```bash
ctest --test-dir build --output-on-failure
```

## 注意事项

- 本库不在信号路径中分配内存，但无法保证栈/堆已彻底损坏时 100% 成功。
- 若需处理栈溢出（stack overflow）导致的 `SIGSEGV`，请提前使用 `sigaltstack` 配置备用信号栈。
```

- [ ] **Step 2: Commit**

```bash
git add README.md
git commit -m "docs: rewrite README for swp_stack_trace library"
```

---

### Task 9: Clean Up Obsolete Files and Final Verification

**Files:**
- Delete: `src/main.cpp`
- Delete: `tests/TestSmoke.cpp`

- [ ] **Step 1: Delete obsolete files**

```bash
rm src/main.cpp tests/TestSmoke.cpp
```

- [ ] **Step 2: Clean build and run everything**

```bash
rm -rf build
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/src/example/crash_demo
```

Expected:
- All unit tests pass.
- Demo prints stack trace and terminates with SIGFPE.

- [ ] **Step 3: Commit**

```bash
git rm src/main.cpp tests/TestSmoke.cpp
git add -A
git commit -m "chore: remove placeholder main and smoke test"
```

---

## Self-Review Checklist

- [ ] Spec coverage: 所有设计点（公共 API、libunwind 实现、无信号处理、无内存分配、示例、测试、README）均有对应任务。
- [ ] Placeholder scan: 无 TBD/TODO/占位符。
- [ ] Type一致性: `print_stacktrace(int fd)` 签名在头文件、实现、测试中一致。
- [ ] Build integrity: 删除 `src/main.cpp` 后，顶层 `CMakeLists.txt` 不再构建 `stack` 可执行文件，而是构建 `libswp_stack_trace.a` 库。
