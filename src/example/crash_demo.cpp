#include "swp_stack_trace.h"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>

namespace {

// 崩溃日志文件路径。
constexpr const char* k_crash_log_path = "/tmp/swp_crash.log";

// 崩溃信号 handler：将堆栈写入文件并输出到 stderr，再恢复默认 handler 并重新抛出信号，
// 以保留原有 core dump 行为。
// 注意：print_stacktrace 设计为 async-signal-safe（仅使用 write() 与 libunwind，
// 不调用 malloc/printf 等不可重入函数），因此可在信号 handler 中安全调用。
void crash_signal_handler(int sig) noexcept {
    swp_stack_trace::print_stacktrace(STDERR_FILENO);

    int log_fd = ::open(k_crash_log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd >= 0) {
        swp_stack_trace::print_stacktrace(log_fd);
        ::close(log_fd);
    }

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

// ---------- 各种崩溃触发函数 ----------

// 除零 → SIGFPE
__attribute__((noinline)) void trigger_sigfpe() {
#if defined(__x86_64__)
    unsigned long dummy = 1;
    asm volatile("xor %%rax, %%rax; div %0" : : "r"(dummy) : "rax", "rdx");
#elif defined(__arm64__) || defined(__aarch64__)
    // ARM64 上整数除零不触发 SIGFPE，使用 __builtin_trap() 代替，
    // 它会触发 SIGTRAP。安装 handler 时也注册了 SIGTRAP，因此能被捕获。
    __builtin_trap();
#else
    volatile int a = 1;
    volatile int b = 0;
    volatile int c = a / b;  // NOLINT
    (void)c;
#endif
}

// 空指针解引用 → SIGSEGV
__attribute__((noinline)) void trigger_sigsegv() {
    volatile int* p = nullptr;
    *p = 42;  // NOLINT
}

// 非法指令 → SIGILL
__attribute__((noinline)) void trigger_sigill() {
#if defined(__arm64__) || defined(__aarch64__)
    asm volatile(".long 0x00000000");  // NOLINT
#elif defined(__x86_64__)
    asm volatile("ud2");  // NOLINT
#else
    __builtin_trap();
#endif
}

// abort() → SIGABRT
__attribute__((noinline)) void trigger_sigabrt() {
    volatile int a = 1;
    volatile int b = 2;
    volatile __attribute__((unused)) int c = a + b;
    std::abort();
}

// 总线错误 → SIGBUS（非对齐访问 / mmap 的边界）
__attribute__((noinline)) void trigger_sigbus() {
#if defined(__x86_64__)
    // x86-64 默认允许非对齐访问，尝试 mmap 边界触发 SIGBUS
    long page_size = ::sysconf(_SC_PAGESIZE);
    void* p = ::mmap(nullptr, static_cast<std::size_t>(page_size),
                     PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        std::abort();
    }
    // 将最后一页保护为只读，写入时触发 SIGBUS
    ::mprotect(p, static_cast<std::size_t>(page_size), PROT_NONE);
    auto* cp = static_cast<char*>(p);
    cp[page_size - 1] = 'x';  // NOLINT
    ::munmap(p, static_cast<std::size_t>(page_size));
#else
    // ARM64 上非对齐访问更严格，可尝试偶数地址上做不对齐的原子操作
    volatile int* up = reinterpret_cast<volatile int*>(0x1);
    *up = 42;  // NOLINT — 大概率 SIGBUS
#endif
}

// 断点陷阱 → SIGTRAP
__attribute__((noinline)) void trigger_sigtrap() {
    __builtin_trap();
}

// ---------- 多层调用封装 ----------

// 在 crash_level_3 → crash_level_2 → crash_level_1 层叠下触发目标信号，
// 便于观察较深的调用栈。
typedef void (*trigger_fn_t)();
trigger_fn_t g_trigger_fn = nullptr;

__attribute__((noinline)) void crash_level_1() {
    g_trigger_fn();
}

__attribute__((noinline)) void crash_level_2() {
    crash_level_1();
}

__attribute__((noinline)) void crash_level_3() {
    crash_level_2();
}

// ---------- 普通堆栈打印 ----------

__attribute__((noinline)) void deep_call_1() {
    std::cerr << "\nStack trace from deep_call_1:" << std::endl;
    swp_stack_trace::print_stacktrace(STDERR_FILENO);
}

__attribute__((noinline)) void deep_call_2() {
    deep_call_1();
}

__attribute__((noinline)) void deep_call_3() {
    deep_call_2();
}

__attribute__((noinline)) void deep_call_4() {
    deep_call_3();
}

// ---------- 使用说明 ----------

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <crash_type>" << std::endl;
    std::cerr << "  crash types:" << std::endl;
    std::cerr << "    sigfpe    division by zero" << std::endl;
    std::cerr << "    sigsegv   null pointer dereference" << std::endl;
    std::cerr << "    sigill    illegal instruction" << std::endl;
    std::cerr << "    sigabrt   abort()" << std::endl;
    std::cerr << "    sigbus    bus error" << std::endl;
    std::cerr << "    sigtrap   breakpoint trap" << std::endl;
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc != 2) {
            print_usage(argc > 0 ? argv[0] : "crash_demo");
            return EXIT_FAILURE;
        }

        const char* crash_type = argv[1];

        // 根据参数选择触发函数
        if (std::strcmp(crash_type, "sigfpe") == 0) {
            g_trigger_fn = trigger_sigfpe;
        } else if (std::strcmp(crash_type, "sigsegv") == 0) {
            g_trigger_fn = trigger_sigsegv;
        } else if (std::strcmp(crash_type, "sigill") == 0) {
            g_trigger_fn = trigger_sigill;
        } else if (std::strcmp(crash_type, "sigabrt") == 0) {
            g_trigger_fn = trigger_sigabrt;
        } else if (std::strcmp(crash_type, "sigbus") == 0) {
            g_trigger_fn = trigger_sigbus;
        } else if (std::strcmp(crash_type, "sigtrap") == 0) {
            g_trigger_fn = trigger_sigtrap;
        } else {
            std::cerr << "Unknown crash type: " << crash_type << std::endl;
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }

        // 安装全部信号 handler
        install_handler(SIGFPE);
        install_handler(SIGSEGV);
        install_handler(SIGILL);
        install_handler(SIGABRT);
        install_handler(SIGBUS);
        install_handler(SIGTRAP);

        // 先打印一次正常深度调用栈
        std::cerr << "Printing stack trace from a deep call stack..." << std::endl;
        deep_call_4();

        // 然后触发指定类型的崩溃
        std::cerr << "\nAbout to crash with signal type: " << crash_type << "..." << std::endl;
        crash_level_3();
    } catch (const std::exception& e) {
        std::cerr << "unexpected exception: " << e.what() << std::endl;
        return EXIT_FAILURE;
    } catch (...) {
        std::cerr << "unknown unexpected exception" << std::endl;
        return EXIT_FAILURE;
    }

    return 0;
}
