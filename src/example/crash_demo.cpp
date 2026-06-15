#include "swp_stack_trace.h"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <unistd.h>

namespace {

// 崩溃信号 handler：先打印堆栈，再恢复默认 handler 并重新抛出信号，
// 以保留原有 core dump 行为。
// 注意：print_stacktrace 设计为 async-signal-safe（仅使用 write() 与 libunwind，
// 不调用 malloc/printf 等不可重入函数），因此可在信号 handler 中安全调用。
void crash_signal_handler(int sig) noexcept {
    swp_stack_trace::print_stacktrace(STDERR_FILENO);

    struct sigaction sa;
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(sig, &sa, nullptr) != 0) {
        _exit(EXIT_FAILURE);
    }

    if (raise(sig) != 0) {
        _exit(EXIT_FAILURE);
    }
}

void install_handler(int sig) noexcept {
    struct sigaction sa;
    sa.sa_handler = crash_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(sig, &sa, nullptr) != 0) {
        std::perror("sigaction");
        std::exit(EXIT_FAILURE);
    }
}

// 触发一个除零错误，产生 SIGFPE。
// 在 macOS ARM64 上整数除零不会触发信号，因此该函数主要在 Linux x86_64 上使用。
void trigger_division_by_zero() {
#if defined(__x86_64__)
    // 使用内联汇编执行 div 0，避免编译器优化掉纯 C 的除零表达式。
    unsigned long dummy = 1;
    asm volatile("xor %%rax, %%rax; div %0" : : "r"(dummy) : "rax", "rdx");
#else
    // 其他架构退化为 C 表达式；volatile 可减少被优化掉的概率。
    volatile int a = 1;
    volatile int b = 0;
    volatile int c = a / b;  // NOLINT
    (void)c;
#endif
}

// 触发一个段错误，产生 SIGSEGV。
void trigger_segfault() {
    int* p = nullptr;
    *p = 42;  // NOLINT
}

// 触发一个非法指令，产生 SIGILL（适用于 macOS ARM64）。
void trigger_sigill() {
#if defined(__arm64__) || defined(__aarch64__)
    asm volatile(".long 0x00000000");  // NOLINT
#elif defined(__x86_64__)
    asm volatile("ud2");  // NOLINT
#else
    // 未知架构：__builtin_trap() 通常触发 SIGTRAP/SIGILL，
    // 本 demo 未安装 SIGTRAP handler，因此该 fallback 主要用于编译通过。
    __builtin_trap();
#endif
}

} // namespace

int main() {
    try {
        install_handler(SIGFPE);
        install_handler(SIGSEGV);
        install_handler(SIGILL);

        std::cerr << "About to crash..." << std::endl;

        // 在 macOS ARM64 上整数除零不会触发 SIGFPE，因此使用 SIGILL 验证。
#if defined(__APPLE__) && (defined(__arm64__) || defined(__aarch64__))
        trigger_sigill();
#else
        trigger_division_by_zero();
#endif
    } catch (const std::exception& e) {
        std::cerr << "unexpected exception: " << e.what() << std::endl;
        return EXIT_FAILURE;
    } catch (...) {
        std::cerr << "unknown unexpected exception" << std::endl;
        return EXIT_FAILURE;
    }

    return 0;
}
